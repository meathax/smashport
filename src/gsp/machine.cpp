// Native Y-unit platform services (PLAN D5). Behaviour mirrors MAME 0.289
// midyunit_v.cpp / midyunit_m.cpp / tms34010.cpp / screen.cpp for lockstep;
// divergences from MAME are marked "MAME:" where MAME's own behaviour is kept.
#include "stv/gsp/machine.hpp"
#include "sprite_shadows.hpp"
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"
#include "stv/zip.hpp"
#include "stv/gsp/rom_import.hpp"
#include <atomic>
#include <memory>
#include <cwctype>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace stv::gsp {

namespace {
constexpr u32 PROG_BASE = 0xFF800000u;

u32 pen_rgb(u16 w) {
    auto p5 = [](u32 v) { v &= 31; return (v << 3) | (v >> 2); };
    return (p5(w >> 10) << 16) | (p5(w >> 5) << 8) | p5(w);
}
} // namespace

void Machine::fault(const char* what, u32 addr) {
    if (faults.size() < 256 || std::strncmp(what, "untranslated", 12) == 0 || std::strncmp(what, "no translated", 13) == 0)
        faults.push_back({what, addr, c.cyc});
    if (std::strncmp(what, "untranslated", 12) == 0 || std::strncmp(what, "no translated", 13) == 0 ||
        std::strncmp(what, "entry is not", 12) == 0)
        stop = true;
}

void Machine::note_big_field_quirk(u32 a, int) {
    ++big_field_quirks;
    fault("MAME WFIELDMAC_BIG high-part write reproduced", a);
}

// ---------------------------------------------------------------- ROM import
namespace {
constexpr struct { const char* name; u32 size, crc; } expect[] = {
        {"la8_smash_tv_game_rom_u105.u105", 0x20000, 0x48cd793f}, {"la8_smash_tv_game_rom_u89.u89", 0x20000, 0x8e7fe463},
        {"la1_smash_tv_game_rom_u111.u111", 0x20000, 0x72f0ba84}, {"la1_smash_tv_game_rom_u112.u112", 0x20000, 0x436f0283},
        {"la1_smash_tv_game_rom_u113.u113", 0x20000, 0x4a4b8110}, {"la1_smash_tv_game_rom_u95.u95", 0x20000, 0xe864a44b},
        {"la1_smash_tv_game_rom_u96.u96", 0x20000, 0x15555ea7}, {"la1_smash_tv_game_rom_u97.u97", 0x20000, 0xccac9d9e},
        {"la1_smash_tv_game_rom_u106.u106", 0x20000, 0x5c718361}, {"la1_smash_tv_game_rom_u107.u107", 0x20000, 0x0fba1e36},
        {"la1_smash_tv_game_rom_u108.u108", 0x20000, 0xcb0a092f}, {"sl2_smash_tv_sound_rom_u4.u4", 0x10000, 0x29d3f6c8},
        {"sl2_smash_tv_sound_rom_u19.u19", 0x10000, 0xac5a402a}, {"sl2_smash_tv_sound_rom_u20.u20", 0x10000, 0x875c66d9}};

using Chips = std::array<std::vector<u8>, std::size(expect)>;
bool complete(const Chips& chips) {
    return std::all_of(chips.begin(), chips.end(), [](const auto& chip) { return !chip.empty(); });
}

bool collect(const std::filesystem::path& path, Chips& chips, std::string& error, bool* clone = nullptr) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto length = in.tellg();
    if (length < 0 || length > std::streamoff(max_archive_size)) {
        error = "Cannot read this ROM ZIP or it exceeds the size limit.";
        return false;
    }
    std::vector<u8> bytes(static_cast<size_t>(length));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), length)) { error = "Cannot read ZIP."; return false; }
    auto zip = std::make_unique<ZipArchive>();
    if (auto status = zip->open({bytes.data(), bytes.size()}); !status) { error = status.message; return false; }
    // L3 through L6 split clones may contain only their two alternate program
    // chips. Recognize those even in a renamed ZIP so we can find the L8 parent.
    if (clone) {
        constexpr std::array<u32, 8> clone_programs{0x33b626c3, 0x5f6fbc25, 0xa50ccb71, 0xef0b0279,
                                                    0x81f564b9, 0xe5017d25, 0xf1666017, 0x908aca5d};
        for (unsigned i = 0; i < zip->count; ++i) {
            const auto& entry = zip->entries[i];
            if (entry.size == 0x20000 && std::find(clone_programs.begin(), clone_programs.end(), entry.crc) != clone_programs.end()) {
                std::vector<u8> data(entry.size);
                if (zip->extract(i, {data.data(), data.size()})) { *clone = true; break; }
            }
        }
    }
    std::string problems;
    for (size_t k = 0; k < chips.size(); ++k) {
        if (!chips[k].empty()) continue;
        const auto& chip = expect[k];
        char problem[192];
        std::snprintf(problem, sizeof problem, "missing: %s", chip.name);
        for (unsigned i = 0; i < zip->count; ++i) {
            const auto& entry = zip->entries[i];
            if (entry.size == chip.size && entry.crc == chip.crc) {
                std::vector<u8> data(chip.size);
                if (zip->extract(i, {data.data(), data.size()})) { chips[k] = std::move(data); break; }
                std::snprintf(problem, sizeof problem, "damaged: %s", chip.name);
            } else if (!std::strcmp(entry.name, chip.name)) {
                if (entry.size != chip.size)
                    std::snprintf(problem, sizeof problem, "wrong size: %s (%u bytes, expected %u)", chip.name, entry.size, chip.size);
                else std::snprintf(problem, sizeof problem, "wrong CRC: %s", chip.name);
            }
        }
        if (chips[k].empty()) problems += std::string(problem) + "\n";
    }
    error = problems.empty() ? "" : "Missing or damaged Smash T.V. L8 data:\n" + problems;
    return true;
}

