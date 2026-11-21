// Real host helpers, real translated main/sound programs, and real PCM mixer.
// No window, endpoint, controller, network socket, or device setting is used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#define wWinMain unused_host_entry
#include "../src/host/win_main.cpp"
#undef wWinMain

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (false)
constexpr u32 kResyncFrame = 910, kFrames = 1300;
constexpr uint64_t kOffset = 14695981039346656037ull, kPrime = 1099511628211ull;

static void word_hash(uint64_t& hash, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) { hash = (hash ^ uint8_t(value)) * kPrime; value >>= 8; }
}
struct FrameDigest {
    uint64_t io_count, io_hash, pcm_count, pcm_hash;
    bool operator==(const FrameDigest& b) const {
        return io_count == b.io_count && io_hash == b.io_hash && pcm_count == b.pcm_count && pcm_hash == b.pcm_hash;
    }
};
struct Capture {
    uint64_t io_count = 0, io_hash = kOffset, pcm_count = 0, pcm_hash = kOffset;
    uint64_t commands = 0, controls = 0, pcm_changes = 0;
    float minimum = std::numeric_limits<float>::max(), maximum = -std::numeric_limits<float>::max(), previous = 0;
    bool finite = true;
    std::vector<FrameDigest> frames;
};

// Test the actual host transition helper with real resampler FIFO ownership.
// The device FIFO contains submitted-but-not-yet-heard samples, so a late reset
// really loses resumed audio. Network callback placement is reviewed separately.
struct PauseOutputFifo {
    stv::host::AudioResampler source;
    std::vector<float> device;
    unsigned discards = 0;
    void append(const std::vector<float>& pcm) { source.append(pcm.data(), pcm.size()); }
    void pump() { float sample; while (source.take(sample)) device.push_back(sample); }
    void discard() { ++discards; source.clear(); device.clear(); }
};

static void network_pause_fifo_test() {
    for (bool historical_order : {false, true}) {
        PauseOutputFifo fifo;
        bool was_paused = false, peer_paused = true;
        const auto observe = [&] {
            sync_audio_pause(peer_paused, was_paused, [&] { fifo.discard(); });
        };
        fifo.append({-.875f, -.75f, -.625f});
        fifo.pump();
        CHECK(fifo.source.queued() == 1 && fifo.device.size() == 2);
        observe(); // outer loop enters while the peer is paused
        CHECK(was_paused && fifo.discards == 0);
        std::vector<float> resumed;
        for (unsigned tick = 0; tick < 4; ++tick) {
            // The first pump receives resume. Its advance was still held;
            // subsequent owed ticks can confirm resumed source blocks.
            peer_paused = false;
            if (!historical_order) observe(); // post-pump, before confirmed PCM
            if (!tick) continue;
            const std::vector<float> block = {
                float(3 * tick - 2) / 32, float(3 * tick - 1) / 32, float(3 * tick) / 32
            };
            resumed.insert(resumed.end(), block.begin(), block.end());
            fifo.append(block);
            fifo.pump();
        }
        const bool has_submitted = fifo.device.size() >= resumed.size() - 1;
        CHECK(has_submitted);
        if (has_submitted)
            CHECK(std::equal(resumed.begin(), resumed.end() - 1,
                             fifo.device.end() - (resumed.size() - 1)));
        if (!historical_order) observe(); // refreshed post-batch state
        observe(); // next outer loop must not erase resumed PCM
        CHECK(!was_paused && fifo.discards == 1);
        if (historical_order) {
            // Negative control: the former outer-only boundary erases every
            // submitted resumed sample and its pending final lookahead.
            CHECK(fifo.device.empty() && fifo.source.queued() == 0);
        } else {
            CHECK(fifo.device.size() + 1 == resumed.size() && fifo.source.queued() == 1);
            CHECK(fifo.device == std::vector<float>(resumed.begin(), resumed.end() - 1));
            fifo.append({.5f}); // supply lookahead for the last real resumed sample
            fifo.pump();
            CHECK(fifo.device == resumed && fifo.source.queued() == 1);
            observe();
            CHECK(fifo.discards == 1 && fifo.device == resumed);
        }
    }

    // Peer resume cannot clear an overlapping local/menu pause. Only the last
    // cause releasing owns the single discard, before any resumed output.
    PauseOutputFifo fifo;
    bool was_paused = false, local_pause = true, menu = false, peer_pause = true;
    const auto observe = [&] {
        sync_audio_pause(local_pause || menu || peer_pause, was_paused, [&] { fifo.discard(); });
    };
    observe();
    peer_pause = false; observe();
    CHECK(was_paused && fifo.discards == 0);
    menu = true; local_pause = false; observe();
    CHECK(was_paused && fifo.discards == 0);
    menu = false; observe(); observe();
    CHECK(!was_paused && fifo.discards == 1);
    peer_pause = true; observe(); // a new peer pause arriving during a tick
    CHECK(was_paused && fifo.discards == 1);
    peer_pause = false; observe(); observe();
    CHECK(!was_paused && fifo.discards == 2);

    // Consume the resume once even when no endpoint can be reset, or its reset
    // fails. A later recovery must not cause another stale boundary discard.
    bool available = false;
    unsigned attempts = 0;
    was_paused = true;
    const auto reset_if_available = [&] { if (available) ++attempts; };
    sync_audio_pause(false, was_paused, reset_if_available);
    available = true;
    sync_audio_pause(false, was_paused, reset_if_available);
    CHECK(!was_paused && attempts == 0);
    was_paused = true;
    const auto failed_reset = [&] { ++attempts; return false; };
    sync_audio_pause(false, was_paused, failed_reset);
    sync_audio_pause(false, was_paused, failed_reset);
    CHECK(!was_paused && attempts == 1);
}

