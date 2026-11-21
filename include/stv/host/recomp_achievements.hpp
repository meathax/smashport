#pragma once

#include "stv/gsp/scene.hpp"

#ifdef STV_RECOMP_ACHIEVEMENTS
#include <rl_achievements.h>
#endif

namespace stv::host {
class RecompAchievements {
public:
    void start(bool automated) {
#ifdef STV_RECOMP_ACHIEVEMENTS
        if (!automated) {
            started_ = true;
            enabled_ = rl_ach_init("", nullptr, nullptr) != 0;
        }
#endif
    }
    ~RecompAchievements() {
#ifdef STV_RECOMP_ACHIEVEMENTS
        if (started_) rl_ach_shutdown();
#endif
    }

    void frame(const gsp::Machine& machine, bool online) {
#ifdef STV_RECOMP_ACHIEVEMENTS
        if (!enabled_) return;
        rl_ach_frame();
        const auto scene = gsp::game_scene_state(machine);
        if (machine.frame_number < last_frame_ || online) run_frames_ = 0;
        last_frame_ = machine.frame_number;
        if (online) return;
        if (scene == 0x40 && previous_scene_ != scene) rl_ach_casual("operator_menu");
        if (scene == 0x30 || scene == 0x40) run_frames_ = 0;
        if (!run_frames_ && scene == 0x18) {
            rl_ach_event("start_game");
            rl_ach_presence(machine.enhanced ? "Custom Widescreen" : "Arcade Original");
            run_frames_ = 1;
            completed_ = false;
        } else if (run_frames_ && !completed_) {
            ++run_frames_;
            // Retail GRATS displays GRAT after the final circuit. Attract can
            // show fixed pages as well, so require a live, observed run.
            if (scene == 0x10 && gsp::scene_background(machine) == 0xffea54f0u) {
                rl_ach_event("complete_game");
                if (run_frames_ <= 0x7fffffff)
                    rl_ach_lboard(machine.enhanced ? 2 : 1, static_cast<int32_t>(run_frames_));
                completed_ = true;
            }
        }
        if (scene == 0x30 && previous_scene_ != scene) rl_ach_presence("Attract mode");
        previous_scene_ = scene;
#endif
    }
private:
#ifdef STV_RECOMP_ACHIEVEMENTS
    bool started_ = false, enabled_ = false, completed_ = false;
    gsp::u64 last_frame_ = 0, run_frames_ = 0;
    gsp::u16 previous_scene_ = 0;
#endif
};
} // namespace stv::host
