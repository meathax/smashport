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
    int crt_beam = 0;       // 0 off, 1 rolling scan, 2 global refresh
    int beam_brightness = 0; // 0 automatic, 1 crisp, 2 balanced, 3 bright
    bool beam_anti_retention = true;
    bool enhance = false;
    bool particles = true, blood = true, debris = true, smoke = true, fire = true, lighting = true;
    bool hardware_rt = false;
    bool ray_tracing = false, voxels = false;
    bool shockwaves = false, tracers = false, brass = false, muzzle_flash = true, room_lights = true;
    bool persistent_gore = false, materials = false, destruction = false, weapon_impacts = false, fluid_smoke = false;
    bool explosive_debris = true;
    bool smooth_motion = true;
    int run_ahead = 0; // offline speculative video frames, 0..3
    bool fullscreen = false;
    bool vsync = false;    // existing QPC/audio pacing remains the default
    bool sync_presentation() const noexcept { return vsync || smooth_motion || crt_beam != 0; }
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

enum class SettingsLoad { Defaults, Loaded, Invalid };
std::string pc_player_name(std::string_view text);
SettingsLoad load_pc_settings(const std::filesystem::path& file, PcSettings& settings, std::string& error);
bool save_pc_settings(const std::filesystem::path& file, const PcSettings& settings, std::string& error);

} // namespace stv::host