void install(Machine& m, Chips& chips) {
    m.enhanced_data.clear();
    m.prog.assign(0x100000 / 2, 0);                 // maindata region, zero filled
    for (usize i = 0; i < 0x20000; ++i) m.prog[0xC0000 / 2 + i] = u16(chips[0][i] | (chips[1][i] << 8));
    static const u32 off[9] = {0x000000, 0x020000, 0x040000, 0x200000, 0x220000, 0x240000, 0x400000, 0x420000, 0x440000};
    std::vector<u8> region(0x800000, 0);
    for (int k = 0; k < 9; ++k) {
        const auto& part = chips[2 + k];
        std::copy(part.begin(), part.end(), region.begin() + off[k]);
    }
    // MAME init_gfxrom(6): three 2-bit planes in chunks of region/4
    m.gfx.assign(0x800000, 0);
    const usize chunk = m.gfx.size() / 4;
    for (usize i = 0; i < m.gfx.size(); i += 2) {
        auto bits = [&](int plane, usize px) { return (region[plane * chunk + px / 4] >> (2 * (px % 4))) & 3; };
        m.gfx[i] = u8(bits(0, i) | (bits(1, i) << 2) | (bits(2, i) << 4));
        m.gfx[i + 1] = u8(bits(0, i + 1) | (bits(1, i + 1) << 2) | (bits(2, i + 1) << 4));
    }
    m.snd_u4 = std::move(chips[11]);
    m.snd_u19 = std::move(chips[12]);
    m.snd_u20 = std::move(chips[13]);
    m.static_data_replaced();
    if(!m.sprite_coverage.empty())m.enable_sprite_coverage();
}
}

void Machine::object_inserted(u32 node) {
    const u32 offset = node - object_pool_base;
    if (offset >= object_slots * object_stride || offset % object_stride) return;
    // Retail GANIOF/GETANIXY: ANI offset from the image header, reflected by flips.
    const u32 image = rlong(*this, node + 0x130), size = rlong(*this, node + 0xf0);
    u32 off = image ? rlong(*this, image + 0x20) : 0;
    if (s32(off) >= 0x02000000) off = 0;
    int x = s16(off), y = s16(off >> 16);
    const u16 flags = rw(node + 0xb0);
    if (flags & 0x10) x = int(size & 0xffff) - 1 - x;
    if (flags & 0x20) y = int(size >> 16) - 1 - y;
    auto& a = spawn_anchor[offset / object_stride];
    a.x = s16((s32(rlong(*this, node + 0x60)) >> 16) + x);
    a.y = s16((s32(rlong(*this, node + 0x80)) >> 16) + y);
    a.valid = true;
    a.raster_muzzle=!sprite_coverage.empty() && track_player_muzzle(node);
}

void Machine::static_data_replaced() {
    static std::atomic<u64> next{0};
    static_revision = ++next;
    retail_shadow_pixels.reset();
}

bool Machine::load_roms(const std::string& path, std::string& error) {
    Chips chips;
    if (!collect(path, chips, error) || !complete(chips)) return false;
    install(*this, chips);
    return true;
}

bool import_rom_set(const std::filesystem::path& selected, const std::filesystem::path& temporary,
                    std::string& error) {
    Chips chips;
    bool clone = false;
    if (!collect(selected, chips, error, &clone)) return false;
    auto family_zip = [](const std::filesystem::path& path) {
        auto name = path.filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        return name.starts_with(L"smashtv") && name.ends_with(L".zip");
    };
    if (!complete(chips) && (clone || family_zip(selected) ||
        std::any_of(chips.begin(), chips.end(), [](const auto& chip) { return !chip.empty(); }))) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(selected.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
            const auto& path = it->path();
            if (family_zip(path) && path != selected) {
                std::string sibling_error;
                if (collect(path, chips, sibling_error)) error = sibling_error;
                if (complete(chips)) break;
            }
        }
    }
    if (!complete(chips)) {
        error += "For a split set, keep the Smash T.V. parent (smashtv.zip) beside the selected ZIP.";
        return false;
    }
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    for (const auto& chip : chips) out.write(reinterpret_cast<const char*>(chip.data()), chip.size());
    out.close();
    if (!out) { error = "Cannot save imported game data."; return false; }
    error.clear();
    return true;
}

