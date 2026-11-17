#pragma once
// Optional C6 instrumentation. This observes the real sound-board I/O hook,
// mixer callbacks and AudioOut queue submissions; it does not drive simulation
// or certify physical playback. Frame numbers are zero-based; confirmed is an
// exclusive boundary, so every owned event requires active_frame < confirmed.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace stv::host {

struct ConfirmedAudioAudit {
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    static constexpr u64 kOffset = 14695981039346656037ull;
    static constexpr u64 kPrime = 1099511628211ull;
    bool enabled{}, active{}, saw_io_time{}, saw_mixer_time{}, comparison_ready{};
    u32 sample_rate{}, comparison_limit{}, next_frame{}, active_frame{}, last_frame{}, max_access_frame{};
    u64 frames_started{}, frames_completed{}, io_accesses{}, io_reads{}, io_writes{};
    u64 mixer_callbacks{}, pcm_generated{}, pcm_queued{}, pcm_discarded{};
    u64 output_frames{}, submission_calls{}, device_frames{}, no_device_frames{};
    u64 outside_frame_accesses{}, unconfirmed_accesses{}, io_time_errors{}, invalid_accesses{};
    u64 outside_frame_mixer_calls{}, unconfirmed_mixer_calls{}, mixer_time_errors{};
    u64 outside_frame_outputs{}, unconfirmed_outputs{}, frame_sequence_errors{};
    u64 frame_scope_errors{}, frame_completion_errors{}, pcm_accounting_errors{}, nonfinite_samples{};
    u64 io_hash{kOffset}, pcm_hash{kOffset}, queued_pcm_hash{kOffset};
    std::int64_t io_sec{}, io_as{}, mixer_sec{}, mixer_as{};
    u64 frame_samples{}, frame_outputs{}, frame_pcm_hash{kOffset};
    struct Comparison {
        u32 frames{};
        u64 io_accesses{}, io_hash{kOffset}, pcm_generated{}, pcm_hash{kOffset};
    } comparison;

