#include "stv/host/pc_settings.hpp"
#include <fstream>
#include <iostream>
using namespace stv::host;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    const auto file = directory / "settings.cfg";
    std::filesystem::remove(file);
    int failures = 0, checks = 0;
    auto check = [&](bool value, const char* what) { ++checks; if (!value) { ++failures; std::cerr << what << '\n'; } };
    std::string error;
    PcSettings s, defaults;
    s.volume = 4;
    check(load_pc_settings(file, s, error) == SettingsLoad::Defaults && s == defaults && s.smooth_motion, "missing file defaults to Smooth Motion");
    s.ray_tracing = true; s.voxels = true; s.shockwaves = s.tracers = s.brass = true;
    s.room_lights = false; s.enhance = true; s.blood = false; s.smoke = false; s.lighting = false;
    s.persistent_gore=s.materials=s.destruction=s.weapon_impacts=s.fluid_smoke=true;
    s.explosive_debris=false;
    s.hardware_rt = true; s.run_ahead = 3; s.smooth_motion = false;
    s.volume = 35; s.filter = 7; s.glow = 2; s.sharpen = 1; s.curvature = 3; s.vsync = true; s.fullscreen = true; s.show_fps = true;
    s.swap_pads = true; s.muted = true; s.window_scale = 4; s.scaling = 2; s.player_name = "MIKE-2";
    check(save_pc_settings(file, s, error), "save preferences");
    PcSettings roundtrip;
    check(load_pc_settings(file, roundtrip, error) == SettingsLoad::Loaded && s == roundtrip, "all preferences survive restart");
    s.volume = 100; s.muted = false;
    check(save_pc_settings(file, s, error), "atomic replacement of existing preferences");
    check(load_pc_settings(file, roundtrip, error) == SettingsLoad::Loaded && roundtrip == s, "replacement readback");
    check(pc_player_name("  mike!_two ") == "MIKETWO" && pc_player_name("abcdefghijk") == "ABCDEFGHIJ" &&
          pc_player_name("!  ") == "PLAYER", "bounded controller-safe names");
    const std::string malformed[] = {
        "version=1\nray_tracing=2\n", "version=1\nvoxels=-1\n", "version=1\nenhance=2\n", "version=1\nblood=-1\n", "version=2\nvolume=10\n", "version=1\nvolume=-1\n", "version=1\nvolume=101\n",
        "version=1\nrun_ahead=4\n", "version=1\nsmooth_motion=-1\n",
        "version=1\nfilter=8\n", "version=1\nglow=4\n", "version=1\nsharpen=-1\n", "version=1\ncurvature=4\n", "version=1\nscaling=3\n", "version=1\nfullscreen=2\n",
        "version=1\nvolume=25\nvolume=30\n", "version=1\nvolume=42junk\n", "volume=50\n",
        std::string("version=1\n\0muted=1\n", 20), std::string(4097, 'x')
    };
    for (const auto& text : malformed) {
        { std::ofstream out(file, std::ios::binary | std::ios::trunc); out.write(text.data(), std::streamsize(text.size())); }
        roundtrip = s;
        check(load_pc_settings(file, roundtrip, error) == SettingsLoad::Invalid && roundtrip == defaults && !error.empty(),
              "malformed settings fail as a unit without partial preferences");
    }
    // Write literal CRLF bytes; Windows text mode would expand them to CRCRLF.
    { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << "# local preferences\r\nversion=1\r\nname=  alex-2! \r\nvolume=0\r\n"; }
    check(load_pc_settings(file, roundtrip, error) == SettingsLoad::Loaded && roundtrip.player_name == "ALEX-2" && roundtrip.volume == 0 && roundtrip.smooth_motion,
          "CRLF/comments/name normalization/mute level");
    { std::ofstream out(file); out << "version=1\nstudio=1\nroom_lights=1\n"; }
    check(load_pc_settings(file, roundtrip, error) == SettingsLoad::Loaded && roundtrip.room_lights,
          "retired spotlight preference preserves room lighting");
    { std::ofstream out(file); out << "version=1\nvolume=0\n"; }
    s.volume = 101;
    check(!save_pc_settings(file, s, error), "invalid save rejected");
    check(load_pc_settings(file, roundtrip, error) == SettingsLoad::Loaded && roundtrip.volume == 0, "rejected save preserves prior file");
    check(!save_pc_settings(directory / "missing" / "settings.cfg", defaults, error), "unwritable path reports failure");
    std::cout << checks << " settings checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
