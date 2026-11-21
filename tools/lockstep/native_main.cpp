// Native L8 lockstep recorder (PLAN P3, development only).
// Runs the translated L8 program on the native Y-unit services and writes an
// STVLS001 stream in the same format as tools/lockstep/record.lua, so
// tools/lockstep/lsfile.py can report the first divergence against MAME.
// With a reference stream, HCOUNT and input-port reads are replayed from it
// (differential mode); without one, native HCOUNT and idle inputs are used.
//
//   stv_l8_lockstep <smashtv.zip> <out.bin> <frames> [reference.bin] [--native-hcount] [--native-inputs]
// --native-inputs drives the ports from record.lua's default schedule (coin1
// frames 2600..2604, start1 3150..3170, applied at vblank like frame_done).
#include "stv/gsp/ops.hpp"
#include "stv/base.hpp"
#include "bot.hpp"
#include "content_observer.hpp"
#include "scene_observer.hpp"
#ifdef STV_WITH_SOUND
#include "stv/game/cosched.hpp"
#include "stv/snd/board.hpp"
#endif
#include <cstdio>
#include <map>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace stv::gsp;

namespace {

struct Writer {
    std::ofstream f;
    std::vector<u8> content;   // lsfile.py content view: kind, frame, payload without PC, '|' per frame
    bool keep_content = false;
    std::vector<u8> prev_ram, prev_pal, prev_cmos;
    bool first = true;
    bool compact = std::getenv("STV_LS_COMPACT") != nullptr;
    // STV_LS_SUMMARY (as record.lua): kinds 4..7 fold into one type 9 record per frame,
    // FNV-1a over kind + the PC-free bytes, and the record count
    bool summary = std::getenv("STV_LS_SUMMARY") != nullptr;
    u32 sum_h = 2166136261u, sum_n = 0;
    void rec(u8 kind, u32 frame, const void* p, u16 n) {
        if (summary && kind >= 4 && kind <= 7) {
            const int keep = kind == 6 ? 2 : kind == 7 ? 6 : 8;
            u32 hh = (sum_h ^ kind) * 16777619u;
            for (int i = 0; i < keep && i < n; ++i) hh = (hh ^ static_cast<const u8*>(p)[i]) * 16777619u;
            sum_h = hh, ++sum_n;
            return;
        }
        if (summary && kind == 8) {
            const u32 s[2] = {sum_h, sum_n};
            sum_h = 2166136261u, sum_n = 0;
            summary = false;
            rec(9, frame, s, 8);
            summary = true;
        }
        u8 h[7] = {kind, u8(frame), u8(frame >> 8), u8(frame >> 16), u8(frame >> 24), u8(n), u8(n >> 8)};
        f.write(reinterpret_cast<const char*>(h), 7);
        if (n) f.write(static_cast<const char*>(p), n);
        if (keep_content) {
            if (kind == 8) { content.push_back('|'); return; }
            u16 keep = n;
            if (kind == 4 || kind == 5) keep = 8;
            else if (kind == 6) keep = 2;
            else if (kind == 7) keep = 6;
            content.insert(content.end(), h, h + 5);
            const u8* q = static_cast<const u8*>(p);
            content.insert(content.end(), q, q + keep);
        }
    }
    void region(u8 kind, u32 frame, const std::vector<u16>& words, std::size_t count, std::vector<u8>& prev) {
        std::vector<u8> bytes(count * 2);
        for (std::size_t i = 0; i < count; ++i) { bytes[2 * i] = u8(words[i]); bytes[2 * i + 1] = u8(words[i] >> 8); }
        if (prev.size() != bytes.size()) prev.assign(bytes.size(), 0), first = true;
        for (std::size_t ch = 0; ch < bytes.size() / 256; ++ch) {
            const u8* a = &bytes[ch * 256];
            u8* b = &prev[ch * 256];
            if (!first && std::memcmp(a, b, 256) == 0) continue;
            std::memcpy(b, a, 256);
            u8 buf[258] = {u8(ch), u8(ch >> 8)};
            if (compact) {
                // STV_LS_COMPACT: chunk hash instead of contents (as record.lua)
                u32 h = 2166136261u;
                for (int k = 0; k < 64; ++k) {
                    u32 wd;
                    std::memcpy(&wd, a + 4 * k, 4);
                    h = (h * 16777619u) ^ wd;
                }
                std::memcpy(buf + 2, &h, 4);
                rec(kind, frame, buf, 6);
                continue;
            }
            std::memcpy(buf + 2, a, 256);
            rec(kind, frame, buf, 258);
        }
    }
};

struct RefReplay : Replay {
    std::deque<u16> hc;
    std::deque<std::pair<u32, u16>> in;
    bool native_hcount = false;
    u64 input_mismatch = 0;
    bool hcount(u16& v) override {
        if (native_hcount || hc.empty()) return false;
        v = hc.front();
        hc.pop_front();
        return true;
    }
    bool native_inputs = false;
    bool input(u32 a, u16& v) override {
        if (native_inputs || in.empty()) return false;
        if (in.front().first != a) { ++input_mismatch; return false; }
        v = in.front().second;
        in.pop_front();
        return true;
    }
};

bool load_reference(const char* path, RefReplay& r) {
    std::ifstream f(path, std::ios::binary);
    char magic[8];
    if (!f.read(magic, 8) || std::memcmp(magic, "STVLS001", 8) != 0) return false;
    u8 h[7];
    std::vector<u8> p;
    while (f.read(reinterpret_cast<char*>(h), 7)) {
        const u16 n = u16(h[5] | (h[6] << 8));
        p.resize(n);
        if (n && !f.read(reinterpret_cast<char*>(p.data()), n)) break;
        if (h[0] == 6) r.hc.push_back(u16(p[0] | (p[1] << 8)));
        if (h[0] == 7) r.in.push_back({u32(p[0] | (p[1] << 8) | (p[2] << 16) | (u32(p[3]) << 24)), u16(p[4] | (p[5] << 8))});
    }
    return true;
}

// Minimal PNG writer (stored deflate blocks) for native framebuffer snapshots.
void write_png(const char* path, int w, int h, const std::vector<u32>& rgb) {
    auto crc = [](const std::vector<u8>& d, std::size_t from) {
        u32 c = 0xffffffffu;
        for (std::size_t i = from; i < d.size(); ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
        return ~c;
    };
    auto be = [](std::vector<u8>& d, u32 v) { d.push_back(u8(v >> 24)); d.push_back(u8(v >> 16)); d.push_back(u8(v >> 8)); d.push_back(u8(v)); };
    std::vector<u8> raw;
    for (int y = 0; y < h; ++y) { raw.push_back(0); for (int x = 0; x < w; ++x) { u32 p = rgb[std::size_t(y) * w + x]; raw.push_back(u8(p >> 16)); raw.push_back(u8(p >> 8)); raw.push_back(u8(p)); } }
    std::vector<u8> z{0x78, 0x01};
    for (std::size_t off = 0; off < raw.size(); off += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(u8(n)); z.push_back(u8(n >> 8)); z.push_back(u8(~n)); z.push_back(u8(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(off), raw.begin() + std::ptrdiff_t(off + n));
    }
    u32 a = 1, b = 0;
    for (u8 v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    be(z, (b << 16) | a);
    std::vector<u8> out{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    auto chunk = [&](const char* type, const std::vector<u8>& data) {
        be(out, u32(data.size()));
        const std::size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        be(out, crc(out, start));
    };
    std::vector<u8> ihdr;
    be(ihdr, u32(w)); be(ihdr, u32(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s smashtv.zip out.bin frames [reference.bin] [--native-hcount]\n", argv[0]);
        return 2;
    }
    static Machine m;
    std::string err;
    if (!m.load_roms(argv[1], err)) { std::fprintf(stderr, "ROM import failed: %s\n", err.c_str()); return 1; }
    const u32 frames = u32(std::strtoul(argv[3], nullptr, 10));
    RefReplay replay;
    bool have_ref = false;
    bool native_inputs = false;
    std::string expect;
    std::vector<u32> snaps;
    std::string coverage_path, content_path, scene_path;
    bool cmd_enabled = false;
    u32 cmd_start = 600, cmd_period = 150, cmd_from = 0, cmd_to = 255;   // --coverage FILE: one byte per translated instruction (executed = 1)
    struct InputEvent { u32 frame; int port; u16 mask; bool press; };
    std::vector<InputEvent> input_events;
    bool scripted_inputs = false;
    std::string snap_dir = ".";
    std::string bot_path, save_path, load_path, cmos_path;
    u32 save_frame = ~0u;
    bool save_pending = false;
    u32 bot_after = 0;
    int bot_seed = 1;
    bool two_players = false;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--snap") == 0 && i + 1 < argc) {
            for (char* t = std::strtok(argv[++i], ","); t; t = std::strtok(nullptr, ",")) snaps.push_back(u32(std::strtoul(t, nullptr, 10)));
            continue;
        }
        if (std::strcmp(argv[i], "--snap-dir") == 0 && i + 1 < argc) { snap_dir = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--content") == 0 && i + 1 < argc) { content_path = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--coverage") == 0 && i + 1 < argc) { coverage_path = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--scene-report") == 0 && i + 1 < argc) { scene_path = argv[++i]; continue; }
        // --enhanced: enhanced_l8_16_9 profile (patch points on, 1024-pixel lines, +137 columns)
        if (std::strcmp(argv[i], "--enhanced") == 0) {
            m.enhanced = true; m.set_wide(1024, 137);
            if (!apply_enhanced(m, 137)) {
                for (const auto& f : m.faults) std::fprintf(stderr, "%s at %08x\n", f.what.c_str(), f.addr);
                return 2; // reset() must never erase a rejected data-patch result
            }
            continue;
        }
        // --snd-commands START,PERIOD,FROM,TO: as tools/lockstep/snd_commands.lua
        if (std::strcmp(argv[i], "--snd-commands") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%u,%u,%u,%u", &cmd_start, &cmd_period, &cmd_from, &cmd_to);
            cmd_enabled = true;
            continue;
        }
        // --inputs FILE: "frame tag mask value" lines as consumed by record.lua (STV_LS_INPUTS),
        // applied at vblank begin like Lua frame_done; replaces the default coin/start schedule
        if (std::strcmp(argv[i], "--inputs") == 0 && i + 1 < argc) {
            std::ifstream in(argv[++i]);
            std::string line;
            while (std::getline(in, line)) {
                unsigned long f = 0, mask = 0;
                int value = 0;
                char tag[16] = {};
                if (std::sscanf(line.c_str(), "%lu %15s %lx %d", &f, tag, &mask, &value) != 4) continue;
                static const char* names[6] = {":IN0", ":IN1", ":IN2", ":DSW", ":UNK0", ":UNK1"};
                for (int k = 0; k < 6; ++k)
                    if (std::strcmp(tag, names[k]) == 0) input_events.push_back({u32(f), k, u16(mask), value != 0});
            }
            std::stable_sort(input_events.begin(), input_events.end(),
                             [](const InputEvent& x, const InputEvent& y) { return x.frame < y.frame; });
            native_inputs = true;
            scripted_inputs = true;
            continue;
        }
        // --bot FILE: the scenario bot (tools/lockstep/bot.hpp) plays from --bot-after
        // (default 0; earlier frames come from --inputs) and every input change of the
        // session is written to FILE for MAME replay (record.lua STV_LS_INPUTS)
        if (std::strcmp(argv[i], "--bot") == 0 && i + 1 < argc) { bot_path = argv[++i]; native_inputs = true; continue; }
        if (std::strcmp(argv[i], "--bot2") == 0) { two_players = true; continue; }   // a second bot plays P2
        if (std::strcmp(argv[i], "--bot-after") == 0 && i + 1 < argc) { bot_after = u32(std::strtoul(argv[++i], nullptr, 10)); continue; }
        // --save-state F FILE: snapshot at the first instruction boundary after frame F's
        // input point; --load-state FILE: start from a snapshot (development speed-up)
        if (std::strcmp(argv[i], "--save-state") == 0 && i + 2 < argc) { save_frame = u32(std::strtoul(argv[++i], nullptr, 10)); save_path = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--load-state") == 0 && i + 1 < argc) { load_path = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--cmos-out") == 0 && i + 1 < argc) { cmos_path = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--bot-seed") == 0 && i + 1 < argc) { bot_seed = std::atoi(argv[++i]); continue; }
        if (std::strcmp(argv[i], "--expect-digest") == 0 && i + 1 < argc) expect = argv[++i];
        else if (std::strcmp(argv[i], "--native-inputs") == 0) native_inputs = true;
        else if (std::strcmp(argv[i], "--native-hcount") == 0) replay.native_hcount = true;
        else if (!have_ref) have_ref = load_reference(argv[i], replay);
    }
    if (!save_path.empty() && save_frame > frames) {
        std::fprintf(stderr, "save-state frame exceeds capture endpoint\n"); return 2;
    }
    m.reset();
    std::unique_ptr<stv::scene_audit::Observer> scene;
    if (!scene_path.empty()) {
        scene = std::make_unique<stv::scene_audit::Observer>(scene_path);
        if (!scene->good()) { std::fprintf(stderr, "scene observer: cannot open output\n"); return 2; }
    }
    std::unique_ptr<stv::content::Observer> content;
    if (!content_path.empty()) {
        content = std::make_unique<stv::content::Observer>(content_path);
        if (!content->good()) { std::fprintf(stderr, "content observer: cannot open output\n"); return 2; }
    }
    std::unique_ptr<stv::bot::Bot> bot, bot2;
    FILE* bot_out = nullptr;
    if (!bot_path.empty()) {
        bot = std::make_unique<stv::bot::Bot>(bot_seed);
        if (two_players) bot2 = std::make_unique<stv::bot::Bot>(bot_seed, 2);   // same seed: both head for the same exit
        bot_out = std::fopen(bot_path.c_str(), "w");
        if (!bot_out) { std::fprintf(stderr, "cannot write %s\n", bot_path.c_str()); return 1; }
    }
    replay.native_inputs = native_inputs;
    if (have_ref) m.replay = &replay;
    Writer w;
    w.keep_content = !expect.empty();
    w.f.open(argv[2], std::ios::binary);
    if (!w.f) { std::fprintf(stderr, "cannot write main stream %s\n", argv[2]); return 2; }
    w.f.write("STVLS001", 8);
#ifdef STV_WITH_SOUND
    // Sound board co-simulation (PLAN P6): both translated programs run under the
    // MAME timeslice scheduler (stv::game::CoScheduler).
    // Output: STVSND01 stream like tools/lockstep/snd_record.lua.
    static stv::snd::Board snd;
    static std::unique_ptr<stv::game::CoScheduler> sched_ptr;   // created only with --snd (it owns the timer hooks)
    std::deque<std::pair<u64, u32>> vblank_marks;     // (ps, frame) awaiting a Lua-style summary
    std::ofstream sf;
    long snd_from = -1, snd_to = -2;
    std::string wav_path;                               // --wav out.wav: 48 kHz mono mix
    static stv::snd::Mixer mixer(48000);
    std::vector<float> pcm;
    unsigned long expect_snd_count = 0, expect_snd_hash = 0;
    bool expect_snd = false, snd_ok = true;
    for (int i = 4; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--snd") == 0) {
            sf.open(argv[i + 1], std::ios::binary);
            if (!sf) { std::fprintf(stderr, "cannot write sound stream %s\n", argv[i + 1]); return 2; }
        }
        if (std::strcmp(argv[i], "--wav") == 0) wav_path = argv[i + 1];
        // --expect-snd COUNT:HASH -- cumulative sound-access count and FNV-1a at the final
        // frame, as recorded by tools/lockstep/snd_record.lua in MAME 0.289
        if (std::strcmp(argv[i], "--expect-snd") == 0)
            expect_snd = std::sscanf(argv[i + 1], "%lu:%lx", &expect_snd_count, &expect_snd_hash) == 2;
        if (std::strcmp(argv[i], "--snd-window") == 0) std::sscanf(argv[i + 1], "%ld,%ld", &snd_from, &snd_to);
    }
    const bool with_snd = sf.is_open();
    if (with_snd && (!save_path.empty() || !load_path.empty())) {
        std::fprintf(stderr, "machine save/load-state is unsupported with --snd; sound scheduler state is not included\n");
        return 2;
    }
    if (with_snd) sched_ptr = std::make_unique<stv::game::CoScheduler>(m, snd);   // sets external timers
    std::deque<std::pair<u64, u32>> frame_marks;      // (ps, frame number from then on)
    u32 snd_frame = 0;
    u32 snd_count = 0, snd_hash = 2166136261u;
    auto snd_frame_at = [&](const stv::snd::Atto& t) {
        const u64 ps = u64(t.sec) * 1000000000000ull + u64(t.as / 1000000);
        while (!frame_marks.empty() && frame_marks.front().first <= ps) { snd_frame = frame_marks.front().second; frame_marks.pop_front(); }
        return snd_frame;
    };
    auto mix = [&](u32 v) { for (int k = 0; k < 4; ++k) snd_hash = (snd_hash ^ ((v >> (8 * k)) & 0xff)) * 16777619u; };
    auto main_atto = [](u64 cyc) { return stv::snd::Atto{int64_t(cyc / 5000000), int64_t(cyc % 5000000) * 200000000000LL}; };
    if (with_snd) {
        sf.write("STVSND01", 8);
        std::string e2;
        if (!snd.load_roms(m.snd_u4, m.snd_u19, m.snd_u20, e2)) { std::fprintf(stderr, "sound: %s\n", e2.c_str()); return 1; }
        if (!wav_path.empty()) snd.mixer = &mixer;
        snd.power_on();
        snd.on_access = [&](const stv::snd::Access& a) {
            const u32 f = snd_frame_at(a.t);
            ++snd_count;
            mix(a.kind); mix(u32(a.t.sec)); mix(u32(u64(a.t.as) >> 20)); mix(a.addr); mix(a.data);
            if (long(f) >= snd_from && long(f) <= snd_to) {
                u8 b[21];
                b[0] = a.kind;
                std::memcpy(b + 1, &f, 4);
                const u32 sec = u32(a.t.sec);
                const u64 as = u64(a.t.as);
                std::memcpy(b + 5, &sec, 4);
                std::memcpy(b + 9, &as, 8);
                std::memcpy(b + 17, &a.addr, 2);
                b[19] = a.data;
                sf.write(reinterpret_cast<const char*>(b), 20);
                const u16 pc = a.pc;
                sf.write(reinterpret_cast<const char*>(&pc), 2);
            }
        };
    }
#endif
    auto tap = [&](u8 kind, u32 a, u16 d) {
        u8 b[12];
        std::memcpy(b, &a, 4);
        b[4] = u8(d); b[5] = u8(d >> 8); b[6] = 0xff; b[7] = 0xff;
        std::memset(b + 8, 0, 4);      // PC is not modelled natively; lsfile compares it loosely
        w.rec(kind, u32(m.frame_number), b, 12);
    };
    m.on_dma_write = [&](u32 idx, u16 d) { tap(4, 0x01a80000u + idx * 16, d); };
    m.on_sound_write = [&](u32 a, u16 d) {
        tap(5, a, d);
#ifdef STV_WITH_SOUND
        if (with_snd && a >= 0x01e00000u && a < 0x01e00020u) {
            const u64 tw = m.c.cyc;
            const u32 f = u32(m.frame_number);
            u8 b[19];
            b[0] = 3;
            std::memcpy(b + 1, &f, 4);
            const stv::snd::Atto t = main_atto(tw);
            const u32 sec = u32(t.sec);
            const u64 as = u64(t.as);
            std::memcpy(b + 5, &sec, 4);
            std::memcpy(b + 9, &as, 8);
            std::memcpy(b + 17, &d, 2);
            sf.write(reinterpret_cast<const char*>(b), 19);
            sched_ptr->main_sound_write(d);
        }
#endif
    };
    m.on_input_read = [&](u32 a, u16 d) {
        u8 b[10];
        std::memcpy(b, &a, 4);
        b[4] = u8(d); b[5] = u8(d >> 8);
        std::memset(b + 6, 0, 4);
        w.rec(7, u32(m.frame_number), b, 10);
    };
    long ht_from = -1, ht_to = -1, ht_n = 0;
    if (const char* e = std::getenv("STV_HT_RANGE")) std::sscanf(e, "%ld,%ld", &ht_from, &ht_to);
    m.on_hcount_read = [&](u16 v) {
        if (ht_n >= ht_from && ht_n <= ht_to)
            std::printf("HT %ld %llu cyc=%llu t=%.9f %04x HTOTAL=%u HEBLNK=%u HSBLNK=%u VTOTAL=%u VEBLNK=%u VSBLNK=%u vbs=%llu\n",
                        ht_n, (unsigned long long)m.frame_number, (unsigned long long)m.c.cyc, double(m.c.cyc) * 200e-9, v,
                        m.io[HTOTAL], m.io[HEBLNK], m.io[HSBLNK], m.io[VTOTAL], m.io[VEBLNK], m.io[VSBLNK],
                        (unsigned long long)m.vblank_start_ps);
        ++ht_n;
        u8 b[6] = {u8(v), u8(v >> 8), 0, 0, 0, 0};
        w.rec(6, u32(m.frame_number), b, 6);
    };
    bool done = false;
    std::size_t content_end = 0;
    m.on_vblank = [&] {
        const u32 f = u32(m.frame_number);
#ifdef STV_WITH_SOUND
        if (with_snd) {
            frame_marks.emplace_back(m.vblank_end_ps, f + 1);
            vblank_marks.emplace_back(m.vblank_start_ps, f);    // summary once the scheduler's base reaches it
            if (cmd_enabled) {
                const u32 finish = cmd_start + (cmd_to - cmd_from + 1) * cmd_period;
                if (f + 60 >= cmd_start) m.ww(0x010865f0u, 0);     // SOUNDSUP = 0: the game makes no sounds
                if (f >= cmd_start && f < finish) {
                    const u32 k = (f - cmd_start) / cmd_period, ph = (f - cmd_start) % cmd_period;
                    const stv::snd::Atto t = stv::game::CoScheduler::from_ps(m.vblank_start_ps);
                    if (ph == 0) snd.main_sound_write(t, u16(0xFD00 | (cmd_from + k)));
                    if (ph == 1) snd.main_sound_write(t, u16(0xFF00 | (cmd_from + k)));
                }
                if (f >= finish) sched_ptr->request_stop();
            }
        }
#endif
        if (f == save_frame) {
            save_pending = true;
            m.run_until_cycle = m.c.cyc;   // l8_run returns at the next instruction boundary
            m.recompute_next_event();
        }
        static const char* const port_names[2] = {":IN0", ":IN1"};
        if (bot && f >= bot_after) {
            for (stv::bot::Bot* b : {bot.get(), bot2.get()}) {
                if (!b) continue;
                if (f == bot_after && bot_after) b->resync();
                for (const auto& e : b->step(f, m)) {
                    m.ports[e.port] = e.value ? u16(m.ports[e.port] & ~e.mask) : u16(m.ports[e.port] | e.mask);
                    std::fprintf(bot_out, "%u %s %x %d\n", f, port_names[e.port], e.mask, e.value);
                }
            }
        } else if (scripted_inputs) {
            static std::size_t next_input = 0;   // events are in file order, which is frame order
            while (next_input < input_events.size() && input_events[next_input].frame < f) ++next_input;
            for (; next_input < input_events.size() && input_events[next_input].frame == f; ++next_input) {
                const InputEvent& e = input_events[next_input];
                m.ports[e.port] = e.press ? u16(m.ports[e.port] & ~e.mask) : u16(m.ports[e.port] | e.mask);
                if (bot_out && e.port < 2) std::fprintf(bot_out, "%u %s %x %d\n", f, port_names[e.port], e.mask, e.press ? 1 : 0);
            }
        } else if (native_inputs && !bot) {
            if (f == 2600) m.ports[1] &= u16(~0x0001);
            if (f == 2604) m.ports[1] |= 0x0001;
            if (f == 3150) m.ports[1] &= u16(~0x0004);
            if (f == 3170) m.ports[1] |= 0x0004;
        }
        if (content && !content->sample(f, m.ram)) {
            std::fprintf(stderr, "content observer: RAM/output boundary at frame %u\n", f);
            std::exit(2);
        }
        if (scene && !scene->sample(f, m)) {
            std::fprintf(stderr, "scene observer: image/output boundary at frame %u\n", f);
            std::exit(2);
        }
        for (u32 sf : snaps) {
            if (sf != f) continue;
            const int vw = VIS_MAXX - VIS_MINX + 1 + int(m.vis_extra), vh = VIS_MAXY - VIS_MINY + 1;
            std::vector<u32> px(std::size_t(vw) * vh);
            for (int y = 0; y < vh; ++y)
                for (int x = 0; x < vw; ++x) px[std::size_t(y) * vw + x] = m.pens[m.frame[std::size_t(y + VIS_MINY) * m.vram_w + VIS_MINX + x] & 0x0fff];
            const std::string path = snap_dir + "/native_" + std::to_string(f) + ".png";
            write_png(path.c_str(), vw, vh, px);
            if (std::getenv("STV_LS_OBJDUMP")) {
                // object list (OBJLST, bit addresses): OID at +0x150, X +0x70, Y +0x90
                auto rl = [&](u32 a) { return u32(m.rw(a)) | (u32(m.rw(a + 16)) << 16); };
                std::printf("objects at frame %u (WAVEYX y=%d x=%d, WORLDTL y=%d x=%d, circuit %u, wave %u):", f,
                            int(int16_t(m.rw(0x010EE090u))), int(int16_t(m.rw(0x010EE080u))), int(int16_t(m.rw(0x01000290u))),
                            int(int16_t(m.rw(0x01000280u))), m.rw(0x010EB610u), m.rw(0x010EB630u));
                u32 a = rl(0x01000040u);
                for (int n = 0; a && n < 600; ++n, a = rl(a))
                    std::printf(" %04x@%d,%d", m.rw(a + 0x150), int(int16_t(m.rw(a + 0x70))), int(int16_t(m.rw(a + 0x90))));
                std::printf("\n");
            }
        }
        static const u32 dump_every = std::getenv("STV_LS_DUMP_EVERY") ? u32(std::atoi(std::getenv("STV_LS_DUMP_EVERY"))) : 1u;
        if (f % dump_every == 0) {
            w.region(1, f, m.ram, m.ram.size(), w.prev_ram);
            w.region(2, f, m.palette, m.palette.size(), w.prev_pal);
            std::vector<u16> page(m.cmos.begin() + m.cmos_page, m.cmos.begin() + m.cmos_page + 0x1000);
            w.region(3, f, page, page.size(), w.prev_cmos);
        }
        w.first = false;
        w.rec(8, f, nullptr, 0);
        if (f >= frames && !done) {
            done = true; content_end = w.content.size();
            // End an uncoupled execution budget at the next instruction boundary.
            // The coupled scheduler stops after its matching sound-frame record.
            if (!m.external_timers) {
                m.run_until_cycle = m.c.cyc;
                m.recompute_next_event();
            }
        }
    };
    // MAME's Lua frame_done at frame 0 happens before any vblank on some runs;
    // the recorder's first marker is emitted at the first vblank begin here.
    u32 pc = m.reset_vector();
    if (!load_path.empty()) {
        std::ifstream lf(load_path, std::ios::binary);
        std::vector<u8> st((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
        if (!m.load_state(st, pc)) { std::fprintf(stderr, "cannot load state %s\n", load_path.c_str()); return 1; }
        std::printf("state loaded: frame %llu pc %08x\n", (unsigned long long)m.frame_number, pc);
    }
    bool save_ok = save_path.empty();
    auto maybe_save = [&] {
        if (!save_pending) return;
        save_pending = false;
        std::vector<u8> st;
        m.save_state(st, pc);
        std::ofstream sf2(save_path, std::ios::binary);
        sf2.write(reinterpret_cast<const char*>(st.data()), std::streamsize(st.size()));
        sf2.close();
        save_ok = bool(sf2);
        if (!save_ok) { std::fprintf(stderr, "cannot write state %s\n", save_path.c_str()); return; }
        std::printf("state saved: frame %llu pc %08x (%zu bytes)\n", (unsigned long long)m.frame_number, pc, st.size());
    };
    const u64 budget = 1000000;
#ifdef STV_WITH_SOUND
    if (with_snd) {
        // Lua frame_done runs at vblank begin, after the timeslice ending there
        sched_ptr->on_base = [&](const stv::snd::Atto& base) {
            if (snd.mixer) {
                snd.ym_update(base);
                mixer.render_until(base, pcm);
            }
            while (!vblank_marks.empty() && !(base < stv::game::CoScheduler::from_ps(vblank_marks.front().first))) {
                const u32 f = vblank_marks.front().second;
                vblank_marks.pop_front();
                u8 b[13];
                b[0] = 9;
                std::memcpy(b + 1, &f, 4);
                std::memcpy(b + 5, &snd_count, 4);
                std::memcpy(b + 9, &snd_hash, 4);
                sf.write(reinterpret_cast<const char*>(b), 13);
                if (f >= frames) {
                    if (expect_snd) {
                        snd_ok = snd_count == expect_snd_count && snd_hash == expect_snd_hash;
                        std::printf("sound accesses %u hash %08x (%s)\n", snd_count, snd_hash,
                                    snd_ok ? "MATCHES the MAME 0.289 sound recording" : "DIFFERS");
                    }
                    sched_ptr->request_stop();
                }
            }
        };
        sched_ptr->start(pc);
        sched_ptr->run();
        if (!wav_path.empty()) {
            std::ofstream wf(wav_path, std::ios::binary);
            const u32 bytes = u32(pcm.size() * 2);
            auto le32 = [&](u32 v) { wf.write(reinterpret_cast<const char*>(&v), 4); };
            auto le16 = [&](u16 v) { wf.write(reinterpret_cast<const char*>(&v), 2); };
            wf.write("RIFF", 4); le32(36 + bytes); wf.write("WAVEfmt ", 8); le32(16); le16(1); le16(1);
            le32(48000); le32(96000); le16(2); le16(16); wf.write("data", 4); le32(bytes);
            for (float v : pcm) le16(u16(int16_t(std::lround(v * 32767.0f))));
            std::printf("wav: %zu samples (%.1f s) -> %s\n", pcm.size(), pcm.size() / 48000.0, wav_path.c_str());
        }
        pc = sched_ptr->main_pc();
        if (!snd.fault.empty()) std::printf("%s at cycle %llu\n", snd.fault.c_str(), (unsigned long long)snd.cpu.cyc);
        sf.close();
        if (!sf) { std::fprintf(stderr, "sound stream write failed\n"); return 2; }
    } else {
        while (!done && !m.stop) { pc = l8_run(m, pc, m.c.cyc + budget); maybe_save(); }
    }
#else
    while (!done && !m.stop) { pc = l8_run(m, pc, m.c.cyc + budget); maybe_save(); }
#endif
    if (bot) bot->finish_pressure(u32(m.frame_number));
    if (bot2) bot2->finish_pressure(u32(m.frame_number));
    if (!save_ok) { std::fprintf(stderr, "requested machine state was not saved\n"); return 2; }
    if (!cmos_path.empty()) {
        std::ofstream out(cmos_path, std::ios::binary);
        for (u16 value : m.cmos) {
            const char bytes[2] = {char(value & 255), char(value >> 8)};
            out.write(bytes, 2);
        }
        out.close();
        if (!out) { std::fprintf(stderr, "cannot write CMOS %s\n", cmos_path.c_str()); return 2; }
    }
    w.f.close();
    if (!w.f) { std::fprintf(stderr, "main stream write failed\n"); return 2; }
    if (!coverage_path.empty()) {
        std::ofstream cf(coverage_path, std::ios::binary);
        cf.write(reinterpret_cast<const char*>(l8_cov), std::streamsize(l8_cov_count));
    }
    std::printf("native: frames=%llu cycles=%llu pc=%08x faults=%zu hcount_left=%zu input_left=%zu input_mismatch=%llu big_field=%llu\n",
                (unsigned long long)m.frame_number, (unsigned long long)m.c.cyc, pc, m.faults.size(), replay.hc.size(),
                replay.in.size(), (unsigned long long)replay.input_mismatch, (unsigned long long)m.big_field_quirks);
    {
        // distinct faults (kind, address) with count and first cycle
        std::map<std::pair<std::string, u32>, std::pair<std::size_t, u64>> seen;
        for (const auto& f : m.faults) {
            auto& e = seen[{f.what, f.addr}];
            if (!e.first++) e.second = f.cycle;
        }
        for (const auto& [k, v] : seen)
            std::printf("  fault: %s at %08x x%zu first cycle %llu\n", k.first.c_str(), k.second, v.first,
                        (unsigned long long)v.second);
    }
#ifdef STV_WITH_SOUND
    if (with_snd && (!snd.fault.empty() || (expect_snd && !snd_ok))) return 5;
#endif
    if (!expect.empty()) {
        const stv::Sha256 d = stv::sha256({w.content.data(), content_end});
        char hex[65];
        for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", unsigned(d.bytes[i]));
        std::printf("content digest %s (%s)\n", hex, expect == hex ? "MATCHES the MAME-verified reference" : "DIFFERS");
        return expect == hex && !m.stop ? 0 : 4;
    }
    return m.stop ? 3 : 0;
}
