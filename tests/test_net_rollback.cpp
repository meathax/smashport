// PLAN P10: rollback co-op over a simulated link. Two peers (P1 and P2 local
// respectively) play the recorded two-player scenario through LoopbackLink with
// latency, jitter and loss; every confirmed frame's RAM hash on both peers must
// equal an offline reference run of the same inputs, and the exchanged hashes
// must never report a desync. A corrupted peer must be detected and resynced.
//
//   stv_net_tests <smashtv.zip> <two-player .inputs> <frames>
#include "stv/net/rollback.hpp"
#include "stv/net/udp.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace stv::net;

namespace {

bool enhanced_profile = false;

struct Inputs { std::vector<PlayerInput> p1, p2; };

Inputs load_inputs(const char* path, u32 frames) {
    Inputs r;
    if (!load_scripted_inputs(path, frames, r.p1, r.p2)) { std::fprintf(stderr, "cannot read %s\n", path); std::exit(2); }
    return r;
}

std::unique_ptr<stv::gsp::Machine> machine(const char* zip) {
    auto m = std::make_unique<stv::gsp::Machine>();
    std::string err;
    if (!m->load_roms(zip, err)) { std::fprintf(stderr, "ROM import failed: %s\n", err.c_str()); std::exit(2); }
    if (enhanced_profile) {
        m->enhanced = true;
        m->set_wide(1024, 137);
        if (!stv::gsp::apply_enhanced(*m, 137)) {
            std::fprintf(stderr, "Enhanced profile table does not match the L8 data\n");
            std::exit(2);
        }
    }
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s smashtv.zip twoplayer.inputs frames\n", argv[0]); return 2; }
    const u32 frames = u32(std::strtoul(argv[3], nullptr, 10));
    if (!frames) { std::fprintf(stderr, "frames must be positive\n"); return 2; }
    for (int i = 4; i < argc; ++i)
        if (std::string(argv[i]) == "--enhanced") enhanced_profile = true;
    const Inputs in = load_inputs(argv[2], frames);
    const bool reference_report = argc > 4 && std::string(argv[4]) == "--ref-report";

    // offline reference
    std::vector<u32> ref(frames);
    {
        auto m = machine(argv[1]);
        FrameRunner r(*m);
        r.start();
        for (u32 f = 0; f < frames; ++f) {
            r.step(in.p1[f], in.p2[f]);
            if (reference_report && m->stop) {
                std::fprintf(stderr, "Offline reference machine stopped before frame %u completed\n", f);
                return 2;
            }
            ref[f] = r.ram_hash();
        }
    }

    if (argc > 4 && std::string(argv[4]) == "--ref-hash") {   // offline RAM hash after the last frame
        std::printf("%08x\n", ref[frames - 1]);
        return 0;
    }
    if (reference_report) {
        std::printf("{\"profile\":\"%s\",\"frames\":%u,\"last_hash\":\"%08x\","
                    "\"periodic_checkpoints\":{\"schema\":\"smash.net.periodic-ram.v1\","
                    "\"enabled\":true,\"frames_requested\":%u,\"period\":%u,\"checkpoints\":[",
                    enhanced_profile ? "enhanced_l8_16_9" : "reference_l8", frames, ref[frames - 1],
                    frames, unsigned(RollbackSession::kHashEvery));
        for (u64 f = 0; f < frames; f += RollbackSession::kHashEvery)
            std::printf("%s[%u,\"%08x\"]", f ? "," : "", unsigned(f), ref[std::size_t(f)]);
        std::fputs("],\"resync_bases\":[]}}\n", stdout);
        return 0;
    }
    struct Case { const char* name; LoopbackLink::Conditions c; };
    const Case cases[] = {
        {"lan 0 ms", {0, 0, 0.0}},
        {"60 ms, 2% loss, jitter", {3, 1, 0.02}},
        {"150 ms, 5% loss, jitter", {8, 3, 0.05}},
        {"60 ms with a 6 s outage", {3, 1, 0.0, 4000, 4330}},
    };
    int fails = 0;
    for (const Case& cs : cases) {
        auto ma = machine(argv[1]), mb = machine(argv[1]);
        FrameRunner ra(*ma), rb(*mb);
        ra.start(), rb.start();
        LoopbackLink link(cs.c, 1234);
        RollbackSession a(ra, link.end(0), 1), b(rb, link.end(1), 2);
        u32 bad = 0, confirmed_a = 0, confirmed_b = 0;
        a.on_confirmed = [&](u32 f, u32 h) { ++confirmed_a; if (h != ref[f]) ++bad; };
        b.on_confirmed = [&](u32 f, u32 h) { ++confirmed_b; if (h != ref[f]) ++bad; };
        // peer A pauses for 200 ticks midway: both sides must hold, then carry on
        const u64 pause_at = frames / 2;
        u32 b_frame_at_pause_end = 0, b_frame_at_pause_start = 0;
        for (u64 tick = 0; tick < u64(frames) * 4 && (a.confirmed() < frames || b.confirmed() < frames); ++tick) {
            if (tick == pause_at) a.set_paused(true);
            if (tick == pause_at + 40) b_frame_at_pause_start = b.frame();
            if (tick == pause_at + 200) { a.set_paused(false); b_frame_at_pause_end = b.frame(); }
            if (a.frame() < frames) a.tick(in.p1[a.frame()]); else a.pump();
            if (b.frame() < frames) b.tick(in.p2[b.frame()]); else b.pump();
            link.tick();
        }
        const bool held = b_frame_at_pause_end == b_frame_at_pause_start;   // B held while A was paused
        const bool ok = bad == 0 && confirmed_a == frames && confirmed_b == frames && a.stats().desyncs == 0 &&
                        b.stats().desyncs == 0 && a.stats().hashes_checked > 0 && held;
        if (!held) std::printf("  peer B kept running during peer A's pause (%u -> %u)\n", b_frame_at_pause_start, b_frame_at_pause_end);
        std::printf("%s %s: confirmed %u/%u, mismatches %u, desyncs %llu/%llu, hash checks %llu, rollbacks %llu "
                    "(%llu frames resimulated), stalls %llu\n",
                    ok ? "PASS" : "FAIL", cs.name, confirmed_a, confirmed_b, bad,
                    (unsigned long long)a.stats().desyncs, (unsigned long long)b.stats().desyncs,
                    (unsigned long long)(a.stats().hashes_checked + b.stats().hashes_checked),
                    (unsigned long long)(a.stats().rollbacks + b.stats().rollbacks),
                    (unsigned long long)(a.stats().resimulated + b.stats().resimulated),
                    (unsigned long long)(a.stats().stalls + b.stats().stalls));
        fails += !ok;
    }
    // fault injection: peer B's RAM is corrupted for 40 frames; the desync must be
    // reported, the host's snapshot must resync B, and both must end on the offline
    // reference with no desync after the resync
    {
        auto ma = machine(argv[1]), mb = machine(argv[1]);
        FrameRunner ra(*ma), rb(*mb);
        ra.start(), rb.start();
        LoopbackLink link({3, 1, 0.02}, 99);
        RollbackSession a(ra, link.end(0), 1), b(rb, link.end(1), 2);
        const u32 inject = frames * 2 / 3;
        u32 reported = 0, first = 0, after_resync = 0, base = 0;
        bool injected = false, resynced = false;
        auto seen = [&](u32 f, u32, u32) { if (!reported++) first = f; if (resynced) ++after_resync; };
        a.on_desync = seen;
        b.on_desync = seen;
        b.on_resync = [&](u32 f, const std::vector<u8>&) { resynced = true; base = f; };
        // every (re)simulation of the injected frames corrupts B, until B starts resyncing
        b.after_step = [&](u32 f) {
            if (!resynced && !b.resyncing() && f >= inject && f < inject + 40) { mb->ram[0x7ff0] = u16(mb->ram[0x7ff0] | 0x8000); injected = true; }
        };
        for (u64 tick = 0; tick < u64(frames) * 4 && (a.confirmed() < frames || b.confirmed() < frames); ++tick) {
            if (a.frame() < frames) a.tick(in.p1[a.frame()]); else a.pump();
            if (b.frame() < frames) b.tick(in.p2[b.frame()]); else b.pump();
            link.tick();
        }
        const bool end_ok = ra.ram_hash() == ref[frames - 1] && rb.ram_hash() == ref[frames - 1];
        const bool ok = injected && reported > 0 && first >= inject && resynced && a.stats().resyncs == 1 &&
                        b.stats().resyncs == 1 && after_resync == 0 && end_ok;
        std::printf("%s fault injection (peer B corrupts RAM from frame %u): desync reported %u times, first at "
                    "frame %u; resynced from the host's frame %u (resyncs %llu/%llu, desyncs after it %u); end state %s "
                    "the offline run\n",
                    ok ? "PASS" : "FAIL", inject, reported, first, base, (unsigned long long)a.stats().resyncs,
                    (unsigned long long)b.stats().resyncs, after_resync, end_ok ? "equals" : "DIFFERS FROM");
        fails += !ok;
    }
#ifdef _WIN32
    // LAN discovery: a waiting host answers a broadcast probe and does not adopt the prober
    {
        UdpTransport host;
        std::string err;
        const bool bound = host.bind(27123, err);
        std::atomic<bool> stop{false};
        std::thread t([&] { while (!stop) { (void)host.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } });
        const std::string found = bound ? discover_lan_host(27123, 2000) : "";
        stop = true;
        t.join();
        const bool ok = bound && !found.empty() && !host.has_peer();
        std::printf("%s LAN discovery: host found at %s%s\n", ok ? "PASS" : "FAIL", found.empty() ? "(none)" : found.c_str(),
                    bound ? "" : (" (" + err + ")").c_str());
        fails += !ok;
    }
#endif
    std::printf(fails ? "FAILED\n" : "all rollback cases passed\n");
    return fails ? 1 : 0;
}
