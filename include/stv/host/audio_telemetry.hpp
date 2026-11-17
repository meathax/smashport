#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace stv::host {

// Diagnostic observations only: no output/simulation callbacks or clock control.
// Retain recent history plus the first bounded set of events, reporting all loss.
class AudioTelemetry {
public:
    static constexpr std::size_t kHistoryLimit = 8192, kEventLimit = 256;
    static constexpr uint64_t kIntervalUs = 10000, kOwnerStallUs = 25000;
    enum Event : uint32_t { Underrun = 1, DeviceError = 2, Recovery = 4, OutputReset = 8,
                           SourceDrop = 16, DeviceDrop = 32, Resync = 64, Pause = 128,
                           Availability = 256, OwnerStall = 512, Priming = 1024 };
    struct Sample {
        uint64_t time_us{}, owner_gap_us{}, frame{}, confirmed{}, played{};
        uint64_t output_samples{}, underruns{}, errors{}, recoveries{}, output_resets{};
        uint64_t source_drops{}, device_drops{}, network_resyncs{}, network_stalls{};
        uint64_t startup_primes{}, underrun_reprimes{}, priming_completions{}, priming_wait_ms{};
        uint32_t padding{UINT32_MAX}, pending{}, buffer_frames{}, event_mask{}, priming_target{};
        bool online{}, paused{}, resyncing{}, available{}, priming{};
    };
    void start(bool enabled) {
        *this = AudioTelemetry{};
        enabled_ = enabled;
        if (enabled) { history_.reserve(kHistoryLimit); events_.reserve(kEventLimit); }
    }
    bool enabled() const { return enabled_; }
    uint64_t observed() const { return observed_; }
    uint64_t events_lost() const { return events_lost_; }
    uint64_t maximum_owner_gap_us() const { return maximum_gap_; }
    const std::vector<Sample>& events() const { return events_; }
    std::size_t size() const { return history_.size(); }
    const Sample& sample(std::size_t i) const {
        return history_[(history_.size() == kHistoryLimit ? next_ + i : i) % history_.size()];
    }
    void observe(uint64_t now_us, Sample value, bool force = false) {
        if (!enabled_) return;
        if (!started_) { epoch_ = last_owner_ = now_us; started_ = true; }
        now_us = std::max(now_us, last_owner_);
        const uint64_t gap = now_us - last_owner_;
        last_owner_ = now_us;
        maximum_gap_ = std::max(maximum_gap_, gap);
        interval_gap_ = std::max(interval_gap_, gap);
        value.time_us = now_us - epoch_;
        value.owner_gap_us = interval_gap_;
        value.event_mask = gap >= kOwnerStallUs ? OwnerStall : 0;
        if (observed_) {
            if (value.underruns != previous_.underruns) value.event_mask |= Underrun;
            if (value.errors != previous_.errors) value.event_mask |= DeviceError;
            if (value.recoveries != previous_.recoveries) value.event_mask |= Recovery;
            if (value.output_resets != previous_.output_resets) value.event_mask |= OutputReset;
            if (value.source_drops != previous_.source_drops) value.event_mask |= SourceDrop;
            if (value.device_drops != previous_.device_drops) value.event_mask |= DeviceDrop;
            if (value.resyncing != previous_.resyncing ||
                value.network_resyncs != previous_.network_resyncs) value.event_mask |= Resync;
            if (value.paused != previous_.paused) value.event_mask |= Pause;
            if (value.available != previous_.available) value.event_mask |= Availability;
            if (value.priming != previous_.priming || value.startup_primes != previous_.startup_primes ||
                value.underrun_reprimes != previous_.underrun_reprimes ||
                value.priming_completions != previous_.priming_completions) value.event_mask |= Priming;
        }
        previous_ = value;
        if (observed_ && !force && !value.event_mask && value.time_us < next_sample_) return;
        ++observed_;
        if (history_.size() < kHistoryLimit) history_.push_back(value);
        else { history_[next_] = value; next_ = (next_ + 1) % kHistoryLimit; }
        if (value.event_mask) {
            if (events_.size() < kEventLimit) events_.push_back(value);
            else ++events_lost_;
        }
        interval_gap_ = 0;
        next_sample_ = value.time_us + kIntervalUs;
    }
    void write_json(FILE* f) const {
        std::fprintf(f, "{\"schema\":\"smash.host.audio-output-timeline.v1\",\"enabled\":%s,"
                     "\"interval_us\":%llu,\"history_limit\":%zu,\"event_limit\":%zu,"
                     "\"samples_observed\":%llu,\"history_discarded\":%llu,\"events_discarded\":%llu,"
                     "\"maximum_owner_gap_us\":%llu,\"samples\":[",
                     enabled_ ? "true" : "false", (unsigned long long)kIntervalUs, kHistoryLimit, kEventLimit,
                     (unsigned long long)observed_, (unsigned long long)(observed_ - history_.size()),
                     (unsigned long long)events_lost_, (unsigned long long)maximum_gap_);
        for (std::size_t i = 0; i < size(); ++i) { if (i) std::fputc(',', f); write_sample(f, sample(i)); }
        std::fputs("],\"events\":[", f);
        for (std::size_t i = 0; i < events_.size(); ++i) { if (i) std::fputc(',', f); write_sample(f, events_[i]); }
        std::fputs("]}", f);
    }
private:
    static void write_sample(FILE* f, const Sample& s) {
        std::fprintf(f, "{\"time_us\":%llu,\"owner_gap_us\":%llu,\"event_mask\":%u,"
                     "\"frame\":%llu,\"confirmed\":%llu,\"played\":%llu,\"padding\":",
                     (unsigned long long)s.time_us, (unsigned long long)s.owner_gap_us, s.event_mask,
                     (unsigned long long)s.frame, (unsigned long long)s.confirmed, (unsigned long long)s.played);
        if (s.padding == UINT32_MAX) std::fputs("null", f); else std::fprintf(f, "%u", s.padding);
        std::fprintf(f, ",\"pending\":%u,\"buffer_frames\":%u,\"output_samples\":%llu,"
                     "\"underruns\":%llu,\"errors\":%llu,\"recoveries\":%llu,\"output_resets\":%llu,"
                     "\"source_drops\":%llu,\"device_drops\":%llu,\"network_resyncs\":%llu,\"network_stalls\":%llu,"
                     "\"online\":%s,\"paused\":%s,\"resyncing\":%s,\"available\":%s,"
                     "\"startup_primes\":%llu,\"underrun_reprimes\":%llu,\"priming_completions\":%llu,"
                     "\"priming_wait_ms\":%llu,\"priming_target\":%u,\"priming\":%s}",
                     s.pending, s.buffer_frames, (unsigned long long)s.output_samples,
                     (unsigned long long)s.underruns, (unsigned long long)s.errors,
                     (unsigned long long)s.recoveries, (unsigned long long)s.output_resets,
                     (unsigned long long)s.source_drops, (unsigned long long)s.device_drops,
                     (unsigned long long)s.network_resyncs, (unsigned long long)s.network_stalls,
                     s.online ? "true" : "false", s.paused ? "true" : "false",
                     s.resyncing ? "true" : "false", s.available ? "true" : "false",
                     (unsigned long long)s.startup_primes, (unsigned long long)s.underrun_reprimes,
                     (unsigned long long)s.priming_completions, (unsigned long long)s.priming_wait_ms,
                     s.priming_target, s.priming ? "true" : "false");
    }
    bool enabled_{}, started_{};
    uint64_t epoch_{}, last_owner_{}, maximum_gap_{}, interval_gap_{}, next_sample_{};
    uint64_t observed_{}, events_lost_{};
    std::size_t next_{};
    Sample previous_{};
    std::vector<Sample> history_, events_;
};
} // namespace stv::host
