// Replay proof for global leaderboard scores.
//
// A local run is deterministic from power-on: the machine, the boot CMOS and
// the per-frame controls decide everything. The player records those controls
// (run-length coded, the same tuple the online drop-in history uses) and attaches
// them to a submitted score. Anyone who owns the retail ROM can re-simulate the
// run and confirm that the claimed high-score entry appears in the table.
// Platform neutral; the verifier tool and the player share this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace stv::host {

// Largest proof the leaderboard accepts (the server stores it in one database row).
inline constexpr std::size_t kMaxReplayBytes = 900'000;
// Profile byte of a proof: 0 Classic, otherwise the Widescreen ruleset. Ruleset 1
// is the v1.0.0-v1.0.4 balance; 2 is the 25 November 2026 player rebalance (speed,
// fire cadence, special charges); 3 is the 27 November 2026 crowd rebalance
// (about 2.25x enemies in larger gangs, 70% mines, more pickups); 4 is the
// 27 November 2026 rule where Widescreen always plays weapon variants at the
// balanced rate (stv::gsp::kWidescreenVariantMode); 5 is the 28 November 2026
// pressure rebalance (172/172/192% budgets, 1.9x release, slower pickups). Bump it whenever
// Widescreen gameplay rules change so a verifier only replays proofs recorded
// under its own rules.
inline constexpr std::uint8_t kWidescreenRuleset = 5;

class ReplayProof {
public:
    // One run of identical controls. `before` marks local play, where the
    // controls (and the raw switch word) are sampled before the frame runs;
    // scripted and online frames apply them at the vblank instead.
    struct Segment {
        std::uint32_t p1 = 0, p2 = 0;
        std::uint16_t frames = 0, switches = 0;
        bool before = false;
        bool operator==(const Segment& o) const {
            return p1 == o.p1 && p2 == o.p2 && frames == o.frames && before == o.before &&
                   (!before || switches == o.switches);
        }
    };
    struct Decoded {
        bool wide = false;
        std::uint8_t ruleset = 0;                 // profile byte: 0 Classic, else Widescreen ruleset
        std::vector<std::uint16_t> boot_cmos;
        std::vector<Segment> segments;
        std::uint64_t frames = 0;
    };

    static constexpr std::size_t kMaxSegments = 2'000'000;   // about 32 MB of history
    static constexpr std::size_t kMaxFrames = 20'000'000;
    static constexpr std::size_t kMaxCmosWords = 0x10000;

    // Start (or restart) recording at power-on with this CMOS image.
    void begin(bool wide, const std::vector<std::uint16_t>& boot_cmos) {
        wide_ = wide;
        boot_cmos_ = boot_cmos;
        segments_.clear();
        segments_.shrink_to_fit();
        frames_ = 0;
        valid_ = true;
    }
    // Anything outside the controls changed the run (network play, snapshots,
    // a machine swap): the history can no longer reproduce it.
    void taint() {
        valid_ = false;
        segments_.clear();
        segments_.shrink_to_fit();
    }
    void add(std::uint32_t p1, std::uint32_t p2, std::optional<std::uint16_t> switches) {
        if (!valid_) return;
        const bool before = switches.has_value();
        const std::uint16_t sw = before ? *switches : std::uint16_t(0);
        ++frames_;
        if (!segments_.empty()) {
            auto& last = segments_.back();
            if (last.p1 == p1 && last.p2 == p2 && last.before == before && (!before || last.switches == sw) &&
                last.frames < 65535) {
                ++last.frames;
                return;
            }
        }
        if (segments_.size() >= kMaxSegments || frames_ > kMaxFrames) { taint(); return; }
        segments_.push_back({p1, p2, 1, sw, before});
    }
    bool valid() const { return valid_; }
    std::uint64_t frames() const { return frames_; }
    std::size_t segments() const { return segments_.size(); }

    // Serialized proof, or empty when the run is not provable or exceeds max_bytes.
    std::vector<std::uint8_t> encode(std::size_t max_bytes) const {
        std::vector<std::uint8_t> out;
        if (!valid_ || segments_.empty()) return out;
        const char magic[4] = {'S', 'T', 'V', 'R'};
        out.insert(out.end(), magic, magic + 4);
        out.push_back(1);                 // version
        out.push_back(wide_ ? kWidescreenRuleset : 0);   // profile / Widescreen ruleset
        varint(out, frames_);
        varint(out, boot_cmos_.size());
        for (std::size_t i = 0; i < boot_cmos_.size();) {
            std::size_t zeros = 0;
            while (i + zeros < boot_cmos_.size() && boot_cmos_[i + zeros] == 0) ++zeros;
            std::size_t lit = 0;
            while (i + zeros + lit < boot_cmos_.size() && boot_cmos_[i + zeros + lit] != 0) ++lit;
            varint(out, zeros);
            varint(out, lit);
            for (std::size_t k = 0; k < lit; ++k) varint(out, boot_cmos_[i + zeros + k]);
            i += zeros + lit;
        }
        varint(out, segments_.size());
        std::uint32_t prev[4] = {0, 0, 0, 0};   // p1 move, p1 low, p2 move, p2 low
        std::uint16_t prev_switches = 0;
        for (const auto& s : segments_) {
            const std::uint32_t now[4] = {s.p1 >> 16, s.p1 & 0xffff, s.p2 >> 16, s.p2 & 0xffff};
            std::uint8_t flags = s.before ? 1 : 0;
            for (int i = 0; i < 4; ++i) if (now[i] != prev[i]) flags |= std::uint8_t(2 << i);
            const bool sw_changed = s.before && s.switches != prev_switches;
            if (sw_changed) flags |= 0x20;
            out.push_back(flags);
            varint(out, s.frames);
            for (int i = 0; i < 4; ++i)
                if (now[i] != prev[i]) varint(out, zigzag(std::int32_t(now[i]) - std::int32_t(prev[i])));
            if (sw_changed) varint(out, s.switches);
            for (int i = 0; i < 4; ++i) prev[i] = now[i];
            if (s.before) prev_switches = s.switches;
            if (out.size() + 8 > max_bytes) return {};
        }
        const std::uint64_t sum = fnv(out.data(), out.size());
        for (int i = 0; i < 8; ++i) out.push_back(std::uint8_t(sum >> (8 * i)));
        if (out.size() > max_bytes) return {};
        return out;
    }