static bool reset_owner(const std::string& zip) {
    g_net = NetPlay{};
    g_sched.reset();
    g_snd.mixer = nullptr;
    g_mixer.reset();
    g_snd.~Board();
    new (&g_snd) stv::snd::Board;
    g_m = Machine{};
    g_pcm.clear();
    g_script1.clear(); g_script2.clear();
    g_started = false; g_frame_done = false; g_enhanced = false;
    std::string error;
    if (!g_m.load_roms(zip, error) || !g_snd.load_roms(g_m.snd_u4, g_m.snd_u19, g_m.snd_u20, error)) {
        std::printf("ROM setup failed: %s\n", error.c_str()); CHECK(false); return false;
    }
    g_m.reset();
    g_pc = g_m.reset_vector();
    g_mixer = std::make_unique<stv::snd::Mixer>(48000);
    g_snd.mixer = g_mixer.get();
    g_snd.power_on();
    g_sched = std::make_unique<stv::game::CoScheduler>(g_m, g_snd);
    g_sched->on_base = [](const stv::snd::Atto& base) {
        g_snd.ym_update(base);
        g_mixer->render_until(base, g_pcm);
    };
    g_m.on_vblank = [] { g_frame_done = true; g_sched->request_stop(); };
    g_net.trace_enabled = true;
    return true;
}

static uint64_t exposed_board_hash() {
    uint64_t h = kOffset;
    const auto& c = g_snd.cpu;
    for (uint64_t value : {uint64_t(c.a), uint64_t(c.b), uint64_t(c.dp), uint64_t(c.cc),
                          uint64_t(c.x), uint64_t(c.y), uint64_t(c.u), uint64_t(c.s), uint64_t(c.pc),
                          c.cyc, c.due, uint64_t(c.nmi_asserted), uint64_t(c.nmi_line),
                          uint64_t(c.firq_line), uint64_t(c.irq_line), uint64_t(c.lds),
                          uint64_t(g_snd.bank), uint64_t(g_snd.bank_base), uint64_t(g_snd.page_index),
                          uint64_t(g_snd.dac), uint64_t(g_snd.halted), uint64_t(g_snd.vector_pending),
                          g_snd.seq, g_snd.ym_samples, uint64_t(g_snd.ym_reset_state),
                          uint64_t(g_snd.cb_time.sec), uint64_t(g_snd.cb_time.as),
                          uint64_t(g_snd.ym_busy_end.sec), uint64_t(g_snd.ym_busy_end.as),
                          g_mixer->ym_generated()}) word_hash(h, value, 8);
    for (auto byte : g_snd.ram) word_hash(h, byte, 1);
    const auto& p = g_snd.pia;
    for (uint64_t value : {uint64_t(p.in_a), uint64_t(p.in_b), uint64_t(p.out_a), uint64_t(p.out_b),
                          uint64_t(p.ddr_a), uint64_t(p.ddr_b), uint64_t(p.ctl_a), uint64_t(p.ctl_b),
                          uint64_t(p.in_ca1), uint64_t(p.in_ca2), uint64_t(p.in_cb1), uint64_t(p.in_cb2),
                          uint64_t(p.irq_a1), uint64_t(p.irq_a2), uint64_t(p.irq_b1), uint64_t(p.irq_b2),
                          uint64_t(p.irq_a_state), uint64_t(p.irq_b_state), uint64_t(p.in_b_pushed)}) word_hash(h, value, 8);
    for (const auto& event : g_snd.q) {
        word_hash(h, uint64_t(event.t.sec), 8); word_hash(h, uint64_t(event.t.as), 8);
        word_hash(h, event.seq, 8); word_hash(h, event.kind, 1);
        word_hash(h, uint32_t(event.arg0), 4); word_hash(h, uint32_t(event.arg1), 4);
    }
    return h;
}

