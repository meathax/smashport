// Actual-event ownership and negative cases for optional net audio instrumentation.
#include "stv/host/net_audio_audit.hpp"
#include <cstdio>
#include <functional>
#include <limits>
#include <string>

using Audit = stv::host::ConfirmedAudioAudit;
static const float samples[] = {-0.25f, 0.0f, 0.25f};
static void frame(Audit& a, unsigned number, bool device = true) {
    const unsigned confirmed = number + 1;
    a.begin_frame(number, confirmed, 0);
    a.access(2, number, 1, 0x4000, 0x55, 0x8123, confirmed);
    a.generated(number, 2, samples, 3, confirmed);
    a.output(samples, 3, device, device, confirmed);
    a.end_frame(number, true, true);
}
int main(int argc, char** argv) {
    unsigned checks = 0, failed = 0;
    auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failed; std::fprintf(stderr, "FAIL: %s\n", name); }
    };
    auto rejects = [&](const char* name, const std::function<void(Audit&)>& operation) {
        Audit a; a.start(true, 48000, 1); operation(a);
        check(!a.good(), name);
    };
    Audit a; a.start(true, 48000, 1); frame(a, 0);
    check(a.good() && !a.active && a.frames_completed == 1 && a.next_frame == 1,
          "one complete confirmed frame");
    check(a.io_accesses == 1 && a.io_writes == 1 && a.pcm_generated == 3 &&
          a.pcm_queued == 3 && a.pcm_discarded == 0 && a.io_hash != Audit::kOffset,
          "actual events counted");
    check(a.pcm_hash == a.queued_pcm_hash && a.comparison_ready &&
          a.comparison.frames == 1, "queued PCM identity and exact prefix");
    const auto prefix = a.comparison;
    frame(a, 1);
    check(a.good() && a.comparison.frames == 1 && a.comparison.io_hash == prefix.io_hash &&
          a.comparison.pcm_hash == prefix.pcm_hash && a.io_hash != prefix.io_hash,
          "confirmation batch overshoot preserves requested comparison");
    a.access(1, 2, 0, 0x2000, 0, 0x8123, 2);
    check(!a.good() && a.outside_frame_accesses == 1,
          "outside access after comparison remains a failure");

    Audit absent; absent.start(true, 48000, 1); frame(absent, 0, false);
    check(absent.good() && absent.no_device_frames == 1 && absent.submission_calls == 0 &&
          absent.pcm_generated == 3 && absent.pcm_discarded == 3 && absent.pcm_queued == 0 &&
          absent.queued_pcm_hash == Audit::kOffset,
          "no endpoint is generation only, never queue submission");
    Audit disabled; disabled.start(false, 48000, 0);
    disabled.access(0, -1, -1, 0, 0, 0, 0);
    disabled.generated(-1, -1, samples, 3, 0);
    disabled.output(samples, 3, true, false, 0);
    disabled.begin_frame(5, 0, 9); disabled.end_frame(6, false, false);
    check(disabled.good() && disabled.io_accesses == 0 && disabled.pcm_generated == 0 &&
          disabled.frames_started == 0, "disabled audit has no observations");

    rejects("I/O outside any confirmed scope", [](Audit& x) { x.access(1, 0, 0, 0, 0, 0, 1); });
    rejects("mixer outside scope, including zero samples", [](Audit& x) {
        x.generated(0, 0, nullptr, 0, 1);
    });
    rejects("output outside scope", [](Audit& x) { x.output(nullptr, 0, false, false, 1); });
    rejects("exclusive boundary rejects current unconfirmed frame", [](Audit& x) {
        x.begin_frame(0, 0, 0); x.access(1, 0, 0, 0, 0, 0, 0);
    });
    rejects("confirmation regression at actual I/O", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.access(1, 0, 0, 0, 0, 0, 0);
    });
    rejects("confirmation regression at mixer", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.generated(0, 0, nullptr, 0, 0);
    });
    rejects("confirmation regression at output", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.output(nullptr, 0, false, false, 0);
    });
    rejects("invalid I/O kind", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.access(3, 0, 0, 0, 0, 0, 1);
    });
    rejects("I/O timestamp reverses", [](Audit& x) {
        frame(x, 0); x.begin_frame(1, 2, 0); x.access(1, 0, 0, 0, 0, 0, 2);
    });
    rejects("negative I/O time", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.access(1, -1, 0, 0, 0, 0, 1);
    });
    rejects("unnormalised I/O attoseconds", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.access(1, 0, 1000000000000000000ll, 0, 0, 0, 1);
    });
    rejects("mixer timestamp reverses", [](Audit& x) {
        frame(x, 0); x.begin_frame(1, 2, 0); x.generated(0, 0, nullptr, 0, 2);
    });
    rejects("negative mixer time", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.generated(0, -1, nullptr, 0, 1);
    });
    rejects("skipped playback frame", [](Audit& x) { frame(x, 1); });
    rejects("repeated playback frame", [](Audit& x) { frame(x, 0); frame(x, 0); });
    rejects("nested playback scope", [](Audit& x) {
        x.begin_frame(0, 2, 0); x.begin_frame(1, 2, 0);
    });
    rejects("wrong frame completion", [](Audit& x) {
        x.begin_frame(0, 2, 0); x.output(nullptr, 0, false, false, 2); x.end_frame(1, true, true);
    });
    rejects("no vblank reached", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.output(nullptr, 0, false, false, 1); x.end_frame(0, false, true);
    });
    rejects("sound or main fault", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.output(nullptr, 0, false, false, 1); x.end_frame(0, true, false);
    });
    rejects("leftover samples at frame start", [](Audit& x) { x.begin_frame(0, 1, 1); });
    rejects("missing output decision", [](Audit& x) { x.begin_frame(0, 1, 0); x.end_frame(0, true, true); });
    rejects("double output decision", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.output(nullptr, 0, false, false, 1);
        x.output(nullptr, 0, false, false, 1); x.end_frame(0, true, true);
    });
    rejects("unaccounted PCM", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.output(samples, 3, true, true, 1);
    });
    rejects("lost PCM at submission", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.generated(0, 0, samples, 3, 1); x.output(samples, 2, true, true, 1);
    });
    rejects("changed PCM at submission", [](Audit& x) {
        const float changed[] = {-0.25f, 0.5f, 0.25f};
        x.begin_frame(0, 1, 0); x.generated(0, 0, samples, 3, 1); x.output(changed, 3, true, true, 1);
    });
    rejects("claimed acceptance without actual push", [](Audit& x) {
        x.begin_frame(0, 1, 0); x.output(nullptr, 0, false, true, 1);
    });
    rejects("nonfinite PCM", [](Audit& x) {
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        x.begin_frame(0, 1, 0); x.generated(0, 0, &invalid, 1, 1);
    });

    Audit equal; equal.start(true, 48000, 1); equal.begin_frame(0, 1, 0);
    equal.access(1, 0, 1, 0x2000, 0, 0x8000, 1);
    equal.access(2, 0, 1, 0x4000, 0, 0x8000, 1);
    equal.generated(0, 1, samples, 3, 1); equal.generated(0, 1, nullptr, 0, 1);
    equal.output(samples, 3, true, true, 1); equal.end_frame(0, true, true);
    check(equal.good(), "equal timestamps preserve legitimate same-cycle accesses");

    std::uint64_t hash = Audit::kOffset;
    for (unsigned char c : std::string("hello")) Audit::hash_word(hash, c, 1);
    check(hash == 0xa430d84680aabd0bull, "canonical FNV-1a stream hash");
    if (argc == 2) {
        FILE* file = std::fopen(argv[1], "w");
        check(file != nullptr, "open JSON fixture");
        if (file) { equal.write_json(file); std::fputs("\n", file); std::fclose(file); }
    }
    std::printf("net audio ownership: %u checks, %u failures\n", checks, failed);
    return failed ? 1 : 0;
}
