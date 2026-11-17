// Device-output clock matching only. Original board/mixer PCM is immutable;
// correcting a sound device's drift must never alter game simulation speed.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>

namespace stv::host {
class AudioResampler {
public:
    static constexpr std::uint64_t kOne = std::uint64_t(1) << 32;
    static constexpr double kMaxCorrection = 0.005;
    void append(const float* samples, std::size_t count) {
        if (!count) return;
        pending_.insert(pending_.end(), samples, samples + count);
        input_frames_ += count;
    }
    void clear() noexcept {
        pending_.clear();
        phase_ = 0;
        step_ = kOne;
    }
    // An explicit output discontinuity: preserve only the newest source tail,
    // then restart interpolation at its exact first sample. Board PCM is never
    // changed. Used only when the bounded device sink abandons stale output.
    std::size_t retain_latest(std::size_t count) noexcept {
        const std::size_t dropped = pending_.size() > count ? pending_.size() - count : 0;
        for (std::size_t i = 0; i < dropped; ++i) pending_.pop_front();
        phase_ = 0;
        step_ = kOne;
        return dropped;
    }
    std::size_t queued() const noexcept { return pending_.size(); }
    void set_ratio(double input_per_output) noexcept {
        if (!std::isfinite(input_per_output)) input_per_output = 1.0;
        input_per_output = std::clamp(input_per_output, 1.0 - kMaxCorrection, 1.0 + kMaxCorrection);
        step_ = std::uint64_t(std::llround(input_per_output * double(kOne)));
    }
    double ratio() const noexcept { return double(step_) / double(kOne); }
    std::size_t available(std::size_t limit) const noexcept {
        if (pending_.size() < 2 || !limit) return 0;
        // One retained sample supplies continuity between separate mixer blocks.
        const auto end = std::uint64_t(pending_.size() - 1) * kOne;
        if (phase_ >= end) return 0;
        return std::size_t(std::min<std::uint64_t>(limit, 1 + (end - phase_ - 1) / step_));
    }
    bool take(float& value) noexcept {
        if (!available(1)) return false;
        value = pending_[0];
        // Unity-rate output preserves exact float bits, including signed zero.
        if (phase_) value += (pending_[1] - value) * float(double(phase_) / double(kOne));
        phase_ += step_;
        const auto consumed = phase_ >> 32;
        phase_ &= kOne - 1;
        for (std::uint64_t i = 0; i < consumed; ++i) pending_.pop_front();
        consumed_frames_ += consumed;
        ++output_frames_;
        return true;
    }
    std::uint64_t input_frames() const noexcept { return input_frames_; }
    std::uint64_t consumed_frames() const noexcept { return consumed_frames_; }
    std::uint64_t output_frames() const noexcept { return output_frames_; }
private:
    std::deque<float> pending_;
    std::uint64_t phase_{}, step_{kOne};
    std::uint64_t input_frames_{}, consumed_frames_{}, output_frames_{};
};

// Queue feedback changes only the device resampling ratio, bounded to 0.5%.
// Called once per source block, independent of the number of GUI redraws.
class AudioQueueServo {
public:
    void reset() noexcept { correction_ = 0.0; }
    double update(std::uint64_t queued, std::uint32_t target, std::uint32_t rate,
                  std::size_t source_frames) noexcept {
        if (!rate || !target || !source_frames) return 1.0 + correction_;
        const double error = (double(queued) - double(target)) / (2.0 * double(rate));
        const double desired = std::clamp(error, -AudioResampler::kMaxCorrection, AudioResampler::kMaxCorrection);
        const double gain = std::clamp(double(source_frames) / (0.25 * double(rate)), 0.0, 1.0);
        correction_ += (desired - correction_) * gain;
        return 1.0 + correction_;
    }
private:
    double correction_{};
};
} // namespace stv::host