bool load_imported_roms(Machine& m, const std::filesystem::path& path, std::string& error) {
    Chips chips;
    std::ifstream in(path, std::ios::binary);
    for (size_t i = 0; i < chips.size(); ++i) {
        auto& data = chips[i];
        data.resize(expect[i].size);
        if (!in.read(reinterpret_cast<char*>(data.data()), data.size()) ||
            crc32({data.data(), data.size()}) != expect[i].crc) {
            error = "Saved game data is incomplete or damaged.";
            return false;
        }
    }
    if (in.peek() != std::char_traits<char>::eof()) { error = "Unexpected data in saved ROM import."; return false; }
    install(m, chips);
    return true;
}

// ---------------------------------------------------------------- reset
u32 Machine::reset_vector() const {
    const u32 w = (0xFFFFFFE0u - PROG_BASE) >> 4;
    return (prog[w] | (u32(prog[w + 1]) << 16)) & 0xFFFFFFF0u;
}

void Machine::reset() {
    if(!sprite_coverage.empty())enable_sprite_coverage();
    move_inputs.fill(0);
    aim_inputs.fill(0);
    shot_aim.fill(0);
    shot_muzzles.fill(0);
    c = Cpu{};
    c.st = 0x10;
    io.fill(0);
    dma.fill(0);
    std::fill(ram.begin(), ram.end(), 0);
    std::fill(vram.begin(), vram.end(), 0);
    cmos_page = 0;
    videobank_select = false;
    autoerase_enable = false;
    convsp = convdp = pixelshift = 0;
    vblank_start_ps = 0;
    vblank_next_ps = PS_PER_FRAME;
    scan_next_ps = 0;           // tms34010 device_start: scantimer->adjust(zero), vcount param 0
    scan_param = 0;
    dma_done_ps = ~0ull;
    vblank_end_ps = ~0ull;     // first armed at the first vblank begin
    frame_number = 0;
    frame_vis_extra = 0;
    stop = false;
    recompute_next_event();
}

// ---------------------------------------------------------------- screen timing (MAME screen.cpp)
// Signed arithmetic with C truncation, as MAME's attoseconds_t math: an event
// may already have moved vblank_start slightly past the CPU's local time.
int Machine::vpos_at(u64 ps) const {
    s64 delta = s64(ps) - s64(vblank_start_ps) + s64(PS_PER_PIXEL / 2);
    int v = int(delta / s64(PS_PER_LINE));
    return ((VIS_MAXY + 1 + v) % SCREEN_H + SCREEN_H) % SCREEN_H;
}
int Machine::hpos_at(u64 ps) const {
    s64 delta = s64(ps) - s64(vblank_start_ps) + s64(PS_PER_PIXEL / 2);
    s64 v = delta / s64(PS_PER_LINE);
    delta -= v * s64(PS_PER_LINE);
    return int(delta / s64(PS_PER_PIXEL));
}
u64 Machine::time_until_pos(u64 now, int vpos, int hpos) const {
    vpos += SCREEN_H - (VIS_MAXY + 1);
    vpos %= SCREEN_H;
    s64 target = s64(vpos) * s64(PS_PER_LINE) + s64(hpos) * s64(PS_PER_PIXEL);
    const s64 cur = s64(now) - s64(vblank_start_ps);
    if (target <= cur + s64(PS_PER_PIXEL / 2)) target += s64(PS_PER_FRAME);
    while (target <= cur) target += s64(PS_PER_FRAME);
    return u64(target - cur);
}

// ---------------------------------------------------------------- scheduling
// An event at time T is serviced at the first instruction boundary whose cycle
// count reaches floor(T / cycle): MAME runs the CPU for floor(delta/period)
// cycles toward the next timer, then fires it before the next slice starts.
void Machine::recompute_next_event() {
    if (external_timers) {
        next_event_cycle = check_requested ? 0 : run_until_cycle;
        return;
    }
    u64 t = std::min({scan_next_ps, dma_done_ps, vblank_next_ps, vblank_end_ps});
    next_event_cycle = std::min(t / PS_PER_CYCLE, run_until_cycle);
}

namespace {
// L8 NDSP1: BOTQ0/1 and TOPQ0/1, 420 five-longword records per queue.
constexpr u32 display_queue_base=0x01033160, display_record_bits=0xa0;
constexpr u32 display_records=4*420;
constexpr u16 has_shadow_underlay=0x8000;

u16 display_pen(u16 pixel) {
    return u16(((pixel & 0xc000) >> 8) | (pixel & 0x0f3f));
}


}
void Machine::enable_sprite_coverage() {
    if(!retail_shadow_pixels && !gfx.empty())
        retail_shadow_pixels=std::make_shared<const std::vector<u8>>(sprite_shadows::build(prog,gfx));
    sprite_coverage.assign(vram.size(),0);
    muzzle_markers.clear();visible_muzzles.clear();display_queue_torso.assign(display_records,{});
    display_queue_coverage.assign(display_records,0);
    presentation_frame=frame;
    shadow_underlay.assign(vram.size(),0);
    presentation_without_blobs=frame;
}

