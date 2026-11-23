// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Sebastian Toyos
// Adapted settings/name handling from cheviman/tmnt-port ports/sdl2/main.c,
// commit 401c3694e5f89fa5844c384cfd82512803202cf9. Smash adds a versioned,
// bounded parser and atomic replacement; only local PC preferences are saved.
// Full upstream notice: third_party/tmnt-port/LICENSE.
#include "stv/host/pc_settings.hpp"
#include <charconv>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace stv::host {

std::string pc_player_name(std::string_view text) {
    std::string result;
    for (char c : text) {
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ' ') {
            if (result.empty() && c == ' ') continue;
            result.push_back(c);
            if (result.size() == 10) break;
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result.empty() ? "PLAYER" : result;
}

namespace {
bool valid(const PcSettings& s) {
    return s.run_ahead >= 0 && s.run_ahead <= 3 && s.window_scale >= 0 && s.window_scale <= 6 && s.scaling >= 0 && s.scaling <= 2 &&
           s.filter >= 0 && s.filter <= 7 && s.glow >= 0 && s.glow <= 3 && s.sharpen >= 0 && s.sharpen <= 3 && s.curvature >= 0 && s.curvature <= 3 &&
           s.room_cinema >= 0 && s.room_cinema <= 3 &&
           s.atmosphere >= 0 && s.atmosphere <= 3 && s.sprite_rim >= 0 && s.sprite_rim <= 3 &&
           s.contact_shadows >= 0 && s.contact_shadows <= 3 && s.wet_materials >= 0 && s.wet_materials <= 3 &&
           s.heat_distortion >= 0 && s.heat_distortion <= 3 && s.emissive_spill >= 0 && s.emissive_spill <= 3 &&
           s.cinematic_grade >= 0 && s.cinematic_grade <= 3 &&
           s.room_cinema >= 0 && s.room_cinema <= 3 &&
           s.atmosphere >= 0 && s.atmosphere <= 3 && s.sprite_rim >= 0 && s.sprite_rim <= 3 &&
           s.contact_shadows >= 0 && s.contact_shadows <= 3 && s.wet_materials >= 0 && s.wet_materials <= 3 &&
           s.heat_distortion >= 0 && s.heat_distortion <= 3 && s.emissive_spill >= 0 && s.emissive_spill <= 3 &&
           s.cinematic_grade >= 0 && s.cinematic_grade <= 3 &&
           s.combat_feedback >= 0 && s.combat_feedback <= 3 &&
           s.camera_shake >= 0 && s.camera_shake <= 3 && s.zoom_punch >= 0 && s.zoom_punch <= 3 &&
           s.hit_flash >= 0 && s.hit_flash <= 3 && s.edge_effects >= 0 && s.edge_effects <= 3 &&
           s.weapon_signatures >= 0 && s.weapon_signatures <= 3 && s.temporal_trails >= 0 && s.temporal_trails <= 3 &&
           s.volume >= 0 && s.volume <= 100 && s.reverb >= 0 && s.reverb <= 2 &&
           s.player_name == pc_player_name(s.player_name);
}
bool number(std::string_view value, int& result) {
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
}

SettingsLoad load_pc_settings(const std::filesystem::path& file, PcSettings& settings, std::string& error) {
    settings = PcSettings{};
    error.clear();
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (!exists && !ec) return SettingsLoad::Defaults;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > 4096) { error = "PC settings could not be read (maximum 4096 bytes)"; return SettingsLoad::Invalid; }
    std::ifstream input(file, std::ios::binary);
    if (!input) { error = "PC settings could not be opened"; return SettingsLoad::Invalid; }
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.size() != size || text.find('\0') != std::string::npos) {
        error = "PC settings are incomplete or contain invalid text"; return SettingsLoad::Invalid;
    }
    PcSettings candidate;
    std::set<std::string> seen;
    std::istringstream lines(text);
    std::string line;
    bool version = false, hasVisualOptions = false, hasRoomCinema = false;
    int versionNumber = 0;
    auto invalid = [&] { error = "Invalid PC setting: " + line.substr(0, 80); return SettingsLoad::Invalid; };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        if (line.size() > 160) return invalid();
        const auto equals = line.find('=');
        if (equals == std::string::npos) return invalid();
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        if (!seen.insert(key).second) return invalid();
        if (key == "name") { candidate.player_name = pc_player_name(value); continue; }
        int n = 0;
        if (!number(value, n)) return invalid();
        if (key == "version") { if (n != 1 && n != 2 && n != 3) return invalid(); version = true; versionNumber = n; }
        else if (key == "window_scale") candidate.window_scale = n;
        else if (key == "scaling") candidate.scaling = n;
        else if (key == "filter") candidate.filter = n;
        else if (key == "glow") candidate.glow = n;
        else if (key == "sharpen") candidate.sharpen = n;
        else if (key == "curvature") candidate.curvature = n;
        else if (key == "atmosphere") candidate.atmosphere = n;
        else if (key == "sprite_rim") candidate.sprite_rim = n;
        else if (key == "contact_shadows") candidate.contact_shadows = n;
        else if (key == "wet_materials") candidate.wet_materials = n;
        else if (key == "heat_distortion") candidate.heat_distortion = n;
        else if (key == "emissive_spill") candidate.emissive_spill = n;
        else if (key == "cinematic_grade") candidate.cinematic_grade = n;
        else if (key == "volume") candidate.volume = n;
        else if (key == "hardware_rt" || key == "explosive_debris" || key == "enhance" || key == "particles" || key == "blood" || key == "debris" || key == "smoke" || key == "fire" || key == "lighting" || key == "ray_tracing" || key == "voxels" || key == "shockwaves" || key == "tracers" || key == "brass" || key == "muzzle_flash" || key == "room_lights" || key == "persistent_gore" || key == "materials" || key == "destruction" || key == "studio" || key == "weapon_impacts" || key == "fluid_smoke") {
            if (n != 0 && n != 1) return invalid();
            if (key == "enhance") candidate.enhance = n != 0;
            if (key == "particles") candidate.particles = n != 0;
            if (key == "blood") candidate.blood = n != 0;
            if (key == "debris") candidate.debris = n != 0;
            if (key == "smoke") candidate.smoke = n != 0;
            if (key == "fire") candidate.fire = n != 0;
            if (key == "lighting") candidate.lighting = n != 0;
            if (key == "hardware_rt") candidate.hardware_rt = n != 0;
            if (key == "ray_tracing") candidate.ray_tracing = n != 0;
            if (key == "shockwaves") candidate.shockwaves = n != 0;
            if (key == "tracers") candidate.tracers = n != 0;
            if (key == "persistent_gore") { candidate.persistent_gore = n != 0; hasVisualOptions = true; }
            if (key == "materials") { candidate.materials = n != 0; hasVisualOptions = true; }
            if (key == "destruction") { candidate.destruction = n != 0; hasVisualOptions = true; }
            if (key == "studio") hasVisualOptions = true; // retired preference
            if (key == "weapon_impacts") { candidate.weapon_impacts = n != 0; hasVisualOptions = true; }
            if (key == "fluid_smoke") { candidate.fluid_smoke = n != 0; hasVisualOptions = true; }
            if (key == "explosive_debris") candidate.explosive_debris = n != 0;
            if (key == "room_lights") candidate.room_lights = n != 0;
            if (key == "muzzle_flash") candidate.muzzle_flash = n != 0;
            if (key == "brass") candidate.brass = n != 0;
            if (key == "voxels") candidate.voxels = n != 0;
        }
        else if (key == "run_ahead") candidate.run_ahead = n;
        else if (key == "room_cinema") { candidate.room_cinema = n; hasRoomCinema = true; }
        else if (key == "combat_feedback") candidate.combat_feedback = n;
        else if (key == "camera_shake") candidate.camera_shake = n;
        else if (key == "zoom_punch") candidate.zoom_punch = n;
        else if (key == "hit_flash") candidate.hit_flash = n;
        else if (key == "edge_effects") candidate.edge_effects = n;
        else if (key == "weapon_signatures") candidate.weapon_signatures = n;
        else if (key == "temporal_trails") candidate.temporal_trails = n;
        else if (key == "reverb") candidate.reverb = n;
        else if (key == "reduce_shake" || key == "reduce_flashing" || key == "reduce_motion") {
            if (n != 0 && n != 1) return invalid();
            if (key == "reduce_shake") candidate.reduce_shake = n != 0;
            if (key == "reduce_flashing") candidate.reduce_flashing = n != 0;
            if (key == "reduce_motion") candidate.reduce_motion = n != 0;
        }
        else if (key == "dash_enabled") {
            // Accept the retired setting from existing player profiles.
            if (n != 0 && n != 1) return invalid();
        }
        else if (key == "extended_sound" || key == "audio_wide" || key == "audio_hifi") {
            if (n != 0 && n != 1) return invalid();
            if (key == "extended_sound") candidate.extended_sound = n != 0;
            if (key == "audio_wide") candidate.audio_wide = n != 0;
            if (key == "audio_hifi") candidate.audio_hifi = n != 0;
        }
        else if (key == "smooth_motion" || key == "fullscreen" || key == "vsync" || key == "fps" || key == "muted" || key == "swap_pads") {
            if (n != 0 && n != 1) return invalid();
            if (key == "smooth_motion") candidate.smooth_motion = n != 0;
            if (key == "fullscreen") candidate.fullscreen = n != 0;
            if (key == "vsync") candidate.vsync = n != 0;
            if (key == "fps") candidate.show_fps = n != 0;
            if (key == "muted") candidate.muted = n != 0;
            if (key == "swap_pads") candidate.swap_pads = n != 0;
        } else return invalid();
    }
    auto has = [&](std::string_view key) { return seen.find(std::string(key)) != seen.end(); };
    const bool hasIntegratedSettings =
        has("room_cinema") && has("atmosphere") && has("sprite_rim") &&
        has("contact_shadows") && has("wet_materials") && has("heat_distortion") &&
        has("emissive_spill") && has("cinematic_grade") && has("combat_feedback") &&
        has("camera_shake") && has("zoom_punch") && has("hit_flash") &&
        has("edge_effects") && has("weapon_signatures") && has("temporal_trails") &&
        has("reduce_shake") && has("reduce_flashing") && has("reduce_motion");
    if (!version || (versionNumber >= 2 && !hasRoomCinema) ||
        (versionNumber == 3 && !hasIntegratedSettings) || !valid(candidate)) {
        error = "PC settings version or range is invalid"; return SettingsLoad::Invalid;
    }
    if (versionNumber == 1 && !hasRoomCinema) candidate.room_cinema = 0;
    if (!hasVisualOptions && candidate.enhance && candidate.shockwaves && candidate.tracers && candidate.brass && candidate.voxels && candidate.ray_tracing && candidate.lighting && candidate.particles && candidate.blood && candidate.smoke && candidate.fire && candidate.debris && candidate.room_lights && candidate.muzzle_flash)
        candidate.persistent_gore = candidate.materials = candidate.destruction = candidate.weapon_impacts = candidate.fluid_smoke = true;
    settings = candidate;
    return SettingsLoad::Loaded;
}