    static bool decode(const std::uint8_t* data, std::size_t size, Decoded& out) {
        if (size < 4 + 2 + 8 || data[0] != 'S' || data[1] != 'T' || data[2] != 'V' || data[3] != 'R' || data[4] != 1)
            return false;
        std::uint64_t sum = 0;
        for (int i = 0; i < 8; ++i) sum |= std::uint64_t(data[size - 8 + i]) << (8 * i);
        if (sum != fnv(data, size - 8)) return false;
        const std::uint8_t* p = data + 6;
        const std::uint8_t* end = data + size - 8;
        Decoded d;
        d.wide = data[5] != 0;
        d.ruleset = data[5];
        std::uint64_t value = 0;
        if (!read(p, end, value) || value > kMaxFrames) return false;
        d.frames = value;
        if (!read(p, end, value) || value > kMaxCmosWords) return false;
        const std::size_t words = std::size_t(value);
        d.boot_cmos.assign(words, 0);
        for (std::size_t i = 0; i < words;) {
            std::uint64_t zeros = 0, lit = 0;
            if (!read(p, end, zeros) || !read(p, end, lit) || zeros > words - i || lit > words - i - zeros) return false;
            i += std::size_t(zeros);
            for (std::uint64_t k = 0; k < lit; ++k) {
                std::uint64_t w = 0;
                if (!read(p, end, w) || w > 0xffff) return false;
                d.boot_cmos[i++] = std::uint16_t(w);
            }
        }
        if (!read(p, end, value) || value > kMaxSegments) return false;
        std::uint32_t prev[4] = {0, 0, 0, 0};
        std::uint16_t prev_switches = 0;
        std::uint64_t frames = 0;
        d.segments.reserve(std::size_t(value));
        for (std::uint64_t n = 0; n < value; ++n) {
            if (p >= end) return false;
            const std::uint8_t flags = *p++;
            if (flags & 0xc0) return false;
            std::uint64_t run = 0;
            if (!read(p, end, run) || !run || run > 65535) return false;
            for (int i = 0; i < 4; ++i) {
                if (!(flags & (2 << i))) continue;
                std::uint64_t z = 0;
                if (!read(p, end, z) || z > 0xffffffffull) return false;
                const std::int64_t next = std::int64_t(prev[i]) + unzigzag(std::uint32_t(z));
                if (next < 0 || next > 0xffff) return false;
                prev[i] = std::uint32_t(next);
            }
            Segment s;
            s.p1 = (prev[0] << 16) | prev[1];
            s.p2 = (prev[2] << 16) | prev[3];
            s.frames = std::uint16_t(run);
            s.before = (flags & 1) != 0;
            if (flags & 0x20) {
                std::uint64_t w = 0;
                if (!s.before || !read(p, end, w) || w > 0xffff) return false;
                prev_switches = std::uint16_t(w);
            }
            if (s.before) s.switches = prev_switches;
            frames += run;
            d.segments.push_back(s);
        }
        if (p != end || frames != d.frames) return false;
        out = std::move(d);
        return true;
    }

private:
    static std::uint32_t zigzag(std::int32_t v) { return (std::uint32_t(v) << 1) ^ std::uint32_t(v >> 31); }
    static std::int64_t unzigzag(std::uint32_t z) { return std::int64_t(z >> 1) ^ -std::int64_t(z & 1); }
    static void varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
        while (v >= 0x80) { out.push_back(std::uint8_t(v | 0x80)); v >>= 7; }
        out.push_back(std::uint8_t(v));
    }
    static bool read(const std::uint8_t*& p, const std::uint8_t* end, std::uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) return false;
            const std::uint8_t b = *p++;
            v |= std::uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    static std::uint64_t fnv(const std::uint8_t* d, std::size_t n) {
        std::uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < n; ++i) h = (h ^ d[i]) * 1099511628211ull;
        return h;
    }

    bool wide_ = false, valid_ = false;
    std::vector<std::uint16_t> boot_cmos_;
    std::vector<Segment> segments_;
    std::uint64_t frames_ = 0;
};

}  // namespace stv::host
