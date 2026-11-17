#pragma once
// Scene ownership is part of the native L8 enhancement policy. The reference
// program always retains its original geometry. Boot/operator/attract pages in
// the enhanced profile also use the original 410-column simulation and image.
#include "stv/gsp/machine.hpp"

namespace stv::gsp {

constexpr u32 L8_GAME_STATE = 0x010865a0u;
constexpr u32 L8_BACKGROUND_MODULES = 0x010dd4e0u;
constexpr u32 L8_NATIVE_VIEW_WIDTH = VIS_MAXX - VIS_MINX + 1;

// Retail bindings, verified against the decoded imported L8 and the generated
// instructions (source annotations alone are not sufficient):
// FFE95EB0/FFE95ED0: ROBOATT ATSTRT writes 30h (attract).
// FFEA31D0/FFEA31F0: ROBOATT GAMESTR writes 10h (live game).
// FFE74CC0/FFE74CE0: WAVES initialisation writes 18h (live intro).
// FFE91850/FFE91870: WAVES game-over writes 50h (live world/continue).
// FFEBAD30/FFEBAD50: TEST diagnostics writes 40h.
inline u16 game_scene_state(const Machine& m) {
    return m.ram[(L8_GAME_STATE - 0x01000000u) >> 4];
}

inline u32 scene_background(const Machine& m) {
    constexpr u32 index = (L8_BACKGROUND_MODULES - 0x01000000u) >> 4;
    return u32(m.ram[index]) | (u32(m.ram[index + 1]) << 16);
}

// World-root selection must work while the previous fixed page is still in
// BAKMODS. The new world pointer is published only after this root is consumed.
inline bool enhanced_world_root(const Machine& m) {
    if (!m.enhanced) return false;
    switch (game_scene_state(m)) {
    case 0x10:
    case 0x18:
    case 0x50:
        return true;
    default:
        return false;
    }
}

inline bool enhanced_scene(const Machine& m) {
    if (!enhanced_world_root(m)) return false;
    // GRATS is also called directly after circuit three while GAMSTATE is10h.
    // Fixed pages retain their original art/text/raster as one intact assembly.
    // Their ownership cannot be inferred from the game-over state alone.
    switch (scene_background(m)) {
        case 0xffea54f0u: // GRAT
        case 0xffea5550u: // LOGO
        case 0xffea55b0u: // LOGO2 / credits
        case 0xffea5610u: // CHAMPS
        case 0xffea5670u: // INITL / initials
            return false;
    default:
        return true;
    }
}

// The widened TV studio owns contestants placed and scripted while a native
// page (the bonus tally) is still on screen. Attract mode and diagnostics keep
// their native studio; reference_l8 is never affected.
inline bool enhanced_studio(const Machine& m) {
    const u16 state = game_scene_state(m);
    return m.enhanced && state != 0x30 && state != 0x40;
}

// Timed room-entry scripts are authored at arcade velocity. Their horizontal
// duration already compensates for the wider room; boosting velocity as well
// overshoots the landing point (and also breaks unchanged vertical scripts).
inline bool enhanced_player_motion(const Machine& m) {
    return enhanced_scene(m) && game_scene_state(m) != 0x18;
}

// Invoked immediately after explicitly verified retail GAMSTATE stores by the
// translator. The live right wall needs the added width and camera offset.
// After SET_WAVE settles the camera, that same four-pixel offset also applies
// to the left clip: otherwise entering enemies show in the gutter before the
// door. ININTRO keeps the studio/scrolling aperture, including its border art.
// Update only known full-screen rectangles; custom windows keep their owner.
// No guest cycles, I/O, original ROM bytes or reference-profile state change.
inline void sync_scene_bounds(Machine& m) {
    if (!m.enhanced || !m.vis_extra) return;
    constexpr u32 index = (0x010002c0u - 0x01000000u) >> 4;
    constexpr u32 top_index = (0x010002a0u - 0x01000000u) >> 4;
    constexpr u32 native_top = 0x00010005u, arena_top = 0x00010009u;
    constexpr u32 native = 0x00fc018bu;
    const u32 wide = native + m.vis_extra + 4u;
    const u32 top = u32(m.ram[top_index]) | (u32(m.ram[top_index+1]) << 16);
    const u32 current = u32(m.ram[index]) | (u32(m.ram[index+1]) << 16);
    const bool full = (top == native_top && (current == native || current == wide)) ||
                      (top == arena_top && current == wide);
    if (!full) return;
    const bool live = enhanced_scene(m);
    const u32 next_top = live && game_scene_state(m) != 0x18 ? arena_top : native_top;
    const u32 next = live ? wide : native;
    m.ram[top_index] = u16(next_top);
    m.ram[top_index+1] = u16(next_top >> 16);
    m.ram[index] = u16(next);
    m.ram[index+1] = u16(next >> 16);
}

// The scanout owner latches this at the first visible line. The host must use
// the width belonging to the completed image, not a newer simulation state.
inline u32 scene_view_width(const Machine& m) {
    return L8_NATIVE_VIEW_WIDTH + m.frame_vis_extra;
}

} // namespace stv::gsp
