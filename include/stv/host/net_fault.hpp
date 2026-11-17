#pragma once
// Development-only fault injection. Select a frame the real rollback protocol
// exchanges, and change exactly one RAM word before its stored hash is taken.
// Replays of that selected frame repeat the mutation from their restored state.
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace stv::host {
struct NetFaultInjection {
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    struct Event {
        u32 frame;
        std::uint16_t before_word, after_word;
        u32 before_hash, after_hash;
    };
    bool enabled{};
    u32 requested_frame{}, effective_frame{}, hash_period{};
    u64 attempts{}, changed_words{}, changed_hashes{};
    std::vector<Event> events;

    bool configure(u32 requested, u32 period) {
        *this = NetFaultInjection{};
        requested_frame = requested; hash_period = period;
        if (!period) return false;
        if (!requested) return true; // CLI zero means no requested injection.
        // RollbackSession::send exchanges g where g % kHashEvery == 0.
        const u64 selected = u64(requested) + (period - requested % period) % period;
        if (selected > std::numeric_limits<u32>::max()) return false;
        effective_frame = u32(selected); enabled = true;
        return true;
    }
    template<class Hash>
    bool after_step(u32 frame, u64 resyncs, bool resyncing, std::uint16_t& word, Hash hash) {
        if (!enabled || resyncs || resyncing || frame != effective_frame) return false;
        Event event{frame, word, 0, hash(), 0};
        word = std::uint16_t(word ^ 0x8000u);
        event.after_word = word; event.after_hash = hash();
        ++attempts;
        changed_words += event.before_word != event.after_word;
        changed_hashes += event.before_hash != event.after_hash;
        events.push_back(event);
        return true;
    }
    void write_json(FILE* file) const {
        std::fprintf(file, "{\"schema\":\"smash.net.fault-injection.v1\","
                     "\"mode\":\"single_exchanged_hash_frame_xor_8000\",\"enabled\":%s,"
                     "\"requested_frame\":%u,\"effective_frame\":%u,\"hash_period\":%u,"
                     "\"attempts\":%llu,\"changed_words\":%llu,\"changed_hashes\":%llu,"
                     "\"events\":[",
                     enabled ? "true" : "false", requested_frame, effective_frame, hash_period,
                     (unsigned long long)attempts, (unsigned long long)changed_words,
                     (unsigned long long)changed_hashes);
        bool first = true;
        for (const auto& event : events) {
            std::fprintf(file, "%s{\"frame\":%u,\"before_word\":\"%04x\",\"after_word\":\"%04x\","
                         "\"before_hash\":\"%08x\",\"after_hash\":\"%08x\"}",
                         first ? "" : ",", event.frame, unsigned(event.before_word), unsigned(event.after_word),
                         event.before_hash, event.after_hash);
            first = false;
        }
        std::fputs("]}", file);
    }
};
} // namespace stv::host
