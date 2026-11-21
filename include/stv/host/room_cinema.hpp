#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace stv::host {

constexpr unsigned RoomCinemaQualityOff = 0;
constexpr unsigned RoomCinemaQualityLow = 1;
constexpr unsigned RoomCinemaQualityMedium = 2;
constexpr unsigned RoomCinemaQualityHigh = 3;

inline const char* room_cinema_quality_name(unsigned quality) {
    static constexpr const char* names[] = {"off", "low", "medium", "high"};
    return names[std::min<unsigned>(quality, 3)];
}

class AudioPeakObserver {
  public:
    void reset() { *this = AudioPeakObserver{}; }
    // Called only over newly rendered, confirmed board PCM before output processing.
    void observe(std::span<const float> samples) {
        if (samples.empty()) return;
        float peak = 0;
        for (float sample : samples)
            if (std::isfinite(sample)) peak = std::max(peak, std::fabs(sample));
        peak_ = std::min(peak, 1.0f);
        pulse_ = peak_ >= pulse_ ? peak_ : pulse_ * 0.82f;
        samples_seen_ += samples.size();
    }
    float peak() const { return peak_; }
    float pulse() const { return pulse_; }
    std::uint64_t samples_seen() const { return samples_seen_; }

  private:
    float peak_{}, pulse_{};
    std::uint64_t samples_seen_{};
};

struct RoomCinemaInput {
    std::uint64_t frame{};
    std::uint32_t room{};
    unsigned roomAge{};
    unsigned bossParts{};
    float bossX{}, bossY{};
    bool inArena{}, roomIntro{}, bossRoom{}, bossRetired{};
};

struct RoomCinemaCue {
    unsigned quality{};
    float audioPulse{};
    float bossIntro{}, bossPower{}, bossPhase{};
    float bossX{}, bossY{};
    float deathX{}, deathY{}, deathPower{}, deathRadius{};
    bool activeBoss{}, death{};
};

class BossCinemaTracker {
  public:
    static constexpr unsigned DeathGuardFrames = 12;
    void reset() { *this = BossCinemaTracker{}; }
    RoomCinemaCue observe(const RoomCinemaInput& in, unsigned quality, float audioPulse) {
        RoomCinemaCue cue;
        cue.quality = std::min<unsigned>(quality, RoomCinemaQualityHigh);
        if (cue.quality == RoomCinemaQualityOff) {
            reset();
            return cue;
        }
        cue.audioPulse = std::clamp(audioPulse, 0.0f, 1.0f);
        if (!in.inArena) {
            reset();
            cue.audioPulse = 0;
            return cue;
        }
        if (in.room != room_ || in.frame < lastFrame_) reset_boss(in.room, in.frame);
        lastFrame_ = in.frame;
        if (!in.bossRoom) {
            active_ = pending_ = dead_ = false;
            return cue;
        }

        cue.bossX = bossX_;
        cue.bossY = bossY_;
        if (in.bossParts) {
            if (!active_) activeSince_ = in.frame;
            active_ = true;
            pending_ = dead_ = false;
            bossX_ = cue.bossX = in.bossX;
            bossY_ = cue.bossY = in.bossY;
            lastBossFrame_ = in.frame;
        }
        if (!in.roomIntro) {
            if (in.bossRetired && active_ && !in.bossParts &&
                in.frame - activeSince_ >= DeathGuardFrames && !dead_) {
                pending_ = true;
                pendingSince_ = in.frame;
            }
            if (pending_ && in.frame >= pendingSince_ + DeathGuardFrames) {
                dead_ = true;
                pending_ = false;
                deathSince_ = in.frame;
                deathX_ = bossX_;
                deathY_ = bossY_;
            }
        }
        if (in.roomIntro) {
            pending_ = dead_ = false;
            cue.bossIntro = std::max(0.0f, 1.0f - float(in.roomAge) / 42.0f);
            cue.bossPhase = cue.bossIntro * 0.25f;
        } else if (dead_) {
            const unsigned age = in.frame >= deathSince_ ? unsigned(std::min<std::uint64_t>(
                in.frame - deathSince_, 240)) : 0;
            const float duration = cue.quality == RoomCinemaQualityLow ? 52.0f :
                                   cue.quality == RoomCinemaQualityMedium ? 86.0f : 120.0f;
            cue.deathPower = std::max(0.0f, 1.0f - float(age) / duration);
            cue.deathRadius = 28.0f + 116.0f * float(age) / duration;
            cue.deathX = deathX_;
            cue.deathY = deathY_;
            cue.death = cue.deathPower > 0;
            cue.bossPhase = 1.0f;
        } else if (active_ && in.bossParts) {
            cue.activeBoss = true;
            cue.bossPower = 0.18f + 0.06f * float(cue.quality);
            cue.bossPhase = 0.55f;
        } else if (pending_) {
            cue.bossPower = 0.22f + 0.07f * float(cue.quality);
            cue.bossPhase = 0.82f;
        }
        return cue;
    }

  private:
    void reset_boss(std::uint32_t room, std::uint64_t frame) {
        room_ = room;
        activeSince_ = lastBossFrame_ = pendingSince_ = deathSince_ = frame;
        lastFrame_ = frame;
        active_ = pending_ = dead_ = false;
        bossX_ = bossY_ = deathX_ = deathY_ = 0;
    }
    std::uint32_t room_{};
    std::uint64_t activeSince_{}, lastBossFrame_{}, pendingSince_{}, deathSince_{}, lastFrame_{};
    float bossX_{}, bossY_{}, deathX_{}, deathY_{};
    bool active_{}, pending_{}, dead_{};
};

// Encounter palettes are presentation data, never inferred attack ownership.
inline std::array<float, 3> boss_attack_tint(std::uint32_t room) {
    switch (room) {
    case 10: return {.16f, .78f, 1.f};
    case 0x10007u: return {.38f, 1.f, .12f};
    case 0x20007u: return {1.f, .32f, .045f};
    case 0x20015u: return {1.f, .12f, .64f};
    default: return {1.f, .55f, .18f};
    }
}

inline std::array<float, 3> room_cinema_tint(std::uint32_t room) {
    switch (room) {
    case 10: return {.62f, .86f, 1.0f};        // Mutoid Man: cold steel
    case 0x10007u: return {.62f, 1.0f, .72f};  // Scarface: phosphor green
    case 0x20007u: return {1.0f, .68f, .40f};  // Cobra: furnace amber
    case 0x20015u: return {1.0f, .58f, .94f};  // finale: hot magenta
    default: return {1.0f, 1.0f, 1.0f};
    }
}

} // namespace stv::host
