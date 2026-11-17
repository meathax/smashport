#include "stv/snd/voices.hpp"

#include <algorithm>
#include <cmath>

namespace stv::snd {

namespace {
constexpr int64_t kQuantumAs = Atto::kPerSec / 60;   // MAME default maximum quantum
constexpr int kMusic = 0, kFirstEffect = 1;
// A voice that is no longer its channel's current sequence keeps sounding for
// this long after its last command (natural tails overlap), then is stopped:
// some retail loops end only because the next sound on the channel replaces them.
constexpr int kSupersededFrames = 180;
constexpr int kQuietFrames = 25;                      // silent this long: the voice is free

Atto add_as(const Atto& t, int64_t as) {
    Atto r{t.sec, t.as + as};
    while (r.as >= Atto::kPerSec) { r.as -= Atto::kPerSec; ++r.sec; }
    return r;
}
Atto min_atto(const Atto& a, const Atto& b) { return b < a ? b : a; }
Atto max_atto(const Atto& a, const Atto& b) { return a < b ? b : a; }
bool is_volume(uint8_t code) { return (code >= 0x20 && code <= 0x2f) || (code >= 0x60 && code <= 0x6f) || code == 0x7f; }
}  // namespace

// MAME timeslices for one board on its own: no main CPU, so a slice ends at the
// board's next timer, one quantum, or the requested limit.
struct VoiceBank::Host : SliceHost {
    Board* b{};
    Atto base{}, target{}, limit{};
    void end_now() { base = target; b->fire_until(base); }
    void start_slice() {
        for (;;) {
            if (!(base < limit)) { b->stop = true; return; }
            const Atto t = min_atto(min_atto(b->next_event_time(), add_as(base, kQuantumAs)), limit);
            target = t;
            const uint64_t st = cyc_floor(t);
            if (b->halted) { if (b->cpu.cyc < st) b->cpu.cyc = st; end_now(); continue; }
            if (b->cpu.cyc >= st) { end_now(); continue; }
            b->cpu.due = st;
            return;
        }
    }
    void slice_end() override {
        if (b->aborted) {
            b->aborted = false;
            const Atto ls = cyc_time(b->cpu.cyc);
            if (ls < target) target = max_atto(ls, base);
        }
        end_now();
        start_slice();
    }
    Atto next_foreign_timer() const override { return limit; }
    void advance(const Atto& to) {
        if (!(base < to)) return;
        limit = to;
        b->stop = false;
        start_slice();
        if (!b->stop) b->run();
    }
};

struct VoiceBank::Voice {
    explicit Voice(uint32_t rate) : mx(rate) { h.b = &b; b.host = &h; b.mixer = &mx; }
    Board b;
    Mixer mx;
    Host h;
    bool playing{};
    uint32_t key{};
    int channel{};
    float pan{};
    uint64_t last_use{};
    int frames_since_command{};
    int quiet{};
    // a sequence the retail loader discarded, replayed as SNDUPD/SNDPRC would
    std::vector<uint16_t> seq;
    std::size_t pos{};
    uint16_t period{}, timer{};
    bool seq_on{};
    std::vector<float> buf;
    float dc{};          // each board idles at a DAC offset: remove it per voice
    bool dc_set{};
    void copy_from(const Voice& o) {
        b.copy_prediction_state(o.b);
        b.host = &h;
        b.mixer = &mx;
        std::vector<uint8_t> state;
        o.mx.save_state(state);
        const uint8_t* p = state.data();
        mx.load_state(p, p + state.size());
        h.base = o.h.base;
        h.target = o.h.target;
        h.limit = o.h.limit;
    }
};

VoiceBank::VoiceBank() = default;
VoiceBank::~VoiceBank() = default;

bool VoiceBank::start(const Board& primary, const Mixer& primary_mixer, const Atto& base, int effect_voices) {
    stop();
    effect_voices = std::clamp(effect_voices, 1, kMaxEffectVoices);
    for (int i = 0; i < kFirstEffect + effect_voices; ++i)
        voices_.push_back(std::make_unique<Voice>(primary_mixer.out_rate()));
    for (int i = 0; i < kFirstEffect; ++i) {
        Voice& v = *voices_[i];
        v.b.copy_prediction_state(primary);
        v.b.host = &v.h;
        v.b.mixer = &v.mx;
        std::vector<uint8_t> state;
        primary_mixer.save_state(state);
        const uint8_t* p = state.data();
        if (!v.mx.load_state(p, p + state.size())) { stop(); return false; }
        v.h.base = v.h.target = v.h.limit = base;
        v.playing = true;
    }
    for (auto& k : channel_key_) k = 0;
    last_route_ = nullptr;
    last_broadcast_ = after_reset_ = false;
    fault_.clear();
    return true;
}

void VoiceBank::stop() { voices_.clear(); }

Atto VoiceBank::base() const { return voices_.empty() ? Atto{} : voices_[kMusic]->h.base; }

int VoiceBank::active_voices() const {
    int n = 0;
    for (std::size_t i = kFirstEffect; i < voices_.size(); ++i) n += voices_[i]->playing;
    return n;
}

void VoiceBank::send(Voice& v, const Atto& t, uint16_t data) {
    // Never earlier than what the board has already run.
    v.b.main_sound_write(max_atto(t, v.h.base), data);
    v.frames_since_command = 0;
    v.quiet = 0;
}

void VoiceBank::broadcast(const Atto& t, uint16_t data, bool include_music) {
    for (std::size_t i = 0; i < voices_.size(); ++i) {
        Voice& v = *voices_[i];
        if ((int(i) == kMusic && !include_music) || !v.playing) continue;
        send(v, t, data);
    }
}

VoiceBank::Voice* VoiceBank::voice_for(uint32_t key, int channel, const Atto& t) {
    (void)t;
    Voice* free_voice = nullptr;
    Voice* oldest = nullptr;
    for (std::size_t i = kFirstEffect; i < voices_.size(); ++i) {
        Voice& v = *voices_[i];
        if (v.playing && v.key == key) { v.last_use = ++clock_; return &v; }   // retrigger, as the channel would
        if (!v.playing) { if (!free_voice) free_voice = &v; continue; }
        if (!oldest || v.last_use < oldest->last_use) oldest = &v;
    }
    Voice* v = free_voice ? free_voice : oldest;
    if (!v) return nullptr;
    if (!free_voice) ++steals_;
    ++allocations_;
    // Start from the music board (same time, same global volume state) without
    // its queued music commands, then silence it: a new, aligned, idle board.
    const Voice& music = *voices_[kMusic];
    v->copy_from(music);
    v->b.q.erase(std::remove_if(v->b.q.begin(), v->b.q.end(),
                                [](const Board::Event& e) { return e.kind == Board::kSyncWrite; }),
                 v->b.q.end());
    for (uint16_t code : {uint16_t(0x00), uint16_t(0x7f)}) {
        send(*v, music.h.base, uint16_t(0xfd00 | code));
        send(*v, music.h.base, uint16_t(0xff00 | code));
    }
    v->playing = true;
    v->key = key;
    v->channel = channel;
    v->seq_on = false;
    v->last_use = ++clock_;
    v->frames_since_command = 0;
    v->quiet = 0;
    v->dc_set = false;
    // The arcade cabinet is mono: keep the announcer and gunfire centred and
    // spread the other channels slightly so simultaneous sounds stay distinct.
    static constexpr float kPan[4] = {0.0f, -0.28f, 0.28f, 0.0f};
    int slot = 0;
    for (std::size_t i = 0; i < voices_.size(); ++i) if (voices_[i].get() == v) slot = int(i);
    v->pan = kPan[channel & 3] + ((slot & 1) ? 0.08f : -0.08f);
    return v;
}

void VoiceBank::sound_write(const Atto& t, int channel, uint16_t data) {
    if (voices_.empty()) return;
    const uint8_t code = uint8_t(data);
    const bool strobe = !(data & 0x200);              // FDxx asserts, FFxx releases
    if (!(data & 0x100)) {                            // SNDRES: reset every board
        broadcast(t, data, true);
        after_reset_ = true;
        return;
    }
    if (after_reset_ && data == 0xffff) {             // the reset release
        broadcast(t, data, true);
        after_reset_ = false;
        return;
    }
    after_reset_ = false;
    if (!strobe) {                                    // release goes where its strobe went
        if (last_broadcast_) broadcast(t, data, true);
        else if (last_route_) send(*last_route_, t, data);
        return;
    }
    channel &= 3;
    last_route_ = nullptr;
    last_broadcast_ = code == 0x00 || (channel == 3 && is_volume(code));
    if (last_broadcast_) { broadcast(t, data, true); return; }
    if (channel == 3) { last_route_ = voices_[kMusic].get(); send(*last_route_, t, data); return; }
    const uint32_t key = channel_key_[channel] ? channel_key_[channel] : 0x80000000u | uint32_t(channel);
    last_route_ = voice_for(key, channel, t);
    if (last_route_) send(*last_route_, t, data);
}

void VoiceBank::sound_accepted(int channel, uint32_t table) {
    if (voices_.empty()) return;
    channel_key_[channel & 3] = table;
}

void VoiceBank::sequence(Voice& v, const Atto& t) {
    while (v.pos < v.seq.size()) {
        const uint16_t w = v.seq[v.pos++];
        if (!w) break;
        if (w >= 0xf000) continue;                    // priority word
        if (w >= 0x4000) {
            send(v, t, uint16_t(0xfd00 | (w & 0xff)));
            send(v, t, uint16_t(0xff00 | (w & 0xff)));
            if (w < 0x8000) continue;                 // zero-time code
            v.timer = v.period;
            if (v.timer) return;
            break;
        }
        v.period = v.timer = w;
    }
    v.seq_on = false;
}

void VoiceBank::sound_dropped(const Atto& t, const std::vector<uint16_t>& words, uint32_t table) {
    if (voices_.empty() || words.empty() || words[0] < 0xf000) return;
    const int channel = (words[0] >> 8) & 3;
    if (channel == 3) return;                         // music decisions stay with the game
    for (uint16_t w : words)
        if (w >= 0x4000 && w < 0xf000 && (w & 0xff) == 0x00) return;   // global stop: not a sound
    Voice* v = voice_for(table, channel, t);
    if (!v) return;
    v->seq = words;
    v->pos = 0;
    v->period = v->timer = 0;
    v->seq_on = true;
    ++dropped_played_;
    sequence(*v, t);
}

void VoiceBank::tick(const Atto& t) {
    for (std::size_t i = kFirstEffect; i < voices_.size(); ++i) {
        Voice& v = *voices_[i];
        if (!v.playing) continue;
        ++v.frames_since_command;
        if (v.seq_on && v.timer && --v.timer == 0) sequence(v, t);
        const bool current = v.key == channel_key_[v.channel & 3] && !(v.key & 0x80000000u);
        if (!v.seq_on && !current && v.frames_since_command > kSupersededFrames) {
            send(v, t, 0xfd00);                       // stop a loop the arcade would have replaced
            send(v, t, 0xff00);
            v.playing = false;
        }
    }
}

void VoiceBank::render(const Atto& base, std::size_t frames, std::vector<float>& out) {
    out.assign(frames * 2, 0.0f);
    if (voices_.empty()) return;
    for (auto& v : voices_) {
        if (!v->playing) continue;
        ++boards_rendered_;
        v->h.advance(base);
        v->b.ym_update(base);
        v->buf.clear();
        v->mx.render_until(base, v->buf);
    }
    for (std::size_t i = 0; i < voices_.size(); ++i) {
        Voice& v = *voices_[i];
        if (!v.playing) continue;
        if (!v.b.fault.empty()) { fault_ = v.b.fault; stop(); out.assign(frames * 2, 0.0f); return; }
        const std::size_t n = std::min(frames, v.buf.size());
        float lo = 0.0f, hi = 0.0f;
        const float gl = std::min(1.0f, 1.0f - v.pan), gr = std::min(1.0f, 1.0f + v.pan);
        if (!v.dc_set && n) { v.dc = v.buf[0]; v.dc_set = true; }   // start at its level: no click
        for (std::size_t k = 0; k < n; ++k) {
            v.dc += 0.00065f * (v.buf[k] - v.dc);         // ~5 Hz tracking
            const float s = v.buf[k] - v.dc;
            out[2 * k] += s * gl;
            out[2 * k + 1] += s * gr;
            if (k == 0) lo = hi = s;
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        if (int(i) >= kFirstEffect) {
            v.quiet = (hi - lo) < 2e-3f ? v.quiet + 1 : 0;
            const bool current = v.key == channel_key_[v.channel & 3];
            if (!v.seq_on && v.quiet > kQuietFrames && (!current || v.frames_since_command > kQuietFrames))
                v.playing = false;                    // silent: free for the next sound
        }
    }
}

}  // namespace stv::snd
