#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "stv/game/cosched.hpp"
#include "stv/gsp/rom_import.hpp"
#include "stv/host/recomp_achievements.hpp"
#include "stv/host/sdl_pads.hpp"
#include "stv/snd/audio.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>

namespace {
using namespace stv::gsp;
namespace fs = std::filesystem;
constexpr int height = VIS_MAXY - VIS_MINY + 1;
constexpr int max_width = VIS_MAXX - VIS_MINX + 1 + 137;

struct Options {
    fs::path data, rom;
    bool wide = true;
    unsigned frames = 0;
};

bool parse(Options& options, int argc, char** argv) {
    if (const auto* data = std::getenv("RECOMPLAUNCHER_DATA")) options.data = data;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--classic") options.wide = false;
        else if (arg == "--widescreen") options.wide = true;
        else if (arg.starts_with("--data=")) options.data = arg.substr(7);
        else if (arg.starts_with("--rom=")) options.rom = arg.substr(6);
        else if (arg.starts_with("--frames=")) {
            const auto value = arg.substr(9);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.frames);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !options.frames) return false;
        } else return false;
    }
    return !options.frames || !options.rom.empty();
}

bool select_rom(Options& options) {
    struct Selection {
        std::string path, error;
        std::atomic<bool> done = false;
    } selection;
    static constexpr SDL_DialogFileFilter filters[] = {{"Retail L8 ROM archive", "zip"}};
    SDL_ShowOpenFileDialog([](void* opaque, const char* const* files, int) {
        auto& result = *static_cast<Selection*>(opaque);
        if (!files) result.error = SDL_GetError();
        else if (*files) result.path = *files;
        result.done.store(true, std::memory_order_release);
    }, &selection, nullptr, filters, 1, nullptr, false);
    while (!selection.done.load(std::memory_order_acquire)) {
        SDL_PumpEvents();
        SDL_Delay(16);
    }
    if (!selection.error.empty()) throw std::runtime_error(selection.error);
    options.rom = selection.path;
    return !options.rom.empty();
}

u16 directions(int x, int y) {
    constexpr int threshold = 9000;
    return u16((y > threshold ? 1 : y < -threshold ? 2 : 0) |
               (x < -threshold ? 4 : x > threshold ? 8 : 0));
}

struct Controls {
    stv::host::PadRouter router;
    stv::host::PadBindings bindings;

    void sample(Machine& m, const bool* keyboard, stv::host::SdlPads& pads,
                SDL_Window* window, bool& paused, bool& quit) {
        using namespace stv::host;
        const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const auto key = [&](SDL_Scancode code) { return focused && keyboard[code]; };
        constexpr SDL_Scancode keys[2][8] = {
            {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
             SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT},
            {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
             SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6},
        };
        u16 in0 = 0, in1 = 0;
        router.set_overlay(paused);
        pads.refresh();
        for (int p = 0; p < 2; ++p) {
            auto pad = pads.read(p);
            pad.state.move = static_cast<u8>(directions(pad.lx, pad.ly));
            pad.state.fire = static_cast<u8>(directions(pad.rx, pad.ry));
            if (pad.state.connected)
                pad.state.buttons |= stick_buttons(pad.lx, pad.ly, 9000, false) |
                                     stick_buttons(pad.rx, pad.ry, 9000, true);
            const auto event = router.update(p, pad.state, focused, SDL_GetTicks());
            if (event == PadEvent::Open || (paused && event == PadEvent::Back)) {
                paused = !paused;
                router.set_overlay(paused);
            }
            if (paused && event == PadEvent::Confirm) quit = true;
            auto state = router.game(p);
            // A stick bound to actions gives up its built-in move/fire role.
            if (bindings.uses(p, PadLeftStick)) state.move = 0;
            if (bindings.uses(p, PadRightStick)) state.fire = 0;
            u16 contacts = state.move | (state.fire << 4);
            for (int b = 0; b < 8; ++b)
                if (key(keys[p][b]) || bindings.held(p * 8 + b, state)) contacts |= u16(1 << b);
            in0 |= u16(contacts << (8 * p));
            if (bindings.held(16 + p, state) || key(p ? SDL_SCANCODE_6 : SDL_SCANCODE_5)) in1 |= u16(1 << p);
            if (bindings.held(18 + p, state) || key(p ? SDL_SCANCODE_2 : SDL_SCANCODE_1)) in1 |= p ? 0x20 : 4;
        }
        if (key(SDL_SCANCODE_F2)) in1 |= 0x10;
        if (key(SDL_SCANCODE_9)) in1 |= 0x40;
        m.ports[0] = u16(~in0);
        m.ports[1] = u16(~in1);
    }
};

