// Main-menu / title-options background music driven by a standalone instance of
// the translated retail L8 sound board (no main CPU, no bundled audio assets).
// Plays the original Smash T.V. main theme (TUNE2, code 0x01) looping at 50%
// volume without touching the gameplay machine, sound board, or replay proof.
#pragma once

#include "stv/snd/audio.hpp"
#include "stv/snd/board.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace stv::host {

class MenuMusic {
public:
    static constexpr uint8_t kDefaultTrackCode = 0x01;  // SOUNDS.ASM TUNE2 (First Bkrnd Tune)
    static constexpr float kDefaultGain = 0.5f;         // 50% volume

    bool start(const std::vector<uint8_t>& u4,
               const std::vector<uint8_t>& u19,
               const std::vector<uint8_t>& u20,
               uint32_t out_rate,
               uint8_t track_code = kDefaultTrackCode,
               float gain = kDefaultGain) {
        stop();
        board_ = std::make_unique<stv::snd::Board>();
        mixer_ = std::make_unique<stv::snd::Mixer>(out_rate ? out_rate : 48000u);
        host_ = Host{};
        host_.b = board_.get();
        board_->host = &host_;
        board_->mixer = mixer_.get();

        std::string err;
        if (!board_->load_roms(u4, u19, u20, err)) {
            stop();
            return false;
        }
        board_->power_on();

        // Mirror SOUNDS.ASM QSNDRST: send a quiet code (0x00) after power-on so
        // the 6809 skips its diagnostic ding and PIA CB1 settles idle-high.
        const stv::snd::Atto kStrobe = {0, stv::snd::Atto::kPerSec / 10000};  // 100 us
        board_->main_sound_write(kStrobe, 0xFD00u);
        board_->main_sound_write(Host::add_as(kStrobe, kStrobe.as), 0xFF00u);

        // Warm up the sound board past boot/init and discard warm-up PCM.
        const stv::snd::Atto kWarmup = {0, stv::snd::Atto::kPerSec / 2};  // 500 ms
        host_.advance(kWarmup);
        if (!board_->fault.empty()) {
            stop();
            return false;
        }
        board_->ym_update(host_.base);
        std::vector<float> discard;
        mixer_->render_until(host_.base, discard);

        // Trigger the retail music track (strobe low then high, matching SNDSND).
        board_->main_sound_write(host_.base, static_cast<uint16_t>(0xFD00u | track_code));
        board_->main_sound_write(Host::add_as(host_.base, kStrobe.as),
                                 static_cast<uint16_t>(0xFF00u | track_code));

        gain_ = std::clamp(gain, 0.0f, 1.0f);
        dc_ = 0.0f;
        dc_set_ = false;
        running_ = true;
        return true;
    }

    void stop() {
        running_ = false;
        board_.reset();
        mixer_.reset();
        host_ = Host{};
        dc_ = 0.0f;
        dc_set_ = false;
    }

    bool running() const noexcept { return running_; }

    void render(int64_t delta_as, std::vector<float>& out) {
        out.clear();
        if (!running_ || delta_as <= 0) return;
        const auto to = Host::add_as(host_.base, delta_as);
        host_.advance(to);
        if (!board_->fault.empty()) {
            stop();
            return;
        }
        board_->ym_update(host_.base);
        mixer_->render_until(host_.base, out);
        if (!out.empty()) {
            if (!dc_set_) {
                dc_ = out[0];
                dc_set_ = true;
            }
            for (float& s : out) {
                dc_ += 0.00065f * (s - dc_);
                s = (s - dc_) * gain_;
            }
        }
    }

private:
    struct Host : stv::snd::SliceHost {
        static constexpr int64_t kQuantumAs = stv::snd::Atto::kPerSec / 60;
        stv::snd::Board* b{};
        stv::snd::Atto base{}, target{}, limit{};

        static stv::snd::Atto add_as(const stv::snd::Atto& t, int64_t as) {
            stv::snd::Atto r{t.sec, t.as + as};
            while (r.as >= stv::snd::Atto::kPerSec) {
                r.as -= stv::snd::Atto::kPerSec;
                ++r.sec;
            }
            return r;
        }
        static stv::snd::Atto min_atto(const stv::snd::Atto& a, const stv::snd::Atto& b) {
            return b < a ? b : a;
        }
        static stv::snd::Atto max_atto(const stv::snd::Atto& a, const stv::snd::Atto& b) {
            return a < b ? b : a;
        }

        void end_now() {
            base = target;
            b->fire_until(base);
        }
        void start_slice() {
            for (;;) {
                if (!(base < limit)) {
                    b->stop = true;
                    return;
                }
                const auto t = min_atto(min_atto(b->next_event_time(), add_as(base, kQuantumAs)), limit);
                target = t;
                const uint64_t st = stv::snd::cyc_floor(t);
                if (b->halted) {
                    if (b->cpu.cyc < st) b->cpu.cyc = st;
                    end_now();
                    continue;
                }
                if (b->cpu.cyc >= st) {
                    end_now();
                    continue;
                }
                b->cpu.due = st;
                return;
            }
        }
        void slice_end() override {
            if (b->aborted) {
                b->aborted = false;
                const auto ls = stv::snd::cyc_time(b->cpu.cyc);
                if (ls < target) target = max_atto(ls, base);
            }
            end_now();
            start_slice();
        }
        stv::snd::Atto next_foreign_timer() const override { return limit; }
        void advance(const stv::snd::Atto& to) {
            if (!(base < to)) return;
            limit = to;
            b->stop = false;
            start_slice();
            if (!b->stop) b->run();
        }
    };

    std::unique_ptr<stv::snd::Board> board_;
    std::unique_ptr<stv::snd::Mixer> mixer_;
    Host host_{};
    float gain_{kDefaultGain};
    float dc_{};
    bool dc_set_{};
    bool running_{};
};

}  // namespace stv::host