void Machine::set_wide(u32 line_width, u32 extra_columns) {
    vram_w = line_width;
    vis_extra = extra_columns;
    frame_vis_extra = 0;
    vram.assign(usize(512) * vram_w, 0);
    frame.assign(usize(vram_w) * SCREEN_H, 0);
    if(!sprite_coverage.empty())enable_sprite_coverage();
}

// Fire the timer expiring at `t` (the earliest pending one).
void Machine::fire_timer(u64 t) {
    if (on_timer_fired) on_timer_fired(t);
    // equal-time timers fire in arming order: vblank (armed a frame earlier)
    // before the scanline timer, as in MAME's timer list
    if (t == vblank_next_ps) {
        vblank_start_ps = vblank_next_ps;
        vblank_next_ps += PS_PER_FRAME;
        vblank_end_ps = vblank_start_ps + PS_VBLANK_PERIOD;
        if (on_vblank) on_vblank();
    } else if (t == vblank_end_ps) {
        vblank_end_ps = ~0ull;
        ++frame_number;
    } else if (t == scan_next_ps) {
        const int v = scan_param;
        const u64 at = scan_next_ps;
        scanline_callback(v);
        int nv = v + 1;
        if (nv > int(io[VTOTAL])) nv = 0;
        scan_param = nv;
        scan_next_ps = at + time_until_pos(at, nv, 0);
    } else {
        dma_done_ps = ~0ull;
        dma[DMA_COMMAND] &= ~0x8000;
        io[INTPEND] |= INT_X1;     // set_input_line(0, ASSERT_LINE)
    }
}

// Machine scheduler (stv::game::CoScheduler): the slice ended at `ps`; MAME fires
// every timer due by then before the main CPU executes again. l8_run's entry
// (execute_run) then performs the slice-start interrupt check.
void Machine::fire_timers_through(u64 ps) {
    for (;;) {
        const u64 t = std::min({scan_next_ps, dma_done_ps, vblank_next_ps, vblank_end_ps});
        if (t > ps) break;
        fire_timer(t);
    }
    recompute_next_event();
}

u32 Machine::boundary(u32 next) {
    if (on_boundary) {
        auto hook = std::move(on_boundary);
        on_boundary = nullptr;
        hook();
    }
    if (external_timers) {
        const bool check = check_requested;
        check_requested = false;
        recompute_next_event();
        return check ? check_interrupt(next) : next;
    }
    check_requested = false;
    for (;;) {
        const u64 t = std::min({scan_next_ps, dma_done_ps, vblank_next_ps, vblank_end_ps});
        if (t / PS_PER_CYCLE > c.cyc) break;
        fire_timer(t);
    }
    recompute_next_event();
    return check_interrupt(next);
}

u32 Machine::check_interrupt(u32 next) {
    if (io[HSTCTLH] & 0x0100) {
        io[HSTCTLH] &= ~0x0100;
        if (!(io[HSTCTLH] & 0x0200)) { push(*this, c, next); push(*this, c, c.st); }
        c.st = 0x10;
        c.cyc += 16;
        return rlong(*this, 0xFFFFFEE0u) & 0xFFFFFFF0u;
    }
    const u16 irq = io[INTPEND] & io[INTENB];
    if (!(c.st & ST_IE) || !irq) return next;
    u32 vector = 0;
    if (irq & INT_HI) vector = 0xFFFFFEC0u;
    else if (irq & INT_DI) vector = 0xFFFFFEA0u;
    else if (irq & INT_WV) vector = 0xFFFFFE80u;
    else if (irq & INT_X1) vector = 0xFFFFFFC0u;
    else if (irq & INT_X2) vector = 0xFFFFFFA0u;
    if (!vector) return next;
    push(*this, c, next);
    push(*this, c, c.st);
    c.st = 0x10;                 // RESET_ST (its own check_interrupt sees IE clear)
    c.cyc += 16;
    return rlong(*this, vector) & 0xFFFFFFF0u;
}

// ---------------------------------------------------------------- TMS34010 scanline callback
void Machine::scanline_callback(int vcount) {
    const bool enabled = io[DPYCTL] & 0x8000;
    const bool master = io[DPYCTL] & 0x2000;
    const int vsblnk = io[VSBLNK], veblnk = io[VEBLNK];
    int vtotal = io[VTOTAL];
    if (!master) {
        vtotal = std::min(SCREEN_H - 1, vtotal);
        vcount = vpos_at(scan_next_ps);
    }
    io[VCOUNT_R] = u16(vcount);
    if (enabled && vcount == io[DPYINT]) io[INTPEND] |= INT_DI;
    if (vcount == vsblnk) io[DPYADR] = io[DPYSTRT];
    if (vcount == vtotal && master) {
        const int htotal = io[HTOTAL];
        if (htotal > 0 && vtotal > 0) {
            const int width = (htotal + 1) * 2, height = vtotal + 1;
            const int minx = io[HEBLNK] * 2, maxx = io[HSBLNK] * 2 - 1, miny = veblnk, maxy = vsblnk - 1;
            if (minx < maxx && maxx <= width && miny < maxy && maxy <= height &&
                (width != SCREEN_W || height != SCREEN_H || miny != VIS_MINY || maxy != VIS_MAXY))
                fault("screen reconfiguration requested by the game (not modelled)", u32(width << 16 | height));
        }
    }
    if (vcount >= VIS_MINY && vcount <= VIS_MAXY) render_line(vcount);
    if (vcount >= veblnk && vcount < vsblnk) {
        u16 dpyadr = io[DPYADR];
        if ((dpyadr & 3) == 0) dpyadr = u16(((dpyadr & 0xfffc) - (io[DPYCTL] & 0x03fc)) | (io[DPYSTRT] & 0x0003));
        else dpyadr = u16((dpyadr & 0xfffc) | ((dpyadr - 1) & 3));
        io[DPYADR] = dpyadr;
    }
}

