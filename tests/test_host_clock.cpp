#include "stv/host/frame_clock.hpp"
#include "stv/host/audio_resampler.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using u64 = std::uint64_t;
using stv::host::FrameClock;
using stv::host::AudioResampler;
using stv::host::AudioQueueServo;
int checks = 0;
void require(bool good, const char* message) {
    ++checks;
    if (!good) throw std::runtime_error(message);
}
void exact_deadlines() {
    FrameClock clock;
    require(!clock.configure(0), "zero counter frequency accepted");
    require(!clock.configure(std::numeric_limits<u64>::max()), "overflowing counter frequency accepted");
    for (u64 frequency : {u64(1'000'000), u64(3'579'545), u64(10'000'000), u64(1'000'000'000)}) {
        require(clock.configure(frequency), "valid counter frequency rejected");
        const u64 epoch = 123'456'789;
        clock.reset(epoch);
        for (u64 frame = 1; frame <= 10'000; ++frame) {
            const u64 expected = epoch + (frame * frequency * FrameClock::kRateDenominator +
                                         FrameClock::kRateNumerator - 1) / FrameClock::kRateNumerator;
            require(clock.deadline() == expected, "deadline drifted from exact rational time");
            require(!clock.due(expected - 1) && clock.due(expected), "frame advanced before deadline");
            clock.consume();
        }
    }
    require(clock.configure(10'000'000), "counter setup");
    clock.reset(0);
    const u64 day_frames = 86'400 * FrameClock::kRateNumerator / FrameClock::kRateDenominator;
    for (u64 i = 0; i < day_frames; ++i) clock.consume();
    const auto next = ((day_frames + 1) * 10'000'000 * FrameClock::kRateDenominator +
                      FrameClock::kRateNumerator - 1) / FrameClock::kRateNumerator;
    require(clock.deadline() == next, "24-hour deadline accumulated drift");
    require(clock.completed() == day_frames, "completed frame count drifted");
}
struct Result { u64 frames{}, state{14695981039346656037ull}, redraws{}; };
Result render_schedule(u64 frequency, unsigned refresh, bool jitter) {
    FrameClock clock;
    require(clock.configure(frequency), "schedule setup");
    clock.reset(0);
    constexpr u64 seconds = 60;
    const u64 end = frequency * seconds;
    u64 now = 0, render = 0;
    Result result;
    while (now < end) {
        ++render;
        // 10kHz stands for an uncapped loop; arbitrary CPU/render stalls are
        // injected without discarding any simulation debt.
        now = std::min(end, (render * frequency + refresh - 1) / refresh);
        if (jitter && render % 41 == 0) now = std::min(end, now + frequency * 3 / 10);
        if (now < clock.deadline() && render > refresh * seconds) throw std::runtime_error("schedule stuck");
        do {
            int budget = 4;
            while (budget-- > 0 && clock.due(now)) {
                result.state ^= result.frames + 1;
                result.state *= 1099511628211ull;
                ++result.frames;
                clock.consume();
            }
            // The production loop pumps messages between batches. A batch
            // limit must not erase the rest of an overdue deadline interval.
        } while (clock.due(now));
        ++result.redraws;
        // A stall advances the wall timeline; don't generate backward samples.
        if (jitter) render = std::max(render, now * refresh / frequency);
    }
    return result;
}
void display_independence_and_pause() {
    const auto reference = render_schedule(10'000'000, 60, false);
    require(reference.frames == 60 * FrameClock::kRateNumerator / FrameClock::kRateDenominator,
            "wrong nominal game rate");
    for (unsigned refresh : {30u, 60u, 120u, 144u, 240u, 1000u, 10000u}) {
        for (bool jitter : {false, true}) {
            const auto actual = render_schedule(10'000'000, refresh, jitter);
            require(actual.frames == reference.frames && actual.state == reference.state,
                    "render cadence or a stall changed game updates");
        }
    }
    FrameClock clock;
    require(clock.configure(10'000'000), "pause setup");
    clock.reset(0);
    while (clock.due(100'000'000)) clock.consume();
    const u64 resumed_at = 72'100'000'000ull;
    clock.reset(resumed_at);
    require(clock.completed() == 0 && !clock.due(resumed_at), "pause became catch-up time");
    require(clock.due(resumed_at + 182'793), "resume first frame has wrong rate");
    const auto paused_deadline = clock.deadline();
    require(clock.remaining_seconds(resumed_at) > 0 &&
            clock.remaining_seconds(paused_deadline) == 0, "wait boundary is inconsistent");
    // A suspension within the limit keeps every owed step; a longer one
    // (system sleep, modal window move) restarts the schedule instead of
    // replaying it as fast-forwarded play.
    constexpr u64 limit = 10'000'000;   // one second at 10 MHz
    clock.reset(0);
    require(!clock.drop_suspension(clock.deadline() + limit, limit), "a one-second stall lost owed steps");
    require(clock.deadline() == 182'793, "kept stall moved the schedule");
    const u64 woke = clock.deadline() + limit + 1;
    require(clock.drop_suspension(woke, limit), "suspension was replayed as catch-up");
    require(!clock.due(woke) && clock.due(woke + 182'793) && clock.completed() == 0,
            "suspension did not restart from the wake time");
    require(!clock.drop_suspension(woke, limit), "suspension dropped twice");
}
std::vector<float> converted(const std::vector<float>& input, std::size_t chunk, double ratio) {
    AudioResampler sampler;
    sampler.set_ratio(ratio);
    std::vector<float> output;
    for (std::size_t start = 0; start < input.size(); start += chunk) {
        sampler.append(input.data() + start, std::min(chunk, input.size() - start));
        float value = 0;
        while (sampler.take(value)) output.push_back(value);
    }
    require(sampler.input_frames() == input.size() && sampler.output_frames() == output.size(),
            "output accounting lost a source block");
    return output;
}
void output_resampling() {
    AudioResampler sampler;
    float value = 42;
    require(!sampler.available(10) && !sampler.take(value) && value == 42, "empty output boundary");
    const float one = -0.0f;
    sampler.append(&one, 1);
    require(!sampler.available(10), "missing interpolation history accepted");
    sampler.set_ratio(9);
    require(std::abs(sampler.ratio() - 1.005) < 1e-9, "positive output correction is unbounded");
    sampler.set_ratio(-2);
    require(std::abs(sampler.ratio() - .995) < 1e-9, "negative output correction is unbounded");
    sampler.set_ratio(std::numeric_limits<double>::quiet_NaN());
    require(sampler.ratio() == 1, "invalid output correction");
    sampler.clear();
    require(!sampler.queued() && sampler.ratio() == 1, "output reset retained old phase");
    std::vector<float> raw(10'001);
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = float(std::sin(double(i) * .017));
    raw[0] = -0.0f;
    const auto copy = raw;
    const auto unity = converted(raw, 137, 1.0);
    require(unity.size() == raw.size() - 1, "unity output should retain exactly one history sample");
    require(std::memcmp(unity.data(), raw.data(), unity.size() * sizeof(float)) == 0,
            "unity output changed PCM bits");
    for (double ratio : {.995, .9975, 1.0, 1.0025, 1.005}) {
        const auto whole = converted(raw, raw.size(), ratio);
        for (std::size_t chunk : {std::size_t(1), std::size_t(7), std::size_t(875), std::size_t(2048)}) {
            const auto pieces = converted(raw, chunk, ratio);
            require(pieces.size() == whole.size() &&
                    std::memcmp(pieces.data(), whole.data(), pieces.size() * sizeof(float)) == 0,
                    "device result changed at a mixer-block boundary");
        }
        require(std::abs(double(whole.size()) - double(raw.size() - 1) / ratio) <= 1.0,
                "output duration does not match corrected device clock");
    }
    require(std::memcmp(raw.data(), copy.data(), raw.size() * sizeof(float)) == 0,
            "device resampling mutated original mixer PCM");
}
void long_clock_skew() {
    // Deterministic one-hour queue model with independently drifting output
    // clocks. Game updates always use the exact same rational interval; only
    // the source samples consumed per device sample are adjusted.
    constexpr u64 rate = 48000, target = 4800;
    const u64 steps = 3600 * FrameClock::kRateNumerator / FrameClock::kRateDenominator;
    for (int ppm : {-2000, -1000, -100, 0, 100, 1000, 2000}) {
        AudioQueueServo servo;
        double queued = double(target), low = queued, high = queued;
        u64 fraction = 0, generated = 0;
        for (u64 frame = 0; frame < steps; ++frame) {
            fraction += FrameClock::kRateDenominator * rate;
            const u64 block = fraction / FrameClock::kRateNumerator;
            fraction %= FrameClock::kRateNumerator;
            generated += block;
            queued += double(block);
            const double ratio = servo.update(u64(std::max(0.0, queued)), std::uint32_t(target),
                                              std::uint32_t(rate), std::size_t(block));
            queued -= rate * FrameClock::kSeconds * (1.0 + ppm * 1e-6) * ratio;
            low = std::min(low, queued);
            high = std::max(high, queued);
        }
        require(low > 3000 && high < 6000, "device clock skew empties or grows the output queue");
        const auto expected = steps * FrameClock::kRateDenominator * rate / FrameClock::kRateNumerator;
        require(generated == expected, "device clock changed generated game audio");
    }
}
} // namespace
int main() {
    try {
        exact_deadlines(); display_independence_and_pause(); output_resampling(); long_clock_skew();
        std::printf("PASS host_clock checks=%d; exact simulation, independent presentation schedules, output-only drift matching\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL host_clock after %d checks: %s\n", checks, error.what());
        return 1;
    }
}
