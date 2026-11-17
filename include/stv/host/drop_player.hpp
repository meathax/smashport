#pragma once
// Online drop-in / drop-out on the retail program.
//
// Drop-out: a departing player's character dies as if hit. The torso process
// watches its DEAD word (+0x208, unaligned 16 bits) and, once raised, runs the
// original death animation and takes the life (ROBO.ASM KILL_PLYR stores the
// killer's OID there). Setting the life count to one first makes this the last
// life, so the character leaves play exactly like an arcade player whose game
// ended. Only the host's machine changes, after the guest has gone.
//
// Drop-in: a joining player inserts a coin and presses START, as in the arcade.
#include "stv/gsp/machine.hpp"
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"

namespace stv::host {
inline constexpr gsp::u32 kPlayerProcesses = 0x010de2b0;              // PLYRPRCS: P1, P2 torso processes
inline constexpr gsp::u32 kPlayerLives[2] = {0x01086360u, 0x01086520u}; // P1DATA/P2DATA lives

inline unsigned player_lives(const gsp::Machine& m, int player) {
    return m.ram[(kPlayerLives[player] - 0x01000000u) >> 4];
}
inline gsp::u32 player_torso(gsp::Machine& m, int player) {
    return gsp::rlong(m, kPlayerProcesses + gsp::u32(player) * 32);
}
// In play: a live torso process, lives left, and play (not attract) running.
inline bool player_active(gsp::Machine& m, int player) {
    const auto scene = gsp::game_scene_state(m);
    return (scene == 0x10 || scene == 0x18) && player_lives(m, player) > 0 && player_torso(m, player) != 0;
}
inline bool drop_player(gsp::Machine& m, int player) {
    if (!player_active(m, player)) return false;
    const gsp::u32 torso = player_torso(m, player);
    if (gsp::rfield(m, torso + 0x208, 16) != 0) {          // already dying
        m.ram[(kPlayerLives[player] - 0x01000000u) >> 4] = 1;
        return true;
    }
    m.ram[(kPlayerLives[player] - 0x01000000u) >> 4] = 1;
    gsp::wfield(m, torso + 0x208, 0x8301u, 16);            // a grunt's OID: an ordinary hit death
    return true;
}
// Arcade-style join, by frame since the join began: coin, release, START, release.
struct JoinPulse { bool coin{}, start{}; };
inline JoinPulse join_pulse(unsigned frame) {
    return {frame < 6, frame >= 12 && frame < 18};
}
}