static void guarded_bridge_test(const std::string& zip) {
    if (!reset_owner(zip)) return;
    g_m.c.cyc = 100;
    const auto time = stv::game::CoScheduler::main_time(g_m.c.cyc);
    const auto initial_sequence = g_snd.seq;
    g_m.ww(0x01e00000, 0x01a5); // absent hook: the real board receives nothing
    CHECK(g_snd.seq == initial_sequence && g_snd.pia.in_b == 0);
    connect_confirmed_sound();
    const auto queue = g_snd.q.size(), sequence = g_snd.seq;
    g_m.ww(0x01f00000, 0x00e0); // video/CMOS control also reaches Machine's observer
    CHECK(g_snd.q.size() == queue && g_snd.seq == sequence && g_snd.pia.in_b == 0);
    CHECK(g_m.cmos_page == 0x3000 && g_m.videobank_select);
    g_m.ww(0x01e00000, 0x01a5);
    CHECK(g_snd.seq > sequence && g_snd.q.size() > queue);
    g_snd.fire_until(time);
    CHECK(g_snd.pia.in_b == 0xa5 && g_snd.pia.in_b_pushed && !g_snd.halted);
    g_m.ww(0x01e00010, 0x0000); // reset asserted at the mirrored word
    g_snd.fire_until(time);
    CHECK(g_snd.halted);
    g_m.ww(0x01e00010, 0x03bc); // release reset, new latch, command strobe
    g_snd.fire_until(time);
    CHECK(!g_snd.halted && g_snd.pia.in_b == 0xbc && g_snd.pia.in_cb1);
    const auto after = g_snd.seq;
    g_m.on_sound_write(0x01e00020, 0x0000); // exclusive upper boundary
    g_m.on_sound_write(0x01dffff0, 0x0000);
    CHECK(g_snd.seq == after);
}

enum class Run { Baseline, Notice, LegacyMainCopy, NoBridge };

