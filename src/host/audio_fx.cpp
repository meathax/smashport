#include "stv/host/audio_fx.hpp"

#include <algorithm>
#include <cmath>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define STV_AUDIO_FTZ 1
#endif

namespace stv::host {

namespace {
constexpr float kPi = 3.14159265358979f;

// RBJ cookbook biquads.
void lowpass(float& b0, float& b1, float& b2, float& a1, float& a2, float rate, float f, float q) {
    const float w = 2 * kPi * f / rate, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void highpass(float& b0, float& b1, float& b2, float& a1, float& a2, float rate, float f, float q) {
    const float w = 2 * kPi * f / rate, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void lowshelf(float& b0, float& b1, float& b2, float& a1, float& a2, float rate, float f, float db) {
    const float A = std::pow(10.0f, db / 40), w = 2 * kPi * f / rate, c = std::cos(w), s = std::sin(w);
    const float al = s / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * al;
    const float a0 = (A + 1) + (A - 1) * c + sa;
    b0 = A * ((A + 1) - (A - 1) * c + sa) / a0;
    b1 = 2 * A * ((A - 1) - (A + 1) * c) / a0;
    b2 = A * ((A + 1) - (A - 1) * c - sa) / a0;
    a1 = -2 * ((A - 1) + (A + 1) * c) / a0;
    a2 = ((A + 1) + (A - 1) * c - sa) / a0;
}
std::size_t scaled(int samples44k, uint32_t rate) {
    return std::max<std::size_t>(1, std::size_t(std::lround(double(samples44k) * rate / 44100.0)));
}
}  // namespace

void AudioFx::configure(uint32_t rate, const AudioFxSettings& s) {
    s_ = s;
    rate_ = rate ? rate : 48000;
    const float r = float(rate_);
    for (int c = 0; c < 2; ++c) {
        auto& l = lp_[c];
        lowpass(l.b0, l.b1, l.b2, l.a1, l.a2, r, std::min(11000.0f, r * 0.45f), 0.707f);
        auto& h = shelf_[c];
        lowshelf(h.b0, h.b1, h.b2, h.a1, h.a2, r, 120.0f, 2.5f);
    }
    dc_r_ = 1.0f - 2 * kPi * 18.0f / r;
    wide_buf_.assign(scaled(620, rate_), 0.0f);            // ~14 ms
    highpass(wide_hp_.b0, wide_hp_.b1, wide_hp_.b2, wide_hp_.a1, wide_hp_.a2, r, 300.0f, 0.707f);
    static constexpr int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr int kAp[4] = {556, 441, 341, 225};
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < 8; ++i) comb_[c][i].buf.assign(scaled(kComb[i] + c * 23, rate_), 0.0f);
        for (int i = 0; i < 4; ++i) ap_[c][i].buf.assign(scaled(kAp[i] + c * 23, rate_), 0.0f);
    }
    // Studio: the TV set, a bright medium room. Arena: a big hall.
    const bool arena = s_.reverb == 2;
    room_ = arena ? 0.86f : 0.74f;
    damp_ = arena ? 0.28f : 0.42f;
    wet_ = s_.reverb == 0 ? 0.0f : (arena ? 0.30f : 0.20f);
    pre_.assign(std::max<std::size_t>(1, std::size_t(r * (arena ? 0.032f : 0.014f))), 0.0f);
    highpass(verb_hp_.b0, verb_hp_.b1, verb_hp_.b2, verb_hp_.a1, verb_hp_.a2, r, 180.0f, 0.707f);
    lowpass(verb_lp_.b0, verb_lp_.b1, verb_lp_.b2, verb_lp_.a1, verb_lp_.a2, r, std::min(7000.0f, r * 0.45f), 0.707f);
    attack_ = 1.0f - std::exp(-1.0f / (0.0008f * r));
    release_ = 1.0f - std::exp(-1.0f / (0.180f * r));
    reset();
}

void AudioFx::reset() {
    for (int c = 0; c < 2; ++c) {
        lp_[c].clear(); shelf_[c].clear();
        dc_x_[c] = dc_y_[c] = 0;
        for (auto& k : comb_[c]) { std::fill(k.buf.begin(), k.buf.end(), 0.0f); k.i = 0; k.store = 0; }
        for (auto& a : ap_[c]) { std::fill(a.buf.begin(), a.buf.end(), 0.0f); a.i = 0; }
    }
    std::fill(wide_buf_.begin(), wide_buf_.end(), 0.0f);
    std::fill(pre_.begin(), pre_.end(), 0.0f);
    wide_i_ = pre_i_ = 0;
    wide_hp_.clear(); verb_hp_.clear(); verb_lp_.clear();
    env_ = 0; gain_ = 1.0f;
}

void AudioFx::process(float* lr, std::size_t frames) {
    if (!s_.any()) return;
#ifdef STV_AUDIO_FTZ
    // Reverb tails and filter states decay into denormals during quiet
    // passages, which made this the hottest host function. Flush them to zero
    // for this output-only stage and restore the caller's mode afterwards.
    struct FlushDenormals {
        unsigned saved = _mm_getcsr();
        FlushDenormals() { _mm_setcsr(saved | 0x8040u); }   // FTZ | DAZ
        ~FlushDenormals() { _mm_setcsr(saved); }
    } flush;
#endif
    const float feedback = room_, damp = damp_;
    for (std::size_t n = 0; n < frames; ++n) {
        float l = lr[2 * n], r = lr[2 * n + 1];
        if (!std::isfinite(l)) l = 0;
        if (!std::isfinite(r)) r = 0;
        if (s_.hifi) {
            float* x[2] = {&l, &r};
            for (int c = 0; c < 2; ++c) {
                const float in = *x[c];
                dc_y_[c] = in - dc_x_[c] + dc_r_ * dc_y_[c];
                dc_x_[c] = in;
                *x[c] = 1.25f * shelf_[c].run(lp_[c].run(dc_y_[c]));   // headroom freed by the DC offset
            }
        }
        if (s_.wide && !wide_buf_.empty()) {
            const float mid = 0.5f * (l + r);
            const float d = wide_buf_[wide_i_];
            wide_buf_[wide_i_] = mid;
            if (++wide_i_ == wide_buf_.size()) wide_i_ = 0;
            const float side = 0.22f * wide_hp_.run(d);   // L/R opposite: cancels in a mono sum
            l += side;
            r -= side;
        }
        if (s_.reverb) {
            const float in = verb_lp_.run(verb_hp_.run(0.5f * (l + r))) * 0.03f;
            const float delayed = pre_[pre_i_];
            pre_[pre_i_] = in;
            if (++pre_i_ == pre_.size()) pre_i_ = 0;
            float out[2] = {0, 0};
            for (int c = 0; c < 2; ++c) {
                for (auto& k : comb_[c]) out[c] += k.run(delayed, feedback, damp);
                for (auto& a : ap_[c]) out[c] = a.run(out[c]);
            }
            l += wet_ * out[0];
            r += wet_ * out[1];
        }
        // Peak limiter at -1 dBFS with a soft knee for anything that still overshoots.
        const float peak = std::max(std::fabs(l), std::fabs(r));
        env_ += (peak > env_ ? attack_ : release_) * (peak - env_);
        const float ceiling = 0.89f;
        const float want = env_ > ceiling ? ceiling / env_ : 1.0f;
        gain_ += (want < gain_ ? 0.5f : release_ * 8.0f) * (want - gain_);
        l *= gain_;
        r *= gain_;
        auto soft = [](float v) { const float a = std::fabs(v); return a < 0.9f ? v : std::copysign(0.9f + 0.1f * std::tanh((a - 0.9f) * 10.0f), v); };
        lr[2 * n] = soft(l);
        lr[2 * n + 1] = soft(r);
    }
}

}  // namespace stv::host