int play(Options options) {
    using namespace stv;
    if (options.data.empty()) {
        std::unique_ptr<char, decltype(&SDL_free)> pref(SDL_GetPrefPath("SmashTV", "SmashTV"), SDL_free);
        if (!pref) throw std::runtime_error(SDL_GetError());
        options.data = pref.get();
    }
    fs::create_directories(options.data);
    const auto cmos = options.data / (options.wide ? "cmos-l8-16x9.bin" : "cmos-l8.bin");
    Machine m;
    snd::Board sound;
    std::string error;
    const auto import = options.data / ("import-" + std::to_string(std::random_device{}()));
    if (!fs::create_directory(import)) throw std::runtime_error("Cannot create an import directory");
    struct RemoveImport {
        fs::path directory;
        ~RemoveImport() {
            std::error_code ignored;
            fs::remove(directory / "chips.bin", ignored);
            fs::remove(directory, ignored);
        }
    } remove_import{import};
    if (!import_rom_set(options.rom, import / "chips.bin", error) ||
        !load_imported_roms(m, import / "chips.bin", error)) throw std::runtime_error(error);
    m.enhanced = options.wide;
    if (options.wide) {
        m.set_wide(1024, 137);
        if (!apply_enhanced(m, 137)) throw std::runtime_error("The enhanced table does not match retail L8");
    }
    m.reset();
    if (!options.frames) {
        std::ifstream file(cmos, std::ios::binary);
        std::vector<u16> saved(m.cmos.size());
        if (file.read(reinterpret_cast<char*>(saved.data()), saved.size() * sizeof(u16))) m.cmos = std::move(saved);
    }
    if (!sound.load_roms(m.snd_u4, m.snd_u19, m.snd_u20, error)) throw std::runtime_error(error);
    snd::Mixer mixer(48000);
    sound.mixer = &mixer;
    sound.power_on();
    game::CoScheduler scheduler(m, sound);
    bool frame_done = false;
    std::vector<float> pcm;
    scheduler.on_base = [&](const snd::Atto& time) {
        sound.ym_update(time);
        mixer.render_until(time, pcm);
    };
    m.on_sound_write = [&](u32, u16 data) { scheduler.main_sound_write(data); };
    m.on_vblank = [&] { frame_done = true; scheduler.request_stop(); };

    const auto* mode = std::getenv("RECOMPLAUNCHER_MODE");
    bool fullscreen = mode && std::string_view(mode) == "bigpicture";
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Smash T.V.", 960, 540,
                                    SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0),
                                    &window, &renderer)) throw std::runtime_error(SDL_GetError());
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window_owner(window, SDL_DestroyWindow);
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer_owner(renderer, SDL_DestroyRenderer);
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, max_width, height),
        SDL_DestroyTexture);
    if (!texture || !SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST))
        throw std::runtime_error(SDL_GetError());
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 48000};
    std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)> audio(
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr),
        SDL_DestroyAudioStream);
    if (audio) SDL_ResumeAudioStreamDevice(audio.get());
    host::SdlPads pads;
    host::RecompAchievements achievements;
    achievements.start(options.frames != 0);
    Controls controls;
    std::vector<u32> pixels(max_width * height);
    bool quit = false, paused = false;
    auto deadline = SDL_GetTicksNS();
    auto previous_paused = paused;
    unsigned frames = 0;
    scheduler.start(m.reset_vector());
    while (!quit && (!options.frames || frames < options.frames)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) quit = true;
            if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) continue;
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                paused = !paused;
                if (paused && audio) SDL_ClearAudioStream(audio.get());
            }
            if (event.key.scancode == SDL_SCANCODE_F11) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(window, fullscreen);
            }
        }
        controls.sample(m, SDL_GetKeyboardState(nullptr), pads, window, paused, quit);
        if (paused != previous_paused) {
            if (paused && audio) SDL_ClearAudioStream(audio.get());
            SDL_SetWindowTitle(window, paused ? "Smash T.V. | Paused: Esc / B resume, A quit" : "Smash T.V.");
            previous_paused = paused;
        }
        if (quit) break;
        if (paused) { deadline = SDL_GetTicksNS(); SDL_Delay(8); continue; }
        if (const auto now = SDL_GetTicksNS(); now < deadline) SDL_DelayNS(deadline - now);
        deadline = std::max(deadline + PS_PER_FRAME / 1000, SDL_GetTicksNS());
        frame_done = false;
        scheduler.resume();
        if (!frame_done || m.stop || !sound.fault.empty())
            throw std::runtime_error("The translated game stopped: " + sound.fault);
        ++frames;
        achievements.frame(m, false);
        if (audio && SDL_GetAudioStreamQueued(audio.get()) < 48000 * int(sizeof(float)) / 10)
            SDL_PutAudioStreamData(audio.get(), pcm.data(), static_cast<int>(pcm.size() * sizeof(float)));
        pcm.clear();
        const int width = enhanced_scene(m) ? max_width : VIS_MAXX - VIS_MINX + 1;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                pixels[y * max_width + x] = m.pens[m.frame[(y + VIS_MINY) * m.vram_w + VIS_MINX + x] & 0xfff];
        const SDL_Rect updated{0, 0, width, height};
        SDL_UpdateTexture(texture.get(), &updated, pixels.data(), max_width * sizeof(u32));
        int w = 0, h = 0;
        if (SDL_GetRenderOutputSize(renderer, &w, &h)) {
            const float aspect = float(width) / height * (5.f / 6.f);
            const float show_w = std::min(float(w), float(h) * aspect);
            const float show_h = show_w / aspect;
            const SDL_FRect source{0, 0, float(width), float(height)};
            const SDL_FRect dest{(w - show_w) / 2, (h - show_h) / 2, show_w, show_h};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, texture.get(), &source, &dest);
            SDL_RenderPresent(renderer);
        }
    }
    if (!options.frames) {
        auto temporary = cmos;
        temporary += ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(m.cmos.data()), m.cmos.size() * sizeof(u16));
        file.close();
        if (!file) throw std::runtime_error("Could not save the arcade CMOS");
        fs::rename(temporary, cmos);
    } else {
        std::cout << "Native SDL frames: " << frames << '\n';
        if (frames != options.frames) return 1;
    }
    return 0;
}
}

int main(int argc, char** argv) {
    Options options;
    if (!parse(options, argc, argv)) {
        std::cerr << "Usage: SmashTV --rom=<retail L8 ZIP> [--data=<path>] [--classic|--widescreen]\n";
        return 64;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::cerr << SDL_GetError() << '\n'; return 1; }
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    int result = 1;
    try { result = (!options.rom.empty() || select_rom(options)) ? play(std::move(options)) : 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    SDL_Quit();
    return result;
}
