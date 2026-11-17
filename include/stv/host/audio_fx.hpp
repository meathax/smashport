// Optional output enhancement for the PC player, applied after the sound
// board(s) produce PCM. Board PCM and its audits are never changed; this only
// shapes what reaches the speakers.
//
//  - Hi-fi: DC blocker, gentle analog-style low-pass (the cabinet amplifier
//    and speaker roll off the 8-bit DAC/CVSD step hash) and a small bass lift.
//  - Wide: mid/side pseudo-stereo from a decorrelated short delay; it cancels
//    exactly in a mono sum.
//  - Reverb: stereo Schroeder/Moorer network (Freeverb topology, original
//    code) with pre-delay; "Studio" and "Arena" rooms.
//  - Limiter: always on with any enhancement, so many simultaneous sounds
//    never clip.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stv::host {

struct AudioFxSettings {
    bool hifi = true;
    bool wide = true;
    int reverb = 1;            // 0 off, 1 studio, 2 arena
    bool any() const { return hifi || wide || reverb != 0; }
};

class AudioFx {
public:
    void configure(uint32_t rate, const AudioFxSettings& s);
    void reset();
    // In place, interleaved stereo.
    void process(float* lr, std::size_t frames);
    float limiter_gain() const { return gain_; }

private:
    struct Biquad {
        float b0{1}, b1{}, b2{}, a1{}, a2{};
        float z1{}, z2{};
        float run(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void clear() { z1 = z2 = 0; }
    };
    struct Comb {
        std::vector<float> buf; std::size_t i{}; float store{};
        float run(float x, float feedback, float damp) {
            const float y = buf[i];
            store = y * (1.0f - damp) + store * damp;
            buf[i] = x + store * feedback;
            if (++i == buf.size()) i = 0;
            return y;
        }
    };
    struct Allpass {
        std::vector<float> buf; std::size_t i{};
        float run(float x) {
            const float b = buf[i];
            buf[i] = x + b * 0.5f;
            if (++i == buf.size()) i = 0;
            return b - x;
        }
    };

    AudioFxSettings s_{};
    uint32_t rate_{48000};
    Biquad lp_[2], shelf_[2];
    float dc_x_[2]{}, dc_y_[2]{}, dc_r_{0.999f};
    std::vector<float> wide_buf_; std::size_t wide_i_{};
    Biquad wide_hp_;
    std::array<Comb, 8> comb_[2];
    std::array<Allpass, 4> ap_[2];
    std::vector<float> pre_; std::size_t pre_i_{};
    float room_{}, damp_{}, wet_{};
    Biquad verb_hp_, verb_lp_;
    float env_{}, gain_{1.0f}, attack_{}, release_{};
};

}  // namespace stv::host
