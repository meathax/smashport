// Extended sound channels (host presentation only).
//
// The retail game drives one sound board through four logical channels with
// priorities (SOUNDS.ASM SNDLD): a sound on a busy channel is discarded when the
// playing one has higher priority, and every channel shares one board, so the
// announcer silences gunfire and a new sample cuts the previous one.
//
// VoiceBank keeps the retail board untouched and plays the same commands on a
// pool of extra boards running the same translated sound program: music on its
// own board, each sound sequence on its own board, and sounds the retail loader
// discarded on a free board as well. Global commands (reset, volume, all-off)
// reach every board. A silent template board receives only those global
// commands; a new voice starts as an exact copy of it, aligned in time.
#pragma once

#include "stv/snd/board.hpp"

#include <memory>
#include <vector>

namespace stv::snd {

class VoiceBank {
public:
    static constexpr int kMaxEffectVoices = 12;
    VoiceBank();
    ~VoiceBank();
    VoiceBank(const VoiceBank&) = delete;
    VoiceBank& operator=(const VoiceBank&) = delete;

    // Copy the live board and mixer (same ROMs) so every voice is aligned with
    // the retail stream from this moment. Returns false if a board faults.
    // `base` is the primary board's current scheduler base (all its timers fired).
    bool start(const Board& primary, const Mixer& primary_mixer, const Atto& base, int effect_voices);
    void stop();
    bool running() const { return !voices_.empty(); }
    Atto base() const;                 // time every voice has reached

    // Main CPU events, in order, at their main-CPU time.
    void sound_write(const Atto& t, int channel, uint16_t data);      // SNDSND, channel = A5
    void sound_accepted(int channel, uint32_t table);                 // SNDLD1: channel's new sequence
    void sound_dropped(const Atto& t, const std::vector<uint16_t>& words, uint32_t table);
    void tick(const Atto& t);                                         // SNDPRC cadence (once per frame)

    // Advance every playing board to `base` and add its audio, panned, to
    // interleaved stereo `out` (frames samples; out sized 2 * frames).
    void render(const Atto& base, std::size_t frames, std::vector<float>& out);

    // Development stem export only: when set, render() also writes the music
    // board's panned share here. The player never sets it; output is unchanged.
    void set_music_tap(std::vector<float>* tap) { music_tap_ = tap; }

    int active_voices() const;
    uint64_t dropped_played() const { return dropped_played_; }
    uint64_t steals() const { return steals_; }
    uint64_t boards_rendered() const { return boards_rendered_; }
    uint64_t allocations() const { return allocations_; }
    const std::string& fault() const { return fault_; }

private:
    struct Voice;
    struct Host;
    Voice* voice_for(uint32_t key, int channel, const Atto& t);
    void broadcast(const Atto& t, uint16_t data, bool include_music);
    void send(Voice& v, const Atto& t, uint16_t data);
    void sequence(Voice& v, const Atto& t);

    std::vector<std::unique_ptr<Voice>> voices_;   // [0] template, [1] music, [2..] effects
    uint32_t channel_key_[4]{};
    Voice* last_route_{};
    bool last_broadcast_{}, after_reset_{};
    uint64_t dropped_played_{}, steals_{}, clock_{}, boards_rendered_{}, allocations_{};
    std::string fault_;
    std::vector<float>* music_tap_{};
};

}  // namespace stv::snd