// midyunit scanline_update: draw the line, then autoerase the previous row.
void Machine::render_line(int scanline) {
    if (scanline == VIS_MINY) frame_vis_extra = enhanced_scene(*this) ? vis_extra : 0;
    if(scanline==VIS_MINY){
        visible_muzzles.clear();
        std::erase_if(muzzle_markers,[&](const auto& marker){return marker.frame+8<frame_number;});
    }
    const bool enabled = io[DPYCTL] & 0x8000;
    u16* dest = &frame[usize(scanline) * vram_w];
    u16* shown=presentation_frame.empty()?nullptr:presentation_frame.data()+usize(scanline)*vram_w;
    u16* clean=shown?presentation_without_blobs.data()+usize(scanline)*vram_w:nullptr;
    if(shown){
        std::fill_n(shown,vram_w,0);
        std::fill_n(clean,vram_w,0);
    }
    if (!enabled) { std::fill(dest, dest + vram_w, 0); return; }
    u16 dpyadr = io[DPYADR];
    if (!(io[DPYCTL] & 0x0400)) dpyadr ^= 0xfffc;
    const int rowaddr = dpyadr >> 4;
    // the start column wraps within the 512-pixel Y-unit line; a wide line then continues past it
    int coladdr = ((((dpyadr & 0x007c) << 4) | (io[DPYTAP] & 0x3fff)) << 1) & 0x1ff;
    const u16* src = &vram[usize(rowaddr & 0x1ff) * vram_w];
    const int heblnk = io[HEBLNK] * 2, hsblnk = io[HSBLNK] * 2 + int(frame_vis_extra);
    const int wmask = (frame_vis_extra ? int(vram_w) : 512) - 1;
    if(!muzzle_markers.empty()){
        std::erase_if(muzzle_markers,[&](const MuzzleMarker& marker){
            if(marker.page_y!=(rowaddr&511))return false;
            auto flash=marker.flash;
            const int screen_x=heblnk+((marker.page_x-coladdr)&wmask);
            if(screen_x>=heblnk && screen_x<hsblnk){
                flash.x=screen_x-VIS_MINX;flash.y=scanline-VIS_MINY;
                visible_muzzles.push_back(flash);
            }
            return true;
        });
    }
    std::fill(dest, dest + vram_w, u16(0));
    for (int x = heblnk; x < hsblnk && x < int(vram_w); ++x) {
        const int column=coladdr++ & wmask;
        const u16 i = src[column];
        dest[x] = display_pen(i);
        if(shown){
            const auto cell=usize(rowaddr&511)*vram_w+column;
            shown[x]=dest[x] | (u16(sprite_coverage[cell])<<12);
            const u16 under=shadow_underlay[cell];
            clean[x]=(under&has_shadow_underlay)?u16(under&~has_shadow_underlay):shown[x];
        }
    }
    autoerase_line(rowaddr - 1);
    // MAME also schedules a final-line autoerase after the last visible line;
    // it is applied here immediately because nothing observes VRAM in between.
    if (scanline == VIS_MAXY) autoerase_line(rowaddr);
}

void Machine::autoerase_line(int scanline) {
    if (autoerase_enable && scanline >= 0 && scanline < 510) {
        if(!sprite_coverage.empty()){
            std::fill_n(sprite_coverage.data()+usize(scanline)*vram_w,vram_w,0);
            std::fill_n(shadow_underlay.data()+usize(scanline)*vram_w,vram_w,0);
        }
        const u16* src = &vram[usize(vram_w) * usize(510 + (scanline & 1))];
        u16* dest = &vram[usize(vram_w) * usize(scanline)];
        if (enhanced_scene(*this) && vram_w > 512) {
            // MYOINIT (retail FFE069C0/FFE069F0) fills 511 columns on
            // rows 510/511. Column 511 and the added backing tail have
            // no initialized erase color. Extend the last valid word
            // for live widescreen without changing CPU/DMA timing or
            // the original template rows; native scenes keep their copy.
            std::copy_n(src, 511, dest);
            std::fill(dest + 511, dest + vram_w, src[510]);
        } else {
            std::copy_n(src, vram_w, dest);
        }
    }
}

