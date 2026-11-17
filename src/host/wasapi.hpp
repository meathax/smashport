// WASAPI shared-mode output for SmashTV.exe (PLAN D9, P8): mono float samples
// from the sound board mixer, converted to the device mix format.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "stv/host/audio_resampler.hpp"
#include "stv/host/frame_clock.hpp"

namespace stv::host {

// Output-volume intent: cheviman/tmnt-port ports/sdl2/main.c:647-674,
// commit 401c3694e5f89fa5844c384cfd82512803202cf9 (Sebastian Toyos, BSD-3-Clause).
// Complete upstream notice is retained in video_filter.cpp and third-party notices.
// Presentation gain only: emulated sound, raw PCM and queued sample counts are
// unchanged. Kept independent of a device so its boundary behavior is testable.
class AudioLevel {
public:
    void set_volume(int value) noexcept { volume_ = std::clamp(value, 0, 100); }
    int volume() const noexcept { return volume_; }
    void set_muted(bool value) noexcept { muted_ = value; }
    bool muted() const noexcept { return muted_; }
    float apply(float value) const noexcept {
        if (muted_ || volume_ == 0 || !std::isfinite(value)) return 0.0f;
        return value * (float(volume_) / 100.0f);
    }
private:
    int volume_{100};
    bool muted_{};
};

// The exact signed-16 conversion used by the device sink, exposed for a pure
// boundary test. Float device output keeps the existing unclipped finite range.
inline std::int16_t audio_pcm16(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    return std::int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

class AudioOut {
public:
    ~AudioOut();
    AudioOut() = default;
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    // Cold open selects the source rate once (48000 if no stream can open).
    // Thereafter service() reopens output only; the board/mixer is never reset.
    bool open();                          // default render device, shared mode
    static constexpr uint64_t kRecoveryRetryMs = 1000;
    // Call on the output owner with a monotonic clock, including while silent.
    // One due attempt at most; availability is the result, not a game-clock cue.
    bool service(uint64_t now_ms);
    void set_volume(int value) noexcept { level_.set_volume(value); }
    int volume() const noexcept { return level_.volume(); }
    void set_muted(bool value) noexcept { level_.set_muted(value); }
    bool muted() const noexcept { return level_.muted(); }
    uint32_t rate() const { return rate_; } // fixed source/WASAPI stream frames per second
    uint32_t endpoint_rate() const { return endpoint_rate_; } // last observed native mix rate
    uint64_t recovery_attempts() const { return recovery_attempts_; }
    uint64_t recoveries() const { return recoveries_; }
    // Observed downtime starts at service()'s first unavailable observation.
    uint64_t unavailable_ms(uint64_t now_ms) const;
    // Only a drained endpoint waits for real PCM. Positive-padding playback
    // keeps its normal latency. Counts include an immediately completed prime.
    uint64_t startup_primes() const { return startup_primes_; }
    uint64_t underrun_reprimes() const { return underrun_reprimes_; }
    uint64_t priming_completions() const { return priming_completions_; }
    bool priming() const { return priming_; }
    uint32_t priming_target_frames() const { return priming_target_frames_; }
    uint64_t priming_wait_ms(uint64_t now_ms) const;
    bool ok() const { return client_ != nullptr; }
    // Frames queued in the device plus waiting here.
    uint32_t queued();
    void push(const float* s, std::size_t n); // immutable source PCM, matched to the device clock only
    void push_stereo(const float* lr, std::size_t frames); // interleaved stereo (enhanced output)
    void pump();                          // move pending samples into the device buffer
    void clear() { finish_priming(false); pending_.clear(); pending_r_.clear(); servo_.reset(); newly_queued_ = 0; resync_pending_ = false; }
    // Explicit game restart/disconnect only, on the output owning thread.
    bool discard_pending();              // reset software and already queued device audio
    // device buffer found empty after playback started (an audible gap), and the
    // lowest device fill seen at a pump, in frames
    uint64_t underruns() const { return underruns_; }
    uint32_t min_padding() const { return min_pad_; }
    uint32_t last_device_padding() const { return last_pad_; } // UINT32_MAX after reset/loss
    uint32_t buffer_frames() const { return buffer_frames_; }
    const char* error_stage() const { return error_stage_; }
    int32_t error_code() const { return error_code_; }
    uint32_t errors() const { return errors_; }
    double output_ratio() const { return pending_.ratio(); }
    uint64_t output_samples() const { return pending_.output_frames(); }
    uint64_t output_resyncs() const { return output_resyncs_; }
    uint64_t dropped_source_samples() const { return dropped_source_samples_; }
    uint64_t dropped_device_frames() const { return dropped_device_frames_; }
    uint32_t pending_limit_samples() const { return pending_limit_; }
    uint32_t maximum_pending_samples() const { return maximum_pending_; }
    uint32_t pending_samples() const { return uint32_t(pending_.queued()); }

private:
    void close();
    bool open_stream(bool preserve_rate);
    void finish_priming(bool completed);
    bool fail(const char* stage, int32_t code);
    void* enumerator_{};
    void* device_{};
    void* client_{};
    void* render_{};
    uint32_t rate_{48000};
    uint32_t endpoint_rate_{};
    uint32_t channels_{2};
    bool is_float_{true};
    uint32_t bits_{32};
    uint32_t buffer_frames_{};
    AudioResampler pending_, pending_r_;   // left (counted) and right, in lockstep
    std::vector<float> stereo_left_, stereo_right_;
    void push_frames(const float* left, const float* right, std::size_t count);
    void set_ratio_both(double r) { pending_.set_ratio(r); pending_r_.set_ratio(r); }
    AudioQueueServo servo_;
    std::size_t newly_queued_{};
    uint32_t pending_target_{}, pending_limit_{}, maximum_pending_{};
    uint64_t output_resyncs_{}, dropped_source_samples_{}, dropped_device_frames_{};
    bool resync_pending_{};
    AudioLevel level_;
    bool started_{false}, underrun_latched_{false};
    uint64_t underruns_{};
    uint32_t min_pad_{UINT32_MAX}, last_pad_{UINT32_MAX};
    bool com_owned_{};
    const char* error_stage_{""};
    int32_t error_code_{};
    uint32_t errors_{};
    bool recovery_pending_{}, unavailable_observed_{};
    uint64_t recovery_attempts_{}, recoveries_{};
    uint64_t last_service_ms_{}, last_retry_ms_{}, unavailable_since_ms_{}, unavailable_total_ms_{};
    bool priming_{};
    uint32_t priming_target_frames_{};
    uint64_t startup_primes_{}, underrun_reprimes_{}, priming_completions_{};
    uint64_t priming_since_ms_{}, priming_total_ms_{};
};

}  // namespace stv::host