bool save_pc_settings(const std::filesystem::path& file, const PcSettings& settings, std::string& error) {
    error.clear();
    if (!valid(settings)) { error = "PC settings contain an invalid value"; return false; }
    const std::filesystem::path temporary = file.wstring() + L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { error = "PC settings could not be saved"; return false; }
    output << "version=3\nwindow_scale=" << settings.window_scale << "\nscaling=" << settings.scaling
           << "\nglow=" << settings.glow << "\nsharpen=" << settings.sharpen << "\ncurvature=" << settings.curvature
           << "\nenhance=" << int(settings.enhance)
           << "\nparticles=" << int(settings.particles)
           << "\nblood=" << int(settings.blood)
           << "\ndebris=" << int(settings.debris)
           << "\nsmoke=" << int(settings.smoke)
           << "\nfire=" << int(settings.fire)
           << "\nlighting=" << int(settings.lighting)
           << "\nhardware_rt=" << int(settings.hardware_rt)
           << "\nray_tracing=" << int(settings.ray_tracing) << "\nvoxels=" << int(settings.voxels)
           << "\nshockwaves=" << int(settings.shockwaves) << "\ntracers=" << int(settings.tracers) << "\nbrass=" << int(settings.brass) << "\nmuzzle_flash=" << int(settings.muzzle_flash) << "\nroom_lights=" << int(settings.room_lights)
           << "\npersistent_gore=" << int(settings.persistent_gore)
           << "\nexplosive_debris=" << int(settings.explosive_debris)
           << "\nmaterials=" << int(settings.materials)
           << "\ndestruction=" << int(settings.destruction)
           << "\nweapon_impacts=" << int(settings.weapon_impacts)
           << "\nfluid_smoke=" << int(settings.fluid_smoke)
           << "\natmosphere=" << settings.atmosphere
           << "\nsprite_rim=" << settings.sprite_rim
           << "\ncontact_shadows=" << settings.contact_shadows
           << "\nwet_materials=" << settings.wet_materials
           << "\nheat_distortion=" << settings.heat_distortion
           << "\nemissive_spill=" << settings.emissive_spill
           << "\ncinematic_grade=" << settings.cinematic_grade
           << "\nroom_cinema=" << settings.room_cinema
           << "\ncombat_feedback=" << settings.combat_feedback
           << "\ncamera_shake=" << settings.camera_shake
           << "\nzoom_punch=" << settings.zoom_punch
           << "\nhit_flash=" << settings.hit_flash
           << "\nedge_effects=" << settings.edge_effects
           << "\nweapon_signatures=" << settings.weapon_signatures
           << "\ntemporal_trails=" << settings.temporal_trails
           << "\nreduce_shake=" << int(settings.reduce_shake)
           << "\nreduce_flashing=" << int(settings.reduce_flashing)
           << "\nreduce_motion=" << int(settings.reduce_motion)
           << "\nfilter=" << settings.filter << "\nfullscreen=" << int(settings.fullscreen)
           << "\nsmooth_motion=" << int(settings.smooth_motion) << "\nrun_ahead=" << settings.run_ahead
           << "\nvsync=" << int(settings.vsync) << "\nfps=" << int(settings.show_fps)
           << "\nvolume=" << settings.volume << "\nmuted=" << int(settings.muted)
           << "\nextended_sound=" << int(settings.extended_sound) << "\nreverb=" << settings.reverb
           << "\naudio_wide=" << int(settings.audio_wide) << "\naudio_hifi=" << int(settings.audio_hifi)
           << "\nswap_pads=" << int(settings.swap_pads) << "\nname=" << settings.player_name << '\n';
    output.flush();
    bool ok = bool(output);
    output.close();
    ok = ok && !output.fail();
    if (!ok) { error = "PC settings write failed"; return false; }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "PC settings replacement failed"; return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    if (ec) { error = "PC settings replacement failed: " + ec.message(); return false; }
#endif
    return true;
}

} // namespace stv::host
