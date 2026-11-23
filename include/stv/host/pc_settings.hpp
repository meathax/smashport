#pragma once
// PC-only preferences inspired by TMNT-port's OPTIONS persistence. They never
// replace game CMOS or participate in the original machine's state.
#include <filesystem>
#include <string>
#include <string_view>

namespace stv::host {

struct PcSettings {
    int window_scale = 0;  // 0 = automatic, 1..6 = requested window size
    int scaling = 1;       // matches ScaleMode: IntegerExact, IntegerFill, Smooth
    int filter = 0;        // matches VideoFilterMode: Nearest, Linear, Sharp, CRT
    int glow = 0, sharpen = 0, curvature = 0; // optional GPU effects: off/low/medium/high
    bool enhance = false;
    bool particles = true, blood = true, debris = true, smoke = true, fire = true, lighting = true;
    bool hardware_rt = false;
    bool ray_tracing = false, voxels = false;
    bool shockwaves = false, tracers = false, brass = false, muzzle_flash = true, room_lights = true;
    int room_cinema = 2; // 0 off, 1 low, 2 medium, 3 high
    bool persistent_gore = false, materials = false, destruction = false, weapon_impacts = false, fluid_smoke = false;
    bool explosive_debris = true;
    int atmosphere = 2, sprite_rim = 1, contact_shadows = 2, wet_materials = 2;
    int heat_distortion = 1, emissive_spill = 2, cinematic_grade = 2;
    // Cosmetic combat feedback. Levels are 0 = off, 1 = low, 2 = medium, 3 = high.
    int combat_feedback = 1;
    int camera_shake = 1, zoom_punch = 1, hit_flash = 0, edge_effects = 0;
    int weapon_signatures = 1, temporal_trails = 0;
    bool reduce_shake = false, reduce_flashing = false, reduce_motion = false;
    bool smooth_motion = true;
    int run_ahead = 0; // offline speculative video frames, 0..3
    bool fullscreen = false;
    bool vsync = false;    // existing QPC/audio pacing remains the default
    bool sync_presentation() const noexcept { return vsync || smooth_motion; }
    bool show_fps = false;
    int volume = 100;
    bool muted = false;
    // Sound enhancements (output only; the retail board stream is unchanged).
    bool extended_sound = true;   // extra sound boards: nothing is cut off
    int reverb = 1;               // 0 off, 1 studio, 2 arena
    bool audio_wide = true;       // stereo widening
    bool audio_hifi = true;       // analog-style smoothing, bass and limiter
    bool swap_pads = false;
    std::string player_name = "PLAYER";
    bool operator==(const PcSettings&) const = default;
};

inline constexpr unsigned HardwareRayTracingEffectMask = 1u << 21;
inline constexpr unsigned RoomCinemaEffectMask = 1u << 29;

inline unsigned environment_levels(const PcSettings& settings) {
    return unsigned(settings.atmosphere) |
           (unsigned(settings.sprite_rim) << 2) |
           (unsigned(settings.contact_shadows) << 4) |
           (unsigned(settings.wet_materials) << 6) |
           (unsigned(settings.heat_distortion) << 8) |
           (unsigned(settings.emissive_spill) << 10) |
           (unsigned(settings.cinematic_grade) << 12);
}

inline unsigned environment_effect_mask(const PcSettings& settings) {
    return (settings.atmosphere ? 1u << 22 : 0u) |
           (settings.sprite_rim ? 1u << 23 : 0u) |
           (settings.contact_shadows ? 1u << 24 : 0u) |
           (settings.wet_materials ? 1u << 25 : 0u) |
           (settings.heat_distortion ? 1u << 26 : 0u) |
           (settings.emissive_spill ? 1u << 27 : 0u) |
           (settings.cinematic_grade ? 1u << 28 : 0u);
}

inline void enable_all_visual_effects(PcSettings& settings) {
    settings.enhance = true;
    settings.particles = settings.blood = settings.debris = true;
    settings.smoke = settings.fire = settings.lighting = true;
    settings.voxels = settings.shockwaves = true;
    settings.tracers = false;
    settings.brass = settings.muzzle_flash = settings.room_lights = true;
    settings.persistent_gore = settings.materials = settings.destruction = true;
    settings.weapon_impacts = settings.fluid_smoke = settings.explosive_debris = true;
    settings.atmosphere = settings.sprite_rim = settings.contact_shadows = 3;
    settings.wet_materials = settings.heat_distortion = settings.emissive_spill = 3;
    settings.cinematic_grade = settings.room_cinema = 3;
    settings.combat_feedback = settings.camera_shake = settings.zoom_punch = 3;
    settings.hit_flash = settings.edge_effects = settings.weapon_signatures = 3;
    settings.temporal_trails = 0;
}

enum class SettingsLoad { Defaults, Loaded, Invalid };
std::string pc_player_name(std::string_view text);
SettingsLoad load_pc_settings(const std::filesystem::path& file, PcSettings& settings, std::string& error);
bool save_pc_settings(const std::filesystem::path& file, const PcSettings& settings, std::string& error);

} // namespace stv::host