    void start(bool on, u32 rate, u32 limit) {
        *this = ConfirmedAudioAudit{};
        enabled = on;
        sample_rate = rate;
        comparison_limit = limit;
    }
    static void hash_word(u64& hash, u64 value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) {
            hash ^= (value >> (i * 8)) & 255u;
            hash *= kPrime;
        }
    }
    static bool valid_time(std::int64_t sec, std::int64_t as) {
        return sec >= 0 && as >= 0 && as < 1000000000000000000ll;
    }
    void context(u32 confirmed, u64& outside, u64& unconfirmed) {
        if (!active) ++outside;
        else if (active_frame >= confirmed) ++unconfirmed;
    }
    void begin_frame(u32 frame, u32 confirmed, std::size_t pending_samples) {
        if (!enabled) return;
        if (active) ++frame_scope_errors;
        if (frame != next_frame || frame >= confirmed) ++frame_sequence_errors;
        if (pending_samples) ++pcm_accounting_errors;
        active = true;
        active_frame = frame;
        frame_samples = frame_outputs = 0;
        frame_pcm_hash = kOffset;
        ++frames_started;
    }
    // Called for every actual Board::on_access event, even outside playback.
    void access(std::uint8_t kind, std::int64_t sec, std::int64_t as,
                std::uint16_t address, std::uint8_t data, std::uint16_t pc,
                u32 confirmed) {
        if (!enabled) return;
        context(confirmed, outside_frame_accesses, unconfirmed_accesses);
        if (kind == 1) ++io_reads;
        else if (kind == 2) ++io_writes;
        else ++invalid_accesses;
        if (!valid_time(sec, as) || (saw_io_time && (sec < io_sec || (sec == io_sec && as < io_as))))
            ++io_time_errors;
        saw_io_time = true; io_sec = sec; io_as = as;
        if (active && active_frame > max_access_frame) max_access_frame = active_frame;
        hash_word(io_hash, active ? active_frame : ~u32(0), 4);
        hash_word(io_hash, kind, 1);
        hash_word(io_hash, u64(sec), 8); hash_word(io_hash, u64(as), 8);
        hash_word(io_hash, address, 2); hash_word(io_hash, data, 1); hash_word(io_hash, pc, 2);
        ++io_accesses;
    }
    // Called after render_until, over precisely the newly appended PCM samples.
    void generated(std::int64_t sec, std::int64_t as, const float* samples,
                   std::size_t count, u32 confirmed) {
        if (!enabled) return;
        context(confirmed, outside_frame_mixer_calls, unconfirmed_mixer_calls);
        if (!valid_time(sec, as) || (saw_mixer_time &&
                (sec < mixer_sec || (sec == mixer_sec && as < mixer_as))))
            ++mixer_time_errors;
        saw_mixer_time = true; mixer_sec = sec; mixer_as = as;
        ++mixer_callbacks;
        pcm_generated += count;
        if (active) frame_samples += count;
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(samples[i])) ++nonfinite_samples;
            u32 bits = 0;
            static_assert(sizeof(bits) == sizeof(samples[i]));
            std::memcpy(&bits, samples + i, sizeof(bits));
            hash_word(pcm_hash, bits, 4);
            if (active) hash_word(frame_pcm_hash, bits, 4);
        }
    }
    // accepted describes the real AudioOut::push software queue, not hearing.
    void output(const float* samples, std::size_t count, bool attempted,
                bool accepted, u32 confirmed) {
        if (!enabled) return;
        context(confirmed, outside_frame_outputs, unconfirmed_outputs);
        ++output_frames;
        if (active) ++frame_outputs;
        if (count != frame_samples || (accepted && !attempted)) ++pcm_accounting_errors;
        u64 output_hash = kOffset;
        for (std::size_t i = 0; i < count; ++i) {
            u32 bits = 0; std::memcpy(&bits, samples + i, sizeof(bits));
            hash_word(output_hash, bits, 4);
        }
        if (output_hash != frame_pcm_hash) ++pcm_accounting_errors;
        if (attempted) ++submission_calls;
        if (accepted) {
            ++device_frames; pcm_queued += count;
            for (std::size_t i = 0; i < count; ++i) {
                u32 bits = 0; std::memcpy(&bits, samples + i, sizeof(bits));
                hash_word(queued_pcm_hash, bits, 4);
            }
        } else {
            ++no_device_frames; pcm_discarded += count;
        }
    }
    void end_frame(u32 frame, bool reached_vblank, bool machines_ok) {
        if (!enabled) return;
        if (!active || frame != active_frame) ++frame_scope_errors;
        if (!reached_vblank || !machines_ok) ++frame_completion_errors;
        if (frame_outputs != 1) ++pcm_accounting_errors;
        last_frame = frame;
        ++frames_completed; ++next_frame;
        active = false;
        // Confirmation may arrive in batches. Freeze an identical requested
        // prefix for cross-peer hashes, but keep checking every later event.
        if (comparison_limit && frames_completed == comparison_limit) {
            comparison = {u32(frames_completed), io_accesses, io_hash, pcm_generated, pcm_hash};
            comparison_ready = true;
        }
    }
    u64 violations() const {
        return outside_frame_accesses + unconfirmed_accesses + io_time_errors + invalid_accesses +
               outside_frame_mixer_calls + unconfirmed_mixer_calls + mixer_time_errors +
               outside_frame_outputs + unconfirmed_outputs + frame_sequence_errors +
               frame_scope_errors + frame_completion_errors + pcm_accounting_errors + nonfinite_samples;
    }
    bool good() const { return !enabled || violations() == 0; }
    void write_json(FILE* file) const {
        std::fprintf(file, "{\"schema\":\"smash.net.confirmed-audio.v1\",\"enabled\":%s,"
                     "\"frame_indexing\":\"zero_based_less_than_exclusive_confirmed\","
                     "\"hash_algorithm\":\"fnv1a64-le-v1\",\"active\":%s",
                     enabled ? "true" : "false", active ? "true" : "false");
        auto count = [file](const char* name, u64 value) {
            std::fprintf(file, ",\"%s\":%llu", name, (unsigned long long)value);
        };
        auto digest = [file](const char* name, u64 value) {
            std::fprintf(file, ",\"%s\":\"%016llx\"", name, (unsigned long long)value);
        };
        count("sample_rate", sample_rate); count("comparison_limit", comparison_limit);
        count("next_frame", next_frame); count("last_frame", last_frame);
        count("max_access_frame", max_access_frame);
        count("frames_started", frames_started); count("frames_completed", frames_completed);
        count("io_accesses", io_accesses); count("io_reads", io_reads); count("io_writes", io_writes);
        count("mixer_callbacks", mixer_callbacks); count("pcm_generated", pcm_generated);
        count("pcm_queued", pcm_queued); count("pcm_discarded", pcm_discarded);
        count("output_frames", output_frames); count("submission_calls", submission_calls);
        count("device_frames", device_frames); count("no_device_frames", no_device_frames);
        count("outside_frame_accesses", outside_frame_accesses);
        count("unconfirmed_accesses", unconfirmed_accesses); count("io_time_errors", io_time_errors);
        count("invalid_accesses", invalid_accesses);
        count("outside_frame_mixer_calls", outside_frame_mixer_calls);
        count("unconfirmed_mixer_calls", unconfirmed_mixer_calls); count("mixer_time_errors", mixer_time_errors);
        count("outside_frame_outputs", outside_frame_outputs); count("unconfirmed_outputs", unconfirmed_outputs);
        count("frame_sequence_errors", frame_sequence_errors); count("frame_scope_errors", frame_scope_errors);
        count("frame_completion_errors", frame_completion_errors); count("pcm_accounting_errors", pcm_accounting_errors);
        count("nonfinite_samples", nonfinite_samples); count("violations", violations());
        digest("io_hash", io_hash); digest("pcm_hash", pcm_hash); digest("queued_pcm_hash", queued_pcm_hash);
        const bool ready = comparison_limit ? comparison_ready : frames_completed != 0;
        const Comparison at = comparison_limit ? comparison :
            Comparison{u32(frames_completed), io_accesses, io_hash, pcm_generated, pcm_hash};
        std::fprintf(file, ",\"comparison\":{\"ready\":%s,\"frames\":%u,"
                     "\"io_accesses\":%llu,\"io_hash\":\"%016llx\","
                     "\"pcm_generated\":%llu,\"pcm_hash\":\"%016llx\"},"
                     "\"submission_scope\":\"AudioOut_software_queue_not_device_rendering_or_listening\"}",
                     ready ? "true" : "false", at.frames,
                     (unsigned long long)at.io_accesses, (unsigned long long)at.io_hash,
                     (unsigned long long)at.pcm_generated, (unsigned long long)at.pcm_hash);
    }
};

} // namespace stv::host