// ---------------------------------------------------------------- DMA (midyunit_v.cpp dma_w)
void Machine::dma_write(u32 idx, u16 v) {
    if (idx >= 16) return;
    dma[idx] = v;
    if (on_dma_write) on_dma_write(idx, v);
    if (idx != DMA_COMMAND) return;
    const u16 command = dma[DMA_COMMAND];
    io[INTPEND] &= ~INT_X1;           // set_input_line(0, CLEAR_LINE): a synchronized line change,
    if (on_sync) on_sync(now_ps());   // whose timer at the current time can abort the main timeslice
    if (!(command & 0x8000)) return;
    s32 rowbytes = s16(dma[DMA_ROWBYTES]);
    int xpos = s16(dma[DMA_XSTART]);
    int ypos = s16(dma[DMA_YSTART]);
    int width = dma[DMA_WIDTH];
    int height = dma[DMA_HEIGHT];
    const u16 pal = u16(dma[DMA_PALETTE] << 8);
    const u16 color = dma[DMA_COLOR] & 0xff;
    u32 gfxoffset = dma[DMA_OFFSETLO] | (u32(dma[DMA_OFFSETHI]) << 16);
    if (command & 0x10) {
        gfxoffset -= u32((width - 1) * 8);
        rowbytes = (rowbytes - width + 3) & ~3;
        xpos += width - 1;
    } else {
        rowbytes = (rowbytes + width + 3) & ~3;
    }
    // MAME: the clipping code adjusts a stale offset that is then overwritten,
    // so clipped blits start from the unclipped source position. Kept for lockstep.
    if (ypos < 0) { height -= -ypos; ypos = 0; }
    if (ypos + height > 512) height = 512 - ypos;
    // Native-width scenes retain the original DMA clip and completion time,
    // even though enhanced mode reserves wider backing rows for live play.
    const int line_width = enhanced_scene(*this) ? int(vram_w) : 512;
    if (!(command & 0x10)) {
        if (xpos < 0) { width -= -xpos; xpos = 0; }
        if (xpos + width > line_width) width = line_width - xpos;
    } else {
        if (xpos >= line_width) { width -= xpos - (line_width - 1); xpos = line_width - 1; }
        if (xpos - width < 0) width = xpos;
    }
    if (gfxoffset < 0x02000000u) gfxoffset += 0x02000000u;
    dma_draw(command, width, height, xpos, ypos, rowbytes, gfxoffset - 0x02000000u, pal, color);
    dma_done_ps = now_ps() + u64(41000) * u64(std::max(width, 0)) * u64(std::max(height, 0));
    recompute_next_event();
    if (on_timer_armed) on_timer_armed(dma_done_ps);
}

void Machine::dma_draw(u16 command, int width, int height, int xpos, int ypos, s32 rowbytes, u32 offset,
                       u16 pal, u16 colorlo) {
    if (enhanced && (offset >> 3) >= aim_graphics_base && (command & 0x3f) == 2) {
        draw_aimed_sprite(offset >> 3, width, height, xpos, ypos, rowbytes, pal);
        return;
    }
    // DMAINT loads B14 past the five-longword record before launching it.
    const u32 record=c.b[14]-display_record_bits-display_queue_base;
    const u8 foreground=!display_queue_coverage.empty() && record<display_records*display_record_bits &&
                        record%display_record_bits==0 ? display_queue_coverage[record/display_record_bits] : 0;
    const int dx = (command & 0x10) ? -1 : 1;
    const u16 color = pal | colorlo;
    u32 off = offset >> 3;
    command &= 0x0f;
    const int txmask=(enhanced_scene(*this) ? int(vram_w) : 512)-1;
    if(!muzzle_markers.empty() && record<display_records*display_record_bits && record%display_record_bits==0){
        const auto torso=display_queue_torso[record/display_record_bits];
        for(auto& marker:muzzle_markers){
            if(marker.torso!=torso.node || marker.cycle>torso.cycle || marker.flipped!=(dx<0) || marker.page_y>=0)continue;
            for(int row=0;row<height;++row){
                const u32 column=marker.texel-(off+u32(row*rowbytes));
                if(column>=u32(width))continue;
                marker.page_x=(xpos+int(column)*dx)&txmask;
                marker.page_y=(ypos+row)&511;
                break;
            }
        }
    }
    const auto* atlas=sprite_coverage.empty()?nullptr:retail_shadow_pixels.get();
    const auto shadow_pixel=[&](u32 at){
        return atlas && at<atlas->size() && (*atlas)[at]==sprite_shadows::Shadow;
    };
    for (int y = 0; y < height; ++y) {
        int tx = xpos;
        const int ty = (ypos + y) & 0x1ff;
        u32 o = off;
        off += u32(rowbytes);
        u16* dest = &vram[usize(ty) * vram_w];
        if (o >= 0x06000000u && command < 0x0c) continue;
        auto px = [&](u32 k) -> u8 { return k < gfx.size() ? gfx[k] : 0; };

        auto put = [&](u16 v, bool blob=false) {
            const auto x=tx & txmask;
            if(!sprite_coverage.empty()){
                const auto cell=usize(ty)*vram_w+x;
                auto& under=shadow_underlay[cell];
                if(!blob)under=0;
                else if(!under)under=has_shadow_underlay|display_pen(dest[x])|(u16(sprite_coverage[cell])<<12);
                sprite_coverage[cell]=foreground;
            }
            dest[x]=v;
        };  // MAME indexes dest[tx] unchecked; x is clipped above
        switch (command) {
        case 0x00: break;
        case 0x01: for (int x = 0; x < width; ++x, tx += dx) if (px(o++) == 0) put(pal); break;
        case 0x02:
            for(int x=0;x<width;++x,tx+=dx,++o){
                const u8 p=px(o);
                if(p)put(u16(pal|p),shadow_pixel(o));
            }
            break;
        case 0x03: for (int x = 0; x < width; ++x, tx += dx,++o) put(u16(pal | px(o)),shadow_pixel(o)); break;
        case 0x04: case 0x05: for (int x = 0; x < width; ++x, tx += dx) if (px(o++) == 0) put(color); break;
        case 0x06: case 0x07: for (int x = 0; x < width; ++x, tx += dx,++o) { u8 p = px(o); put(p == 0 ? color : u16(pal | p),shadow_pixel(o)); } break;
        case 0x08: case 0x0a: for (int x = 0; x < width; ++x, tx += dx,++o) { u8 p=px(o); if(p)put(color,shadow_pixel(o)); } break;
        case 0x09: case 0x0b: for (int x = 0; x < width; ++x, tx += dx,++o) { u8 p = px(o); put(p != 0 ? color : u16(pal | p),shadow_pixel(o)); } break;
        default: for (int x = 0; x < width; ++x, tx += dx) put(color); break;
        }
    }
}