static Capture play(const std::string& zip, const std::vector<stv::net::PlayerInput>& p1,
                    const std::vector<stv::net::PlayerInput>& p2, const std::vector<u8>& snapshot, Run run) {
    Capture result;
    if (!reset_owner(zip)) return result;
    if (run != Run::NoBridge) connect_confirmed_sound();
    const auto bridge = g_m.on_sound_write;
    g_m.on_sound_write = [&result, bridge](u32 address, u16 data) {
        if (address >= 0x01e00000 && address < 0x01e00020) ++result.commands;
        if (address >= 0x01f00000 && address < 0x01f00020) ++result.controls;
        if (bridge) bridge(address, data);
    };
    g_snd.on_access = [&result](const stv::snd::Access& access) {
        ++result.io_count;
        word_hash(result.io_hash, access.kind, 1);
        word_hash(result.io_hash, uint64_t(access.t.sec), 8);
        word_hash(result.io_hash, uint64_t(access.t.as), 8);
        word_hash(result.io_hash, access.addr, 2);
        word_hash(result.io_hash, access.data, 1);
        word_hash(result.io_hash, access.pc, 2);
    };
    const u32 end = run == Run::LegacyMainCopy ? kResyncFrame + 10 : kFrames;
    for (u32 frame = 0; frame < end; ++frame) {
        if (run == Run::Notice && frame == kResyncFrame - 1) {
            const auto before = net_machine_observation();
            net_confirmed_resync_notice(kResyncFrame);
            observe_confirmed_resync(); // not at B yet: defer the diagnostic
            CHECK(g_net.resync_base == kResyncFrame && g_net.resync_trace.empty());
            CHECK(before == net_machine_observation());
        }
        if (run == Run::LegacyMainCopy && frame == kResyncFrame) {
            const auto before = net_machine_observation();
            const auto base = g_sched->base(), target = g_sched->target();
            const auto board = exposed_board_hash(), old_cycles = g_m.c.cyc;
            u32 pc = 0;
            CHECK(g_m.load_state(snapshot, pc));
            g_sched->set_main_pc(pc); // intentionally reproduce the removed bug
            CHECK(!g_m.external_timers && before != net_machine_observation());
            CHECK(g_sched->base() == base && g_sched->target() == target && exposed_board_hash() == board);
            std::printf("legacy main-only copy: main cycles %llu -> %llu, coupled clock/board untouched\n",
                        (unsigned long long)old_cycles, (unsigned long long)g_m.c.cyc);
        }
        run_one_frame();
        if (!g_frame_done || g_m.stop || !g_snd.fault.empty()) {
            std::printf("run=%d stopped at frame %u: %s\n", int(run), frame, g_snd.fault.c_str());
            CHECK(false); break;
        }
        // Exactly the host's confirmed-input playback order; no prediction.
        stv::net::apply_inputs(g_m, p1[frame], p2[frame]);
        ++g_net.played;
        if (run == Run::Notice && g_net.played == kResyncFrame) {
            std::vector<u8> before_main, after_main;
            g_m.save_state(before_main, g_sched->main_pc(), false);
            const auto before = net_machine_observation();
            const auto board = exposed_board_hash();
            const auto pcm = g_pcm;
            CHECK(!pcm.empty()); // pending real PCM must survive the notice too
            observe_confirmed_resync();
            g_m.save_state(after_main, g_sched->main_pc(), false);
            CHECK(before_main == after_main && before == net_machine_observation());
            CHECK(board == exposed_board_hash() && pcm == g_pcm && g_m.external_timers);
            CHECK(g_net.resync_trace.size() == 1 && !g_net.resync_trace[0].loaded);
            CHECK(g_net.resync_trace[0].before == g_net.resync_trace[0].after);
            CHECK(g_net.resync_base == ~0u);
            observe_confirmed_resync();
            CHECK(g_net.resync_trace.size() == 1); // one observation, no replay
        }
        for (float sample : g_pcm) {
            uint32_t bits; std::memcpy(&bits, &sample, sizeof bits);
            word_hash(result.pcm_hash, bits, 4);
            result.finite = result.finite && std::isfinite(sample);
            result.minimum = std::min(result.minimum, sample); result.maximum = std::max(result.maximum, sample);
            if (result.pcm_count && sample != result.previous) ++result.pcm_changes;
            result.previous = sample; ++result.pcm_count;
        }
        g_pcm.clear();
        result.frames.push_back({result.io_count, result.io_hash, result.pcm_count, result.pcm_hash});
    }
    CHECK(g_net.played == end && result.frames.size() == end);
    if (run != Run::LegacyMainCopy) CHECK(g_m.external_timers);
    std::printf("run=%d frames=%zu commands=%llu controls=%llu io=%llu/%016llx pcm=%llu/%016llx range=%g..%g changes=%llu\n",
        int(run), result.frames.size(), (unsigned long long)result.commands, (unsigned long long)result.controls,
        (unsigned long long)result.io_count, (unsigned long long)result.io_hash,
        (unsigned long long)result.pcm_count, (unsigned long long)result.pcm_hash,
        result.minimum, result.maximum, (unsigned long long)result.pcm_changes);
    // Captured lambdas must not outlive their referenced local result.
    g_m.on_sound_write = {}; g_snd.on_access = {};
    return result;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string zip = argv[1];
    std::vector<stv::net::PlayerInput> p1, p2;
    if (!stv::net::load_scripted_inputs(argv[2], kFrames, p1, p2)) return 2;
    network_pause_fifo_test();
    guarded_bridge_test(zip);
    Machine display;
    std::string error;
    if (!display.load_roms(zip, error)) { std::printf("%s\n", error.c_str()); return 2; }
    stv::net::FrameRunner predictive(display);
    predictive.start();
    for (u32 f = 0; f < kResyncFrame; ++f) predictive.step(p1[f], p2[f]);
    std::vector<u8> snapshot;
    predictive.save(snapshot);
    CHECK(!display.external_timers && predictive.frame() == kResyncFrame);
    const auto baseline = play(zip, p1, p2, snapshot, Run::Baseline);
    const auto notice = play(zip, p1, p2, snapshot, Run::Notice);
    const auto legacy = play(zip, p1, p2, snapshot, Run::LegacyMainCopy);
    const auto absent = play(zip, p1, p2, snapshot, Run::NoBridge);
    CHECK(baseline.commands > 0 && baseline.controls > 0);
    CHECK(baseline.finite && baseline.pcm_count > 48000 && baseline.io_count > 1000);
    CHECK(baseline.maximum - baseline.minimum > 0.001f && baseline.pcm_changes > 100);
    CHECK(baseline.frames == notice.frames && baseline.commands == notice.commands);
    CHECK(legacy.frames.size() >= kResyncFrame && baseline.frames.size() >= kResyncFrame &&
          std::equal(legacy.frames.begin(), legacy.frames.begin() + kResyncFrame, baseline.frames.begin()));
    bool legacy_diverged = false;
    for (std::size_t f = kResyncFrame; f < legacy.frames.size() && f < baseline.frames.size(); ++f)
        legacy_diverged = legacy_diverged || !(legacy.frames[f] == baseline.frames[f]);
    CHECK(legacy_diverged);
    CHECK(absent.commands > 0 && (absent.io_count != baseline.io_count || absent.io_hash != baseline.io_hash));
    CHECK(absent.pcm_hash != baseline.pcm_hash); // idle/DC is not a delivered game soundtrack
    g_net = NetPlay{}; g_sched.reset(); g_snd.mixer = nullptr; g_mixer.reset();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
