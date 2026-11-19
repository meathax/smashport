#pragma once
#include "stv/base.hpp"
namespace stv::source_state {
// Native continuation identifiers. These are NOT arcade program counters.
enum State:u32 {
    boot=1,torso_init,torso_top,torso_resume,torso_frames,legs_top,legs_after_shoot,
    bullet_init,bullet_loop,bullet_frames,hulk_init,hulk_loop,
    death_init,death_frames,collision_sleep,collision_scan,
    player_spin_wait,player_spin_frames,player_fall_frames,player_all_die,
    player_respawn_wait,player_buyin_wait,legs_spin_wait,legs_spin_frames,
    legs_dead_wait,cheer_wait,cheer_emit,blood_wait_extra,blood_wait,
    blood_init,blood_frames,entry_init,entry_loop,entry_sleep,ring_init,
    ring_loop,ring_blink_init,ring_blink_on,ring_blink_off,door_step,
    // Distinct native tokens for source-equivalent Hulk sleep return addresses.
    // These are not CPU PCs: evidence maps T4/CONT1 to the noted L8 PWAKEs.
    hulk_resume_after_t4,       // HULKS.ASM T4: SLEEP 3 (MAME PWAKE 0xFFE3E580)
    hulk_resume_after_way,      // WAY -> CONT1: SLEEP 3 (MAME PWAKE 0xFFE3DD50)
    hulk_resume_after_retry,    // changed seek / SLEEP 1
    hulk_resume_after_get_run,  // GET_RUN / SLEEP 1
    hulk_resume_after_franim   // UTIL.ASM FRANIM sleep return
};
}