// ---------------------------------------------------------------- TMS34010 I/O registers
u16 Machine::io_read(u32 idx) {
    switch (idx) {
    case HCOUNT_R: {
        u16 v;
        if (replay && replay->hcount(v)) { if (on_hcount_read) on_hcount_read(v); return v; }
        int result = hpos_at(now_ps());
        const int total = io[HTOTAL] + 1;
        result = result * total / SCREEN_W;
        result += io[HEBLNK];
        if (result > total) result -= total;
        if (on_hcount_read) on_hcount_read(u16(result));
        return u16(result);
    }
    case REFCNT: return u16((c.cyc / 16) & 0xfffc);
    case INTPEND: {
        u16 r = io[INTPEND];
        // MAME: DI reported early when the display interrupt line is imminent
        if (io[VCOUNT_R] + 1 == io[DPYINT] && scan_next_ps - now_ps() < 600000) r |= INT_DI;
        return r;
    }
    default: return io[idx & 31];
    }
}

void Machine::io_write(u32 idx, u16 data) {
    idx &= 31;
    const u16 old = io[idx];
    io[idx] = data;
    switch (idx) {
    case PSIZE:
        switch (data) { case 2: pixelshift = 1; break; case 4: pixelshift = 2; break; case 8: pixelshift = 3; break;
                        case 0x10: pixelshift = 4; break; default: pixelshift = 0; }
        break;
    case HSTCTLH:
        if (data & 0x8000) fault("CPU self-halt via HSTCTLH (not modelled)", data);
        if (data & 0x0100) force_boundary();
        break;
    case HSTCTLL: {
        u16 nr = u16((old & 0xff8f) | (data & 0x0070));
        nr |= data & 0x0080;
        nr &= u16(data | ~0x0008);
        io[idx] = nr;
        break;
    }
    case CONVSP: convsp = 1u << (~data & 0x1f); break;
    case CONVDP: convdp = 1u << (~data & 0x1f); break;
    case INTENB: force_boundary(); break;                   // MAME calls check_interrupt()
    case INTPEND:
        io[INTPEND] = old;
        if (!(data & INT_WV)) io[INTPEND] &= ~INT_WV;
        if (!(data & INT_DI)) io[INTPEND] &= ~INT_DI;
        break;
    default: break;
    }
}

// ---------------------------------------------------------------- bus
u16 Machine::rw(u32 a) {
    const u32 w = a >> 4;
    if (a >= 0x01000000u && a < 0x01100000u) return ram[(a - 0x01000000u) >> 4];
    if (a >= PROG_BASE) return prog[(a - PROG_BASE) >> 4];
    if (enhanced && a >= ENHANCED_DATA_BASE && a < ENHANCED_DATA_END) {
        const usize index = (a - ENHANCED_DATA_BASE) >> 4;
        if (index < enhanced_data.size()) return enhanced_data[index];
        fault("read from unprepared native enhancement data", a);
        stop = true;
        return 0;
    }
    if (a < 0x00200000u) {
        const usize off = vidx(usize(w) * 2);
        return videobank_select ? u16((vram[off] & 0x00ff) | (vram[off + 1] << 8))
                                : u16((vram[off] >> 8) | (vram[off + 1] & 0xff00));
    }
    if (a >= 0x01400000u && a < 0x01410000u) return cmos[((a - 0x01400000u) >> 4) + cmos_page];
    if (a >= 0x01800000u && a < 0x01820000u) return palette[(a - 0x01800000u) >> 4];
    if ((a & ~0x00080000u) >= 0x01a00000u && (a & ~0x00080000u) < 0x01a000a0u) return dma[((a & ~0x00080000u) - 0x01a00000u) >> 4];
    if (a >= 0x01c00000u && a < 0x01c00060u) {
        const u32 k = (a - 0x01c00000u) >> 4;
        u16 v = ports[k];
        if (replay) replay->input(a & ~0xfu, v);
        if (on_input_read) on_input_read(a & ~0xfu, v);
        return v;
    }
    if (a >= 0x01c00060u && a < 0x01c00080u) return 0;      // protection_r: no protection data for smashtv
    if (a >= 0x02000000u && a < 0x06000000u) {
        const usize off = usize((a - 0x02000000u) >> 4) * 2;
        return u16(gfx[off] | (gfx[off + 1] << 8));
    }
    if (a >= 0xC0000000u && a < 0xC0000200u) return io_read((a - 0xC0000000u) >> 4);
    fault("read from unmapped address", a);
    return 0;
}

