// Machine state snapshot (PLAN D3): everything the translated program and the
// native platform services read, taken at an instruction boundary. Used by the
// development harness (--save-state / --load-state) and later by rollback (P10).
// Host hooks (std::function members, replay pointer) are not state.
#include "stv/gsp/machine.hpp"

#include <cstring>
#include <type_traits>
#include <new>

namespace stv::gsp {

namespace {
constexpr char kMagic[8] = {'S', 'T', 'V', 'M', 'S', '0', '0', '3'};   // 003: original/native data images and latched scene width
// Optional enhanced trailer: weapon/power-up variant state (VariantState, version-checked).
constexpr char kVariantTag[4] = {'V', 'A', 'R', '1'};
static_assert(std::is_trivially_copyable_v<VariantState>);

struct Out {
    std::vector<u8>& b;
    void raw(const void* p, std::size_t n) { const u8* q = static_cast<const u8*>(p); b.insert(b.end(), q, q + n); }
    template <class T> void pod(const T& v) { raw(&v, sizeof v); }
    template <class T> void vec(const std::vector<T>& v) { const u64 n = v.size(); pod(n); raw(v.data(), n * sizeof(T)); }
};
struct In {
    const std::vector<u8>& b;
    std::size_t at = 0;
    bool ok = true;
    void raw(void* p, std::size_t n) {
        if (!ok || n > b.size() - at) { ok = false; return; }
        std::memcpy(p, b.data() + at, n);
        at += n;
    }
    template <class T> void pod(T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            u8 value = 0;
            raw(&value, 1);
            if (value > 1) { ok = false; return; }
            v = value != 0;
        } else raw(&v, sizeof v);
    }
    template <class T> void vec(std::vector<T>& v) {
        u64 n = 0;
        pod(n);
        if (!ok || n > (b.size() - at) / sizeof(T)) { ok = false; return; }
        v.resize(std::size_t(n));
        raw(v.data(), std::size_t(n) * sizeof(T));
    }
};

template <class S, class M> void fields(S& s, M& m, bool with_prog) {
    s.pod(m.c);
    s.vec(m.ram); s.vec(m.palette); s.vec(m.pens); s.vec(m.cmos); s.vec(m.vram); s.vec(m.frame);
    // Program, native tables and derived graphics are immutable during play.
    // Full snapshots own them; rollback uses the verified local data. Carrying
    // gfx includes the enhanced layout's derived strip tail on a fresh restore.
    if (with_prog) { s.vec(m.prog); s.vec(m.enhanced_data); s.vec(m.gfx); }
    s.pod(m.vram_w); s.pod(m.vis_extra); s.pod(m.frame_vis_extra);
    s.pod(m.io); s.pod(m.dma); s.pod(m.cmos_page); s.pod(m.videobank_select); s.pod(m.autoerase_enable); s.pod(m.ports);
    s.pod(m.convsp); s.pod(m.convdp); s.pod(m.pixelshift); s.pod(m.gfxcycles);
    s.pod(m.vblank_start_ps); s.pod(m.scan_next_ps); s.pod(m.scan_param); s.pod(m.dma_done_ps); s.pod(m.vblank_next_ps);
    s.pod(m.vblank_end_ps); s.pod(m.next_event_cycle); s.pod(m.frame_number); s.pod(m.stop); s.pod(m.enhanced);
    s.pod(m.big_field_quirks); s.pod(m.external_timers); s.pod(m.check_requested);
}
}  // namespace

void Machine::save_state(std::vector<u8>& out, u32 pc, bool with_prog) const {
    out.clear();
    Out o{out};
    o.raw(kMagic, sizeof kMagic);
    o.pod(pc);
    const u8 flags = with_prog ? 1 : 0;
    o.pod(flags);
    fields(o, *this, with_prog);
    if (enhanced) {
        o.pod(aim_inputs); o.pod(shot_aim); o.pod(shot_muzzles); o.pod(move_inputs);
        // Variants off and never used: the snapshot keeps its previous layout.
        if (variant_mode || !variant_state_empty()) { o.raw(kVariantTag, sizeof kVariantTag); o.pod(variants); }
    }
}

