#include "stv/snd/audio.hpp"
#include "stv/snd/state_stream.hpp"

#include <algorithm>

namespace stv::snd {

uint64_t sample_index(const Atto& t, uint32_t rate) {
    // muldivu_64(attoseconds, rate, 1e18) without overflow: split attoseconds
    const uint64_t as = uint64_t(t.as);
    const uint64_t hi = as / 1000000000ull, lo = as % 1000000000ull;     // as = hi * 1e9 + lo
    // exact floor of (hi * rate * 1e9 + lo * rate) / 1e18
    const uint64_t part = (hi * rate + (lo * rate) / 1000000000ull) / 1000000000ull;
    return uint64_t(t.sec) * rate + part;
}

// --------------------------------------------------------------------------- HC55516
namespace {
int32_t sext10(int32_t v) { v &= 0x3ff; return (v & 0x200) ? v - 0x400 : v; }
}

void Hc55516::clock_w(bool state) {
    if (state != last_clock) process_bit(buffered_bit, state);
    last_clock = state;
}

void Hc55516::process_bit(bool bit, bool clock_state) {
    const bool frozen = (intfilter >= 0x180 && !bit) || (intfilter <= -0x180 && bit);
    int32_t sum;
    if (clock_state != last_clock && clock_state) {       // active (rising) edge
        shiftreg = (shiftreg << 1) | (bit ? 1u : 0u);      // /FZ is not connected: never forced
        if ((shiftreg & 7) == 0 || (shiftreg & 7) == 7) {
            if (!frozen) sylfilter += ((~sylfilter) & 0xfc0) >> 6;
        } else {
            if (!frozen) sylfilter += (((~sylfilter) & 0xfc0) >> 6) + 0xfc1;
        }
        sylfilter &= 0xfff;
        sum = sext10(((~intfilter) >> 4) + 1);
    } else if (shiftreg & 1) {
        sum = sext10((~std::max(2, sylfilter >> 6)) + 1);
    } else {
        sum = sext10(std::max(2, sylfilter >> 6));
    }
    if (!frozen) intfilter = sext10(intfilter + sum);
    next_sample = int32_t(uint32_t(intfilter) << 6) | (((intfilter & 0x3ff) ^ 0x200) >> 4);
}

// --------------------------------------------------------------------------- Mixer
Mixer::Mixer(uint32_t out_rate) : out_rate_(out_rate) {}

void Mixer::ym_sample(int32_t l, int32_t r) { ym_.push_back(0.10f * float(l + r) / 32768.0f); }

void Mixer::dac(const Atto& t, uint8_t code) {
    // dac_mapper_unsigned * dac_gain_r2r mapped onto the default -1..+1 range, route gain 0.25
    const float v = 0.25f * (-1.0f + 2.0f * float(code) / 256.0f);
    dac_.push_back(Step{sample_index(t, kDacRate) + 1, v});
}

void Mixer::cvsd(const Atto& t, int32_t sample16) {
    cvsd_.push_back(Step{sample_index(t, kCvsdRate) + 1, 0.60f * float(sample16) / 32768.0f});
}

// Mean of a piecewise-constant stream over [t0, t1) seconds (a box filter over
// its high-rate samples); consumes steps that take effect before t1.
float Mixer::level_over(std::deque<Step>& steps, float& current, uint32_t rate, double t0, double t1) {
    double acc = 0.0, at = t0;
    while (!steps.empty()) {
        const double ts = double(steps.front().index) / rate;
        if (ts >= t1) break;
        if (ts > at) { acc += current * (ts - at); at = ts; }
        current = steps.front().value;
        steps.pop_front();
    }
    acc += current * (t1 - at);
    return float(acc / (t1 - t0));
}

void Mixer::render_until(const Atto& known, std::vector<float>& out) {
    const double known_s = double(known.sec) + double(known.as) * 1e-18;
    for (;;) {
        const double t0 = double(out_index_) / out_rate_, t1 = double(out_index_ + 1) / out_rate_;
        if (t1 > known_s) break;
        // YM: linear interpolation of the 55,930 Hz stream at the output sample time
        const double p = t0 * kYmRate;
        const uint64_t i = uint64_t(p);
        if (i + 1 >= ym_generated()) break;
        while (ym_base_ < i) { ym_.pop_front(); ++ym_base_; }
        const float frac = float(p - double(i));
        const float ym = ym_[0] + (ym_[1] - ym_[0]) * frac;
        const float dac = level_over(dac_, dac_level_, kDacRate, t0, t1);
        const float cvsd = level_over(cvsd_, cvsd_level_, kCvsdRate, t0, t1);
        out.push_back(std::clamp(ym + dac + cvsd, -1.0f, 1.0f));
        ++out_index_;
    }
}

}  // namespace stv::snd

namespace stv::snd {
void Mixer::save_state(std::vector<uint8_t>& out) const {
    StateOut o{out};
    o.pod(out_rate_); o.pod(out_index_); o.pod(ym_base_); o.pod(dac_level_); o.pod(cvsd_level_);
    o.vec(std::vector<float>(ym_.begin(), ym_.end()));
    o.vec(std::vector<Step>(dac_.begin(), dac_.end()));
    o.vec(std::vector<Step>(cvsd_.begin(), cvsd_.end()));
}
bool Mixer::load_state(const uint8_t*& p, const uint8_t* end) {
    StateIn i{p, end};
    uint32_t rate; uint64_t index, base; float dl, cl; std::vector<float> ym; std::vector<Step> d, c;
    i.pod(rate); i.pod(index); i.pod(base); i.pod(dl); i.pod(cl);
    i.vec(ym, 1u << 22); i.vec(d, 1u << 20); i.vec(c, 1u << 20);
    if (!i.ok) return false;
    p = i.p;
    // A different output rate cannot resume a sample position: start the local
    // output at the host's current chip time instead.
    if (rate != out_rate_) {
        out_index_ = uint64_t(double(index) * out_rate_ / rate);
    } else {
        out_index_ = index;
    }
    ym_base_ = base; dac_level_ = dl; cvsd_level_ = cl;
    ym_.assign(ym.begin(), ym.end()); dac_.assign(d.begin(), d.end()); cvsd_.assign(c.begin(), c.end());
    return true;
}
}