void Machine::ww(u32 a, u16 v) {
    if (a >= 0x01000000u && a < 0x01100000u) {
        auto& dst=ram[(a-0x01000000u)>>4];
        if(on_object && a-object_pool_base<object_slots*object_stride &&
           (a-object_pool_base)%object_stride==0x150 && dst!=v)
            on_object(ObjectEvent::Retag,a-0x150,v);
        const u32 q=a-display_queue_base;
        if(!display_queue_coverage.empty() && q<display_records*display_record_bits && q%display_record_bits==0){
            // DCLIPL queues A0's clipped image. Its return address identifies
            // OBJLST versus BAKLST even when a transparent image uses bank zero.
            const u32 node=c.a[0], sp=c.sp-0x01000000;
            u8 coverage=0;
            if(node-object_pool_base<object_slots*object_stride && (node-object_pool_base)%object_stride==0 && sp<=0xfffe0){
                const auto caller=u32(ram[sp>>4]) | (u32(ram[(sp>>4)+1])<<16);
                const auto field=[&](u32 off){return ram[(node+off-0x01000000)>>4];};
                const auto oid=field(0x150);
                // ADDBLOCK stores the tile's BAKBITS pointer in OPLINK.
                // GETOBJ clears OPLINK but leaves the previous occupant's OID.
                const u32 link=u32(field(0x160)) | (u32(field(0x170))<<16);
                const bool roomTile=link-0x010dadd0u<10000u;
                const bool lives=oid==6 && field(0xf0)==52 && field(0x100)==56;
                const bool front=!roomTile && ((caller==0xffe01530 && oid!=6 && ((oid>>8)&63)!=26) ||
                      ((caller==0xffe01370 || caller==0xffe01530) && (lives || oid==2 || oid==3 || (oid>>8)==7)));
                if(front){
                    const bool overlay=lives || oid==1 || oid==2 || oid==3 || (oid&0x1f00)==0x0700;
                    coverage=overlay ? overlay_ink : world_ink;
                }
            }
            display_queue_torso[q/display_record_bits]={coverage?node:0,c.cyc};
            display_queue_coverage[q/display_record_bits]=coverage;
        }
        dst=v;return;
    }
    if (a < 0x00200000u) {
        const usize off = vidx(usize(a >> 4) * 2);
        if(!sprite_coverage.empty()){
            sprite_coverage[off]=sprite_coverage[off+1]=0;
            shadow_underlay[off]=shadow_underlay[off+1]=0;
        }
        if (videobank_select) {
            vram[off] = u16((v & 0x00ff) | (dma[DMA_PALETTE] << 8));
            vram[off + 1] = u16((v >> 8) | (dma[DMA_PALETTE] & 0xff00));
        } else {
            vram[off] = u16((vram[off] & 0x00ff) | (v << 8));
            vram[off + 1] = u16((vram[off + 1] & 0x00ff) | (v & 0xff00));
        }
        return;
    }
    if (a >= 0x01400000u && a < 0x01410000u) { cmos[((a - 0x01400000u) >> 4) + cmos_page] = v; return; }
    if (a >= 0x01800000u && a < 0x01820000u) {
        const u32 k = (a - 0x01800000u) >> 4;
        palette[k] = v;
        pens[k & 0x0fff] = pen_rgb(v);
        return;
    }
    if ((a & ~0x00080000u) >= 0x01a00000u && (a & ~0x00080000u) < 0x01a000a0u) {
        dma_write(((a & ~0x00080000u) - 0x01a00000u) >> 4, v);
        return;
    }
    if (a >= 0x01c00060u && a < 0x01c00080u) return;          // cmos_enable_w; no protection
    if (a >= 0x01e00000u && a < 0x01e00020u) {
        if (on_sound_write) on_sound_write(a, v);
        return;
    }
    if (a >= 0x01f00000u && a < 0x01f00020u) {                  // control_w (low byte)
        if (on_sound_write) on_sound_write(a, v);                // recorded with sound-board writes
        cmos_page = ((v >> 6) & 3) * 0x1000;
        videobank_select = (v >> 5) & 1;
        autoerase_enable = !((v >> 4) & 1);
        return;
    }
    if (a >= 0xC0000000u && a < 0xC0000200u) { io_write((a - 0xC0000000u) >> 4, v); return; }
    if (a >= PROG_BASE) { fault("write to program ROM", a); return; }
    fault("write to unmapped address", a);
}

} // namespace stv::gsp
