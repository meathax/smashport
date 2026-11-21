#include "stv/host/audio_telemetry.hpp"
#include <cstdio>
#include <string>

using Trace = stv::host::AudioTelemetry;
static int checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (false)

int main() {
    Trace trace;
    Trace::Sample s;
    s.available = true;
    s.online = true;
    s.buffer_frames = 4800;
    trace.observe(100, s);
    CHECK(trace.size() == 0); // ordinary launches have no diagnostic allocation/history
    trace.start(true);
    trace.observe(1000000, s);
    for (uint64_t us = 1000001; us < 1010000; ++us) trace.observe(us, s);
    CHECK(trace.size() == 1); // uncapped presentation must not inflate history
    CHECK(trace.maximum_owner_gap_us() == 1);
    s.frame = 42; s.confirmed = s.played = 36; s.padding = 1200;
    trace.observe(1010000, s);
    CHECK(trace.size() == 2 && trace.sample(1).confirmed == 36);

    // A held protocol can empty output even while the host owner keeps running.
    s.resyncing = true;
    trace.observe(1010001, s);
    CHECK(trace.events().back().event_mask == Trace::Resync);
    for (uint64_t us = 1011000; us <= 1060000; us += 1000) trace.observe(us, s);
    s.underruns = 1; s.padding = 0;
    trace.observe(1060001, s);
    const auto& hold = trace.events().back();
    CHECK(hold.event_mask == Trace::Underrun && hold.resyncing && hold.played == 36);
    CHECK(hold.owner_gap_us <= 1000);

    // A distinct owner stall is retained with the same output observation.
    s.underruns = 2;
    trace.observe(1410001, s);
    CHECK((trace.events().back().event_mask & (Trace::OwnerStall | Trace::Underrun)) ==
          (Trace::OwnerStall | Trace::Underrun));
    CHECK(trace.events().back().owner_gap_us == 350000);
    CHECK(trace.maximum_owner_gap_us() == 350000);
    s.available = false; ++s.errors; s.padding = UINT32_MAX;
    trace.observe(1410002, s);
    CHECK((trace.events().back().event_mask & (Trace::DeviceError | Trace::Availability)) ==
          (Trace::DeviceError | Trace::Availability));
    s.available = true; ++s.recoveries;
    trace.observe(1410003, s);
    CHECK((trace.events().back().event_mask & Trace::Recovery) != 0);
    const auto count = trace.observed();
    trace.observe(5, s); // bad/backward observation cannot underflow a duration
    CHECK(trace.observed() == count && trace.maximum_owner_gap_us() == 350000);
    s.played = s.confirmed = 37;
    trace.observe(1410004, s, true); // reports retain the exact final owner boundary
    CHECK(trace.observed() == count + 1 && trace.sample(trace.size() - 1).played == 37);
    CHECK(trace.sample(trace.size() - 1).event_mask == 0);

    FILE* f = nullptr;
#ifdef _WIN32
    (void)tmpfile_s(&f);
#else
    f = std::tmpfile();
#endif
    CHECK(f != nullptr);
    if (f) {
        trace.write_json(f);
        std::rewind(f);
        char buffer[4096];
        std::string json;
        while (const auto n = std::fread(buffer, 1, sizeof(buffer), f)) json.append(buffer, n);
        std::fclose(f);
        CHECK(json.find("\"schema\":\"smash.host.audio-output-timeline.v1\"") != std::string::npos);
        CHECK(json.find("\"padding\":null") != std::string::npos);
        CHECK(json.find("\"maximum_owner_gap_us\":350000") != std::string::npos);
        CHECK(json.back() == '}');
    }

    trace.start(true);
    s = {};
    for (std::size_t i = 0; i < Trace::kHistoryLimit + 3; ++i) {
        s.played = i;
        trace.observe(uint64_t(i) * Trace::kIntervalUs, s);
    }
    CHECK(trace.size() == Trace::kHistoryLimit);
    CHECK(trace.observed() == Trace::kHistoryLimit + 3);
    CHECK(trace.sample(0).played == 3);
    CHECK(trace.sample(trace.size() - 1).played == Trace::kHistoryLimit + 2);
    for (std::size_t i = 0; i <= Trace::kEventLimit; ++i) {
        ++s.underruns;
        trace.observe((Trace::kHistoryLimit + 4 + i) * Trace::kIntervalUs, s);
    }
    CHECK(trace.events().size() == Trace::kEventLimit && trace.events_lost() == 1);
    CHECK(trace.events().front().underruns == 1); // early incident survives rolling history
    CHECK(trace.events().back().underruns == Trace::kEventLimit);
    trace.start(false);
    CHECK(trace.size() == 0 && trace.events().empty() && trace.observed() == 0);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