bool Machine::load_state(const std::vector<u8>& in, u32& pc) {
    In i{in};
    char magic[8];
    i.raw(magic, sizeof magic);
    if (!i.ok || std::memcmp(magic, kMagic, sizeof magic) != 0) return false;
    u32 next_pc = 0;
    i.pod(next_pc);
    u8 flags = 0;
    i.pod(flags);
    if (!i.ok || (flags & ~u8(1)) != 0) return false;
    // Decode into an isolated owner. Neither malformed input nor allocation
    // failure can alter live state, its hooks, or the caller's program counter.
    try {
        Machine staged;
        fields(i, staged, flags != 0);
        // Older enhanced snapshots predate analog input.
        if (staged.enhanced && i.at < in.size()) {
            i.pod(staged.aim_inputs); i.pod(staged.shot_aim);
            if (i.at < in.size()) i.pod(staged.shot_muzzles);
            if (i.at < in.size()) i.pod(staged.move_inputs);
            if (i.ok && in.size() - i.at >= sizeof kVariantTag + sizeof(VariantState) &&
                std::memcmp(in.data() + i.at, kVariantTag, sizeof kVariantTag) == 0) {
                i.at += sizeof kVariantTag;
                i.pod(staged.variants);
                if (i.ok && staged.variants.version != VariantState::kVersion) return false;
            }
            // Older widescreen snapshots appended two 12-byte movement records.
            if (i.ok && in.size() - i.at == 24) i.at += 24;
        }
        if (!i.ok || i.at != in.size()) return false;
        if (staged.ram.size() != 0x10000 || staged.palette.size() != 0x2000 ||
            staged.pens.size() != 0x1000 || staged.cmos.size() != 0x4000 ||
            (staged.vram_w != 512 && staged.vram_w != 1024) ||
            staged.vis_extra > staged.vram_w - 410 || staged.frame_vis_extra > staged.vis_extra ||
            staged.vram.size() != std::size_t(staged.vram_w) * 512 ||
            staged.frame.size() != std::size_t(staged.vram_w) * SCREEN_H ||
            staged.cmos_page > 0x3000 || (staged.cmos_page & 0xfff) || staged.pixelshift > 4)
            return false;
        if (!flags && (staged.vram_w != vram_w || staged.vis_extra != vis_extra || staged.enhanced != enhanced))
            return false;
        if (flags && ((!staged.prog.empty() && staged.prog.size() != 0x80000) ||
                      (!staged.enhanced_data.empty() && staged.enhanced_data.size() != 0x80000) ||
                      ((!staged.prog.empty() || !staged.gfx.empty()) && staged.gfx.size() < 0x800000))) return false;
        if(!sprite_coverage.empty())staged.enable_sprite_coverage();
        // All allocation has finished. Swapping memories leaves the second
        // decode with exactly sized vectors, so this commit cannot allocate.
        ram.swap(staged.ram); palette.swap(staged.palette); pens.swap(staged.pens);
        cmos.swap(staged.cmos); vram.swap(staged.vram); frame.swap(staged.frame);
        if (flags) {
            prog.swap(staged.prog); enhanced_data.swap(staged.enhanced_data); gfx.swap(staged.gfx);
            static_data_replaced();
            retail_shadow_pixels=std::move(staged.retail_shadow_pixels);
        }
        In commit{in, 13};
        fields(commit, *this, flags != 0);
        move_inputs=staged.move_inputs;
        aim_inputs=staged.aim_inputs; shot_aim=staged.shot_aim; shot_muzzles=staged.shot_muzzles;
        variants=staged.variants;
        sprite_coverage.swap(staged.sprite_coverage);
        display_queue_coverage.swap(staged.display_queue_coverage);
        presentation_frame.swap(staged.presentation_frame);
        shadow_underlay.swap(staged.shadow_underlay);
        presentation_without_blobs.swap(staged.presentation_without_blobs);
        pc = next_pc;
        run_until_cycle = ~0ull;
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

}  // namespace stv::gsp
