// Exact host pacing for the native arcade scheduler. Presentation never advances
// this clock. Deadlines are ceil(frame * QPC_frequency * 146234 / 8000000),
// so there is no floating-point accumulation, audio-queue speed adjustment,
// refresh-rate rounding, or discarded elapsed time after a stall.
#pragma once
#include "stv/reference_timing.hpp"
#include <cstdint>
#include <limits>

namespace stv::host {
class FrameClock {
public:
    static constexpr std::uint64_t kRateNumerator = L8ReferenceTiming::pixel_clock_hz;
    static constexpr std::uint64_t kRateDenominator = L8ReferenceTiming::pixels_per_frame;
    static constexpr double kSeconds = double(kRateDenominator) / double(kRateNumerator);

    bool configure(std::uint64_t frequency) noexcept {
        if (!frequency || frequency > std::numeric_limits<std::uint64_t>::max() / kRateDenominator)
            return false;
        frequency_ = frequency;
        const auto period = frequency * kRateDenominator;
        whole_ = period / kRateNumerator;
        remainder_ = period % kRateNumerator;
        return true;
    }
    void reset(std::uint64_t now) noexcept {
        deadline_ = now;
        fraction_ = kRateNumerator - 1; // ceil every absolute deadline; never run early
        completed_ = 0;
        next_deadline();
    }
    bool due(std::uint64_t now) const noexcept { return frequency_ && now >= deadline_; }
    void consume() noexcept { ++completed_; next_deadline(); }
    // Ordinary stalls keep every owed step. A host suspension (system sleep,
    // a modal window move, a debugger) beyond `limit` ticks is not replayed as
    // fast-forwarded play: the schedule restarts from `now`, as after a pause.
    bool drop_suspension(std::uint64_t now, std::uint64_t limit) noexcept {
        if (!frequency_ || now < deadline_ || now - deadline_ <= limit) return false;
        reset(now);
        return true;
    }
    std::uint64_t deadline() const noexcept { return deadline_; }
    std::uint64_t completed() const noexcept { return completed_; }
    std::uint64_t frequency() const noexcept { return frequency_; }
    double remaining_seconds(std::uint64_t now) const noexcept {
        return frequency_ && now < deadline_ ? double(deadline_ - now) / double(frequency_) : 0.0;
    }
private:
    void next_deadline() noexcept {
        deadline_ += whole_;
        fraction_ += remainder_;
        if (fraction_ >= kRateNumerator) { ++deadline_; fraction_ -= kRateNumerator; }
    }
    std::uint64_t frequency_{}, whole_{}, remainder_{}, fraction_{}, deadline_{}, completed_{};
};
} // namespace stv::host
