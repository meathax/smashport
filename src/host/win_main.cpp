// SmashTV.exe: Windows host for the natively translated L8 program (PLAN P4/P6/P8).
// Runs the translated main and sound programs on the native Y-unit services under
// the machine scheduler (stv::game::CoScheduler), presents the visible 410x256
// area with nearest-neighbour scaling, plays the sound board through WASAPI,
// maps keyboard and XInput pads to the original switch ports, and keeps CMOS
// under %LOCALAPPDATA%\SmashTV. No emulator or interpreter: the game and sound
// code are compiled C++ (generated/private).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <xinput.h>
#include "stv/game/cosched.hpp"
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"
#include "stv/snd/board.hpp"
#include "stv/host/scale.hpp"
#include "stv/host/frame_clock.hpp"
#include "stv/host/camera_motion.hpp"
#include "stv/host/audio_telemetry.hpp"
#include "stv/host/compositor_wait.hpp"
#include "stv/host/pad.hpp"
#include "stv/host/recomp_achievements.hpp"
#ifdef STV_SDL_PADS
#include "stv/host/sdl_pads.hpp"
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#endif
#include "stv/host/net_audio_audit.hpp"
#include "stv/host/net_fault.hpp"
#include "stv/host/pc_menu.hpp"
#include "stv/host/menu_draw_win.hpp"
#include "stv/host/global_leaderboard.hpp"
#include "stv/host/replay_proof.hpp"
#include "stv/host/gpu_filter.hpp"
#include "stv/gsp/rom_import.hpp"
#include "resources/resource.h"
#include "stv/host/enhance.hpp"
#include "stv/host/gpu_resident.hpp"
#include "stv/host/pc_settings.hpp"
#include "stv/host/video_filter.hpp"
#include "stv/net/handshake.hpp"
#include "stv/net/udp.hpp"
#include "stv/net/lobby.hpp"
#if STV_HAS_EOS
#include "stv/net/epic_rooms.hpp"
#endif
#include "wasapi.hpp"
#include "title_background.hpp"
#include "stv/host/firing_point.hpp"
#include "stv/host/drop_player.hpp"
#include "stv/host/audio_fx.hpp"
#include "stv/host/room_cinema.hpp"
#include "stv/snd/voices.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <thread>
#include <optional>
#include <string>
#include <vector>

using namespace stv::gsp;

namespace {
constexpr int VIS_W = VIS_MAXX - VIS_MINX + 1;   // 410
constexpr int VIS_H = VIS_MAXY - VIS_MINY + 1;   // 256
constexpr double FRAME_SECONDS = double(PS_PER_FRAME) * 1e-12;

Machine g_m;
stv::snd::Board g_snd;
std::unique_ptr<stv::game::CoScheduler> g_sched;
std::unique_ptr<stv::snd::Mixer> g_mixer;
stv::host::AudioOut g_audio;
stv::host::AudioTelemetry g_audio_telemetry;
std::vector<float> g_pcm;
stv::host::AudioPeakObserver g_audio_peaks;
// Enhanced sound output (presentation only; g_pcm stays the retail board PCM).
stv::snd::VoiceBank g_voices;
stv::host::AudioFx g_fx;
stv::host::AudioFxSettings g_fx_applied{false, false, 0};
bool g_fx_configured = false;
std::vector<float> g_stereo;
uint64_t g_voice_starts = 0;
u32 g_probe_table = 0;
bool g_probe_pending = false;
bool g_started = false;
u32 g_pc = 0;
bool g_frame_done = false;
bool g_paused = false;
bool g_fullscreen = false;
stv::host::ScaleMode g_scale_mode = stv::host::ScaleMode::IntegerFill;
stv::host::PcSettings g_settings;
stv::host::PcMenu g_pc_menu;
stv::host::VideoFilterBuffer g_filter_buffer;
using DwmFlushFn = HRESULT(WINAPI*)();
DwmFlushFn g_dwm_flush = nullptr;
stv::host::CompositorWait g_compositor;
bool g_compositor_failed = false;
double g_display_fps = 0, g_simulation_hz = 0;
u64 g_simulation_steps = 0;
int g_frame_width = VIS_W; // captured with the completed framebuffer, never live GAMSTATE
u64 g_pixel_revision = 0, g_frame_captures = 0;
bool g_frame_ready = false, g_last_paint_ok = false;
struct PresentationStats {
    u64 count{}, failures{}, last{}, frequency{}, rate_start{}, rate_count{}, rate_steps{};
    std::array<u64, 41> histogram{};
    double maximum_ms{};
    RECT destination{};
    int client_width{}, client_height{};
    void record() {
        LARGE_INTEGER stamp{}, hz{};
        QueryPerformanceCounter(&stamp);
        if (!frequency) { QueryPerformanceFrequency(&hz); frequency = u64(hz.QuadPart); }
        const auto now = u64(stamp.QuadPart);
        ++count;
        if (last) {
            const double ms = 1000.0 * double(now - last) / double(frequency);
            ++histogram[std::min<std::size_t>(std::size_t(ms), histogram.size() - 1)];
            maximum_ms = std::max(maximum_ms, ms);
        }
        last = now;
        if (!rate_start) { rate_start = now; rate_count = count; rate_steps = g_simulation_steps; }
        if (now - rate_start >= frequency) {
            const double elapsed = double(now - rate_start) / double(frequency);
            g_display_fps = double(count - rate_count) / elapsed;
            g_simulation_hz = double(g_simulation_steps - rate_steps) / elapsed;
            rate_start = now; rate_count = count; rate_steps = g_simulation_steps;
        }
    }
} g_present;
// Host CPU stage costs for the smoke report (simulation, scene/effects
// preparation and window presentation). Measurement only; never paces work.
struct StageTiming {
    std::vector<float> samples;
    void add(const LARGE_INTEGER& a, const LARGE_INTEGER& b, u64 frequency) {
        if (samples.size() < (1u << 20)) samples.push_back(float(1000.0 * double(b.QuadPart - a.QuadPart) / double(frequency)));
    }
    void write(FILE* f, const char* name, bool comma) const {
        std::vector<float> s = samples;
        std::sort(s.begin(), s.end());
        double sum = 0; for (float v : s) sum += v;
        auto at = [&](double q) { return s.empty() ? 0.0 : double(s[std::min(s.size() - 1, std::size_t(q * double(s.size())))]); };
        std::fprintf(f, "\"%s\": {\"count\": %zu, \"mean_ms\": %.4f, \"p50_ms\": %.4f, \"p95_ms\": %.4f, \"p99_ms\": %.4f, \"max_ms\": %.4f}%s",
                     name, s.size(), s.empty() ? 0.0 : sum / double(s.size()), at(.5), at(.95), at(.99), s.empty() ? 0.0 : double(s.back()),
                     comma ? ", " : "");
    }
};
StageTiming g_time_sim, g_time_scene, g_time_present;
std::filesystem::path g_rom_path;
bool g_restart_requested = false, g_disconnect_requested = false;
bool g_connecting = false, g_cancel_connect = false;
enum class ConfirmAction { None, Restart, MainMenu, Disconnect, Quit };
ConfirmAction g_confirm = ConfirmAction::None;
bool g_confirm_yes = false;
bool g_name_edit = false;
std::string g_name_draft;
std::size_t g_name_cursor = 0;
std::string g_menu_status;
void save_preferences();
void save_cmos();
void service_lobby();
void start_lobby_host(bool online = false, bool epic = false);
void start_lobby_search(bool epic = false);
std::wstring exe_dir();
bool handle_lobby_key(WPARAM key);
const stv::net::LobbyRoom* selected_room();
void select_room(const stv::net::LobbyRoom& room);
bool lobby_menu_lines(std::vector<std::string>& lines);
bool reset_coupled_game(bool keep_cmos = true);
bool scripted_run();
void poll_pad_controls();
void write_player_log(std::string_view text);
void frontend_event(const char* event, std::uint64_t value = 0, const std::string& detail = "");
int vis_w() { return VIS_W + int(g_m.vis_extra); }
const char* scale_name() {
    switch (g_scale_mode) {
    case stv::host::ScaleMode::IntegerExact: return "integer, exact aspect";
    case stv::host::ScaleMode::IntegerFill: return "integer, fill";
    default: return "fit, exact aspect";
    }
}
void cycle_scale() {
    g_scale_mode = g_scale_mode == stv::host::ScaleMode::IntegerFill ? stv::host::ScaleMode::IntegerExact
                 : g_scale_mode == stv::host::ScaleMode::IntegerExact ? stv::host::ScaleMode::Smooth
                                                                       : stv::host::ScaleMode::IntegerFill;
}
HWND g_wnd = nullptr;
WINDOWPLACEMENT g_place{sizeof(WINDOWPLACEMENT)};
std::vector<u32> g_pixels;
std::wstring g_data_dir;
bool g_enhanced = true;        // default to the enhanced_l8_16_9 widened playfield
std::string g_status;
std::string g_toast;           // short notice (pad connected), shown for 3 s
ULONGLONG g_toast_until = 0;
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinput = nullptr;
stv::host::RecompAchievements g_achievements;
#ifdef STV_SDL_PADS
std::unique_ptr<stv::host::SdlPads> g_sdl_pads;
#endif

// Remappable keyboard actions (PC menu > Controls); saved in keys.cfg.
enum Action { P1_UP, P1_DOWN, P1_LEFT, P1_RIGHT, P1_FUP, P1_FDOWN, P1_FLEFT, P1_FRIGHT,
              P2_UP, P2_DOWN, P2_LEFT, P2_RIGHT, P2_FUP, P2_FDOWN, P2_FLEFT, P2_FRIGHT,
              COIN1, COIN2, START1, START2, ACTION_COUNT };
const char* const kActionNames[ACTION_COUNT] = {
    "P1 MOVE UP", "P1 MOVE DOWN", "P1 MOVE LEFT", "P1 MOVE RIGHT",
    "P1 FIRE UP", "P1 FIRE DOWN", "P1 FIRE LEFT", "P1 FIRE RIGHT",
    "P2 MOVE UP", "P2 MOVE DOWN", "P2 MOVE LEFT", "P2 MOVE RIGHT",
    "P2 FIRE UP", "P2 FIRE DOWN", "P2 FIRE LEFT", "P2 FIRE RIGHT",
    "COIN 1", "COIN 2", "START 1", "START 2"};
const int kDefaultKeys[ACTION_COUNT] = {'W', 'S', 'A', 'D', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
                                        'I', 'K', 'J', 'L', VK_NUMPAD8, VK_NUMPAD5, VK_NUMPAD4, VK_NUMPAD6,
                                        '5', '6', '1', '2'};
int g_keys[ACTION_COUNT];
bool g_keyboard_blocked = false;
static_assert(ACTION_COUNT == stv::host::PadBindings::Count);
stv::host::PadBindings g_pad_bindings;
stv::host::PadRouter g_pad_router;
stv::host::ConfirmStartRouter g_confirm_start;
int g_deadzone = 12000;
bool g_pad_controls = false, g_pad_capture = false;
int g_pad_player = 0, g_pad_selection = 0;
int pad_action_for(int player, int selection) {
    return selection < 8 ? player * 8 + selection
         : selection == 8 ? COIN1 + player : START1 + player;
}
int pad_action() { return pad_action_for(g_pad_player, g_pad_selection); }
void handle_menu_key(HWND wnd, WPARAM wp);
void save_keys();

// Controller-operated PC pages, adapted from TMNT-port's console menu.
bool g_swap_pads = false;      // XInput pad 1 drives player 2 and pad 2 drives player 1
bool g_menu = false;
stv::host::ArcadeMenuFont g_menu_font;
bool g_menu_font_ready = false;
bool g_mode_menu = false;
int g_mode_selection = 1; // Start
bool g_title_submenu = false;
bool g_online_room = false, g_epic_room = false, g_close_room_requested = false;
enum class PasswordPrompt { None, Host, Join };
PasswordPrompt g_password_prompt{};
std::string g_room_password, g_password_draft;
size_t g_password_cursor = 0;
uint64_t g_password_room = 0;
stv::net::LobbyAddress g_password_address{};
#if STV_HAS_EOS
stv::net::EpicRooms g_epic;
#endif
std::optional<bool> g_guest_room_profile;
bool g_room_scores_open = false;
bool g_host_solo = false, g_start_solo = false, g_record_local = false;
std::vector<u16> g_room_boot_cmos;
std::optional<bool> g_host_mode;
bool g_host_booting = false, g_start_together = false;
// Controls of the current local run from power-on, attached to leaderboard scores
// so anyone with the ROM can re-simulate and confirm them (see replay_proof.hpp).
stv::host::ReplayProof g_proof;
std::vector<stv::net::PlayerInput> g_script1, g_script2;   // STV_HOST_INPUTS scenario (frames from power-on)
struct RoomInputs { u32 p1, p2; u16 frames; std::optional<u16> local_switches; };
u32 g_room_frames = 0;
std::vector<RoomInputs> g_room_history;
void record_room_frame(u32 p1, u32 p2, std::optional<u16> local_switches = {}) {
    ++g_room_frames;
    if (!g_room_history.empty()) {
        auto& last = g_room_history.back();
        if (last.p1 == p1 && last.p2 == p2 && last.local_switches == local_switches && last.frames < 65535) { ++last.frames; return; }
    }
    g_room_history.push_back({p1, p2, 1, local_switches});
}
// Local play samples switches before running the coupled frame. Keep that
// original edge when reconstructing an already-running solo or couch game.
void record_local_frame() {
    if (!g_record_local && !g_proof.valid()) return;
    const u16 controls = ~g_m.ports[0], switches = ~g_m.ports[1];
    const u32 p1 = (u32(g_m.move_inputs[0]) << 16) | g_m.aim_inputs[0] | (controls & 255) |
        ((switches & 1) ? 0x100 : 0) | ((switches & 4) ? 0x200 : 0);
    const u32 p2 = (u32(g_m.move_inputs[1]) << 16) | g_m.aim_inputs[1] | (controls >> 8) |
        ((switches & 2) ? 0x100 : 0) | ((switches & 32) ? 0x200 : 0);
    if (g_script1.empty()) g_proof.add(p1,p2,g_m.ports[1]);   // scripted runs record at the vblank instead
    if (g_record_local) record_room_frame(p1,p2,g_m.ports[1]);
}

std::string g_room_guest;
std::string g_room_notice;
ULONGLONG g_room_notice_until = 0;
int g_launch_mode = -1;
std::wstring g_requested_data_dir;
bool g_recomp_launcher = false;
bool g_launcher_big_picture = false;
bool g_launcher_playstation = false;
bool g_transient_fullscreen = false;
// Retail TITLPAGE selects TITLE (FFEA56D0). Let the background renderer and
// colour cycler settle for 320 native frames before exposing the intro image.
// Merely seeing an attract background would stop on CHAMPS, the scoreboard.
bool startup_intro_ready(const stv::gsp::Machine& m, unsigned& title_frames) {
    if (game_scene_state(m)==0x30 && scene_background(m)==0xffea56d0u)
        return ++title_frames >= 320;
    title_frames=0;
    return false;
}

int g_remap = -1;              // action being assigned, -1 when not remapping
int g_test_frames = 0;         // frames left holding the operator test switch

bool key(int vk) { return !g_keyboard_blocked && (GetAsyncKeyState(vk) & 0x8000) && GetForegroundWindow() == g_wnd; }
void sync_menu_input() {
    static bool previous_menu = false;
    if (previous_menu != g_menu) {
        g_keyboard_blocked = true;
        g_confirm_start.reset();
        previous_menu = g_menu;
        frontend_event(g_menu ? "menu_open" : "menu_closed");
    }
    if (g_menu && !g_pc_menu.is_open()) g_pc_menu.open(g_started);
    if (!g_menu && g_pc_menu.is_open()) g_pc_menu.close();
    g_pad_router.set_overlay(g_menu || g_connecting);
}

// 8-way stick with a dead zone; returns bits up/down/left/right.
u16 stick_bits(SHORT x, SHORT y) {
    if (s64(x)*x + s64(y)*y <= s64(g_deadzone)*g_deadzone) return 0;
    const int ax = std::abs(int(x)), ay = std::abs(int(y));
    // Nearest of eight equal sectors. tan(22.5 degrees) separates an axis
    // from its adjacent diagonal. Throw outside the dead zone is digital.
    constexpr double boundary = 0.414213562373095;
    const u16 horizontal = x < 0 ? 4 : 8;
    const u16 vertical = y < 0 ? 2 : 1;
    if (ax < ay * boundary) return vertical;
    if (ay < ax * boundary) return horizontal;
    return horizontal | vertical;
}

u16 arcade_contacts(u16 bits) {
    // Opposed keys or overlapping device bindings cannot close both
    // contacts on the same axis of one cabinet stick.
    if ((bits & 3) == 3) bits &= ~3;
    if ((bits & 12) == 12) bits &= ~12;
    return bits;
}

// XInput slot with hotplug: a pad that is absent is polled again only every 60 calls
// (XInputGetState on an empty slot is slow); connect and disconnect show as a notice.
bool pad_state(DWORD slot, XINPUT_STATE& s) {
    static int retry[XUSER_MAX_COUNT] = {};
    static bool connected[XUSER_MAX_COUNT] = {};
    if (!g_xinput || slot >= XUSER_MAX_COUNT) return false;
    if (!connected[slot] && retry[slot] > 0) { --retry[slot]; return false; }
    const bool ok = g_xinput(slot, &s) == ERROR_SUCCESS;
    if (ok != connected[slot]) {
        connected[slot] = ok;
        g_toast = "Pad " + std::to_string(slot + 1) + (ok ? " connected" : " disconnected");
        g_toast_until = GetTickCount64() + 3000;
    }
    if (!ok) retry[slot] = 60;
    return ok;
}

enum class StartContext { Other, Attract, Continue };
StartContext start_context(int player);

// Keyboard and pads as Y-unit IN0/IN1 bits (active high).
void read_controls(u16& in0, u16& in1, std::array<u32,2>* angles=nullptr) {
    in0 = 0, in1 = 0;
    if (g_menu || g_paused) return;
    if (g_keyboard_blocked && GetForegroundWindow() == g_wnd) {
        bool held = (GetAsyncKeyState(VK_F2) & 0x8000) || (GetAsyncKeyState('9') & 0x8000);
        for (int i = 0; i < ACTION_COUNT; ++i) held = held || (GetAsyncKeyState(g_keys[i]) & 0x8000);
        if (!held) g_keyboard_blocked = false;
    }
    for (int p = 0; p < 2; ++p) {
        u16 mv = 0, fr = 0;
        const int* k = &g_keys[p * 8];
        for (int b = 0; b < 4; ++b) {
            if (key(k[b])) mv |= u16(1 << b);
            if (key(k[4 + b])) fr |= u16(1 << b);
        }
        const auto s = g_pad_router.game(g_swap_pads ? 1 - p : p);
        if (s.connected) {
            mv |= s.move;
            fr |= s.fire;
            for (int b = 0; b < 4; ++b) {
                if (g_pad_bindings.held(p * 8 + b, s)) mv |= u16(1 << b);
                if (g_pad_bindings.held(p * 8 + 4 + b, s)) fr |= u16(1 << b);
            }
            if (g_pad_bindings.held(COIN1 + p, s)) in1 |= p ? 0x0002 : 0x0001;

        }
        if (g_enhanced && angles && s.motion) {
            // Supply both requested axes for the retail wall tests. The leg
            // animation chooses the nearest facing after blocked axes drop out.
            const int dx=static_cast<std::int8_t>(s.motion&255),dy=static_cast<std::int8_t>(s.motion>>8);
            mv=u16((dx<0 ? 4 : dx>0 ? 8 : 0) | (dy<0 ? 1 : dy>0 ? 2 : 0));
            (*angles)[p] = u32(s.motion)<<16;
        }
        if (g_enhanced && angles && s.aim) {
            fr = s.fire;
            (*angles)[p] |= u32(s.aim << 10);
        }
        // Host-only input policy. The translated game still owns credits and admission.
        if (!g_enhanced) { mv = arcade_contacts(mv); fr = arcade_contacts(fr); }
        const auto context = start_context(p);
        const bool start_key = key(g_keys[START1 + p]);
        const bool confirm = start_key || (s.connected && (s.buttons & stv::host::PadA));
        const bool attract_fire = context == StartContext::Attract && g_enhanced && fr != 0;
        const bool fresh_confirm = g_confirm_start.update(p, confirm || attract_fire, context != StartContext::Other);
        const bool mapped_start = start_key || g_pad_bindings.held(START1 + p, s);
        if (fresh_confirm || (context != StartContext::Continue && mapped_start))
            in1 |= p ? 0x0020 : 0x0004;
        in0 |= u16((mv | (fr << 4)) << (p * 8));
    }
    if (key(g_keys[COIN1])) in1 |= 0x0001;
    if (key(g_keys[COIN2])) in1 |= 0x0002;
    if (key(VK_F2) || g_test_frames > 0) in1 |= 0x0010;   // test switch (operator menus)
    if (g_test_frames > 0) --g_test_frames;
    if (key('9')) in1 |= 0x0040;          // service credit
}

// STV_HOST_INPUTS=FILE replays a recorded scenario (lockstep input format) instead of
// the controls, frame by frame from power-on (soak and gameplay automation).

// Scripted inputs are applied inside the vblank callback, at the same point as the
// lockstep harness; the scheduler only stops at the end of its timeslice.
void apply_script(u64 frame) {
    const bool in = frame < g_script1.size();
    const stv::net::PlayerInput p1 = in ? g_script1[frame] : 0, p2 = in ? g_script2[frame] : 0;
    g_proof.add(p1, p2, std::nullopt);
    stv::net::apply_inputs(g_m, p1, p2);
}

void sample_inputs() {
    if (!g_script1.empty()) return;
    u16 in0, in1;
    std::array<u32,2> angles{};
    read_controls(in0, in1, &angles);
    g_m.move_inputs = {u16(angles[0]>>16),u16(angles[1]>>16)};
    g_m.aim_inputs = g_enhanced ? std::array<u16,2>{u16(angles[0] | (in0 & 0xf0)), u16(angles[1] | ((in0 >> 8) & 0xf0))} : std::array<u16,2>{};
    g_m.ports[0] = u16(~in0);
    g_m.ports[1] = u16(~in1);
}

// Online co-op: this PC's player uses the player 1 controls (keys and pad 1).
stv::net::PlayerInput online_join_input(stv::net::PlayerInput input);
stv::net::PlayerInput local_player_input() {
    u16 in0, in1;
    std::array<u32,2> angles{};
    read_controls(in0, in1, &angles);
    auto input = stv::net::PlayerInput(angles[0] | (in0 & 0xff) | (in1 & 0x1 ? 0x100 : 0) | (in1 & 0x4 ? 0x200 : 0));
    input = online_join_input(input);
    return input;
}

// Capture only a completed arcade image. Repeated host redraws reuse this
// immutable RGB snapshot; they never run a CPU, sample inputs, or produce sound.
stv::host::EnhanceEffects g_enhance_effects;
stv::host::EnemyRemainsTracker g_enemy_remains;
stv::host::ResidentEffects g_resident_effects;
bool g_resident_ready = false;

void build_software_frame(const Machine& m) {
    g_frame_width = stv::gsp::scene_view_width(m);
    g_pixels.resize(std::size_t(g_frame_width) * VIS_H);
    // The CPU fallback has no ray-traced shadows to replace the retail blob
    // shadows, so it always shows the untouched arcade frame.
    const auto& frame = m.frame;
    for (int y = 0; y < VIS_H; ++y) {
        const u16* src = &frame[std::size_t(y + VIS_MINY) * m.vram_w + VIS_MINX];
        u32* dst = &g_pixels[std::size_t(y) * g_frame_width];
        for (int x = 0; x < g_frame_width; ++x) dst[x] = m.pens[src[x] & 0x0fff];
    }
    stv::host::extend_title(g_pixels,g_frame_width,m.enhanced,
                           game_scene_state(m)==0x30 && scene_background(m)==0xffea56d0u);
}

// Menu backdrop from the live machine's last completed frame (547x256 RGB),
// empty when the game has not drawn a visible frame yet.
// Menu artwork comes only from the player's ROM: a private machine boots the
// retail program in the background (no sound board, no presentation) until the
// title page has faded in, and keeps that 410x256 frame. Nothing is shipped.
struct MenuTitle {
    std::mutex lock;
    std::vector<u32> pixels;          // 547x256, native page centred
    std::atomic<bool> ready{false}, stop{false};
    std::thread worker;
    ~MenuTitle() { stop = true; if (worker.joinable()) worker.join(); }
} g_menu_title;

void capture_menu_title(std::filesystem::path rom) {
    g_menu_title.worker = std::thread([rom] {
        auto m = std::make_unique<Machine>();
        std::string error;
        if (!stv::gsp::load_imported_roms(*m, rom, error)) return;
        m->reset();
        u32 pc = m->reset_vector();
        bool frame_done = false;
        m->on_vblank = [&] { frame_done = true; m->run_until_cycle = m->c.cyc; m->recompute_next_event(); };
        for (unsigned frame = 0; frame < 4000 && !m->stop && !g_menu_title.stop; ++frame) {
            frame_done = false;
            m->run_until_cycle = ~0ull;
            m->recompute_next_event();
            while (!frame_done && !m->stop && !g_menu_title.stop) pc = stv::gsp::l8_run(*m, pc, m->c.cyc + 1000000);
            if (game_scene_state(*m) != 0x30 || scene_background(*m) != 0xffea56d0u) continue;
            std::vector<u32> page(std::size_t(VIS_W) * VIS_H);
            std::size_t lit = 0;
            for (int y = 0; y < VIS_H; ++y)
                for (int x = 0; x < VIS_W; ++x) {
                    const u32 c = m->pens[m->frame[std::size_t(y + VIS_MINY) * m->vram_w + VIS_MINX + x] & 0x0fff] & 0xffffffu;
                    page[std::size_t(y) * VIS_W + x] = c;
                    lit += c != 0;
                }
            if (lit < page.size() * 6 / 10) continue;   // wait until the fade-in completes
            int width = VIS_W;
            stv::host::extend_title(page, width, true);
            {
                std::lock_guard guard(g_menu_title.lock);
                g_menu_title.pixels = std::move(page);
            }
            g_menu_title.ready = true;
            if (g_wnd) PostMessageW(g_wnd, WM_APP + 12, 0, 0);
            return;
        }
    });
}

std::vector<u32> build_menu_backdrop(const Machine& m) {
    if (m.frame.size() < std::size_t(VIS_MINY + VIS_H) * m.vram_w || m.pens.size() != 0x1000) return {};
    int width = std::min(int(stv::gsp::scene_view_width(m)), 547);
    std::vector<u32> pixels(std::size_t(width) * VIS_H);
    bool drawn = false;
    for (int y = 0; y < VIS_H; ++y) {
        const u16* src = &m.frame[std::size_t(y + VIS_MINY) * m.vram_w + VIS_MINX];
        for (int x = 0; x < width; ++x) {
            const u32 c = m.pens[src[x] & 0x0fff] & 0xffffffu;
            pixels[std::size_t(y) * width + x] = c;
            drawn = drawn || c;
        }
    }
    if (!drawn) return {};
    if (width == VIS_W) stv::host::extend_title(pixels, width, true);
    if (width != 547) {           // a narrower native page: centre it
        std::vector<u32> wide(std::size_t(547) * VIS_H);
        const int x0 = (547 - width) / 2;
        for (int y = 0; y < VIS_H; ++y)
            std::copy_n(pixels.data() + std::size_t(y) * width, width, wide.data() + std::size_t(y) * 547 + x0);
        pixels.swap(wide);
    }
    return pixels;
}

// Each menu paint reused to copy (or rebuild) a 560 KB image. Keep one copy
// until the ROM title arrives or the live frame changes.
const std::vector<u32>& menu_backdrop(const Machine& m) {
    static std::vector<u32> cached;
    static u64 revision = ~0ull;
    static bool title = false;
    if (g_menu_title.ready) {
        if (!title) {
            std::lock_guard guard(g_menu_title.lock);
            cached = g_menu_title.pixels;
            title = true;
        }
        return cached;
    }
    if (revision != g_pixel_revision) {
        cached = build_menu_backdrop(m);
        revision = g_pixel_revision;
    }
    return cached;
}

stv::host::CameraMotion g_camera;
float g_camera_phase = 1.0f;

void present_frame(const Machine& m) {
    const int width = stv::gsp::scene_view_width(m);
    const bool wide = m.enhanced && width == VIS_W;
    const bool title = wide && game_scene_state(m)==0x30 && scene_background(m)==0xffea56d0u;
    g_frame_width = wide ? 547 : width;
    auto scene = stv::host::observe_effect_scene(m.ram, m.frame_number, g_frame_width, VIS_H,
                                                g_settings.enhance || g_settings.hardware_rt || g_settings.room_cinema);
    g_camera.observe(scene);
    for (auto& obj : scene.objects) {
        if (obj.kind != stv::host::EffectKind::Projectile && obj.kind != stv::host::EffectKind::EnemyShot) continue;
        float x = 0, y = 0;
        if (!stv::host::firing_point(m, obj, scene.camera_x, scene.camera_y, x, y)) continue;
        obj.raster_muzzle=m.spawn_anchor[(obj.slot-object_pool_base)/object_stride].raster_muzzle;
        obj.barrel = true;
        obj.muzzle_x = x;
        obj.muzzle_y = y;
    }
    scene.visible_muzzles=m.visible_muzzles;
    scene.remains=g_enemy_remains.take(scene.room);
    scene.impacts=g_enemy_remains.take_impacts(scene.room);
    g_enhance_effects.advance(scene, g_settings, g_audio_peaks.pulse());
    const auto start=std::size_t(VIS_MINY)*m.vram_w + VIS_MINX;
    const auto visible=std::span<const u16>(m.presentation_frame.empty()?m.frame:m.presentation_frame).subspan(start);
    auto without_blobs=std::span<const u16>(m.presentation_without_blobs);
    if(!without_blobs.empty())without_blobs=without_blobs.subspan(start);
    if (g_wnd && g_resident_effects.device_lost()) {
        // TDR, driver update or adapter removal. Rebuild the GPU path instead
        // of leaving a frozen image until restart.
        const bool recovered = g_resident_effects.recover(g_settings.hardware_rt);
        g_toast = recovered ? "Graphics device reset; display restored." : "Graphics device lost; using software display.";
        g_toast_until = GetTickCount64() + 5000;
        frontend_event("gpu_device_reset", recovered ? 1 : 0, g_resident_effects.error());
    }
    g_resident_ready = g_wnd && g_resident_effects.update_arcade(visible, m.vram_w, width, VIS_H, m.pens,
                                                                wide, title, g_enhance_effects.packet(), g_settings,
                                                                !m.presentation_frame.empty(),without_blobs);
    if (!g_resident_ready) {
        build_software_frame(m);
        if (g_wnd) {
            std::string status = "GPU effects: " + g_resident_effects.error();
            if (status != g_menu_status) g_menu_status = std::move(status);
        }
    }
    ++g_pixel_revision;
    ++g_frame_captures;
    g_frame_ready = true;
}

bool present_current_frame() {
    if (!g_frame_ready || !IsWindowVisible(g_wnd) || IsIconic(g_wnd)) return false;
    if (g_settings.sync_presentation() && !g_compositor_failed) {
        if (!g_dwm_flush || !g_compositor.start(g_dwm_flush) || g_compositor.errors()) {
            g_compositor_failed = true;
            // Keep the saved preference intact. Report a failed compositor
            // explicitly and continue simulation/output with unlocked rendering.
            g_toast = "VSync unavailable; rendering unlocked.";
            g_toast_until = GetTickCount64() + 5000;
        } else if (!g_compositor.ready()) {
            return false;
        }
    }
    g_last_paint_ok = false;
    InvalidateRect(g_wnd, nullptr, FALSE);
    // WM_PAINT is synchronous here: count completed GDI draws, not invalidations
    // coalesced by the message queue. There is no software presentation cap.
    UpdateWindow(g_wnd);
    return g_last_paint_ok;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
// The compositor can release presentation before the next simulation deadline.
// Neither it nor the message queue may block fixed-rate game/audio work.
void wait_seconds(HANDLE timer, double s, HANDLE compositor_ready = nullptr) {
    if (s < 0.0005) return;
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(s * 1e7);   // relative, 100 ns units
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        const HANDLE handles[] = {timer, compositor_ready};
        MsgWaitForMultipleObjects(compositor_ready ? 2 : 1, handles, FALSE, INFINITE, QS_ALLINPUT);
    } else {
        MsgWaitForMultipleObjects(compositor_ready ? 1 : 0, &compositor_ready, FALSE, 1, QS_ALLINPUT);
    }
}

HANDLE presentation_wait_event() {
    return g_settings.sync_presentation() && !g_compositor_failed ? g_compositor.pending_event() : nullptr;
}

// One video frame: the scheduler runs both programs until the next vblank begins;
// sound samples are produced as timeslices end.
void run_one_frame() {
    g_frame_done = false;
    if (!g_started) {
        g_started = true;
        g_sched->start(g_pc);
        g_sched->run();
    } else {
        g_sched->resume();
    }
}

// Fork at a completed coupled timeslice. Rebuild the callbacks after copying
// the machine so speculative sound, timers and vblank touch only this fork.
const Machine& local_video_frame() {
    if (!g_settings.run_ahead || g_m.stop || !g_snd.fault.empty()) return g_m;
    static Machine future;
    static stv::snd::Board sound;
    // Program, native tables, graphics and sound ROMs (about 10 MB) are
    // immutable during play. Keep the fork's copy while their revision matches
    // instead of duplicating them for every displayed frame.
    struct Immutable {
        std::vector<u16> prog, data;
        std::vector<u8> gfx, u4, u19, u20;
        void take(Machine& m) {
            prog.swap(m.prog); data.swap(m.enhanced_data); gfx.swap(m.gfx);
            u4.swap(m.snd_u4); u19.swap(m.snd_u19); u20.swap(m.snd_u20);
        }
    };
    if (future.static_revision && future.static_revision == g_m.static_revision) {
        Immutable kept, live;
        kept.take(future);
        live.take(g_m);
        try {
            future = g_m;
        } catch (...) {
            live.take(g_m);
            future.static_revision = 0;
            throw;
        }
        live.take(g_m);
        kept.take(future);
    } else {
        future.static_revision = 0;   // a failed copy must not claim the new revision
        future = g_m;
    }
    future.replay = nullptr;
    future.on_boundary = {};
    future.on_dma_write = {};
    future.on_input_read = {};
    future.on_hcount_read = {};
    future.on_object = {};
    future.on_sound_probe = {};
    sound.copy_prediction_state(g_snd);
    stv::game::CoScheduler scheduler(future, sound);
    scheduler.copy_prediction_timing(*g_sched);
    future.on_sound_write = [&](u32 addr, u16 value) {
        if (addr >= 0x01e00000 && addr < 0x01e00020) scheduler.main_sound_write(value);
    };
    future.on_vblank = [&] { scheduler.request_stop(); };
    scheduler.on_base = [](const stv::snd::Atto& time) { sound.ym_update(time); };
    for (int i = 0; i < g_settings.run_ahead; ++i) {
        scheduler.resume();
        if (future.stop || !sound.fault.empty()) {
            g_toast = "Run-ahead prediction failed. Showing arcade frame.";
            g_toast_until = GetTickCount64() + 3000;
            return g_m;
        }
    }
    return future;
}

// ---- Online co-op (PLAN P10) -------------------------------------------------
// Started from the command line (--host [port] | --join host[:port]) or STV_NET
// ("host:PORT" / "join:HOST:PORT"). The displayed machine is the rollback machine;
// g_m (with the sound board) plays confirmed frames only, so sound never has to be
// taken back. STV_NET_INPUTS drives this peer from a scripted input file and
// STV_NET_FRAMES ends the session after that many confirmed frames (automation).
struct NetPlay {
    std::string contestant;
    // Drop-in: a coin and START for this PC's player until it is in play.
    bool auto_join = false, joining = false, confirm_held = false;
    unsigned join_frame = 0;
    bool on = false, host = false;
    std::string peer;
    uint16_t port = 27015;
    std::unique_ptr<stv::net::Transport> udp;
    std::unique_ptr<Machine> mr;
    // A guest retains its own latest local image independently of host CMOS.
    std::optional<std::vector<u16>> local_cmos;
    std::unique_ptr<stv::net::FrameRunner> runner;
    std::unique_ptr<stv::net::RollbackSession> session;
    u32 played = 0;
    std::vector<stv::net::PlayerInput> script;
    u32 exit_after = 0;
    u32 last_hash = 0;
    bool ended = false;
    u32 resync_base = ~0u;           // diagnostic boundary of the predictive machine's completed resync
    u32 first_desync_frame = ~0u, first_resync_base = ~0u;
    bool checkpoint_audit = false;
    std::vector<std::pair<u32, u32>> periodic_hashes;
    std::vector<u32> checkpoint_resync_bases;
    stv::host::ConfirmedAudioAudit audio_audit;
    stv::host::NetFaultInjection fault_injection;
    struct AudioTraceRow {
        u32 frame;
        u64 io_accesses, io_hash, pcm_generated, pcm_hash, mixer_callbacks;
    };
    struct ResyncTraceRow { u32 base; bool loaded; std::string before, after; };
    bool trace_enabled = false, trace_invalid = false, resync_trace_truncated = false;
    u32 trace_first = 0, trace_end = 0;
    std::vector<AudioTraceRow> audio_trace;
    std::vector<ResyncTraceRow> resync_trace;
} g_net;

stv::net::PlayerInput online_join_input(stv::net::PlayerInput input) {
    if (!g_net.on || !g_net.mr) return input;
    {
        // Online drop-in, exactly like an arcade player walking up: a coin and
        // START for this PC's player. Automatic on joining, and on confirmation after
        // a game over. Retried until the game admits the player (a departing
        // player's continue countdown can briefly hold the slot).
        const int me = g_net.host ? 0 : 1;
        const bool active = stv::host::player_active(*g_net.mr, me);
        const bool confirm = (input & 0x200) != 0;
        if (active && g_net.joining) {
            frontend_event("player_joined", unsigned(me + 1), std::to_string(g_net.join_frame));
            g_toast = me ? "PLAYER 2 - YOU'RE IN" : "PLAYER 1 - YOU'RE IN"; g_toast_until = GetTickCount64() + 2500;
        }
        if (active) g_net.joining = g_net.auto_join = false;
        else if ((g_net.auto_join || (confirm && !g_net.confirm_held)) && !g_net.joining && !g_menu && !g_paused) {
            g_net.joining = true; g_net.join_frame = 0; g_net.auto_join = false;
            g_toast = me ? "JOINING AS PLAYER 2..." : "JOINING AS PLAYER 1..."; g_toast_until = GetTickCount64() + 15000;
        }
        g_net.confirm_held = confirm;
        if (g_net.joining && !active) {
            const auto pulse = stv::host::join_pulse(g_net.join_frame % 120);
            if (pulse.coin) input |= 0x100;
            if (pulse.start) input |= 0x200;
            if (++g_net.join_frame >= 120 * 15) g_net.joining = false;   // the game would not admit a player
        }
    }
    return input;
}

// Sample output and protocol ownership together without changing either owner.
// This is opt-in; normal launches allocate no history and write no audio log.
void observe_audio_output(bool force = false) {
    if (!g_audio_telemetry.enabled()) return;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const u64 ticks = u64(now.QuadPart), hz = u64(frequency.QuadPart);
    const u64 microseconds = ticks / hz * 1000000 + (ticks % hz) * 1000000 / hz;
    stv::host::AudioTelemetry::Sample s;
    s.frame = g_m.frame_number;
    s.padding = g_audio.last_device_padding();
    s.pending = g_audio.pending_samples();
    s.buffer_frames = g_audio.buffer_frames();
    s.output_samples = g_audio.output_samples();
    s.underruns = g_audio.underruns();
    s.errors = g_audio.errors();
    s.recoveries = g_audio.recoveries();
    s.output_resets = g_audio.output_resyncs();
    s.source_drops = g_audio.dropped_source_samples();
    s.device_drops = g_audio.dropped_device_frames();
    s.available = g_audio.ok();
    s.startup_primes = g_audio.startup_primes();
    s.underrun_reprimes = g_audio.underrun_reprimes();
    s.priming_completions = g_audio.priming_completions();
    s.priming_wait_ms = g_audio.priming_wait_ms(GetTickCount64());
    s.priming_target = g_audio.priming_target_frames();
    s.priming = g_audio.priming();
    s.paused = g_paused || g_menu;
    s.online = g_net.on && bool(g_net.session);
    if (s.online) {
        s.frame = g_net.session->frame();
        s.confirmed = g_net.session->confirmed();
        s.played = g_net.played;
        s.paused = s.paused || g_net.session->paused_by_peer();
        s.resyncing = g_net.session->resyncing();
        s.network_resyncs = g_net.session->stats().resyncs;
        s.network_stalls = g_net.session->stats().stalls;
    }
    g_audio_telemetry.observe(microseconds, s, force);
}

void write_audio_recovery(FILE* f) {
    std::fprintf(f, "\"audio_endpoint_rate\":%u,\"audio_recovery_attempts\":%llu,"
                 "\"audio_recoveries\":%llu,\"audio_observed_unavailable_ms\":%llu,",
                 g_audio.endpoint_rate(), (unsigned long long)g_audio.recovery_attempts(),
                 (unsigned long long)g_audio.recoveries(),
                 (unsigned long long)g_audio.unavailable_ms(GetTickCount64()));
    std::fprintf(f, "\"audio_startup_primes\":%llu,\"audio_underrun_reprimes\":%llu,"
                 "\"audio_priming_completions\":%llu,\"audio_priming_wait_ms\":%llu,"
                 "\"audio_priming_active\":%s,\"audio_priming_target_frames\":%u,",
                 (unsigned long long)g_audio.startup_primes(), (unsigned long long)g_audio.underrun_reprimes(),
                 (unsigned long long)g_audio.priming_completions(),
                 (unsigned long long)g_audio.priming_wait_ms(GetTickCount64()),
                 g_audio.priming() ? "true" : "false", g_audio.priming_target_frames());
}

std::unique_ptr<stv::net::Lobby> g_lobby;
stv::net::LobbyUdp g_lobby_udp;
std::unique_ptr<stv::net::UdpTransport> g_lobby_game_socket;
std::optional<stv::net::LobbyHandoff> g_pending_handoff;
bool g_room_browser = false, g_room_view = false;
std::uint64_t g_selected_room = 0;
stv::net::LobbyAddress g_selected_room_address{};
int g_lobby_last_state = -1;
std::string g_lobby_last_rooms, g_lobby_last_members;

// Opt-in, bounded evidence for real window/menu/lobby tests. No game data or
// settings are changed by this journal. Ordinary player launches create no log.
const std::wstring& frontend_report_path() {
    static const std::wstring path = [] {
        wchar_t value[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"STV_FRONTEND_REPORT", value, MAX_PATH);
        return length && length < MAX_PATH ? std::wstring(value) : std::wstring{};
    }();
    return path;
}
void frontend_event(const char* event, std::uint64_t value, const std::string& detail) {
    const auto& path = frontend_report_path();
    static unsigned count = 0;
    if (path.empty() || count >= 4096) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") != 0 || !file) return;
    std::string escaped;
    for (const unsigned char c : detail) {
        if (c == '\\' || c == '"') { escaped += '\\'; escaped += char(c); }
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else if (c >= 32) escaped += char(c);
    }
    std::fprintf(file, "{\"event\":\"%s\",\"pid\":%lu,\"time_ms\":%llu,\"value\":%llu,\"detail\":\"%s\"}\n",
                 event, GetCurrentProcessId(), (unsigned long long)GetTickCount64(),
                 (unsigned long long)value, escaped.c_str());
    std::fclose(file);
    ++count;
}

void connect_machine_callbacks() {
    g_m.enable_sprite_coverage();
    g_camera = {};
    g_enemy_remains={};
    g_audio_peaks.reset();
    g_enhance_effects.reset_room_cinema();
    // In online play this coupled machine executes confirmed inputs only.
    g_m.on_object=[](stv::gsp::ObjectEvent event,u32 node,u32 partner){
        if((g_settings.enhance && (g_settings.explosive_debris || g_settings.persistent_gore ||
            g_settings.destruction || g_settings.blood || g_settings.particles)) || g_settings.room_cinema)
            g_enemy_remains.observe(g_m.ram,event,node,g_m.c.cyc,g_m.gfx,g_m.pens,partner);
    };
    g_sched->on_base = [](const stv::snd::Atto& base) {
        const std::size_t first = g_pcm.size();
        g_snd.ym_update(base);
        g_mixer->render_until(base, g_pcm);
        if (g_net.audio_audit.enabled) {
            const std::size_t count = g_pcm.size() - first;
            g_net.audio_audit.generated(base.sec, base.as, count ? g_pcm.data() + first : nullptr, count,
                                       g_net.session ? g_net.session->confirmed() : 0);
        }
    };
    g_m.on_vblank = [] {
        if (!g_script1.empty()) apply_script(g_m.frame_number);
        g_achievements.frame(g_m, g_net.on);
        g_frame_done = true;
        g_sched->request_stop();
    };
}

void configure_net_diagnostics() {
    wchar_t env[256];
    if (GetEnvironmentVariableW(L"STV_NET_FRAMES", env, 256)) g_net.exit_after = u32(_wtoi(env));
    if (GetEnvironmentVariableW(L"STV_NET_INPUTS", env, 256)) {
        std::string path;
        for (wchar_t* p = env; *p; ++p) path.push_back(char(*p));
        std::vector<stv::net::PlayerInput> p1, p2;
        if (stv::net::load_scripted_inputs(path, 1u << 22, p1, p2)) g_net.script = g_net.host ? p1 : p2;
    }
    wchar_t audit[8]{};
    const bool enabled = GetEnvironmentVariableW(L"STV_NET_AUDIO_AUDIT", audit, 8) == 1 && audit[0] == L'1';
    g_net.audio_audit.start(enabled, g_mixer->out_rate(), g_net.exit_after);
    wchar_t checkpoints[8]{};
    g_net.checkpoint_audit = g_net.exit_after &&
        GetEnvironmentVariableW(L"STV_NET_CHECKPOINT_AUDIT", checkpoints, 8) == 1 && checkpoints[0] == L'1';
    g_net.fault_injection.configure(0, stv::net::RollbackSession::kHashEvery);
    wchar_t trace[64]{}, extra{};
    const DWORD size = GetEnvironmentVariableW(L"STV_NET_AUDIO_TRACE", trace, 64);
    if (size) {
        unsigned first = 0, end = 0;
        g_net.trace_invalid = size >= 64 || std::swscanf(trace, L"%u,%u%c", &first, &end, &extra) != 2 ||
            !enabled || end <= first || end - first > 4096 || (g_net.exit_after && first >= g_net.exit_after);
        if (!g_net.trace_invalid) {
            g_net.trace_enabled = true; g_net.trace_first = first; g_net.trace_end = end;
            g_net.audio_trace.reserve(end - first);
        }
    }
}

bool parse_net_args() {
    std::wstring spec;
    wchar_t env[256];
    if (GetEnvironmentVariableW(L"STV_NET", env, 256)) spec = env;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--host") spec = L"host:" + std::wstring(i + 1 < argc && argv[i + 1][0] != L'-' ? argv[++i] : L"27015");
        if (a == L"--join" && i + 1 < argc) spec = L"join:" + std::wstring(argv[++i]);
    }
    if (argv) LocalFree(argv);
    if (spec.empty()) return false;
    std::string s;
    for (wchar_t c : spec) s.push_back(char(c));
    g_net.on = true;
    g_net.host = s.rfind("host", 0) == 0;
    std::string rest = s.substr(s.find(':') == std::string::npos ? s.size() : s.find(':') + 1);
    if (g_net.host) {
        if (!rest.empty()) g_net.port = uint16_t(std::atoi(rest.c_str()));
    } else {
        const std::size_t c = rest.rfind(':');
        g_net.peer = c == std::string::npos ? rest : rest.substr(0, c);
        if (c != std::string::npos) g_net.port = uint16_t(std::atoi(rest.c_str() + c + 1));
    }
    configure_net_diagnostics();
    return true;
}

std::string net_machine_observation() {
    u32 ram_hash = 2166136261u;
    for (u16 word : g_m.ram) ram_hash = (ram_hash ^ word) * 16777619u;
    const auto base = g_sched->base(), target = g_sched->target();
    char text[2048]{};
    std::snprintf(text, sizeof text,
        "{\"pc\":\"%08x\",\"cycles\":%llu,\"frame\":%llu,\"ram_hash\":\"%08x\","
        "\"external_timers\":%s,\"check_requested\":%s,\"next_event_cycle\":%llu,\"run_until_cycle\":%llu,"
        "\"scan_next_ps\":%llu,\"vblank_next_ps\":%llu,\"vblank_end_ps\":%llu,\"dma_done_ps\":%llu,"
        "\"scheduler_base_sec\":%lld,\"scheduler_base_as\":%lld,\"scheduler_target_sec\":%lld,\"scheduler_target_as\":%lld,"
        "\"sound_cycles\":%llu,\"sound_due\":%llu,\"audio_io_accesses\":%llu,\"audio_io_hash\":\"%016llx\","
        "\"pcm_generated\":%llu,\"pcm_hash\":\"%016llx\"}",
        g_sched->main_pc(), (unsigned long long)g_m.c.cyc, (unsigned long long)g_m.frame_number, ram_hash,
        g_m.external_timers ? "true" : "false", g_m.check_requested ? "true" : "false",
        (unsigned long long)g_m.next_event_cycle, (unsigned long long)g_m.run_until_cycle,
        (unsigned long long)g_m.scan_next_ps, (unsigned long long)g_m.vblank_next_ps,
        (unsigned long long)g_m.vblank_end_ps, (unsigned long long)g_m.dma_done_ps,
        (long long)base.sec, (long long)base.as, (long long)target.sec, (long long)target.as,
        (unsigned long long)g_snd.cpu.cyc, (unsigned long long)g_snd.cpu.due,
        (unsigned long long)g_net.audio_audit.io_accesses, (unsigned long long)g_net.audio_audit.io_hash,
        (unsigned long long)g_net.audio_audit.pcm_generated, (unsigned long long)g_net.audio_audit.pcm_hash);
    return text;
}

void net_trace_report(FILE* file) {
    std::fprintf(file, "{\"enabled\":%s,\"first_frame\":%u,\"end_frame_exclusive\":%u,"
                 "\"resync_trace_truncated\":%s,\"records\":[",
                 g_net.trace_enabled ? "true" : "false", g_net.trace_first, g_net.trace_end,
                 g_net.resync_trace_truncated ? "true" : "false");
    bool first = true;
    for (const auto& row : g_net.audio_trace) {
        std::fprintf(file, "%s{\"frame\":%u,\"io_accesses\":%llu,\"io_hash\":\"%016llx\","
                     "\"pcm_generated\":%llu,\"pcm_hash\":\"%016llx\",\"mixer_callbacks\":%llu}",
                     first ? "" : ",", row.frame, (unsigned long long)row.io_accesses,
                     (unsigned long long)row.io_hash, (unsigned long long)row.pcm_generated,
                     (unsigned long long)row.pcm_hash, (unsigned long long)row.mixer_callbacks);
        first = false;
    }
    std::fputs("],\"resyncs\":[", file); first = true;
    for (const auto& row : g_net.resync_trace) {
        std::fprintf(file, "%s{\"base\":%u,\"loaded\":%s,\"before\":%s,\"after\":%s}",
                     first ? "" : ",", row.base, row.loaded ? "true" : "false",
                     row.before.c_str(), row.after.c_str());
        first = false;
    }
    std::fputs("]}", file);
}

// The main-only rollback machine owns its own resync. Confirmed input history
// is immutable, so the independent coupled playback owner keeps its state/time.
void net_confirmed_resync_notice(u32 base) {
    if (g_net.checkpoint_audit) g_net.checkpoint_resync_bases.push_back(base);
    if (g_net.first_resync_base == ~0u) g_net.first_resync_base = base;
    g_net.resync_base = base;
}

void observe_confirmed_resync() {
    if (g_net.played != g_net.resync_base) return;
    if (g_net.trace_enabled) {
        const std::string before = net_machine_observation();
        if (g_net.resync_trace.size() < 8)
            g_net.resync_trace.push_back({g_net.resync_base, false, before, net_machine_observation()});
        else g_net.resync_trace_truncated = true;
    }
    g_net.resync_base = ~0u;
}

// The bus observer also receives video/CMOS control writes. Only the sound
// command port belongs to the board; use the same guard as the native recorder.
void connect_confirmed_sound() {
    g_m.on_sound_write = [](u32 address, u16 data) {
        if (address >= 0x01e00000u && address < 0x01e00020u) {
            g_sched->main_sound_write(data);
            if (g_voices.running()) {
                const u32 channel = g_m.c.a[5];   // SNDSND: A5 = logical sound channel
                g_voices.sound_write(stv::game::CoScheduler::main_time(g_m.c.cyc), channel <= 3 ? int(channel) : 0, data);
            }
        }
    };
    // SNDLD: which sounds the retail loader accepts, and which it discards.
    g_m.on_sound_probe = [](int probe) {
        if (!g_voices.running()) { g_probe_pending = false; return; }
        if (probe == 0) { g_probe_table = g_m.c.a[0]; g_probe_pending = true; return; }
        if (!g_probe_pending) return;
        g_probe_pending = false;
        const u32 table = g_probe_table;
        if (probe == 1) {
            g_voices.sound_accepted(int((g_m.rw(table) >> 8) & 3), table);
            return;
        }
        std::vector<u16> words;
        for (u32 i = 0; i < 64; ++i) {
            const u16 w = g_m.rw(table + i * 16);
            words.push_back(w);
            if (!w) break;
        }
        g_voices.sound_dropped(stv::game::CoScheduler::main_time(g_m.c.cyc), words, table);
    };
}

// One confirmed frame of sound to the device. With every enhancement off this
// is exactly the retail mono path. Otherwise the extra sound boards and the
// output chain shape a stereo copy; g_pcm (audits, hashes) is never changed.
void deliver_audio(bool to_device) {
    if (g_settings.room_cinema) g_audio_peaks.observe(g_pcm);
    else g_audio_peaks.reset();
    const stv::host::AudioFxSettings want{g_settings.audio_hifi, g_settings.audio_wide, g_settings.reverb};
    const bool extended = g_settings.extended_sound && g_mixer && g_sched;
    if (extended && g_voices.running()) {
        // A reset, snapshot load or long pause moved the board: realign.
        const auto base = g_sched->base();
        const auto vb = g_voices.base();
        const bool behind = vb.sec + 1 < base.sec, ahead = base < vb;
        if (behind || ahead) g_voices.stop();
    }
    if (extended && !g_voices.running() && g_snd.fault.empty())
        ++g_voice_starts, g_voices.start(g_snd, *g_mixer, g_sched->base(), stv::snd::VoiceBank::kMaxEffectVoices);
    if (!extended && g_voices.running()) g_voices.stop();
    if (!extended && !want.any()) {
        if (to_device) g_audio.push(g_pcm.data(), g_pcm.size());
        return;
    }
    if (!g_fx_configured || !(want.hifi == g_fx_applied.hifi && want.wide == g_fx_applied.wide && want.reverb == g_fx_applied.reverb)) {
        g_fx.configure(g_audio.ok() ? g_audio.rate() : 48000, want);
        g_fx_applied = want;
        g_fx_configured = true;
    }
    const std::size_t frames = g_pcm.size();
    if (g_voices.running()) {
        g_voices.tick(g_sched->base());
        g_voices.render(g_sched->base(), frames, g_stereo);
        if (!g_voices.fault().empty()) {           // never lose sound: fall back to the retail mix
            g_settings.extended_sound = false;
            g_toast = "Extended sound stopped: " + g_voices.fault();
            g_toast_until = GetTickCount64() + 5000;
        }
    }
    if (!g_voices.running()) {
        g_stereo.resize(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) g_stereo[2 * i] = g_stereo[2 * i + 1] = g_pcm[i];
    }
    g_fx.process(g_stereo.data(), frames);
    if (to_device) g_audio.push_stereo(g_stereo.data(), frames);
}

void write_net_checkpoints(FILE* file) {
    std::fprintf(file, "{\"schema\":\"smash.net.periodic-ram.v1\",\"enabled\":%s,"
                 "\"frames_requested\":%u,\"period\":%u,\"checkpoints\":[",
                 g_net.checkpoint_audit ? "true" : "false", g_net.exit_after,
                 unsigned(stv::net::RollbackSession::kHashEvery));
    bool first = true;
    for (const auto& item : g_net.periodic_hashes) {
        std::fprintf(file, "%s[%u,\"%08x\"]", first ? "" : ",", item.first, item.second);
        first = false;
    }
    std::fputs("],\"resync_bases\":[", file);
    first = true;
    for (const u32 base : g_net.checkpoint_resync_bases) {
        std::fprintf(file, "%s%u", first ? "" : ",", base);
        first = false;
    }
    std::fputs("]}", file);
}

void net_report(const char* why) {
    observe_audio_output(true);
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    FILE* f = nullptr;
    const std::wstring name = std::wstring(path) + (g_net.host ? L"smashtv_net_host.json" : L"smashtv_net_join.json");
    if (_wfopen_s(&f, name.c_str(), L"w") != 0 || !f) return;
    const auto& st = g_net.session->stats();
    std::fprintf(f,
                 "{\"role\": \"%s\", \"end\": \"%s\", \"confirmed\": %u, \"frame\": %u, \"played\": %u, \"last_hash\": \"%08x\", "
                 "\"rollbacks\": %llu, \"resimulated\": %llu, \"stalls\": %llu, \"desyncs\": %llu, \"hash_checks\": %llu, "
                 "\"resyncs\": %llu,",
                 g_net.host ? "host" : "join", why, g_net.session->confirmed(), g_net.session->frame(), g_net.played,
                 g_net.last_hash, (unsigned long long)st.rollbacks, (unsigned long long)st.resimulated,
                 (unsigned long long)st.stalls, (unsigned long long)st.desyncs, (unsigned long long)st.hashes_checked,
                 (unsigned long long)st.resyncs);
    std::fprintf(f, "\"time_sync_waits\":%llu,\"frame_advantage\":%d,", (unsigned long long)st.time_sync_waits,
                 g_net.session->frame_advantage());
    std::fprintf(f, "\"audio_underruns\":%llu,\"audio_output_resyncs\":%llu,"
                    "\"audio_dropped_source_samples\":%llu,\"audio_dropped_device_frames\":%llu,"
                    "\"audio_pending_limit_samples\":%u,\"audio_max_pending_samples\":%u,",
                 (unsigned long long)g_audio.underruns(),
                 (unsigned long long)g_audio.output_resyncs(),
                 (unsigned long long)g_audio.dropped_source_samples(),
                 (unsigned long long)g_audio.dropped_device_frames(),
                 g_audio.pending_limit_samples(), g_audio.maximum_pending_samples());
    write_audio_recovery(f);
    std::fputs("\"first_desync_frame\":", f);
    if (g_net.first_desync_frame == ~0u) std::fputs("null", f);
    else std::fprintf(f, "%u", g_net.first_desync_frame);
    std::fputs(",\"first_resync_base\":", f);
    if (g_net.first_resync_base == ~0u) std::fputs("null", f);
    else std::fprintf(f, "%u", g_net.first_resync_base);
    std::fputs(",", f);
    std::fprintf(f, "\"audio_endpoint_available\":%s,\"audio_device_errors\":%llu,"
                    "\"sound_fault\":%s,\"main_stopped\":%s,\"predictive_stopped\":%s,\"audio_audit\":",
                 g_audio.ok() ? "true" : "false", (unsigned long long)g_audio.errors(),
                 g_snd.fault.empty() ? "false" : "true", g_m.stop ? "true" : "false",
                 (g_net.mr && g_net.mr->stop) ? "true" : "false");
    g_net.audio_audit.write_json(f);
    std::fputs(",\"fault_injection\":", f);
    g_net.fault_injection.write_json(f);
    std::fputs(",\"periodic_checkpoints\":", f);
    write_net_checkpoints(f);
    std::fputs(",\"audio_trace\":", f);
    net_trace_report(f);
    std::fputs(",\"audio_output_timeline\":", f);
    g_audio_telemetry.write_json(f);
    std::fputs("}\n", f);
    std::fclose(f);
}

// ---- Drop-in join: exact coupled snapshot ---------------------------------
// The host sends its live game (main machine, sound board, scheduler, mixer)
// instead of the input history from power-on: joining takes about a second
// however long the room has been playing, and the host never re-simulates.
constexpr u16 kSnapshotTag[2] = {0x5453, 0x4e53};   // "STSN"
std::vector<u8> coupled_snapshot() {
    std::vector<u8> main, out;
    g_m.save_state(main, g_sched->main_pc(), false);
    const u64 size = main.size();
    out.insert(out.end(), reinterpret_cast<const u8*>(&size), reinterpret_cast<const u8*>(&size) + sizeof size);
    out.insert(out.end(), main.begin(), main.end());
    g_snd.save_state(out);
    g_sched->save_state(out);
    g_mixer->save_state(out);
    return out;
}
// Split the main-machine part (also the rollback machine's start) from the rest.
bool snapshot_main(const std::vector<u8>& snap, std::vector<u8>& main) {
    u64 size = 0;
    if (snap.size() < sizeof size) return false;
    std::memcpy(&size, snap.data(), sizeof size);
    if (size > snap.size() - sizeof size) return false;
    main.assign(snap.begin() + sizeof size, snap.begin() + std::ptrdiff_t(sizeof size + size));
    return true;
}
bool load_coupled_snapshot(const std::vector<u8>& snap) {
    g_proof.taint();   // a joined snapshot has no controls history from power-on
    std::vector<u8> main;
    if (!snapshot_main(snap, main)) return false;
    u32 pc = 0;
    if (!g_m.load_state(main, pc)) return false;
    const u8* p = snap.data() + sizeof(u64) + main.size();
    const u8* end = snap.data() + snap.size();
    if (!g_snd.load_state(p, end) || !g_sched->load_state(p, end) || !g_mixer->load_state(p, end) || p != end) return false;
    g_started = true;
    g_pcm.clear();
    return true;
}
void append_snapshot(std::vector<u16>& words, const std::vector<u8>& snap) {
    const u32 n = u32(snap.size());
    words.insert(words.end(), {kSnapshotTag[0], kSnapshotTag[1], u16(n), u16(n >> 16)});
    for (std::size_t i = 0; i < snap.size(); i += 2)
        words.push_back(u16(snap[i] | (i + 1 < snap.size() ? snap[i + 1] << 8 : 0)));
}
bool extract_snapshot(const std::vector<u16>& words, std::size_t at, std::vector<u8>& snap) {
    if (words.size() < at + 4 || words[at] != kSnapshotTag[0] || words[at + 1] != kSnapshotTag[1]) return false;
    const u32 n = u32(words[at + 2]) | (u32(words[at + 3]) << 16);
    if ((std::size_t(n) + 1) / 2 != words.size() - at - 4) return false;
    snap.resize(n);
    for (u32 i = 0; i < n; ++i) snap[i] = u8(words[at + 4 + i / 2] >> ((i & 1) * 8));
    return true;
}

// Connect, check compatibility, agree on the host's CMOS and set up both machines.
bool start_rollback_session();
bool net_connect(const std::filesystem::path& zip) {
    struct Connecting {
        Connecting() { g_connecting = true; g_cancel_connect = false; }
        ~Connecting() { g_connecting = false; }
    } connecting;
    if (!g_net.host) g_net.local_cmos = g_m.cmos;
    const bool resume_host = g_net.host && g_host_solo;
    std::vector<u16> agreed_cmos = g_m.cmos;
    const auto cmos_words = agreed_cmos.size();
    std::vector<u8> join_snapshot;
    if (resume_host) {
        join_snapshot = coupled_snapshot();
        append_snapshot(agreed_cmos, join_snapshot);
    }
    std::string err;
    if (g_net.trace_invalid) {
        MessageBoxA(g_wnd, "Online audio trace needs an enabled audio audit and an interval of 1 to 4096 frames",
                    "Smash T.V.", MB_ICONERROR);
        return false;
    }
    if (g_net.audio_audit.enabled) {
        if (g_snd.on_access) {
            MessageBoxA(g_wnd, "Online audio audit cannot replace an existing sound I/O observer",
                        "Smash T.V.", MB_ICONERROR);
            return false;
        }
        g_snd.on_access = [](const stv::snd::Access& a) {
            g_net.audio_audit.access(a.kind, a.t.sec, a.t.as, a.addr, a.data, a.pc,
                                    g_net.session ? g_net.session->confirmed() : 0);
        };
    }
    if (!g_net.udp) {
        auto direct = std::make_unique<stv::net::UdpTransport>();
        if (!direct->bind(g_net.host ? g_net.port : 0, err) ||
            (!g_net.host && !direct->set_peer(g_net.peer, g_net.port, err))) {
            g_menu_status = "Connection failed: " + err;
            if (!g_lobby) MessageBoxA(g_wnd,g_menu_status.c_str(),"Smash T.V.",MB_ICONERROR);
            return false;
        }
        g_net.udp = std::move(direct);
    }
    stv::net::Handshake hs(g_net.host, stv::net::fingerprint(g_m), agreed_cmos, bool(g_lobby));
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    double transfer_start = -1;
    int transfer_done = -1;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(int(msg.wParam)); // the outer loop still owns application shutdown
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        service_lobby();
        poll_pad_controls();
        if (g_cancel_connect) return false;
        if (g_lobby && g_lobby->state() != stv::net::LobbyState::Handshaking &&
            g_lobby->state() != stv::net::LobbyState::Starting &&
            g_lobby->state() != stv::net::LobbyState::Playing) {
            g_menu_status = g_lobby->problem().empty() ? "The room closed during connection." : g_lobby->problem();
            return false;
        }
        const auto st = hs.step(*g_net.udp);
        QueryPerformanceCounter(&now);
        const double secs = double(now.QuadPart - t0.QuadPart) / double(freq.QuadPart);
        if (st == stv::net::Handshake::State::Ready) break;
        if (st == stv::net::Handshake::State::Failed) {
            g_menu_status = hs.error();
            if (!g_lobby) MessageBoxA(g_wnd,g_menu_status.c_str(),"Smash T.V.",MB_ICONERROR);
            return false;
        }
        if (st == stv::net::Handshake::State::Transferring && transfer_done != hs.chunks_done()) {
            transfer_done = hs.chunks_done(); transfer_start = secs;
        }
        if ((!g_net.host && secs > 30 && st == stv::net::Handshake::State::Waiting) || (g_lobby && transfer_start < 0 && secs > 45) ||
            (transfer_start >= 0 && secs - transfer_start > 45)) {
            g_menu_status = g_epic_room ? "The other player stopped responding. Try joining again." : "Connection timed out. Check the host address and UDP ports.";
            if (!g_lobby) MessageBoxA(g_wnd,g_menu_status.c_str(),"Smash T.V.",MB_ICONERROR);
            return false;
        }
        g_status = g_net.host ? "Hosting on UDP port " + std::to_string(g_net.port) + ", waiting for player 2" +
                                    (st == stv::net::Handshake::State::Transferring
                                         ? " (sending settings " + std::to_string(hs.chunks_done()) + "/" + std::to_string((agreed_cmos.size()+511)/512) + ")" : "")
                              : "Joining " + g_net.peer + ":" + std::to_string(g_net.port) +
                                    (st == stv::net::Handshake::State::Transferring
                                         ? " (receiving settings " + std::to_string(hs.chunks_done()) + "/" + std::to_string((agreed_cmos.size()+511)/512) + ")" : "...");
        if (g_epic_room) {
            g_status = "Connecting to " + g_net.contestant;
            if (st==stv::net::Handshake::State::Transferring)
                g_status = std::string(g_net.host ? "Sending" : "Receiving") + " game history  " +
                    std::to_string(hs.chunks_done()) + " / " + std::to_string((agreed_cmos.size()+511)/512);
        }
        g_paused = true;   // paint shows g_status
        g_status += "  (Esc / B cancels)";
        InvalidateRect(g_wnd, nullptr, FALSE);
        Sleep(15);
    }
    g_paused = false;
    g_status.clear();
    // the rollback machine: same program image, same (host's) CMOS
    g_net.mr = std::make_unique<Machine>();
    if (!stv::gsp::load_imported_roms(*g_net.mr, zip, err)) return false;
    if (g_enhanced) {
        g_net.mr->enhanced = true;
        g_net.mr->set_wide(1024, 137);
        if (!stv::gsp::apply_enhanced(*g_net.mr, 137)) {
            err = "Enhanced profile table does not match the L8 data";
            MessageBoxA(g_wnd, err.c_str(), "Smash T.V.", MB_ICONERROR);
            return false;
        }
    }
    g_net.runner = std::make_unique<stv::net::FrameRunner>(*g_net.mr);
    g_net.runner->start();
    // Drop-in: start both machines from the host's live game.
    if (agreed_cmos.size() > cmos_words) {
        std::vector<u8> snapshot, main;
        if (!extract_snapshot(agreed_cmos, cmos_words, snapshot) || !snapshot_main(snapshot, main) ||
            !g_net.runner->load(main, 0)) {
            g_menu_status = "The host's game could not be loaded.";
            return false;
        }
        g_net.mr->external_timers = false;   // the rollback machine runs main-only
        g_net.mr->check_requested = false;
        g_net.mr->recompute_next_event();
        if (!g_net.host && !load_coupled_snapshot(snapshot)) {
            g_menu_status = "The host's game could not be loaded.";
            return false;
        }
        frontend_event("room_game_ready", 0, std::string(g_enhanced ? "wide/" : "arcade/") + "snapshot");
        g_net.runner->begin_session();
        g_net.auto_join = true;
        return start_rollback_session();
    }
    if ((agreed_cmos.size() - cmos_words) % 7) return false;
    u64 history_frames = 0;
    for (std::size_t i = cmos_words + 4; i < agreed_cmos.size(); i += 7) {
        if (!agreed_cmos[i] || agreed_cmos[i+1] > 1) return false;
        history_frames += agreed_cmos[i];
    }
    if (history_frames > 8000000) return false;
    std::vector<u16> boot_cmos(agreed_cmos.begin(), agreed_cmos.begin() + cmos_words);
    g_net.mr->cmos = boot_cmos;
    if (g_lobby && g_net.host && !resume_host) {
        g_room_boot_cmos = boot_cmos;
        g_room_history.clear(); g_room_frames = 0;
    }
    if (!resume_host) g_m.cmos = boot_cmos;
    for (std::size_t i = cmos_words; i < agreed_cmos.size(); i += 7) {
        const u32 p1 = agreed_cmos[i] | (u32(agreed_cmos[i+1]) << 16);
        const u32 p2 = agreed_cmos[i+2] | (u32(agreed_cmos[i+3]) << 16);
        for (unsigned repeat = 0; repeat < agreed_cmos[i+4]; ++repeat) {
            const bool before = agreed_cmos[i+5] != 0;
            if (before) {
                stv::net::apply_inputs(*g_net.mr,p1,p2);
                g_net.mr->ports[1] = agreed_cmos[i+6];
                if (!resume_host) {
                    stv::net::apply_inputs(g_m,p1,p2);
                    g_m.ports[1] = agreed_cmos[i+6];
                }
            }
            g_net.runner->step(p1,p2);
            if (before) g_net.mr->ports[1] = agreed_cmos[i+6];
            if (!resume_host) {
                run_one_frame();
                if (!before) stv::net::apply_inputs(g_m,p1,p2);
                g_pcm.clear();
            }
            if (g_net.runner->frame() % 64 == 0) {
                g_status = "Joining game - catching up " + std::to_string(g_net.runner->frame()) + " / " + std::to_string(history_frames);
                InvalidateRect(g_wnd,nullptr,FALSE);
                service_lobby();
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) { PostQuitMessage(int(msg.wParam)); return false; }
                    TranslateMessage(&msg); DispatchMessageW(&msg);
                }
                if (g_cancel_connect || g_lobby->state() == stv::net::LobbyState::Idle) return false;
            }
        }
    }
    g_status.clear();
    if (g_lobby) {
        u32 coupled_hash = 2166136261u;
        for (auto word : g_m.ram) coupled_hash = (coupled_hash ^ word) * 16777619u;
        frontend_event("room_game_ready", history_frames,
            std::string(g_enhanced ? "wide/" : "arcade/") + std::to_string(coupled_hash));
    }
    g_net.runner->begin_session();
    g_net.auto_join = !g_net.host;
    return start_rollback_session();
}

bool start_rollback_session() {
    g_net.session = std::make_unique<stv::net::RollbackSession>(*g_net.runner, *g_net.udp, g_net.host ? 1 : 2);
    g_net.session->on_confirmed = [](u32 f, u32 h) {
        if (!g_net.exit_after || f + 1 == g_net.exit_after) g_net.last_hash = h;
        if (g_net.checkpoint_audit && f < g_net.exit_after && f % stv::net::RollbackSession::kHashEvery == 0)
            g_net.periodic_hashes.emplace_back(f, h);
    };
    g_net.session->on_desync = [](u32 f, u32, u32) {
        if (g_net.first_desync_frame == ~0u) g_net.first_desync_frame = f;
        g_status = "Online game: the two machines differed at frame " + std::to_string(f) + "; resynchronising from the host";
        InvalidateRect(g_wnd, nullptr, FALSE);
    };
    // RollbackSession has already restored the predicted/displayed FrameRunner.
    // Its main-only snapshot cannot restore the coupled confirmed-audio owner.
    g_net.session->on_resync = [](u32 base, const std::vector<u8>&) {
        net_confirmed_resync_notice(base);
    };
    // Automation mutates one bit at the first exchanged-hash frame >= F.
    // Every replay repeats it from the restored before-frame state; subsequent
    // frames and all activity after/during resync are left alone.
    wchar_t fault[32]{};
    const DWORD fault_size = GetEnvironmentVariableW(L"STV_NET_FAULT", fault, 32);
    if (!g_net.host && fault_size) {
        wchar_t* end = nullptr;
        const bool digits = fault_size < 32 && fault[0] >= L'1' && fault[0] <= L'9';
        const unsigned long from = digits ? std::wcstoul(fault, &end, 10) : 0;
        if (!from || !end || *end ||
            !g_net.fault_injection.configure(u32(from), stv::net::RollbackSession::kHashEvery) ||
            (g_net.exit_after && g_net.fault_injection.effective_frame >= g_net.exit_after)) {
            MessageBoxA(g_wnd, "Online fault test needs a positive frame whose exchanged hash is inside the run",
                        "Smash T.V.", MB_ICONERROR);
            return false;
        }
        g_net.session->after_step = [](u32 f) {
            g_net.fault_injection.after_step(f, g_net.session->stats().resyncs, g_net.session->resyncing(),
                                            g_net.mr->ram[0x7ff0], [] { return g_net.runner->ram_hash(); });
        };
    }
    // Commit only after every handshake/setup check succeeded. Partial or
    // cancelled transfers leave the coupled owner's CMOS unchanged.
    g_host_solo = g_record_local = false;
    return true;
}

// One shared pause boundary owns the output discard. A peer can resume inside
// the first of several owed protocol ticks, before the outer loop observes it.
// Consume that boundary before resumed PCM, including if output is unavailable.
template<class Discard>
void sync_audio_pause(bool paused, bool& was_paused, Discard&& discard) {
    if (paused) { was_paused = true; return; }
    if (!was_paused) return;
    was_paused = false;
    discard();
}
void sync_audio_pause(bool paused, bool& was_paused) {
    sync_audio_pause(paused, was_paused, [] {
        if (g_audio.ok()) (void)g_audio.discard_pending();
    });
}

// One frame of online play: advance the rollback machine with this PC's input,
// exchange inputs, then play every newly confirmed frame on the sound machine.
void net_tick(bool audio, bool& output_was_paused) {
    if (g_net.ended) return;
    auto& s = *g_net.session;
    s.set_paused(g_paused || g_menu);   // a pause on either PC holds both
    const u32 f = s.frame();
    const stv::net::PlayerInput local = !g_net.script.empty() ? (f < g_net.script.size() ? g_net.script[f] : 0)
                                                              : local_player_input();
    if (s.tick(local)) ++g_simulation_steps;
    const bool paused = g_paused || g_menu || s.paused_by_peer();
    sync_audio_pause(paused, output_was_paused);
    observe_confirmed_resync();
    while (g_net.played < s.confirmed()) {
        observe_confirmed_resync();
        stv::net::PlayerInput p1, p2;
        s.confirmed_inputs(g_net.played, p1, p2);
        const u32 playing = g_net.played;
        if (g_net.audio_audit.enabled) g_net.audio_audit.begin_frame(playing, s.confirmed(), g_pcm.size());
        run_one_frame();                       // to vblank 'played'
        stv::net::apply_inputs(g_m, p1, p2);   // that frame's inputs from its vblank on, as FrameRunner does
        if (g_lobby && g_net.host) record_room_frame(p1,p2);
        ++g_net.played;
        const bool accepted = audio && g_audio.ok();
        deliver_audio(accepted);
        if (accepted) {
            // Drain each confirmed block before another can fill the bounded
            // software queue. The sound owner still advances only once here.
            if (!paused) g_audio.pump();
        }
        if (g_net.audio_audit.enabled) {
            g_net.audio_audit.output(g_pcm.data(), g_pcm.size(), audio, accepted, s.confirmed());
            g_net.audio_audit.end_frame(playing, g_frame_done, !g_m.stop && g_snd.fault.empty());
        }
        if (g_net.trace_enabled && playing >= g_net.trace_first && playing < g_net.trace_end) {
            const auto& a = g_net.audio_audit;
            g_net.audio_trace.push_back({playing, a.io_accesses, a.io_hash, a.pcm_generated, a.pcm_hash, a.mixer_callbacks});
        }
        g_pcm.clear();
        observe_audio_output();
        if (!g_net.audio_audit.good()) break;
    }
    if (!g_net.audio_audit.good()) {
        net_report("audio_audit_failure");
        g_net.ended = true;
        PostQuitMessage(4);
        return;
    }
    if (s.pumps() - s.last_receive_tick() > 5 * 55) {
        g_status = "Online game: no data from the other player for 5 seconds; waiting for the connection to come back";
        InvalidateRect(g_wnd, nullptr, FALSE);
    } else if (s.paused_by_peer()) {
        g_status = "Online game: paused by the other player";
        InvalidateRect(g_wnd, nullptr, FALSE);
    } else if (!g_net.ended && g_status.rfind("Online game: ", 0) == 0 && g_status.find("stopped") == std::string::npos) {
        g_status.clear();
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
    if (g_net.exit_after && s.confirmed() >= g_net.exit_after) {
        net_report("frames");
        g_net.ended = true;
        PostQuitMessage(0);
    }
}

// the enhanced profile keeps its own CMOS (high scores and audits differ from the original game)
std::wstring cmos_path() { return g_data_dir + (g_enhanced ? L"\\cmos-l8-16x9.bin" : L"\\cmos-l8.bin"); }
std::wstring profile_path() { return g_data_dir + L"\\profile.cfg"; }
bool load_profile() {
    if (g_launch_mode >= 0) return g_launch_mode == 1;
    wchar_t buf[32];
    if (GetEnvironmentVariableW(L"STV_PROFILE", buf, 32)) return std::wcscmp(buf, L"enhanced") == 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, profile_path().c_str(), L"r") != 0 || !f) return true;
    char line[64] = "";
    const bool enh = std::fgets(line, sizeof line, f) && std::strncmp(line, "enhanced", 8) == 0;
    std::fclose(f);
    return enh;
}

std::filesystem::path preferences_path() { return std::filesystem::path(g_data_dir) / L"pc-settings.cfg"; }
void load_preferences() {
    std::string error;
    if (stv::host::load_pc_settings(preferences_path(), g_settings, error) == stv::host::SettingsLoad::Invalid)
        g_menu_status = error + "; using defaults.";
    g_scale_mode = static_cast<stv::host::ScaleMode>(g_settings.scaling);
    g_swap_pads = g_settings.swap_pads;
    g_audio.set_volume(g_settings.volume);
    g_audio.set_muted(g_settings.muted);
}
void save_preferences() {
    if (g_data_dir.empty() || scripted_run()) return;
    g_settings.scaling = int(g_scale_mode);
    if (!g_transient_fullscreen) g_settings.fullscreen = g_fullscreen;
    g_settings.swap_pads = g_swap_pads;
    std::string error;
    if (!stv::host::save_pc_settings(preferences_path(), g_settings, error)) g_menu_status = error;
}

stv::host::PcMenuContext menu_context() {
    stv::host::PcMenuContext context;
    context.online = g_net.on;
    context.public_host = g_epic_room && g_lobby && g_lobby->hosting();
    context.title_options = g_title_submenu;
#if !defined(STV_PLAYER_RELEASE) || !STV_PLAYER_RELEASE
    context.development_profile_chooser = false;
#endif
    context.gpu_effects = g_settings.filter >= 4;
    context.combat_feedback = g_settings.combat_feedback != 0;
    context.reduce_shake = g_settings.reduce_shake;
    context.reduce_flashing = g_settings.reduce_flashing;
    context.reduce_motion = g_settings.reduce_motion;
    context.set_available(stv::host::PcMenuItem::Vsync, g_dwm_flush != nullptr && !g_settings.smooth_motion);
    context.set_available(stv::host::PcMenuItem::WindowScale, !g_fullscreen);
    // DXR traces actor shadows itself and the renderer disables the 2.5D contact pass.
    context.set_available(stv::host::PcMenuItem::ContactShadows, !g_settings.hardware_rt);
    const bool in_room = g_lobby && g_lobby->state() != stv::net::LobbyState::Idle &&
                         g_lobby->state() != stv::net::LobbyState::Searching;
    if (context.public_host) {
        using I=stv::host::PcMenuItem;
        for (auto item : {I::Connections,I::PublicHost,I::PublicBrowser}) context.set_available(item,false);
    }
    context.set_available(stv::host::PcMenuItem::HostMode, !in_room && !g_net.on);
    context.set_available(stv::host::PcMenuItem::PlayerName, !in_room && !g_net.on);
    return context;
}

void resize_pc_window() {
    if (!g_wnd || g_fullscreen) return;
    RECT work{0, 0, 1280, 960};
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &monitor)) work = monitor.rcWork;
    const int max_w = std::max(320L, work.right - work.left - 32);
    const int max_h = std::max(240L, work.bottom - work.top - 64);
    const int requested_w = g_settings.window_scale ? vis_w() * g_settings.window_scale : max_w * 85 / 100;
    const int requested_h = g_settings.window_scale ? VIS_H * g_settings.window_scale * 6 / 5 : max_h * 85 / 100;
    RECT area{0, 0, std::min(max_w, std::max(640, requested_w)), std::min(max_h, std::max(480, requested_h))};
    AdjustWindowRect(&area, DWORD(GetWindowLongW(g_wnd, GWL_STYLE)), FALSE);
    SetWindowPos(g_wnd, nullptr, 0, 0, area.right - area.left, area.bottom - area.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_wnd, nullptr, FALSE);
}
// Direct connections use the same in-process startup and recovery as LAN rooms.
std::wstring g_join_addr;          // address being typed for "join"; empty when not typing
bool g_join_edit = false;
std::wstring net_cfg_path() { return g_data_dir + L"\\net.cfg"; }
std::wstring last_join_addr() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, net_cfg_path().c_str(), L"r") != 0 || !f) return L"";
    char line[264] = "";
    std::fgets(line, sizeof line, f);
    std::fclose(f);
    std::wstring w;
    for (char* p = line; *p && *p != '\n' && *p != '\r'; ++p) w.push_back(wchar_t(*p));
    return w;
}
bool valid_join_address(const std::wstring& address) {
    const auto colon = address.find(L':');
    const auto host = address.substr(0, colon);
    if (host.empty() || host.size() > 253 || host.find_first_not_of(
            L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::wstring::npos) return false;
    if (colon == std::wstring::npos) return true;
    const auto port = address.substr(colon + 1);
    return !port.empty() && port.size() <= 5 && port.find_first_not_of(L"0123456789") == std::wstring::npos &&
           std::stoul(port) > 0 && std::stoul(port) <= 65535;
}
void paste_join_address(HWND wnd) {
    if (!OpenClipboard(wnd)) return;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const auto* value = static_cast<const wchar_t*>(GlobalLock(data));
        if (value) {
            const auto count = wcsnlen_s(value, std::min<size_t>(GlobalSize(data) / sizeof(wchar_t), 300));
            std::wstring address(value, count);
            const auto first = address.find_first_not_of(L" \t\r\n");
            if (first != std::wstring::npos) {
                address = address.substr(first, address.find_last_not_of(L" \t\r\n") - first + 1);
                if (count < 300 && valid_join_address(address)) {
                    g_join_addr = address;
                    g_menu_status.clear();
                } else g_menu_status = "Paste a hostname or IPv4 address with an optional port (1-65535).";
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
}
bool create_lobby(bool host, bool online, bool epic = false);
void close_lobby();
void start_join() {
    if (!valid_join_address(g_join_addr)) {
        g_menu_status = "Enter a hostname or IPv4 address with an optional port (1-65535).";
        return;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, net_cfg_path().c_str(), L"w") == 0 && f) {
        for (wchar_t c : g_join_addr) std::fputc(char(c), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
    std::string endpoint;
    for (wchar_t c : g_join_addr) endpoint.push_back(static_cast<char>(c));
    if (!create_lobby(false, true)) return;
    const auto colon = endpoint.find(':');
    const auto port = colon == std::string::npos ? 27016 : std::stoul(endpoint.substr(colon + 1));
    std::string error;
    if (!g_lobby_udp.target(endpoint.substr(0, colon), uint16_t(port), error) || !g_lobby->search(GetTickCount64())) {
        close_lobby(); g_menu_status = error; return;
    }
    g_room_browser = true;
    g_join_edit = false;
}

// A scripted replay (STV_HOST_INPUTS, or STV_NET_INPUTS online) starts from power-on
// CMOS like the lockstep harness and leaves the player's saved CMOS alone.
bool scripted_run() {
    return GetEnvironmentVariableW(L"STV_HOST_INPUTS", nullptr, 0) != 0 || GetEnvironmentVariableW(L"STV_NET_INPUTS", nullptr, 0) != 0;
}

void load_cmos() {
    if (scripted_run()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, cmos_path().c_str(), L"rb") != 0 || !f) return;
    std::vector<u16> tmp(g_m.cmos.size());
    if (std::fread(tmp.data(), 2, tmp.size(), f) == tmp.size()) g_m.cmos = tmp;
    std::fclose(f);
}

// Retail ALL_TAB uses eleven eight-byte records in CMOS bank 2. Keep each
// PC's shared-session leaderboard separate from its operator settings.
using RoomScore = std::array<unsigned char,8>;
bool valid_room_score(const RoomScore& row) {
    unsigned sum = 0;
    for (auto byte : row) sum += byte;
    if ((sum & 255) != 255) return false;
    for (int i=0; i<4; ++i) if ((row[i] >> 4)>9 || (row[i]&15)>9) return false;
    return row[0] || row[1] || row[2] || row[3];
}
std::wstring room_score_path() {
    return g_data_dir + (g_enhanced ? L"\\room-scores-wide.bin" : L"\\room-scores-arcade.bin");
}
std::vector<RoomScore> room_scores() {
    std::vector<RoomScore> rows;
    FILE* file = nullptr;
    if (!_wfopen_s(&file,room_score_path().c_str(),L"rb") && file) {
        RoomScore row{};
        while (rows.size()<11 && std::fread(row.data(),1,row.size(),file)==row.size()) {
            if (!valid_room_score(row)) break;
            rows.push_back(row);
        }
        std::fclose(file);
    }
    return rows;
}
void retain_room_scores() {
    if (scripted_run() || !g_net.session || !g_lobby || g_m.cmos.size()<0x2000+88) return;
    const auto saved = room_scores();
    auto rows = saved;
    for (int i=0; i<11; ++i) {
        RoomScore row{};
        for (int j=0; j<8; ++j) row[j]=u8(g_m.cmos[0x2000+i*8+j]);
        if (valid_room_score(row) && std::find(rows.begin(),rows.end(),row)==rows.end()) rows.push_back(row);
    }
    std::stable_sort(rows.begin(),rows.end(),[](const RoomScore& a,const RoomScore& b) {
        return std::lexicographical_compare(b.begin(),b.begin()+4,a.begin(),a.begin()+4);
    });
    if (rows.size()>11) rows.resize(11);
    if (rows==saved) return;
    const auto path=room_score_path(), temp=path+L".tmp";
    FILE* file=nullptr;
    if (_wfopen_s(&file,temp.c_str(),L"wb") || !file) return;
    const bool written=std::fwrite(rows.data(),sizeof(RoomScore),rows.size(),file)==rows.size();
    if (std::fclose(file)==0 && written) MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
// Global online leaderboards. Scores come only from entries the retail game
// itself writes to its high-score tables during local play; the network
// client only fetches while the leaderboard screen is open.
stv::host::GlobalLeaderboardScreen g_global_lb;
rl::lb::Client g_lb_client;
std::array<stv::host::ArcadeScoreWatch,2> g_lb_watch;
std::uint64_t g_lb_revision = 0;
ULONGLONG g_lb_repaint_at = 0;
std::int64_t lb_now() { return std::int64_t(std::time(nullptr)); }
// STV_LEADERBOARD_TEST=1 lets a scripted replay (fresh power-on CMOS, never
// saved) exercise the real capture/upload path; point it at a test server.
bool g_lb_scripted_test = false;
void start_leaderboard(bool automated) {
    g_lb_scripted_test = GetEnvironmentVariableW(L"STV_LEADERBOARD_TEST", nullptr, 0) != 0 &&
                         GetEnvironmentVariableW(L"STV_LEADERBOARD_URL", nullptr, 0) != 0;
    if ((automated && !g_lb_scripted_test) || GetEnvironmentVariableW(L"STV_LEADERBOARD_OFF", nullptr, 0) != 0) return;
    rl::lb::Config config;
    wchar_t env[512]{};
    if (GetEnvironmentVariableW(L"STV_LEADERBOARD_URL", env, DWORD(std::size(env))) && env[0]) config.url = env;
    if (config.url.empty()) {
        FILE* file = nullptr;
        if (!_wfopen_s(&file, (g_data_dir + L"\\leaderboard-url.txt").c_str(), L"rb") && file) {
            char text[512]{};
            const auto n = std::fread(text, 1, sizeof text - 1, file);
            std::fclose(file);
            for (std::size_t i = 0; i < n && text[i] > ' '; ++i) config.url.push_back(wchar_t(text[i]));
        }
    }
    if (config.url.empty()) config.url = STV_LEADERBOARD_URL;
    config.game = "smashtv";
    config.version = "l8-player";
    config.data_dir = g_data_dir + L"\\leaderboard";
    if (!g_lb_client.start(config)) write_player_log("Global leaderboard client could not start.");
}
void leaderboard_after_frame() {
    if (!g_lb_client.started() || g_net.on || (scripted_run() && !g_lb_scripted_test) || g_m.frame_number % 30) return;
    const int board = g_enhanced ? 1 : 0;
    const auto fresh = g_lb_watch[board].update(g_m.cmos);
    if (fresh.empty()) return;
    // Attach the run's controls from power-on (empty when the run is not provable
    // or too long); the server stores it so the score can be verified by replay.
    const auto proof = g_proof.encode(stv::host::kMaxReplayBytes);
    if (proof.empty()) write_player_log("Leaderboard: this run cannot be attached as a replay proof.");
    for (const auto& score : fresh)
        g_lb_client.submit(stv::host::kLeaderboardBoards[board], score.name, score.score, proof);
}
void service_leaderboard() {
    if (!g_lb_client.started()) return;
    std::string notice;
    for (const auto& result : g_lb_client.take_submitted()) {
        if (!result.fresh || result.rank <= 0) continue;
        notice += (notice.empty() ? "GLOBAL RANK #" : "    #") + std::to_string(result.rank) + "  " + result.name;
        if (notice.size() < 40) notice += result.board == "wide" ? "  WIDESCREEN" : "  ARCADE";
    }
    if (!notice.empty()) { g_toast = notice; g_toast_until = GetTickCount64() + 6000; }
    if (!g_global_lb.is_open()) return;
    g_global_lb.sync(g_lb_client, lb_now());
    const auto revision = g_lb_client.revision();
    const auto now = GetTickCount64();
    if (revision != g_lb_revision || now >= g_lb_repaint_at) {
        g_lb_revision = revision;
        g_lb_repaint_at = now + 1000;   // keeps "UPDATED 12S AGO" current
        if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

void save_cmos() {
    if (scripted_run()) return;
    const std::vector<u16>* local = &g_m.cmos;
    if (g_net.on && !g_net.host) {
        if (!g_net.local_cmos) return; // never persist an unowned/host image
        local = &*g_net.local_cmos;
    }
    const std::wstring tmp = cmos_path() + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return;
    bool ok = std::fwrite(local->data(), 2, local->size(), f) == local->size();
    // Buffered data reaches the disk at close: a failed close (disk full) must
    // not replace the previous complete image with a truncated one.
    ok = std::fclose(f) == 0 && ok;
    if (ok) MoveFileExW(tmp.c_str(), cmos_path().c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring keys_path() { return g_data_dir + L"\\keys.cfg"; }

void load_keys() {
    for (int i = 0; i < ACTION_COUNT; ++i) g_keys[i] = kDefaultKeys[i];
    g_pad_bindings.reset();
    g_deadzone = 12000;
    FILE* f = nullptr;
    if (_wfopen_s(&f, keys_path().c_str(), L"r") != 0 || !f) return;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        int i = 0, vk = 0;
        if (sscanf_s(line, "%d %d", &i, &vk) == 2 && i >= 0 && i < ACTION_COUNT && vk > 0 && vk < 256) g_keys[i] = vk;
        unsigned button = 0;
        if (sscanf_s(line, "pad %d %u", &i, &button) == 2) g_pad_bindings.assign(i, button);
        int dz = 0;
        if (sscanf_s(line, "deadzone %d", &dz) == 1 && dz >= 4000 && dz <= 24000) g_deadzone = dz;
    }
    std::fclose(f);
}

void save_keys() {
    const std::wstring tmp = keys_path() + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w") != 0 || !f) return;
    for (int i = 0; i < ACTION_COUNT; ++i) std::fprintf(f, "%d %d  # %s\n", i, g_keys[i], kActionNames[i]);
    for (int i = 0; i < ACTION_COUNT; ++i) std::fprintf(f, "pad %d %u\n", i, g_pad_bindings.buttons[i]);
    std::fprintf(f, "deadzone %d\n", g_deadzone);
    const bool ok = std::fclose(f) == 0;
    if (ok) MoveFileExW(tmp.c_str(), keys_path().c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string key_name(int vk) {
    char name[64] = "?";
    UINT sc = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
    switch (vk) {   // extended keys need the extended bit for their names
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: sc |= 0x100; break;
    default: break;
    }
    if (!GetKeyNameTextA(LONG(sc << 16), name, int(sizeof name))) std::snprintf(name, sizeof name, "key %d", vk);
    return name;
}

void toggle_fullscreen(bool persist = true) {
    if (persist) g_transient_fullscreen = false;
    const DWORD style = DWORD(GetWindowLongW(g_wnd, GWL_STYLE));
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(g_wnd, &g_place) && GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(g_wnd, GWL_STYLE, LONG(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    } else {
        SetWindowLongW(g_wnd, GWL_STYLE, LONG(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(g_wnd, &g_place);
        SetWindowPos(g_wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
    if (persist) save_preferences();
}

const char* pad_button_name(std::uint32_t b) {
    if (g_launcher_playstation) {
        switch (b) {
        case stv::host::PadA: return "Cross";
        case stv::host::PadB: return "Circle";
        case stv::host::PadX: return "Square";
        case stv::host::PadY: return "Triangle";
        default: break;
        }
    }
    switch (b) {
    case 0: return "unassigned";
    case stv::host::PadUp: return "D-pad up";
    case stv::host::PadDown: return "D-pad down";
    case stv::host::PadLeft: return "D-pad left";
    case stv::host::PadRight: return "D-pad right";
    case stv::host::PadStart: return "Start";
    case stv::host::PadBack: return "Back";
    case stv::host::PadA: return "A";
    case stv::host::PadB: return "B";
    case stv::host::PadX: return "X";
    case stv::host::PadY: return "Y";
    case stv::host::PadLT: return "left trigger";
    case stv::host::PadRT: return "right trigger";
    default: return "invalid";
    }
}

// Explicit user restart or lobby start builds the same fresh coupled owners as
// process startup. No main-only snapshot is ever installed in the audio owner.
bool reset_coupled_game(bool keep_cmos) {
    auto fresh = std::make_unique<Machine>();
    std::string error;
    if (!stv::gsp::load_imported_roms(*fresh, g_rom_path, error)) {
        g_menu_status = "Restart failed: " + error; return false;
    }
    if (g_enhanced) {
        fresh->enhanced = true;
        fresh->set_wide(1024, 137);
        if (!stv::gsp::apply_enhanced(*fresh, 137)) {
            g_menu_status = "Restart failed: enhanced L8 tables do not match."; return false;
        }
    }
    fresh->reset();
    if (keep_cmos) fresh->cmos = g_m.cmos;
    g_sched.reset();
    g_snd.host = nullptr;
    g_m = std::move(*fresh);
    if (!keep_cmos) load_cmos();
    g_pc = g_m.reset_vector();
    g_mixer = std::make_unique<stv::snd::Mixer>(g_audio.rate());
    // Board's YM interface retains a reference to its owner, so reconstruct in
    // place rather than move an interface bound to another Board object.
    std::destroy_at(&g_snd);
    std::construct_at(&g_snd);
    if (!g_snd.load_roms(g_m.snd_u4, g_m.snd_u19, g_m.snd_u20, error)) {
        g_menu_status = "Restart failed: " + error; return false;
    }
    g_snd.mixer = g_mixer.get();
    g_snd.power_on();
    g_sched = std::make_unique<stv::game::CoScheduler>(g_m, g_snd);
    connect_confirmed_sound();
    connect_machine_callbacks();
    g_pcm.clear();
    const bool had_audio_device = g_audio.ok();
    if (!g_audio.discard_pending() && had_audio_device) {
        g_menu_status = std::string("Audio reset failed at ") + g_audio.error_stage() +
                        "; retrying audio output while gameplay continues.";
        frontend_event("audio_reset_failed", g_audio.error_code(), g_audio.error_stage());
    }
    g_started = false;
    g_frame_done = false;
    g_paused = false;
    g_test_frames = 0;
    g_status.clear();
    g_frame_width = stv::gsp::scene_view_width(g_m);
    g_pixels.assign(std::size_t(g_frame_width) * VIS_H, 0);
    ++g_pixel_revision;
    g_frame_ready = false;
    // The opaque title must still present through an existing flip swap chain.
    // Its next gameplay frame replaces the old GPU texture on first update.
    if (!g_mode_menu) g_resident_ready = false;
    g_keyboard_blocked = true;
    g_pad_router.suppress_until_neutral();
    g_proof.begin(g_enhanced, g_m.cmos);   // a fresh machine starts a new provable run
    return true;
}

bool select_profile(bool enhanced) {
    if (g_enhanced == enhanced) return true;
    save_cmos();
    g_enhanced = enhanced;
    if (!reset_coupled_game(false)) {
        g_enhanced = !enhanced;
        return false;
    }
    g_record_local = g_host_solo = false;
    g_room_history.clear(); g_room_frames = 0; g_room_boot_cmos.clear();
    g_paused = g_menu;
    return true;
}

// TMNT/arcade-lan-inspired room UI around the existing rollback transport.
void close_lobby() {
    if (g_lobby) {
        g_lobby->leave(GetTickCount64());
        std::string ignored;
#if STV_HAS_EOS
        if (g_epic_room) { g_epic.tick(g_lobby.get(), GetTickCount64()); g_epic.close(); }
        else
#endif
        g_lobby_udp.poll(*g_lobby, GetTickCount64(), ignored);
    }
    g_host_booting = g_start_together = false;
    g_epic_room = false; g_room_password.clear();
    g_lobby_udp.close();
    g_lobby.reset();
    g_lobby_game_socket.reset();
    g_pending_handoff.reset();
    g_room_browser = g_room_view = false;
    g_selected_room = 0;
    g_guest_room_profile.reset();
    g_lobby_last_state = -1;
    g_lobby_last_rooms.clear();
    g_lobby_last_members.clear();
    g_room_guest.clear(); g_room_notice.clear(); g_room_notice_until = 0;
}

bool create_lobby(bool host, bool online, bool epic) {
    if (g_net.on) return false;
    close_lobby();
    g_online_room = online; g_epic_room = epic;
    const auto fingerprint = stv::net::fingerprint(g_m);
    stv::net::LobbyConfig config;
    GUID id{};
    if (SUCCEEDED(CoCreateGuid(&id))) std::memcpy(&config.node_id, &id, sizeof(config.node_id));
    if (!config.node_id) config.node_id = (std::uint64_t(GetCurrentProcessId()) << 32) ^ GetTickCount64() ^ 1u;
    config.name = g_settings.player_name; config.internet = epic;
    config.identity = {fingerprint.build, fingerprint.program, fingerprint.profile, fingerprint.vram_width, fingerprint.visible_extra};
    g_lobby = std::make_unique<stv::net::Lobby>(config);
    std::string error;
    if (epic) {
#if STV_HAS_EOS
        if (g_epic.open(std::filesystem::path(exe_dir()) / L"eos-client.ini",config.name,host,error)) {
            g_menu_status.clear(); return true;
        }
#else
        error = "This build does not include the Epic Online Services SDK.";
#endif
        close_lobby(); g_menu_status = error; return false;
    }
    g_lobby_game_socket = std::make_unique<stv::net::UdpTransport>();
    if (!g_lobby_game_socket->bind(host ? 27015 : 0, error) || !g_lobby_udp.open(config.discovery_port, error, online && host)) {
        g_menu_status = "LAN room: " + error;
        close_lobby();
        return false;
    }
    g_menu_status.clear();
    frontend_event("lobby_open", host ? 1 : 0, g_settings.player_name);
    return true;
}

void start_lobby_host(bool online, bool epic) {
    const auto password = g_room_password;
    const bool enhanced = g_host_mode.value_or(g_enhanced);
    g_host_mode.reset();
    if (enhanced != g_enhanced && !select_profile(enhanced)) return;
    if (!create_lobby(true, online, epic)) return;
    if (epic) { g_room_password = password; g_lobby->set_password(password); }
    if (!g_lobby->host(epic ? 1 : g_lobby_game_socket->local_port(), GetTickCount64())) {
        g_menu_status = g_lobby->problem(); close_lobby(); return;
    }
    if (g_record_local) {
        g_host_solo = true; g_record_local = false;
    }
    g_room_view = true;
    service_lobby();
}
void start_lobby_search(bool epic) {
    if (!create_lobby(false,epic,epic)) return;
    if (!g_lobby->search(GetTickCount64())) {
        g_menu_status = g_lobby->problem(); close_lobby(); return;
    }
    g_room_browser = true;
    service_lobby();
}

void service_lobby() {
#if STV_HAS_EOS
    g_epic.tick(g_epic_room ? g_lobby.get() : nullptr,GetTickCount64());
#endif
    if (!g_lobby) return;
    std::string error;
    const auto now = GetTickCount64();
    if (!g_epic_room && !g_lobby_udp.poll(*g_lobby, now, error)) {
        g_menu_status = "LAN room: " + error;
        if (g_connecting) g_cancel_connect = true;
    }
    if (auto handoff = g_lobby->take_handoff()) {
        frontend_event("lobby_handoff", handoff->host ? 1 : 0,
                       stv::net::Lobby::ip_text(handoff->peer_ip) + ":" +
                       std::to_string(handoff->peer_game_port) + "/" + handoff->peer_name);
        g_pending_handoff = std::move(handoff);
    }
    if (g_lobby_last_state != int(g_lobby->state())) {
        g_lobby_last_state = int(g_lobby->state());
        frontend_event("lobby_state", unsigned(g_lobby_last_state));
    }
    const bool report = !frontend_report_path().empty();
    if (g_room_browser && g_password_prompt==PasswordPrompt::None && !selected_room() && !g_lobby->rooms().empty())
        select_room(g_lobby->rooms().front());
    if (report) {
        std::string rooms, members;
        for (const auto& room : g_lobby->rooms()) rooms += room.name + "/" + std::to_string(room.players) + ";";
        const auto roster = g_lobby->members();
        for (const auto& member : roster) members += member.name + ";";
        if (rooms != g_lobby_last_rooms) {
            g_lobby_last_rooms = rooms;
            frontend_event("lobby_rooms", g_lobby->rooms().size(), rooms);
        }
        if (members != g_lobby_last_members) {
            g_lobby_last_members = members;
            frontend_event("lobby_members", roster.size(), members);
        }
    }
    if (g_lobby->hosting()) {
        const auto& guest = g_lobby->peer_name();
        if (guest != g_room_guest) {
            g_room_notice = guest.empty() ? g_room_guest + " LEFT THE ROOM" : guest + " JOINED THE SHOW!";
            g_room_guest = guest;
            g_room_notice_until = now + 6000;
        }
    }
#if STV_HAS_EOS
    if (g_epic_room && g_epic.published() && g_room_view && g_lobby->state()==stv::net::LobbyState::Hosting) {
        g_start_solo = true; g_toast = "Public room open. Player 2 can join."; g_toast_until=now+4000;
    }
#endif
    if (g_host_solo && !g_host_booting && !g_net.on && g_lobby->can_start() && !g_room_view && !g_pending_handoff && g_lobby->state() == stv::net::LobbyState::Hosting) {
        // Drop-in: a player who picks this room walks straight into the running
        // game, like an arcade player inserting a coin. No host prompt.
        g_start_together = false; g_lobby->start(now);
    }
    if (g_net.session && g_lobby->state() == stv::net::LobbyState::Idle) g_disconnect_requested = true;
    if (!g_lobby->problem().empty()) {
        g_menu_status = g_lobby->problem();
        if (g_epic_room && g_room_view && !g_net.on && !g_connecting && g_lobby->state()==stv::net::LobbyState::Idle) {
            g_lobby->search(now); g_room_view=false; g_room_browser=true;
        }
    }
    static ULONGLONG previous_paint = 0;
    if ((g_room_browser || g_room_view) && now - previous_paint >= 100) {
        previous_paint = now;
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

const stv::net::LobbyRoom* selected_room() {
    if (!g_lobby) return nullptr;
    for (const auto& room : g_lobby->rooms())
        if (room.room_id == g_selected_room && room.address == g_selected_room_address) return &room;
    return nullptr;
}
void select_room(const stv::net::LobbyRoom& room) {
    g_selected_room = room.room_id;
    g_selected_room_address = room.address;
}

bool lobby_menu_lines(std::vector<std::string>& lines) {
    if (!g_lobby || (!g_room_browser && !g_room_view)) return false;
    if (g_epic_room && g_room_browser) {
        lines.push_back("PUBLIC GAMES   /   LOWEST PING FIRST");
        lines.push_back("-- HOST         SEATS   ACCESS         PING --");
        const auto& rooms = g_lobby->rooms();
        if (!selected_room() && !rooms.empty()) select_room(rooms.front());
        const auto* selected = selected_room();
        size_t index = selected ? size_t(selected - rooms.data()) : 0;
        size_t first = index / 6 * 6;
        for (size_t i=first; i<std::min(rooms.size(),first+6); ++i) {
            const auto& r=rooms[i];
            std::string state = !r.compatible ? "Version mismatch" : r.players==2 ? "Full" : r.busy ? "Starting" : r.locked ? "Password" : "Open";
            char row[96]{};
            const auto ping=r.ping_ms<0 ? "Measuring" : std::to_string(r.ping_ms)+" ms";
            std::snprintf(row,sizeof(row),"%s%-10s   %u/2     %-16s %s",i==index ? "> " : "  ",
                          r.name.c_str(),r.players,state.c_str(),ping.c_str());
            lines.push_back(row);
        }
        if (rooms.empty()) lines.push_back("Open seats appear here as hosts come online.");
        if (selected) lines.push_back(std::string(stv::host::game_mode_label(selected->identity.profile != 0)) + "    " +
            std::to_string(index+1) + " of " + std::to_string(rooms.size()) + " games");
        lines.push_back("Up/down: choose    A / Enter: join    B / Esc: back");
    } else if (g_room_browser) {
        lines.push_back(std::string(g_online_room ? "ONLINE ROOM" : "LAN ROOMS") + "  /  " + g_settings.player_name);
        lines.push_back("Up/down: choose   Enter / A: join   Esc / B: back");
        lines.push_back("");
        const auto& rooms = g_lobby->rooms();
        if (!g_selected_room && !rooms.empty()) select_room(rooms.front());
        for (const auto& room : rooms) {
            const bool selected = room.room_id == g_selected_room && room.address == g_selected_room_address;
            const std::string availability = !room.compatible ? "different build/profile" : room.busy ? "playing" :
                                             room.players >= 2 ? "full" : "ready";
            lines.push_back(std::string(selected ? "> " : "  ") + room.name + "  " + std::to_string(room.players) + "/2  " + availability + (room.identity.profile ? "  16:9" : "  4:3"));
            lines.push_back("    " + stv::net::Lobby::ip_text(room.address.ip) + ":" + std::to_string(room.game_port));
        }
        if (rooms.empty()) lines.push_back(g_online_room ? "Looking for the host..." : "Searching the local network...");
        if (g_selected_room && !selected_room()) lines.push_back("Selected room expired; choose another room.");
        lines.push_back("");
        lines.push_back(g_online_room ? "Host forwards UDP 27015 and 27016." : "Choose a room to use the host's game version.");
    } else {
        const auto state = g_lobby->state();
        lines.push_back(std::string(g_online_room ? "ONLINE" : "LAN") + "  /  CONTESTANT ROOM");
        lines.push_back(stv::host::game_mode_label(g_guest_room_profile.value_or(g_enhanced)));
        if (GetTickCount64() < g_room_notice_until) lines.push_back("[ " + g_room_notice + " ]");
        if (g_epic_room && g_lobby->hosting()) lines.push_back(g_room_password.empty() ? "Public game  /  Open seat" : "Public game  /  Password required");
        else if (g_online_room && g_lobby->hosting()) lines.push_back("Share your IP. Forward UDP 27015 and 27016.");
        lines.push_back("");
        for (const auto& member : g_lobby->members())
            lines.push_back(std::string(member.host ? "P1  " : "P2  ") + member.name + (member.own ? "  (you)" : ""));
        lines.push_back("");
        if (state == stv::net::LobbyState::Joining) lines.push_back("Requesting a place in the room...");
        else if (state == stv::net::LobbyState::Starting || state == stv::net::LobbyState::Handshaking)
            lines.push_back("Connecting both contestants...");
        else if (g_epic_room && g_lobby->hosting()) lines.push_back("Play begins when the room is published.");
        else if (g_lobby->hosting())
            lines.push_back(g_lobby->can_start()
                ? (g_host_solo ? "> Admit player 2 and resume  (Enter / A)" : "> Start together  (Enter / A)")
                : (g_host_solo ? "> Resume game  (Enter / A)" : "> Start solo  (Enter / A) - room stays open"));
        else if (state == stv::net::LobbyState::Joined) lines.push_back("Waiting for the host...");
        lines.push_back("Esc / B: leave room");
    }
#if STV_HAS_EOS
    if (g_epic_room && !g_epic.status().empty()) lines.push_back(g_epic.status());
#endif
    return true;
}

bool handle_lobby_key(WPARAM key) {
    if (!g_lobby || (!g_room_browser && !g_room_view)) return false;
    if (key == VK_ESCAPE) {
        if (g_host_solo && !g_net.on) { g_record_local = true; g_host_solo = false; }
        close_lobby(); g_menu_status.clear(); return true;
    }
    if (g_room_browser) {
        const auto& rooms = g_lobby->rooms();
        if (!rooms.empty() && (key == VK_UP || key == VK_DOWN)) {
            std::size_t index = rooms.size();
            for (std::size_t i = 0; i < rooms.size(); ++i)
                if (rooms[i].room_id == g_selected_room && rooms[i].address == g_selected_room_address) index = i;
            if (index == rooms.size()) index = key == VK_UP ? rooms.size() - 1 : 0;
            else index = (index + rooms.size() + (key == VK_UP ? -1 : 1)) % rooms.size();
            select_room(rooms[index]); g_menu_status.clear();
        } else if (key == VK_RETURN) {
            if (const auto* room = selected_room()) {
                if (g_epic_room && room->locked && room->compatible && !room->busy && room->players<2 && g_room_password.empty()) {
                    g_password_prompt=PasswordPrompt::Join; g_password_draft.clear(); g_password_cursor=0;
                    g_password_room=room->room_id; g_password_address=room->address; return true;
                }
                g_lobby->set_password(g_epic_room ? g_room_password : "");
                g_room_password.clear();
                const auto selected = *room; // joining can replace the browser cache
                if (!selected.busy && selected.players < 2 && selected.compatible &&
                    selected.identity.profile <= 1 && bool(selected.identity.profile) != g_enhanced) {
                    // Check the host's profile against our ROM without disturbing
                    // the current game while the guest is only waiting in a room.
                    auto candidate = std::make_unique<Machine>();
                    std::string error;
                    if (!stv::gsp::load_imported_roms(*candidate,g_rom_path,error)) {
                        g_menu_status = error; return true;
                    }
                    if (selected.identity.profile) {
                        candidate->enhanced = true;
                        candidate->set_wide(1024,137);
                        if (!stv::gsp::apply_enhanced(*candidate,137)) {
                            g_menu_status = "Cannot load the host's widescreen profile."; return true;
                        }
                    }
                    const auto fp = stv::net::fingerprint(*candidate);
                    frontend_event("room_profile_selected", selected.identity.profile);
                    g_lobby->set_search_identity({fp.build, fp.program, fp.profile, fp.vram_width, fp.visible_extra});
                }
                if (g_lobby->join(selected, g_epic_room ? 1 : g_lobby_game_socket->local_port(), GetTickCount64())) {
                    g_guest_room_profile = selected.identity.profile != 0;
                    g_room_browser = false; g_room_view = true; g_menu_status.clear(); service_lobby();
                } else g_menu_status = g_lobby->problem();
            } else g_menu_status = "Choose an available room.";
        }
    } else if (key == VK_RETURN && g_lobby->hosting() && !g_epic_room) {
        if (!g_host_solo || (!g_lobby->can_start() && g_lobby->members().size() == 1)) {
            g_start_together = g_lobby->can_start();
            g_start_solo = true; return true;
        }
        if (!g_lobby->start(GetTickCount64())) g_menu_status = g_lobby->problem();
        else { g_menu_status.clear(); service_lobby(); }
    }
    return true;
}

bool service_frontend_requests() {
    if (g_start_solo) {
        g_start_solo = false;
        if (!g_host_solo) {
            if (!reset_coupled_game()) return true;
            g_host_booting = true;
            g_room_boot_cmos = g_m.cmos; g_room_history.clear(); g_room_frames = 0;
        }
        g_host_solo = true;
        g_room_view = g_title_submenu = g_menu = g_paused = false;
        frontend_event("room_solo", g_room_frames);
        sync_menu_input();
        return true;
    }
    if (g_restart_requested || g_disconnect_requested) {
        const bool closing_public = g_close_room_requested && g_epic_room && (g_host_solo || g_net.host);
        g_close_room_requested = false;
        const bool continue_host = !closing_public && g_disconnect_requested && g_net.host && g_lobby && !g_mode_menu;
        const bool depart_guest = g_disconnect_requested && !g_net.host;
        const auto departed = g_net.contestant;
        const bool was_online_room = g_online_room, was_epic_room = g_epic_room;
        const auto password = g_room_password;
        retain_room_scores();
        const bool restore_guest_cmos = g_net.on && !g_net.host;
        g_restart_requested = g_disconnect_requested = false;
        if (restore_guest_cmos) {
            if (!g_net.local_cmos || g_net.local_cmos->size() != g_m.cmos.size()) {
                g_menu_status = "Cannot restore the local CMOS image: guest ownership is missing.";
                g_menu = g_paused = true;
                sync_menu_input();
                InvalidateRect(g_wnd, nullptr, FALSE);
                return true;
            }
            g_m.cmos = *g_net.local_cmos;
        }
        save_cmos();
        if (g_net.on && g_net.session && !g_net.ended) net_report("left");
        close_lobby();
        g_net = NetPlay{};
        if (closing_public && !g_mode_menu) {
            g_host_solo=false; g_record_local=true; g_snd.on_access={};
            g_menu=g_paused=false; g_toast="Public room closed"; g_toast_until=GetTickCount64()+4000;
            sync_menu_input(); return true;
        }
        if (continue_host) {
            // The departed contestant's character dies, as if hit on their last life.
            if (stv::host::drop_player(g_m, 1)) frontend_event("player_dropped", 2, departed);
            g_host_solo = true;
            g_room_password = password;
            start_lobby_host(was_online_room,was_epic_room);
            g_room_view = g_menu = g_paused = false;
            g_snd.on_access = {};
            frontend_event("room_continues", g_room_frames);
            g_toast = departed + " LEFT THE SHOW"; g_toast_until = GetTickCount64()+5000;
            sync_menu_input();
            return true;
        }
        g_record_local = g_host_solo = false; g_room_history.clear(); g_room_frames = 0; g_room_boot_cmos.clear();
        if (depart_guest) g_mode_menu = true;
        const bool reset = reset_coupled_game();
        g_menu = g_paused = g_mode_menu || !reset;
        sync_menu_input();
        InvalidateRect(g_wnd, nullptr, FALSE);
        return true;
    }
    if (!g_pending_handoff || g_net.on) return false;
    const auto handoff = *g_pending_handoff;
    g_pending_handoff.reset();
    g_room_browser = g_room_view = false;
    g_menu = false;
    sync_menu_input();
    g_net = NetPlay{};
    std::string error;
    const bool learn_port = g_online_room && handoff.host && g_lobby_game_socket;
    if (learn_port) g_lobby_game_socket->expect_ip(handoff.peer_ip);
#if STV_HAS_EOS
    if (g_epic_room) g_net.udp = g_epic.transport(handoff);
#endif
    const bool have_peer = bool(g_net.udp) || learn_port || (g_lobby_game_socket && g_lobby_game_socket->set_peer(
        stv::net::Lobby::ip_text(handoff.peer_ip), handoff.peer_game_port, error));
    const bool resume_host = g_host_solo && handoff.host;
    const bool change_profile = !handoff.host && g_guest_room_profile && *g_guest_room_profile != g_enhanced;
    const bool ready = have_peer && (resume_host ||
        (change_profile ? select_profile(*g_guest_room_profile) : reset_coupled_game()));
    if (!ready) {
        if (!error.empty()) g_menu_status = "LAN start: " + error;
        close_lobby();
        g_menu = true; g_paused = true; sync_menu_input();
        return true;
    }
    if (!resume_host) g_record_local = g_host_solo = false;
    g_net.on = true;
    g_net.contestant = handoff.peer_name;
    g_net.host = handoff.host;
    g_net.peer = stv::net::Lobby::ip_text(handoff.peer_ip);
    g_net.port = handoff.host ? handoff.local_game_port : handoff.peer_game_port;
    if (!g_epic_room) g_net.udp = std::move(g_lobby_game_socket);
    configure_net_diagnostics();
    if (net_connect(g_rom_path)) {
        g_title_submenu = false;
        {
            if (g_lobby && g_lobby->established(handoff.session_token, GetTickCount64()))
                frontend_event("lobby_established", handoff.session_token, handoff.peer_name);
            else frontend_event("lobby_error", 0, "Game handshake completed without a live room handoff");
        }
        g_toast = "Online with " + handoff.peer_name + ": you are player " + (g_net.host ? "1" : "2");
        if (!g_audio.ok()) g_toast += ". Audio output unavailable.";
        g_toast_until = GetTickCount64() + 5000;
        if (g_audio.ok()) g_menu_status.clear();
    } else {
        const bool restore_public = g_epic_room;
        const auto restore_password = g_room_password;
        if (g_menu_status.empty()) g_menu_status = g_cancel_connect ? "Connection cancelled." : "Could not start the online game.";
        close_lobby();
        if (!g_net.host && g_net.local_cmos) g_m.cmos = *g_net.local_cmos;
        g_net = NetPlay{};
        if (resume_host && restore_public) {
            g_room_password = restore_password;
            start_lobby_host(true,true);
            g_room_view=g_paused=g_menu=false;
            g_toast="Guest connection lost. Your game continues."; g_toast_until=GetTickCount64()+5000;
            sync_menu_input(); return true;
        }
        if (!g_host_solo) (void)reset_coupled_game();
        g_menu = true; g_paused = true; sync_menu_input();
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
    return true;
}

StartContext start_context(int player) {
    const Machine& owner=g_net.on && g_net.mr ? *g_net.mr : g_m;
    if (g_net.on && player == 0) player = g_net.host ? 0 : 1;
    const auto scene=game_scene_state(owner);
    const auto lives=owner.ram[((player ? 0x01086520u : 0x01086360u)-0x01000000u)>>4];
    if (scene == 0x30 && !g_net.on) return StartContext::Attract;
    if ((scene == 0x10 || scene == 0x18 || scene == 0x50) && lives == 0) return StartContext::Continue;
    return StartContext::Other;
}

std::string menu_value(stv::host::PcMenuItem item) {
    using I = stv::host::PcMenuItem;
    switch (item) {
    case I::Glow: case I::Sharpen: case I::Curvature:
    case I::Atmosphere: case I::SpriteRim: case I::ContactShadows: case I::WetMaterials:
    case I::HeatDistortion: case I::EmissiveSpill: case I::CinematicGrade: {
        const char* levels[]={"off","low","medium","high"};
        if(item==I::Glow) return levels[g_settings.glow];
        if(item==I::Sharpen) return levels[g_settings.sharpen];
        if(item==I::Curvature) return levels[g_settings.curvature];
        if(item==I::Atmosphere) return levels[g_settings.atmosphere];
        if(item==I::SpriteRim) return levels[g_settings.sprite_rim];
        if(item==I::ContactShadows) return levels[g_settings.contact_shadows];
        if(item==I::WetMaterials) return levels[g_settings.wet_materials];
        if(item==I::HeatDistortion) return levels[g_settings.heat_distortion];
        if(item==I::EmissiveSpill) return levels[g_settings.emissive_spill];
        return levels[g_settings.cinematic_grade];
    }
    case I::EnhanceAll: return g_settings.enhance ? "on" : "off";
    case I::RoomCinema: return stv::host::room_cinema_quality_name(unsigned(g_settings.room_cinema));
    case I::Particles: return g_settings.particles ? "on" : "off";
    case I::Blood: return g_settings.blood ? "on" : "off";
    case I::Debris: return g_settings.debris ? "on" : "off";
    case I::Smoke: return g_settings.smoke ? "on" : "off";
    case I::Fire: return g_settings.fire ? "on" : "off";
    case I::Shockwaves: return g_settings.shockwaves ? "on" : "off";
    case I::PersistentGore: return g_settings.persistent_gore ? "on" : "off";
    case I::Destruction: return g_settings.destruction ? "on" : "off";
    case I::WeaponImpacts: return g_settings.weapon_impacts ? "on" : "off";
    case I::FluidSmoke: return g_settings.fluid_smoke ? "on" : "off";
    case I::HardwareRT: return g_settings.hardware_rt ? (g_resident_effects.dxr_error().empty() ? "on" : "unavailable") : "off";
    case I::ExplosiveDebris: return g_settings.explosive_debris ? "on" : "off";
    case I::MuzzleFlash: return g_settings.muzzle_flash ? "on" : "off";
    case I::Brass: return g_settings.brass ? "on" : "off";
    case I::Voxels: return g_settings.voxels ? "on" : "off";
    case I::CombatFeedback: case I::CameraShake: case I::ZoomPunch:
    case I::HitFlash: case I::EdgeEffects: case I::WeaponSignatures: {
        const char* levels[]={"off","low","medium","high"};
        const int value=item==I::CombatFeedback?g_settings.combat_feedback:
            item==I::CameraShake?g_settings.camera_shake:
            item==I::ZoomPunch?g_settings.zoom_punch:
            item==I::HitFlash?g_settings.hit_flash:
            item==I::EdgeEffects?g_settings.edge_effects:
            g_settings.weapon_signatures;
        return levels[value];
    }
    case I::ReduceShake: return g_settings.reduce_shake ? "on" : "off";
    case I::ReduceFlashing: return g_settings.reduce_flashing ? "on" : "off";
    case I::ReduceMotion: return g_settings.reduce_motion ? "on" : "off";
    case I::WindowScale: return g_settings.window_scale ? std::to_string(g_settings.window_scale) + "x" : "automatic";
    case I::Fullscreen: return g_fullscreen ? "on" : "off";
    case I::Scaling: return scale_name();
    case I::PixelFilter: return stv::host::video_filter_name(static_cast<stv::host::VideoFilterMode>(g_settings.filter));
    case I::Motion: return g_settings.smooth_motion ? "SMOOTH MOTION" : "Arcade exact";
    case I::RunAhead: return g_settings.run_ahead ? std::to_string(g_settings.run_ahead) + (g_settings.run_ahead == 1 ? " frame" : " frames") : "Off";
    case I::Vsync: if (g_settings.smooth_motion) return "on (Smooth Motion)";
                   return g_settings.vsync ? "on (desktop sync)" : "off";
    case I::ShowFps: return g_settings.show_fps ? "on" : "off";
    case I::HostMode: return stv::host::game_mode_label(g_host_mode.value_or(g_enhanced));
    case I::PlayfieldProfile: return g_enhanced ? "16:9 enhanced" : "reference";
    case I::MasterVolume: return std::to_string(g_settings.volume) + "%";
    case I::Mute: return g_settings.muted ? "on" : "off";
    case I::SoundChannels: return g_settings.extended_sound ? "Extended" : "Arcade";
    case I::Reverb: return g_settings.reverb == 2 ? "Arena" : g_settings.reverb == 1 ? "Studio" : "Off";
    case I::StereoWidth: return g_settings.audio_wide ? "Wide" : "Mono";
    case I::HiFiAudio: return g_settings.audio_hifi ? "on" : "off";
    case I::SwapPads: return g_swap_pads ? "pad 1 -> P2, pad 2 -> P1" : "pad 1 -> P1, pad 2 -> P2";
    case I::StickDeadzone: return std::to_string((g_deadzone * 100 + 16383) / 32767) + "%";
    case I::PlayerName: return g_settings.player_name;
    default: return "";
    }
}

const char* menu_help(stv::host::PcMenuItem item) {
    using I = stv::host::PcMenuItem;
    switch (item) {
    case I::Resume: return "RETURN TO THE SHOW";
    case I::LocalCoop: return "TWO CONTESTANTS / ONE SHARED ARENA";
    case I::RoomScores: return "HIGH SCORES FROM SHARED GAMES ON THIS PC";
    case I::PublicBrowser: return "CHOOSE A GAME / FASTEST CONNECTIONS FIRST";
    case I::PublicHost: return "PLAY SOLO UNTIL PLAYER TWO DROPS IN";
    case I::Connections: return "LOCAL NETWORK AND DIRECT IP PLAY";
    case I::DirectJoin: return "CONNECT TO A HOST BY IPV4 ADDRESS OR NAME";
    case I::DirectHost: return "OPEN AN ONLINE ROOM FOR A FRIEND";
    case I::FindRooms: return "FIND HOSTS ON YOUR LOCAL NETWORK";
    case I::HostGame: return "OPEN A ROOM ON YOUR LOCAL NETWORK";
    case I::Multiplayer: return "HOST OR JOIN ONLINE AND LAN GAMES";
    case I::Options: return "VIDEO / ENHANCE / AUDIO / CONTROLS";
    case I::Video: return "PICTURE / FULLSCREEN / PERFORMANCE";
    case I::Enhance: return "COSMETIC EFFECTS / ORIGINAL GAMEPLAY";
    case I::EnhanceAll: return "MASTER SWITCH / INDIVIDUAL CHOICES ARE PRESERVED";
    case I::Fire: return "FOUR EXPLOSION PATTERNS / REQUIRES ENHANCE ON";
    case I::Shockwaves: return "EXPANDING PRESSURE RING / LOCAL HEAT DISTORTION";
    case I::PersistentGore: return "BLOOD POOLS, WALL RUNS AND BLOODY BOOT PRINTS";
    case I::Destruction: return "FLESH, PLATING AND DISCHARGING ORB CORES";
    case I::WeaponImpacts: return "BALLISTIC, GRENADE AND ENERGY IMPACTS";
    case I::FluidSmoke: return "LIT SMOKE ROLLS AROUND WALLS AND RESPONDS TO SHOTS";
    case I::HardwareRT: return "DXR 1.1 / ROOM MOODS, LIVE LIGHTS AND SPRITE SHADOWS";
    case I::Atmosphere: return "SUBTLE ROOM HAZE / LIGHT CONES / DRIFTING DUST";
    case I::SpriteRim: return "PIXEL-EDGE RIM LIGHT / ORIGINAL SPRITE ART PRESERVED";
    case I::ContactShadows: return "CHEAP 2.5D GROUND CONTACT / HUD EXCLUDED";
    case I::WetMaterials: return "BLOOD / OIL SHEENS / RIPPLES AT SMOKE EDGES";
    case I::HeatDistortion: return "SHORT LOCAL WARP AROUND FIRE / NO FULL-SCREEN REFRACTION";
    case I::EmissiveSpill: return "DIRECTIONAL COLOR LEAK FROM BRIGHT SPRITES AND LAMPS";
    case I::CinematicGrade: return "SUBTLE PER-ROOM LUT-LIKE COLOR GRADE / HUD UNCHANGED";
    case I::RoomCinema: return "OFF / LOW / MEDIUM / HIGH / BOSS + AUDIO ROOM PRESENTATION";
    case I::ExplosiveDebris: return "BLASTS THROW FLESH AND STEEL / PERSISTENT GORE KEEPS THE MARKS";
    case I::MuzzleFlash: return "DIRECTIONAL FLASH AT WEAPON DISCHARGE";
    case I::Brass: return "SPINNING CASINGS BOUNCE AND SETTLE";
    case I::Voxels: return "3D CUBE FRAGMENTS / COSMETIC PHYSICS";
    case I::CombatFeedback: return "MASTER LEVEL / PRESENTATION ONLY / LOW SAVES GPU WORK";
    case I::CameraShake: return "CAMERA SHAKE ON BOSS PART BREAKS AND PLAYER DEATHS";
    case I::ZoomPunch: return "BRIEF ZOOM ON BOSS PART BREAKS AND PLAYER DEATHS";
    case I::HitFlash: return "CONTROLLED IMPACT FLASH / REDUCED BY ACCESSIBILITY OPTION";
    case I::EdgeEffects: return "VIGNETTE + CHROMATIC EDGE SPLIT / COSMETIC POST ONLY";
    case I::WeaponSignatures: return "SIX WEAPON FAMILIES / MUZZLE, WAKE, IMPACT, RICOCHET";
    case I::ReduceShake: return "SUPPRESSES IMPACT CAMERA SHAKE";
    case I::ReduceFlashing: return "SUPPRESSES HIT FLASH AND EDGE PULSES";
    case I::ReduceMotion: return "SUPPRESSES IMPACT ZOOM PUNCH";
    case I::Blood: return "BLOOD DROPLETS / COSMETIC GRAVITY AND SPLATS";
    case I::Audio: return "VOLUME / SOUND CHANNELS / REVERB / STEREO / HI-FI";
    case I::SoundChannels: return g_settings.extended_sound ? "EXTRA SOUND BOARDS / SPEECH NO LONGER CUTS GUNFIRE"
                                                            : "ONE BOARD, ORIGINAL PRIORITIES / SOUNDS CAN CUT EACH OTHER";
    case I::Reverb: return "ROOM ECHO / STUDIO: THE TV SET / ARENA: A BIG HALL";
    case I::StereoWidth: return "STEREO SPREAD / SUMS CLEANLY TO MONO";
    case I::HiFiAudio: return "SMOOTHS 8-BIT HASH / CABINET BASS / CLIP-FREE LIMITER";
    case I::Controls: return "REMAP KEYS AND PADS / TUNE YOUR STICKS";
    case I::HostMode: return g_host_mode.value_or(g_enhanced) != g_enhanced && g_record_local
        ? "HOSTING THIS MODE STARTS A NEW GAME" : "LEFT / RIGHT: CHOOSE THE MODE GUESTS WILL PLAY";
    case I::MainMenu: return "CHOOSE ORIGINAL ARCADE OR CUSTOM WIDESCREEN / ENDS THIS GAME";
    case I::RestartGame: return "ENDS THIS GAME / CONFIRMATION REQUIRED";
    case I::OperatorMenu: case I::SoundTest: return "OPENS THE ORIGINAL ARCADE OPERATOR MENU";
    case I::Quit: return "LEAVE THE SHOW / CONFIRMATION REQUIRED";
    case I::Motion: return "SMOOTH CAMERA PANS BETWEEN ARCADE FRAMES";
    case I::RunAhead: return "PREDICT LOCAL VIDEO / HIGHER VALUES USE MORE CPU";
    case I::Vsync: return "DISPLAY SYNC ONLY / GAME SPEED STAYS EXACT";
    case I::PixelFilter: return "GUEST: ARCADE / ROYALE: FINE MASK / LOTTES: FAST";
    case I::Glow: return "BRIGHT-PIXEL LIGHT SPILL / NOT OBJECT LIGHTING";
    case I::Sharpen: return "CONTRAST-ADAPTIVE DETAIL / ZERO CHANGES NO PIXELS";
    case I::Curvature: return "OPTIONAL TUBE SHAPE / OFF KEEPS STRAIGHT EDGES";
    case I::Scaling: return "CHOOSE HOW THE PICTURE FITS YOUR SCREEN";
    case I::StickDeadzone: return "RAISE TO STOP DRIFT / LOWER FOR SENSITIVITY";
    case I::ResetControls: return "RESTORE KEYBOARD AND CONTROLLER DEFAULTS";
    default: return "LEFT / RIGHT ADJUSTS / SETTINGS SAVE AUTOMATICALLY";
    }
}

// Reuse the menu surface while navigating. Only the final blit reaches
// the window, after the background, highlight and lettering are complete.
class MenuPaintSurface {
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_ = nullptr;
    int width_ = 0, height_ = 0;
public:
    ~MenuPaintSurface() {
        if (original_) SelectObject(memory_, original_);
        if (bitmap_) DeleteObject(bitmap_);
        if (memory_) DeleteDC(memory_);
    }
    HDC acquire(HDC window, int width, int height) {
        if (!memory_) memory_ = CreateCompatibleDC(window);
        if (!memory_) return nullptr;
        if (bitmap_ && width_ == width && height_ == height) return memory_;
        HBITMAP next = CreateCompatibleBitmap(window, width, height);
        if (!next) return nullptr;
        HGDIOBJ prior = SelectObject(memory_, next);
        if (!prior || prior == HGDI_ERROR) { DeleteObject(next); return nullptr; }
        if (bitmap_) DeleteObject(bitmap_);
        else original_ = prior;
        bitmap_ = next;
        width_ = width;
        height_ = height;
        return memory_;
    }
};

void paint(HWND wnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    RECT rc;
    GetClientRect(wnd, &rc);
    const int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) { EndPaint(wnd, &ps); return; }
    // Choose scale from the full enhanced canvas. A native scene keeps exactly
    // that pixel scale, crops to 410 source columns, and is centered as a unit.
    // The odd 137-column extension must not become a rounded 68-column shift.
    const stv::host::Scale sc = stv::host::compute_scale(cw, ch, vis_w(), VIS_H, !g_enhanced && g_scale_mode == stv::host::ScaleMode::IntegerFill
            ? stv::host::ScaleMode::IntegerExact : g_scale_mode);
    int w = int((std::int64_t(sc.w) * g_frame_width) / vis_w()), h = sc.h;
    int x0 = (cw - w) / 2, y0 = (ch - h) / 2;
    const auto& director=g_enhance_effects.packet().weaponFx.director;
    static constexpr float levels[]{0.f,.45f,.72f,1.f};
    const float overall=g_settings.enhance&&g_settings.combat_feedback
        ?levels[std::clamp(g_settings.combat_feedback,0,3)]:0.f;
    float trauma=overall*levels[std::clamp(g_settings.camera_shake,0,3)]*director.camera_trauma;
    if(g_settings.reduce_shake || g_settings.reduce_motion || g_menu || g_paused || g_connecting)trauma=0;
    const float phase = g_settings.smooth_motion &&
                        !g_menu && !g_paused && !g_connecting ? g_camera_phase : 1.0f;
    const float frame=float(g_enhance_effects.packet().frame);
    const float traumaX=std::sin(frame*2.399f)*trauma*3.2f;
    const float traumaY=std::cos(frame*1.713f)*trauma*2.4f;
    const float zoom=overall*levels[std::clamp(g_settings.zoom_punch,0,3)]*director.zoom_punch*
        (g_settings.reduce_motion||g_menu||g_paused||g_connecting?0.f:1.f);
    const int baseW=w,baseH=h;
    if(zoom>0){
        const int zoomW=std::max(w,int(w*(1.f+zoom))),zoomH=std::max(h,int(h*(1.f+zoom)));
        x0-=(zoomW-w)/2;y0-=(zoomH-h)/2;w=zoomW;h=zoomH;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_frame_width;
    bi.bmiHeader.biHeight = -VIS_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    // A paused player may only receive this one WM_PAINT until the next input.
    const bool one_shot = !g_net.on && (g_menu || g_paused || g_m.stop || g_connecting);
    bool overlay=g_menu||g_paused||g_m.stop||g_connecting||!g_status.empty()||(!g_toast.empty()&&GetTickCount64()<g_toast_until)||g_settings.show_fps;
    bool direct=g_resident_ready&&g_resident_effects.draw(wnd,cw,ch,x0,y0,w,h,g_settings,overlay,one_shot,
        g_camera.offset(g_camera.dx,phase)+traumaX,g_camera.offset(g_camera.dy,phase)+traumaY,baseW,baseH);
    static MenuPaintSurface menu_surface;
    const HDC window_dc = dc;
    HDC buffered = nullptr;
    if (!direct && (g_menu || (g_connecting && g_lobby))) {
        buffered = menu_surface.acquire(window_dc,cw,ch);
        if (buffered) dc = buffered;
    }
    bool drew_image=direct;
    if(direct&&overlay)dc=static_cast<HDC>(g_resident_effects.overlay_dc());
    if(!direct){
        HBRUSH black = HBRUSH(GetStockObject(BLACK_BRUSH));
        RECT bars[4] = {{0, 0, cw, y0}, {0, y0 + h, cw, ch}, {0, y0, x0, y0 + h}, {x0 + w, y0, cw, y0 + h}};
        for (auto& b : bars) FillRect(dc, &b, black);
        SetStretchBltMode(dc, COLORONCOLOR);
        if(g_resident_ready){
            g_menu_status="GPU presentation: "+g_resident_effects.error();
            build_software_frame(g_net.on && g_net.mr ? *g_net.mr : g_m);
        }
        if(g_pixels.size()<std::size_t(g_frame_width)*VIS_H)g_pixels.resize(std::size_t(g_frame_width)*VIS_H);
        // Explicit device-error fallback displays only the untouched arcade image.
        int result=StretchDIBits(dc,x0,y0,w,h,0,0,g_frame_width,VIS_H,g_pixels.data(),&bi,DIB_RGB_COLORS,SRCCOPY);
        drew_image=result!=0&&result!=int(GDI_ERROR);
    }
    g_present.destination = {x0, y0, x0 + w, y0 + h};
    g_present.client_width = cw; g_present.client_height = ch;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    if (g_menu || (g_connecting && g_lobby)) {
        RECT menu_background{0,0,cw,ch};
        SetDCBrushColor(dc,RGB(0,0,0));
        FillRect(dc,&menu_background,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (g_mode_menu) {
            BITMAPINFO art = bi;
            art.bmiHeader.biWidth = 547; art.bmiHeader.biHeight = -256;
            // The release ships no game artwork. Menus show the frame the
            // player's own ROM last drew in the main-menu artwork pane. Every
            // menu otherwise keeps a plain black background.
            const auto& backdrop = menu_backdrop(g_net.on && g_net.mr ? *g_net.mr : g_m);
            if (!backdrop.empty() && g_mode_menu && g_menu_font_ready) {
                const auto layout=stv::host::arcade_title_layout(cw,ch);
                SetDCBrushColor(dc,RGB(84,159,158));
                FillRect(dc,&layout.artwork_pane,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                RECT inner=layout.artwork_pane;
                const int rim=std::max(1,std::min(3,ch/240));
                InflateRect(&inner,-rim,-rim);
                SetDCBrushColor(dc,RGB(4,18,22));
                FillRect(dc,&inner,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                StretchDIBits(dc,layout.artwork.left,layout.artwork.top,
                              layout.artwork.right-layout.artwork.left,
                              layout.artwork.bottom-layout.artwork.top,
                              68,0,410,256,backdrop.data(),&art,DIB_RGB_COLORS,SRCCOPY);
            }
        }
        std::vector<std::string> lines;
        int footer_lines = 0;
        if (g_connecting) {
            lines={"SMASH T.V.", "[ CONNECTING CONTESTANTS ]", g_status, "B / ESC   CANCEL"};
        } else if (g_confirm != ConfirmAction::None) {
            const bool close_room = g_confirm == ConfirmAction::Disconnect && g_epic_room && (g_host_solo || g_net.host);
            const char* action = g_confirm == ConfirmAction::Quit ? "Quit to desktop" :
                g_confirm == ConfirmAction::MainMenu ? "Return to main menu" :
                g_confirm == ConfirmAction::Restart ? "Restart game" :
                close_room ? "Close public room" : "Leave online game";
            lines.push_back(std::string(action) + "?");
            lines.push_back(close_room ? "You will continue playing solo." :
                g_mode_menu ? "Smash T.V. will close." : "The current game will end.");
            lines.push_back("");
            lines.push_back(g_confirm_yes ? "  Cancel" : "> Cancel");
            lines.push_back(std::string(g_confirm_yes ? "> " : "  ") + action);
            lines.push_back("Up / Down: choose    A / Enter: select    B / Esc: back");
        } else if (g_mode_menu) {
            lines = {"SMASH T.V.", "[ MAIN MENU ]", ""};
            const std::string rows[] = {std::string("GAME  - < ") + stv::host::game_mode_label(g_enhanced) + " >",
                                        "START", "MULTIPLAYER", "OPTIONS", "GLOBAL LEADERBOARDS", "EXIT"};
            for (int i = 0; i < 6; ++i) lines.push_back(std::string(i == g_mode_selection ? "> " : "  ") + rows[i]);
            lines.push_back("");
            lines.push_back("ARROWS / D-PAD   ENTER / A TO SELECT");
        } else if (g_room_scores_open) {
            lines.push_back(g_enhanced ? "ROOM HIGH SCORES  /  WIDESCREEN" : "ROOM HIGH SCORES  /  ARCADE");
            for (const auto& row : room_scores()) {
                char text[64]{};
                std::snprintf(text,sizeof text,"%02X%02X%02X%02X     %c%c%c",row[0],row[1],row[2],row[3],
                    row[4]>=32 && row[4]<127 ? row[4] : ' ', row[5]>=32 && row[5]<127 ? row[5] : ' ',
                    row[6]>=32 && row[6]<127 ? row[6] : ' ');
                lines.push_back(text);
            }
            if (lines.size()==1) lines.push_back("No shared scores recorded yet.");
            lines.push_back("B / ESC   BACK");
        } else if (g_pad_controls) {
            lines.push_back("PAD CONTROLS - PLAYER " + std::to_string(g_pad_player + 1));
            lines.push_back(g_pad_capture ? "Press this player's button; RB clears; START cancels"
                                         : "Up/down: action; left/right: player; A: bind; B: back; X: defaults");
            lines.push_back("Left stick moves, right stick fires; a duplicate button swaps actions.");
            lines.push_back("");
            for (int i = 0; i < 10; ++i) {
                const int action = pad_action_for(g_pad_player, i);
                lines.push_back(std::string(i == g_pad_selection ? "> " : "  ") + kActionNames[action] + ": " +
                                pad_button_name(g_pad_bindings.buttons[action]));
            }
            if (!g_toast.empty() && GetTickCount64() < g_toast_until) lines.push_back(g_toast);
        } else if (g_remap >= 0) {
            lines.push_back("CONTROLS  (Esc keeps the current key, Backspace restores defaults)");
            lines.push_back("");
            for (int i = 0; i < ACTION_COUNT; ++i)
                lines.push_back(std::string(i == g_remap ? "> " : "  ") + kActionNames[i] + ":  " + key_name(g_keys[i]) +
                                (i == g_remap ? "   <- press a key" : ""));
        } else if (g_name_edit) {
            lines.push_back("PLAYER NAME");
            lines.push_back(g_name_draft + (g_name_draft.size() < 10 ? "_" : ""));
            lines.push_back(std::string(g_name_cursor, ' ') + "^");
            lines.push_back("Type a name, or use the controller:");
            lines.push_back("Left/right: position   Up/down: change letter");
            lines.push_back("Enter / A: save   Esc / B: cancel   Backspace / X: delete");
        } else if (g_password_prompt != PasswordPrompt::None) {
            lines.push_back(g_password_prompt==PasswordPrompt::Host ? "HOST PUBLIC GAME" : "LOCKED GAME");
            lines.push_back(g_password_prompt==PasswordPrompt::Host ? "Optional password (leave blank for open play)" : "Enter the room password");
            std::string masked(g_password_draft.size(), '*');
            if (g_password_cursor<masked.size()) masked.replace(g_password_cursor,1,"["+g_password_draft.substr(g_password_cursor,1)+"]");
            else masked += "_";
            lines.push_back("> " + masked);
            lines.push_back("Keyboard: type    Pad: up/down letter, left/right position");
            lines.push_back("Enter / A: continue    Esc / B: back");
        } else if (g_join_edit) {
            lines.push_back("JOIN BY ADDRESS");
            std::string address;
            for (wchar_t c : g_join_addr) address.push_back(char(c));
            lines.push_back(address + "_");
            lines.push_back("Type host[:port] or paste with Ctrl+V. Enter joins.");
            lines.push_back("Same network? Use Join LAN game.");
        } else if (!lobby_menu_lines(lines)) {
            lines.push_back("SMASH T.V.");
            lines.push_back(std::string("[ ") + g_pc_menu.title() + " ]");
            for (const auto& row : g_pc_menu.rows(menu_context())) {
                using I = stv::host::PcMenuItem;
                if (g_pc_menu.page() == stv::host::PcMenuPage::Main) {
                    if (row.item == I::Resume) lines.push_back("-- BACK TO THE SHOW --");
                    if (row.item == I::Video) lines.push_back("-- MAKE IT YOURS --");
                    if (row.item == I::RestartGame || (!g_started && row.item == I::OperatorMenu))
                        lines.push_back("-- ARCADE & SESSION --");
                }
                std::string label = std::string(row.selected ? "> " : "  ") + row.label;
                const auto value = menu_value(row.item);
                if (row.item == I::Vsync && g_settings.smooth_motion) label += " - on (Smooth Motion)";
                else if (!row.enabled) label += std::string(" - ") + row.unavailable_reason;
                else if (!value.empty()) label += " - " + value;
                else if (row.kind == stv::host::PcMenuRowKind::Submenu) label += " - >>";
                lines.push_back(label);
            }
            lines.push_back(menu_help(g_pc_menu.selected_item()));
            lines.push_back("D-PAD / ARROWS: MOVE   A / ENTER: SELECT");
            lines.push_back("B / ESC   BACK");
            footer_lines = 3;
        }
        if (!g_menu_status.empty()) {
            lines.push_back("");
            lines.push_back(g_menu_status);
            if (footer_lines) footer_lines += 2;
        }
        if (g_global_lb.is_open() && g_confirm == ConfirmAction::None) {
            g_global_lb.draw(dc, cw, ch, g_menu_font_ready ? &g_menu_font : nullptr, g_lb_client, lb_now());
        } else if(g_mode_menu && g_confirm == ConfirmAction::None && g_menu_status.empty()) {
            const auto layout=stv::host::arcade_title_layout(cw,ch);
            if(g_menu_font_ready)
                stv::host::arcade_title_controls(dc,cw,ch,g_menu_font,g_mode_selection,g_enhanced);
            else
                stv::host::draw_pc_menu(dc,cw,ch,std::move(lines),nullptr,&layout.controls);
        } else stv::host::draw_pc_menu(dc,cw,ch,std::move(lines),g_menu_font_ready?&g_menu_font:nullptr,
                                       nullptr,footer_lines);
    } else if (g_paused || g_m.stop || !g_status.empty()) {
        const char* msg = !g_status.empty() ? g_status.c_str() : "PAUSED (P)   START / F1: pause menu";
        TextOutA(dc, 8, 8, msg, int(std::strlen(msg)));
    } else if (!g_toast.empty() && GetTickCount64() < g_toast_until) {
        const int banner_h=std::clamp(ch/14,20,54);
        RECT banner{cw/12,ch/20,cw-cw/12,ch/20+banner_h};
        SetDCBrushColor(dc,RGB(127,26,28));
        FillRect(dc,&banner,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        InflateRect(&banner,-6,-banner_h/5);
        if(g_menu_font_ready) g_menu_font.draw(dc,banner,g_toast,false,RGB(255,212,78),RGB(127,26,28),DT_CENTER);
        else TextOutA(dc,banner.left,banner.top,g_toast.c_str(),int(g_toast.size()));
    }
    if (g_settings.show_fps && !g_menu) {
        char text[96];
        std::snprintf(text, sizeof(text), "Render %.1f FPS  |  Game %.2f Hz",
                      (g_paused || g_connecting) ? 0.0 : g_display_fps,
                      (g_paused || g_connecting) ? 0.0 : g_simulation_hz);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, RGB(0, 0, 0));
        SetTextColor(dc, RGB(255, 224, 80));
        SIZE extent{};
        GetTextExtentPoint32A(dc, text, int(std::strlen(text)), &extent);
        TextOutA(dc, std::max(8, cw - int(extent.cx) - 8), 8, text, int(std::strlen(text)));
    }
    if (buffered) drew_image = BitBlt(window_dc,0,0,cw,ch,buffered,0,0,SRCCOPY) != FALSE;
    const bool flushed = direct ? g_resident_effects.present() : GdiFlush() != FALSE;
    EndPaint(wnd, &ps);
    g_last_paint_ok = drew_image && flushed;
    if (g_frame_ready && !g_menu && !g_paused && !g_connecting && IsWindowVisible(wnd) && !IsIconic(wnd)) {
        if (g_last_paint_ok) {
            g_present.record();
            // Only the wait runs elsewhere. The completed GDI draw and every
            // game/audio action retain their original single-thread ownership.
            if (g_settings.sync_presentation() && !g_compositor_failed) (void)g_compositor.request();
        } else {
            if(!direct || !g_resident_effects.error().empty())++g_present.failures;
        }
    }
}

void handle_menu_key(HWND wnd, WPARAM wp) {
    if (g_menu && g_confirm != ConfirmAction::None) {
        if (wp == VK_UP || wp == VK_LEFT) g_confirm_yes = false;
        if (wp == VK_DOWN || wp == VK_RIGHT) g_confirm_yes = true;
        if (wp == VK_ESCAPE || wp == VK_RETURN) {
            const ConfirmAction selected = g_confirm;
            g_confirm = ConfirmAction::None;
            if (wp == VK_RETURN && g_confirm_yes) {
                if (selected == ConfirmAction::Quit) PostMessageW(wnd, WM_CLOSE, 0, 0);
                if (selected == ConfirmAction::MainMenu) {
                    g_restart_requested = true; g_mode_menu = true; g_title_submenu = false;
                    g_mode_selection = 1; g_pc_menu.close();
                }
                if (selected == ConfirmAction::Restart) g_restart_requested = true;
                if (selected == ConfirmAction::Disconnect) { g_close_room_requested=true; g_disconnect_requested=true; }
            }
        }
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
    if (g_global_lb.is_open()) {
        stv::host::LeaderboardInput input{};
        if (stv::host::leaderboard_key(wp, input)) {
            g_global_lb.input(input);
            g_global_lb.sync(g_lb_client, lb_now());
        }
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
    if (g_mode_menu) {
        if (wp == VK_UP || wp == VK_DOWN) g_mode_selection = (g_mode_selection + (wp == VK_UP ? 5 : 1)) % 6;
        if (g_mode_selection == 0 && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_RETURN)) {
            if (select_profile(!g_enhanced)) {
                FILE* saved = nullptr;
                if (!_wfopen_s(&saved, profile_path().c_str(), L"w") && saved) {
                    std::fputs(g_enhanced ? "enhanced" : "reference", saved); std::fclose(saved);
                }
            }
        } else if (wp == VK_RETURN && g_mode_selection == 5) {
            g_confirm = ConfirmAction::Quit; g_confirm_yes = false;
        } else if (wp == VK_RETURN && g_mode_selection == 4) {
            g_global_lb.open(g_enhanced ? 1 : 0);
            g_global_lb.sync(g_lb_client, lb_now());
        }
        else if (wp == VK_RETURN && g_mode_selection == 1) {
            g_record_local = true;
            g_room_boot_cmos = g_m.cmos;
            g_room_history.clear(); g_room_frames = 0;
            frontend_event("title_start", g_enhanced);
            g_mode_menu = g_menu = g_paused = false;
            sync_menu_input();
        } else if (wp == VK_RETURN && (g_mode_selection == 2 || g_mode_selection == 3)) {
            g_mode_menu = false;
            g_title_submenu = true;
            g_host_mode.reset();
            g_pc_menu.open(false);
            g_pc_menu.enter(g_mode_selection == 2 ? stv::host::PcMenuPage::Multiplayer : stv::host::PcMenuPage::Options);
            g_pc_menu.rows(menu_context());
        }
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
    if (g_menu && g_room_scores_open) {
        if (wp == VK_ESCAPE || wp == VK_RETURN) g_room_scores_open = false;
        InvalidateRect(wnd,nullptr,FALSE);
        return;
    }
    if (g_menu && g_name_edit) {
        const std::string alphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-";
        if (wp == VK_ESCAPE) g_name_edit = false;
        else if (wp == VK_RETURN) {
            g_settings.player_name = stv::host::pc_player_name(g_name_draft);
            if (g_lobby) g_lobby->set_name(g_settings.player_name);
            g_name_edit = false;
            save_preferences();
        } else if (wp == VK_LEFT && g_name_cursor) --g_name_cursor;
        else if (wp == VK_RIGHT) g_name_cursor = std::min<std::size_t>(9, std::min(g_name_cursor + 1, g_name_draft.size()));
        else if (wp == VK_BACK) {
            if (g_name_cursor) { --g_name_cursor; g_name_draft.erase(g_name_cursor, 1); }
            else if (!g_name_draft.empty()) g_name_draft.erase(0, 1);
        } else if (wp == VK_UP || wp == VK_DOWN) {
            g_name_cursor = std::min<std::size_t>(9, g_name_cursor);
            if (g_name_draft.size() <= g_name_cursor) g_name_draft.resize(g_name_cursor + 1, ' ');
            const auto index = alphabet.find(g_name_draft[g_name_cursor]);
            const int before = index == std::string::npos ? 0 : int(index);
            const int after = (before + (wp == VK_UP ? 1 : -1) + int(alphabet.size())) % int(alphabet.size());
            g_name_draft[g_name_cursor] = alphabet[std::size_t(after)];
        }
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
    if (g_menu && g_password_prompt!=PasswordPrompt::None) {
        auto prompt=g_password_prompt;
        if (wp==VK_ESCAPE) g_password_prompt=PasswordPrompt::None;
        else if (wp==VK_BACK && g_password_cursor) {
            --g_password_cursor;
            g_password_draft.erase(g_password_cursor,1);
        } else if (wp==VK_LEFT && g_password_cursor) --g_password_cursor;
        else if (wp==VK_RIGHT) g_password_cursor=std::min(g_password_cursor+1,g_password_draft.size());
        else if ((wp==VK_UP || wp==VK_DOWN) && g_password_cursor<32) {
            const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            if (g_password_cursor==g_password_draft.size()) g_password_draft += wp==VK_UP ? 'A' : '9';
            else {
                size_t pos=alphabet.find(g_password_draft[g_password_cursor]);
                if (pos==std::string::npos) pos=0;
                g_password_draft[g_password_cursor]=alphabet[(pos+alphabet.size()+(wp==VK_UP ? 1 : -1))%alphabet.size()];
            }
        }
        else if (wp==VK_RETURN && (prompt==PasswordPrompt::Host || !g_password_draft.empty())) {
            g_password_prompt=PasswordPrompt::None;
            g_room_password=g_password_draft;
            if (prompt==PasswordPrompt::Host) start_lobby_host(true,true);
            else if (selected_room() && selected_room()->room_id==g_password_room && selected_room()->address==g_password_address)
                handle_lobby_key(VK_RETURN);
            else { g_room_password.clear(); g_menu_status="That room closed. Choose another game."; }
        }
        if (g_password_prompt==PasswordPrompt::None) g_password_draft.clear();
        InvalidateRect(wnd,nullptr,FALSE); return;
    }
    if (g_menu && handle_lobby_key(wp)) {
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
    if (g_menu && g_pad_controls) {
        if (wp == VK_ESCAPE) {
            if (g_pad_capture) g_pad_capture = false;
            else g_pad_controls = false;
            g_pad_router.suppress_until_neutral();
        } else if (!g_pad_capture) {
            if (wp == VK_UP) g_pad_selection = (g_pad_selection + 9) % 10;
            else if (wp == VK_DOWN) g_pad_selection = (g_pad_selection + 1) % 10;
            else if (wp == VK_LEFT || wp == VK_RIGHT) g_pad_player = 1 - g_pad_player;
            else if (wp == VK_RETURN) {
                g_pad_capture = true;
                g_pad_router.suppress_until_neutral();
            } else if (wp == VK_DELETE) {
                g_pad_bindings.assign(pad_action(), 0); save_keys();
            } else if (wp == VK_BACK) {
                g_pad_bindings.reset(); save_keys();
                g_pad_router.suppress_until_neutral();
            }
        }
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
        if (g_menu && g_join_edit) {   // typing the host address for "join"
            if (wp == VK_ESCAPE) g_join_edit = false;
            else if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) paste_join_address(wnd);
            else if (wp == VK_RETURN && !g_join_addr.empty()) start_join();
            else if (wp == VK_RETURN) g_menu_status = "Enter a host address, or return and choose Find rooms.";
            else if (wp == VK_BACK && !g_join_addr.empty()) g_join_addr.pop_back();
            InvalidateRect(wnd, nullptr, FALSE);
            return;
        }
        if (g_menu && g_remap >= 0) {
            if (wp == VK_BACK) {
                for (int i = 0; i < ACTION_COUNT; ++i) g_keys[i] = kDefaultKeys[i];
                g_remap = -1;
            } else {
                if (wp != VK_ESCAPE) g_keys[g_remap] = int(wp);
                if (++g_remap >= ACTION_COUNT) g_remap = -1;
            }
            if (g_remap < 0) save_keys();
            InvalidateRect(wnd, nullptr, FALSE);
            return;
        }
        if (g_menu) {
            stv::host::PcMenuAction action;
            switch (wp) {
            case VK_UP: action = stv::host::PcMenuAction::Up; break;
            case VK_DOWN: action = stv::host::PcMenuAction::Down; break;
            case VK_LEFT: action = stv::host::PcMenuAction::Left; break;
            case VK_RIGHT: action = stv::host::PcMenuAction::Right; break;
            case VK_RETURN: action = stv::host::PcMenuAction::Accept; break;
            case VK_ESCAPE: action = stv::host::PcMenuAction::Back; break;
            default: return;
            }
            sync_menu_input();
            const auto event = g_pc_menu.handle(action, menu_context());
            if (event.item == stv::host::PcMenuItem::Multiplayer) g_host_mode.reset();
            frontend_event("menu_selection", unsigned(g_pc_menu.selected_item()), g_pc_menu.title());
            if (event.item != stv::host::PcMenuItem::None) frontend_event("menu_action", unsigned(event.item));
            if (g_title_submenu && g_pc_menu.page() == stv::host::PcMenuPage::Main) {
                g_title_submenu = false; g_mode_menu = g_menu = g_paused = true;
                return;
            }
            g_menu = g_pc_menu.is_open();
            using I = stv::host::PcMenuItem;
            bool save = event.delta != 0;
            switch (event.item) {
            case I::Resume: g_paused = false; break;
            case I::LocalCoop:
                g_title_submenu = false; g_paused = false;
                g_toast = g_enhanced ? "Player 2: add a credit, then press A or Arcade Start to join"
                                     : "Player 2: add a credit, then press Arcade Start to join";
                g_toast_until = GetTickCount64() + 5000;
                break;
            case I::MainMenu: g_confirm = ConfirmAction::MainMenu; break;
            case I::RestartGame: g_confirm = ConfirmAction::Restart; break;
            case I::Quit: g_confirm = ConfirmAction::Quit; break;
            case I::Disconnect: g_confirm = ConfirmAction::Disconnect; break;
            case I::KeyboardBindings: g_remap = 0; break;
            case I::PadBindings: g_pad_controls = true; g_pad_capture = false; break;
            case I::ResetControls:
                std::copy(std::begin(kDefaultKeys), std::end(kDefaultKeys), g_keys);
                g_pad_bindings.reset(); g_deadzone = 12000; g_swap_pads = false;
                save_keys(); save = true; g_pad_router.suppress_until_neutral(); break;
            case I::SwapPads: g_swap_pads = !g_swap_pads; break;
            case I::StickDeadzone:
                g_deadzone = std::clamp(g_deadzone + event.delta * 4000, 4000, 24000); save_keys(); break;
            case I::WindowScale:
                g_settings.window_scale = (g_settings.window_scale + event.delta + 7) % 7; resize_pc_window(); break;
            case I::Scaling:
                g_scale_mode = static_cast<stv::host::ScaleMode>((int(g_scale_mode) + event.delta + 3) % 3); break;
            case I::Fullscreen: toggle_fullscreen(); break;
            case I::PixelFilter: g_settings.filter = (g_settings.filter + event.delta + 8) % 8; g_menu_status.clear(); break;
            case I::Glow: g_settings.glow = (g_settings.glow + event.delta + 4) % 4; break;
            case I::Sharpen: g_settings.sharpen = (g_settings.sharpen + event.delta + 4) % 4; break;
            case I::Curvature: g_settings.curvature = (g_settings.curvature + event.delta + 4) % 4; break;
            case I::EnableAllEffects:
                stv::host::enable_all_visual_effects(g_settings);
                save = true; break;
            case I::EnhanceAll: g_settings.enhance = !g_settings.enhance; break;
            case I::Particles: g_settings.particles = !g_settings.particles; break;
            case I::Blood: g_settings.blood = !g_settings.blood; break;
            case I::Debris: g_settings.debris = !g_settings.debris; break;
            case I::Smoke: g_settings.smoke = !g_settings.smoke; break;
            case I::Fire: g_settings.fire = !g_settings.fire; break;
            case I::Shockwaves: g_settings.shockwaves = !g_settings.shockwaves; break;
            case I::PersistentGore: g_settings.persistent_gore = !g_settings.persistent_gore; break;
            case I::Destruction: g_settings.destruction = !g_settings.destruction; break;
            case I::WeaponImpacts: g_settings.weapon_impacts = !g_settings.weapon_impacts; break;
            case I::FluidSmoke: g_settings.fluid_smoke = !g_settings.fluid_smoke; break;
            case I::HardwareRT:
                if(g_settings.hardware_rt)g_settings.hardware_rt=false;
                else if(g_resident_effects.prepare_dxr())g_settings.hardware_rt=true;
                else g_menu_status="Hardware ray tracing: "+g_resident_effects.dxr_error();
                break;
            case I::ExplosiveDebris: g_settings.explosive_debris = !g_settings.explosive_debris; break;
            case I::MuzzleFlash: g_settings.muzzle_flash = !g_settings.muzzle_flash; break;
            case I::Brass: g_settings.brass = !g_settings.brass; break;
            case I::Voxels: g_settings.voxels = !g_settings.voxels; break;
            case I::Atmosphere: g_settings.atmosphere = (g_settings.atmosphere + event.delta + 4) % 4; break;
            case I::SpriteRim: g_settings.sprite_rim = (g_settings.sprite_rim + event.delta + 4) % 4; break;
            case I::ContactShadows: g_settings.contact_shadows = (g_settings.contact_shadows + event.delta + 4) % 4; break;
            case I::WetMaterials: g_settings.wet_materials = (g_settings.wet_materials + event.delta + 4) % 4; break;
            case I::HeatDistortion: g_settings.heat_distortion = (g_settings.heat_distortion + event.delta + 4) % 4; break;
            case I::EmissiveSpill: g_settings.emissive_spill = (g_settings.emissive_spill + event.delta + 4) % 4; break;
            case I::CinematicGrade: g_settings.cinematic_grade = (g_settings.cinematic_grade + event.delta + 4) % 4; break;
            case I::RoomCinema:
                g_settings.room_cinema = (g_settings.room_cinema + event.delta + 4) % 4;
                if (!g_settings.room_cinema) g_audio_peaks.reset();
                break;
            case I::CombatFeedback: g_settings.combat_feedback=(g_settings.combat_feedback+event.delta+4)%4;break;
            case I::CameraShake: g_settings.camera_shake=(g_settings.camera_shake+event.delta+4)%4;break;
            case I::ZoomPunch: g_settings.zoom_punch=(g_settings.zoom_punch+event.delta+4)%4;break;
            case I::HitFlash: g_settings.hit_flash=(g_settings.hit_flash+event.delta+4)%4;break;
            case I::EdgeEffects: g_settings.edge_effects=(g_settings.edge_effects+event.delta+4)%4;break;
            case I::WeaponSignatures: g_settings.weapon_signatures=(g_settings.weapon_signatures+event.delta+4)%4;break;
            case I::ReduceShake: g_settings.reduce_shake=!g_settings.reduce_shake;break;
            case I::ReduceFlashing: g_settings.reduce_flashing=!g_settings.reduce_flashing;break;
            case I::ReduceMotion: g_settings.reduce_motion=!g_settings.reduce_motion;break;
            case I::Motion: g_settings.smooth_motion = !g_settings.smooth_motion; break;
            case I::RunAhead: g_settings.run_ahead = (g_settings.run_ahead + event.delta + 4) % 4; break;
            case I::Vsync: g_settings.vsync = !g_settings.vsync; break;
            case I::ShowFps: g_settings.show_fps = !g_settings.show_fps; break;
            case I::MasterVolume:
                g_settings.volume = std::clamp(g_settings.volume + event.delta * 5, 0, 100);
                g_audio.set_volume(g_settings.volume); break;
            case I::Mute: g_settings.muted = !g_settings.muted; g_audio.set_muted(g_settings.muted); break;
            case I::SoundChannels: g_settings.extended_sound = !g_settings.extended_sound; break;
            case I::Reverb: g_settings.reverb = (g_settings.reverb + event.delta + 3) % 3; break;
            case I::StereoWidth: g_settings.audio_wide = !g_settings.audio_wide; break;
            case I::HiFiAudio: g_settings.audio_hifi = !g_settings.audio_hifi; break;
            case I::PlayerName:
                g_name_edit = true; g_name_draft = g_settings.player_name; g_name_cursor = 0; break;
            case I::HostMode: g_host_mode = !g_host_mode.value_or(g_enhanced); save = false; break;
            case I::PublicBrowser: start_lobby_search(true); break;
            case I::PublicHost: g_password_prompt=PasswordPrompt::Host; g_password_draft.clear(); g_password_cursor=0; break;
            case I::HostGame: start_lobby_host(); break;
            case I::RoomScores: retain_room_scores(); g_room_scores_open = true; g_menu = true; break;
            case I::DirectHost: start_lobby_host(true); break;
            case I::FindRooms: start_lobby_search(); break;
            case I::DirectJoin: g_join_edit = true; g_join_addr = last_join_addr(); break;
            case I::OperatorMenu: case I::SoundTest:
                g_test_frames = 30; g_menu = false; g_paused = false; break;
#if !defined(STV_PLAYER_RELEASE) || !STV_PLAYER_RELEASE
            case I::PlayfieldProfile: select_profile(!g_enhanced); break;
#endif
            default: break;
            }
            if (((event.item >= I::EnhanceAll && event.item <= I::ExplosiveDebris) ||
                 event.item==I::RoomCinema || event.item==I::HardwareRT ||
                 (event.item >= I::Atmosphere && event.item <= I::CinematicGrade) ||
                 (event.item>=I::CombatFeedback && event.item<=I::ReduceMotion)) &&
                g_frame_ready && g_resident_ready) {
                g_resident_ready = g_resident_effects.recompose(g_enhance_effects.packet(), g_settings);
                if (!g_resident_ready) build_software_frame(g_net.on && g_net.mr ? *g_net.mr : g_m);
                ++g_pixel_revision;
            }
            if (g_confirm != ConfirmAction::None) {
                g_confirm_yes = false; g_menu = true;
            }
            if (save) save_preferences();
            sync_menu_input();
            InvalidateRect(wnd, nullptr, FALSE);
            return;
        }
}

void poll_pad_controls() {
    static ULONGLONG previous = 0;
    const auto now = GetTickCount64();
    if (now - previous < 8) return;
    previous = now;
    sync_menu_input();
    stv::host::PadEvent event = stv::host::PadEvent::None;
#ifdef STV_SDL_PADS
    if (g_sdl_pads && g_sdl_pads->ready()) g_sdl_pads->refresh();
#endif
    for (int slot = 0; slot < 2; ++slot) {
        static std::array<bool,2> firing{};
        XINPUT_STATE x{};
        stv::host::PadSnapshot state{};
#ifdef STV_SDL_PADS
        if (g_sdl_pads && g_sdl_pads->ready()) {
            const auto sample = g_sdl_pads->read(slot);
            state = sample.state;
            x.Gamepad.sThumbLX = SHORT(sample.lx);
            x.Gamepad.sThumbLY = SHORT(std::clamp(sample.ly, -32768, 32767));
            x.Gamepad.sThumbRX = SHORT(sample.rx);
            x.Gamepad.sThumbRY = SHORT(std::clamp(sample.ry, -32768, 32767));
        } else
#endif
        if ((state.connected = pad_state(DWORD(slot), x))) {
            state.buttons = x.Gamepad.wButtons;
            if (x.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) state.buttons |= stv::host::PadLT;
            if (x.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) state.buttons |= stv::host::PadRT;
        }
        if (state.connected && !g_enhanced) {
            state.move = std::uint8_t(stick_bits(x.Gamepad.sThumbLX, x.Gamepad.sThumbLY));
            state.fire = std::uint8_t(stick_bits(x.Gamepad.sThumbRX, x.Gamepad.sThumbRY));
        }
        if (state.connected && g_enhanced) {
            const double lx=x.Gamepad.sThumbLX, ly=-double(x.Gamepad.sThumbLY);
            const double radius=std::hypot(lx,ly);
            state.move=0;
            if (radius>g_deadzone) {
                const double strength=std::min(1.0,(radius-g_deadzone)/(32767.0-g_deadzone));
                // Keep the direction true on screen with non-square arcade pixels.
                const double nx=lx/stv::host::kPixelAspect;
                const double scale=strength*127/std::hypot(nx,ly);
                const int mx=int(std::lround(nx*scale)), my=int(std::lround(ly*scale));
                state.motion=u16(u8(mx))|(u16(u8(my))<<8);
                if(state.motion) {
                    constexpr u8 directions[]={1,9,8,10,2,6,4,5};
                    const int sector=int(std::lround(std::atan2(nx,-ly)*4/3.14159265358979323846));
                    state.move=directions[(sector+8)%8];
                }
            }
            const double sx=x.Gamepad.sThumbRX, sy=x.Gamepad.sThumbRY;
            const double threshold=g_deadzone * (firing[slot] ? 0.82 : 1.0);
            state.fire=0;
            if (sx*sx + sy*sy > threshold*threshold) {
                // Clockwise from up, corrected for the arcade pixel aspect.
                const double angle=std::atan2(sx/stv::host::kPixelAspect,sy);
                const double sectors=angle*(4.0/3.14159265358979323846);
                const int sector=int(std::floor(sectors+0.5));
                constexpr int bits[]={1,9,8,10,2,6,4,5};
                state.fire=std::uint8_t(bits[(sector+8)%8]);
                state.aim=std::uint8_t(32+std::lround((sectors-sector)*62));
            }
        }
        firing[slot]=state.aim!=0;
        const auto next = g_pad_router.update(slot, state, GetForegroundWindow() == g_wnd, now);
        if (event == stv::host::PadEvent::None || next == stv::host::PadEvent::Open) event = next;
    }
    // START (or the legacy stick chord) backs out of a child or toggles pause.
    if (g_connecting) {
        if (event == stv::host::PadEvent::Open || event == stv::host::PadEvent::Back) g_cancel_connect = true;
        return;
    }
    if (event == stv::host::PadEvent::Open) {
        if (g_menu && g_remap >= 0) { g_remap = -1; save_keys(); }
        else if (g_menu) handle_menu_key(g_wnd, VK_ESCAPE);
        else g_menu = true;
        sync_menu_input();
        g_pad_router.suppress_until_neutral();
        InvalidateRect(g_wnd, nullptr, FALSE);
        return;
    }
    // Pad navigation must not manufacture keyboard bindings in the key-capture page.
    if (g_menu && g_remap >= 0) {
        if (event == stv::host::PadEvent::Back) {
            g_remap = -1; save_keys();
            InvalidateRect(g_wnd, nullptr, FALSE);
        }
        return;
    }
    if (g_menu && g_pad_controls && g_pad_capture) {
        const auto fresh = g_pad_router.fresh(g_swap_pads ? 1 - g_pad_player : g_pad_player);
        if (fresh & stv::host::PadClear) {
            g_pad_bindings.assign(pad_action(), 0);
        } else {
            const auto button = fresh & stv::host::PadAssignable;
            if (!button || !stv::host::PadBindings::valid(button)) return;
            const int swapped = g_pad_bindings.assign(pad_action(), button);
            if (swapped >= 0) {
                g_toast = "Swapped duplicate binding with " + std::string(kActionNames[swapped]);
                g_toast_until = now + 3000;
            }
        }
        g_pad_capture = false;
        save_keys();
        g_pad_router.suppress_until_neutral();
        InvalidateRect(g_wnd, nullptr, FALSE);
        return;
    }
    if (!g_menu) return;
    if (g_global_lb.is_open()) {
        // Shoulders/triggers/Y are not menu events; read them as fresh presses.
        for (int slot = 0; slot < 2; ++slot) {
            stv::host::LeaderboardInput inputs[5];
            const int count = stv::host::leaderboard_pad(g_pad_router.fresh(slot), inputs);
            for (int i = 0; i < count && g_global_lb.is_open(); ++i) g_global_lb.input(inputs[i]);
            if (count) InvalidateRect(g_wnd, nullptr, FALSE);
        }
        if (event == stv::host::PadEvent::Reset) {
            g_global_lb.input(stv::host::LeaderboardInput::Refresh);
            InvalidateRect(g_wnd, nullptr, FALSE);
        }
    }
    switch (event) {
    case stv::host::PadEvent::Up: handle_menu_key(g_wnd, VK_UP); break;
    case stv::host::PadEvent::Down: handle_menu_key(g_wnd, VK_DOWN); break;
    case stv::host::PadEvent::Left: handle_menu_key(g_wnd, VK_LEFT); break;
    case stv::host::PadEvent::Right: handle_menu_key(g_wnd, VK_RIGHT); break;
    case stv::host::PadEvent::Confirm: handle_menu_key(g_wnd, VK_RETURN); break;
    case stv::host::PadEvent::Back: handle_menu_key(g_wnd, VK_ESCAPE); break;
    case stv::host::PadEvent::Reset: if (g_pad_controls || g_name_edit || g_password_prompt!=PasswordPrompt::None) handle_menu_key(g_wnd, VK_BACK); break;
    default: break;
    }
    sync_menu_input();
}

// Losing the foreground (Alt+Tab, another window, minimize) never pauses or
// otherwise changes the game; it keeps running. Only input is protected: keys
// and pad buttons held while away are ignored until released after returning,
// and the window repaints on return so no stale frame is left on screen. The
// main loop re-checks every iteration, so a missed message cannot skip this.
bool g_window_inactive = false;
bool window_inactive() {
    return g_wnd && (GetForegroundWindow() != g_wnd || IsIconic(g_wnd) || !IsWindowVisible(g_wnd));
}
void drop_held_input() {
    g_keyboard_blocked = true;
    g_pad_router.suppress_until_neutral();
}
void window_left() {
    if (g_window_inactive) return;
    g_window_inactive = true;
    drop_held_input();
}
void window_reactivated() {
    if (!g_window_inactive) return;
    g_window_inactive = false;
    drop_held_input();
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}
void service_window_focus() {
    if (window_inactive()) window_left();
    else window_reactivated();
}

LRESULT CALLBACK wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) == wnd && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_APP + 12: InvalidateRect(wnd, nullptr, FALSE); return 0;
    case WM_PAINT: paint(wnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_KILLFOCUS:
        window_left();
        return 0;
    case WM_SETFOCUS:
        if (!window_inactive()) window_reactivated();
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) window_left();
        else if (!window_inactive()) window_reactivated();
        break;
    case WM_ACTIVATEAPP:
        if (!wp) window_left();
        break;
    case WM_SYSCOMMAND:
        // A bare Alt tap would open the system menu's modal loop and freeze the game loop.
        if ((wp & 0xfff0) == SC_KEYMENU && lp == 0) return 0;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { if (!(lp & (LPARAM(1) << 30))) toggle_fullscreen(); return 0; }
        break;
    case WM_CHAR:
        if (g_menu && g_password_prompt!=PasswordPrompt::None) {
            if (wp>=32 && wp<=126 && g_password_draft.size()<32)
                g_password_draft.insert(g_password_cursor++,1,char(wp));
            InvalidateRect(wnd,nullptr,FALSE); return 0;
        }
        if (g_menu && g_name_edit && g_name_cursor < 10 &&
            ((wp >= '0' && wp <= '9') || (wp >= 'a' && wp <= 'z') || (wp >= 'A' && wp <= 'Z') ||
             wp == ' ' || wp == '.' || wp == '-')) {
            char c = char(wp);
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
            if (g_name_cursor == g_name_draft.size()) g_name_draft.push_back(c);
            else g_name_draft[g_name_cursor] = c;
            g_name_cursor = std::min<std::size_t>(10, g_name_cursor + 1);
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }
        if (g_menu && g_join_edit && g_join_addr.size() < 259 &&
            ((wp >= '0' && wp <= '9') || (wp >= 'a' && wp <= 'z') || (wp >= 'A' && wp <= 'Z') || wp == '.' || wp == ':' || wp == '-')) {
            g_join_addr.push_back(wchar_t(wp));
            InvalidateRect(wnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (g_connecting) {
            if (wp == VK_ESCAPE) g_cancel_connect = true;
            return 0;
        }
        if (g_mode_menu) {
            if ((lp & (LPARAM(1) << 30)) && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
            handle_menu_key(wnd, wp); return 0;
        }
        if ((lp & (LPARAM(1) << 30)) && (wp == 'P' || wp == VK_F9 || wp == VK_F11 || wp == VK_F1 || wp == VK_ESCAPE)) return 0;
        if (wp == VK_F1 || (wp == VK_ESCAPE && !g_menu)) {
            if (g_menu && g_remap >= 0) { g_remap = -1; save_keys(); }
            else if (g_menu) handle_menu_key(wnd, VK_ESCAPE);
            else g_menu = true;
            sync_menu_input(); InvalidateRect(wnd, nullptr, FALSE); return 0;
        }
        if (g_menu) {
            if ((lp & (LPARAM(1) << 30)) && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_BACK)) return 0;
            handle_menu_key(wnd, wp);
            sync_menu_input();
            return 0;
        }
        if (wp == VK_F11) { toggle_fullscreen(); return 0; }
        if (wp == 'P') { g_paused = !g_paused; InvalidateRect(wnd, nullptr, FALSE); return 0; }
        if (wp == VK_F9) { cycle_scale(); save_preferences(); InvalidateRect(wnd, nullptr, FALSE); return 0; }
        break;
    case WM_CLOSE:
        write_player_log("Session closed normally.");
        retain_room_scores();
        g_lb_client.stop();   // queued scores stay on disk for the next launch
        close_lobby();
        save_preferences(); save_cmos(); DestroyWindow(wnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/"));
}

// The narrow-path loaders use the ANSI code page. A lossy conversion once
// caused a failed import to delete the user's copy. Try lossless conversion,
// then the 8.3 short path, before falling back to best effort.
std::string narrow(const std::wstring& w) {
    const auto convert = [](const std::wstring& text, bool& lossy) {
        std::string s;
        BOOL replaced = FALSE;
        const int n = text.empty() ? 0 : WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(), int(text.size()),
                                                             nullptr, 0, nullptr, &replaced);
        if (n > 0) {
            s.resize(std::size_t(n));
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(), int(text.size()), s.data(), n, nullptr, &replaced);
        }
        lossy = replaced != FALSE || (n <= 0 && !text.empty());
        return s;
    };
    bool lossy = false;
    std::string s = convert(w, lossy);
    if (!lossy) return s;
    wchar_t brief[MAX_PATH];
    const DWORD n = GetShortPathNameW(w.c_str(), brief, MAX_PATH);
    if (n && n < MAX_PATH) {
        bool short_lossy = false;
        std::string t = convert(std::wstring(brief, n), short_lossy);
        if (!short_lossy) return t;
    }
    return s;
}

void parse_launcher_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--classic") g_launch_mode = 0;
        else if (arg == L"--widescreen") g_launch_mode = 1;
        else if (arg.rfind(L"--data=", 0) == 0) g_requested_data_dir = arg.substr(7);
        else if (arg == L"--data" && i + 1 < argc) g_requested_data_dir = argv[++i];
    }
    if (argv) LocalFree(argv);

    wchar_t value[32768]{};
    if (!GetEnvironmentVariableW(L"RECOMPLAUNCHER", value, DWORD(std::size(value)))) g_recomp_launcher = false;
    else g_recomp_launcher = value[0] == L'1' && value[1] == L'\0';
    if (!g_recomp_launcher) return;
    if (g_requested_data_dir.empty() &&
        GetEnvironmentVariableW(L"RECOMPLAUNCHER_DATA", value, DWORD(std::size(value))) &&
        value[0]) {
        g_requested_data_dir = value;
    }
    if (GetEnvironmentVariableW(L"RECOMPLAUNCHER_MODE", value, DWORD(std::size(value))) &&
        std::wcscmp(value, L"bigpicture") == 0) {
        g_launcher_big_picture = true;
    }
    if (GetEnvironmentVariableW(L"RECOMPLAUNCHER_PAD", value, DWORD(std::size(value))) &&
        std::wcscmp(value, L"playstation") == 0) {
        g_launcher_playstation = true;
    }
}

void write_player_log(std::string_view text) {
    if (g_data_dir.empty()) return;
    const auto directory = std::filesystem::path(g_data_dir) / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return;
    FILE* file = nullptr;
    const auto path = (directory / L"SmashTV.log").wstring();
    if (_wfopen_s(&file, path.c_str(), L"a") != 0 || !file) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u  %.*s\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                 int(text.size()), text.data());
    std::fclose(file);
}

// Milliseconds since process start, for the startup stage lines in SmashTV.log.
void log_startup_stage(const char* stage) {
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) { return (u64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    char line[128];
    std::snprintf(line, sizeof line, "Startup: %s at %llu ms.", stage,
                  (unsigned long long)((ticks(now) - ticks(created)) / 10000));
    write_player_log(line);
}

bool find_roms(std::filesystem::path& cache) {
    if (!g_requested_data_dir.empty()) {
        g_data_dir = g_requested_data_dir;
    } else {
        wchar_t app[32768];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", app, DWORD(std::size(app)));
        g_data_dir = n && n < std::size(app) ? std::wstring(app) + L"\\SmashTV" : exe_dir();
    }
    std::error_code ec;
    std::filesystem::create_directories(g_data_dir, ec);
    if (ec) {
        MessageBoxW(nullptr, L"Could not create the SmashTV data folder.", L"Smash T.V. setup", MB_ICONERROR);
        return false;
    }
    const auto log = std::filesystem::path(g_data_dir) / L"logs" / L"SmashTV.log";
    const auto log2 = std::filesystem::path(g_data_dir) / L"logs" / L"SmashTV.previous.log";
    const auto log3 = std::filesystem::path(g_data_dir) / L"logs" / L"SmashTV.older.log";
    MoveFileExW(log2.c_str(), log3.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    MoveFileExW(log.c_str(), log2.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    DeleteFileW(log.c_str());
    write_player_log(g_recomp_launcher ? "Launcher session starting." : "Standalone session starting.");
    cache = std::filesystem::path(g_data_dir) /
            (g_recomp_launcher ? std::filesystem::path(L"cache") / L"retail-l8.dat"
                                : std::filesystem::path(L"retail-l8.dat"));
    std::filesystem::create_directories(cache.parent_path(), ec);
    if (ec) {
        write_player_log("The data cache folder could not be created.");
        return false;
    }
    std::string error;
    if (stv::gsp::load_imported_roms(g_m, cache, error)) {
        write_player_log("Private retail L8 cache loaded.");
        return true;
    }

    wchar_t selected[32768]{};
    const DWORD selected_length = GetEnvironmentVariableW(L"STV_HOST_ROM_ZIP", selected, DWORD(std::size(selected)));
    std::filesystem::path source;
    if (selected_length && selected_length < std::size(selected)) source = selected;
    while (true) {
        if (source.empty()) {
            wchar_t file[32768]{};
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.lpstrFilter = L"Smash T.V. ROM archives (*.zip)\0*.zip\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = DWORD(std::size(file));
            ofn.lpstrTitle = L"Smash T.V. setup | Select ROM ZIP (split, merged or non-merged)";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
            if (!GetOpenFileNameW(&ofn)) return false;
            source = file;
        }
        wchar_t temp[MAX_PATH]{};
        if (!GetTempFileNameW(g_data_dir.c_str(), L"stv", 0, temp)) {
            write_player_log("The temporary import file could not be created.");
            MessageBoxW(nullptr, L"Could not create a file in the SmashTV data folder.", L"Smash T.V. setup", MB_ICONERROR);
            return false;
        }
        if (stv::gsp::import_rom_set(source, temp, error) && stv::gsp::load_imported_roms(g_m, temp, error)) {
            if (MoveFileExW(temp, cache.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                write_player_log("Private retail L8 ROM import completed.");
                return true;
            }
            error = "Could not finish saving the imported data in the SmashTV data folder.";
        }
        DeleteFileW(temp);
        write_player_log("ROM import failed: " + error);
        if (MessageBoxA(nullptr, error.c_str(), "Smash T.V. setup", MB_ICONWARNING | MB_RETRYCANCEL) != IDRETRY)
            return false;
        source.clear();
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // per-monitor DPI aware: Windows must not bitmap-stretch the window (4K integer scaling)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    parse_launcher_args();
    std::filesystem::path zip;
    if (!find_roms(zip)) return 1;
    std::string err;
    g_rom_path = zip;
    capture_menu_title(g_rom_path);
    g_menu_font_ready = g_menu_font.bind(g_m,[](u32 addr){return stv::gsp::rlong(g_m,addr);});
    g_enhanced = load_profile();
    if (g_enhanced) {
        g_m.enhanced = true;
        g_m.set_wide(1024, 137);
        if (!stv::gsp::apply_enhanced(g_m, 137)) {
            MessageBoxA(nullptr, "Enhanced profile table does not match the L8 data", "Smash T.V.", MB_ICONERROR);
            return 2;
        }
    }
    g_m.reset();
    load_cmos();
    load_keys();
    g_pc = g_m.reset_vector();
    g_proof.begin(g_enhanced, g_m.cmos);
    log_startup_stage("ROM data and profile ready");
    if (!g_snd.load_roms(g_m.snd_u4, g_m.snd_u19, g_m.snd_u20, err)) {
        MessageBoxA(nullptr, ("The sound ROMs could not be imported:\n" + err).c_str(), "Smash T.V.", MB_ICONERROR);
        return 2;
    }
    // Compile the resident shaders before starting the sound device and game clock.
    g_enhance_effects.use_gpu_simulation(true);
    if (!g_resident_effects.prepare()) g_menu_status = "GPU effects: " + g_resident_effects.error();
    log_startup_stage("GPU effects prepared");
    bool audio = g_audio.open();
    log_startup_stage("audio opened");
    load_preferences();
    if(g_settings.hardware_rt && !g_resident_effects.prepare_dxr())
        g_menu_status="Hardware ray tracing: "+g_resident_effects.dxr_error();
    log_startup_stage("ray tracing prepared");
    const bool audio_initial_device = audio;
    g_mixer = std::make_unique<stv::snd::Mixer>(g_audio.rate());
    wchar_t audio_timeline[8]{};
    g_audio_telemetry.start(GetEnvironmentVariableW(L"STV_AUDIO_TELEMETRY", audio_timeline, 8) == 1 &&
                            audio_timeline[0] == L'1');
    g_snd.mixer = g_mixer.get();
    g_snd.power_on();
    g_sched = std::make_unique<stv::game::CoScheduler>(g_m, g_snd);
    connect_confirmed_sound();
    connect_machine_callbacks();
#ifdef STV_SDL_PADS
    SDL_SetMainReady();
    g_sdl_pads = std::make_unique<stv::host::SdlPads>();
    if (g_sdl_pads->ready()) {
        SDL_SetGamepadEventsEnabled(false);
        SDL_SetJoystickEventsEnabled(false);
    }
#endif
    g_achievements.start(scripted_run() || GetEnvironmentVariableW(L"STV_HOST_FRAMES", nullptr, 0) != 0);
    start_leaderboard(scripted_run() || GetEnvironmentVariableW(L"STV_HOST_FRAMES", nullptr, 0) != 0);
    if (HMODULE x = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        g_xinput = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")));

    if (HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        g_dwm_flush = reinterpret_cast<DwmFlushFn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmFlush")));

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_SMASHTV));
    wc.lpszClassName = L"SmashTVNative";
    RegisterClassW(&wc);
    // initial client area: the integer "fill" size for 85% of the primary work area
    // (physical pixels, the process is DPI aware), so 4K starts at 5x6 rather than tiny
    RECT work{0, 0, 1280, 960};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const stv::host::Scale start = stv::host::compute_scale((work.right - work.left) * 85 / 100,
                                                            (work.bottom - work.top) * 85 / 100, vis_w(), VIS_H,
                                                            stv::host::ScaleMode::IntegerFill);
    RECT r{0, 0, std::max(start.w, 640), std::max(start.h, 480)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowW(wc.lpszClassName, L"Smash T.V.", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    const bool launch_online = parse_net_args();
    if (!scripted_run() && !launch_online && g_launch_mode < 0) {
        g_mode_menu = g_menu = g_paused = true;
    }
    ShowWindow(g_wnd, show);
    if (g_settings.window_scale) resize_pc_window();
    if (g_launcher_big_picture && !g_settings.fullscreen) g_transient_fullscreen = true;
    if (g_settings.fullscreen || g_launcher_big_picture) toggle_fullscreen(false);
    if (launch_online && !net_connect(zip)) return 3;
    log_startup_stage("window shown");

    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    stv::host::FrameClock simulation_clock;
    if (!simulation_clock.configure(u64(freq.QuadPart))) {
        MessageBoxA(g_wnd, "The performance-counter frequency is unsupported", "Smash T.V.", MB_ICONERROR);
        return 4;
    }
    static_assert(PS_PER_FRAME == stv::host::FrameClock::kRateDenominator * PS_PER_PIXEL);
    u64 frames_since_save = 0, clock_rebases = 0, clock_suspensions = 0;
    // Offline host suspensions longer than this are not replayed as fast-forward.
    constexpr u64 kSuspensionSeconds = 1;
    double simulation_max_late_ms = 0;
    bool clock_paused = false, output_was_paused = false, previous_net_mode = g_net.on;
    g_present = {};
    g_simulation_steps = 0;
    // Output failures never rebase or retime the game. The same exact QPC
    // deadlines apply with or without an endpoint; raw mixer samples are kept
    // on the original board clock and only the device sink matches clock drift.
    auto audio_status = [&] {
        const bool was_available = audio;
        audio = g_audio.ok();
        if (audio == was_available) return;
        g_toast = audio ? "Audio output restored." : "Audio output unavailable; reconnecting.";
        g_toast_until = GetTickCount64() + 5000;
        frontend_event(audio ? "audio_output_restored" : "audio_output_lost",
                       g_audio.errors(), g_audio.error_stage());
    };
    if (!audio) {
        g_toast = "Audio output unavailable; reconnecting.";
        g_toast_until = GetTickCount64() + 5000;
    }
    // Smoke test: STV_HOST_FRAMES=N runs N frames, writes timing/audio statistics to
    // %TEMP%\smashtv_host.json and exits.
    u64 smoke_frames = 0, frames_run = 0, samples_made = 0;
    u64 raw_pcm_hash = stv::host::ConfirmedAudioAudit::kOffset;
    {
        wchar_t buf[32];
        if (GetEnvironmentVariableW(L"STV_HOST_FRAMES", buf, 32)) smoke_frames = _wtoi64(buf);
        wchar_t path[MAX_PATH];
        if (GetEnvironmentVariableW(L"STV_HOST_INPUTS", path, MAX_PATH) &&
            !stv::net::load_scripted_inputs(narrow(path), smoke_frames ? u32(smoke_frames) : 1u << 21, g_script1, g_script2)) {
            MessageBoxA(nullptr, "STV_HOST_INPUTS: cannot read the input file", "Smash T.V.", MB_ICONERROR);
            return 1;
        }
    }
    // Player start: the original power-on tests, memory check and CMOS notice still
    // execute, in full and in order, but unseen and unpaced with released controls;
    // the first presented frame is the retail TITLPAGE intro image (ATSTRT has stored
    // GAMSTATE 30h and TITLE has rendered for 320 frames, also past the
    // "FAILURE IN CMOS RAM / DEFAULT ADJUSTMENTS RESTORED" notice of a first run:
    // default-adjustments screen). Nothing is patched or skipped in the program; RAM/CMOS/sound state is
    // exactly the power-on result. Scripted, smoke and online runs keep the visible
    // paced boot; STV_BOOT=full restores it for a normal launch.
    wchar_t boot_mode[8]{};
    const bool boot_skip_allowed = !smoke_frames && !scripted_run() &&
        !(GetEnvironmentVariableW(L"STV_BOOT", boot_mode, 8) == 4 && std::wcscmp(boot_mode, L"full") == 0);
    bool boot_skip = boot_skip_allowed && !g_net.on;
    u32 boot_skip_frames = 0;
    unsigned intro_title_frames = 0;
    LARGE_INTEGER boot_skip_started{};
    constexpr u32 kBootSkipLimit = 4000;   // about 73 s of arcade time: then show whatever is there
    // ROM/window/lobby setup and scripted-input parsing can outlast the initial
    // device silence. Prime once at the actual game-clock epoch, never by
    // running simulation early to refill an empty output queue.
    (void)g_audio.service(GetTickCount64());
    audio_status();
    if (audio) { (void)g_audio.discard_pending(); audio_status(); }
    LARGE_INTEGER start_time{};
    QueryPerformanceCounter(&start_time);
    simulation_clock.reset(u64(start_time.QuadPart));
    auto finish_report = [&](bool timeout = false) {
        if (smoke_frames && (frames_run >= smoke_frames || timeout)) {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            wchar_t path[MAX_PATH];
            GetTempPathW(MAX_PATH, path);
            FILE* f = nullptr;
            std::wstring report = std::wstring(path) + L"smashtv_host.json";
            wchar_t custom[MAX_PATH];
            if (GetEnvironmentVariableW(L"STV_HOST_REPORT", custom, MAX_PATH)) report = custom;   // automation: another file
            const u32 audio_queued = g_audio.queued();
            audio_status();
            observe_audio_output(true);
            if (_wfopen_s(&f, report.c_str(), L"w") == 0 && f) {
                std::fprintf(f,
                             "{\"frames\": %llu, \"seconds\": %.3f, \"audio_device\": %s, \"audio_rate\": %u, "
                             "\"samples\": %llu, \"main_faults\": %zu, \"sound_fault\": \"%s\", \"frame_number\": %llu, "
                             "\"queued\": %u, \"timed_out\": %s, \"first_fault\": \"%s\", \"first_fault_addr\": \"%08x\", ",
                             (unsigned long long)frames_run, double(t.QuadPart - start_time.QuadPart) / double(freq.QuadPart),
                             audio ? "true" : "false", g_audio.rate(), (unsigned long long)samples_made, g_m.faults.size(),
                             g_snd.fault.c_str(), (unsigned long long)g_m.frame_number, audio_queued,
                             timeout ? "true" : "false", g_m.faults.empty() ? "" : g_m.faults.front().what.c_str(),
                             g_m.faults.empty() ? 0u : unsigned(g_m.faults.front().addr));
                // Retain the bounded access observations so a later gameplay
                // access can be distinguished from the existing POST boundary.
                // This reports the recorded list; the machine caps it at 256.
                std::fputs("\"main_fault_details\": [", f);
                const std::size_t fault_count = std::min(g_m.faults.size(), std::size_t(256));
                for (std::size_t i = 0; i < fault_count; ++i) {
                    const auto& fault = g_m.faults[i];
                    std::fprintf(f, "%s{\"what\":\"", i ? "," : "");
                    for (unsigned char ch : fault.what) {
                        if (ch < 0x20) std::fprintf(f, "\\u%04x", unsigned(ch));
                        else {
                            if (ch == '"' || ch == '\\') std::fputc('\\', f);
                            std::fputc(ch, f);
                        }
                    }
                    std::fprintf(f, "\",\"address\":\"%08x\",\"cycle\":%llu}",
                                 unsigned(fault.addr), (unsigned long long)fault.cycle);
                }
                std::fputs("], \"main_fault_record_limit\": 256, ", f);
                std::fprintf(f, "\"audio_initial_device\": %s, \"audio_errors\": %u, \"audio_error_stage\": \"%s\", \"audio_error_code\": \"%08x\", ",
                             audio_initial_device ? "true" : "false", g_audio.errors(), g_audio.error_stage(),
                             unsigned(g_audio.error_code()));
                write_audio_recovery(f);
                // game position (CIRCUIT, WAVE, P1 lives) to check a replayed scenario against lockstep
                std::fprintf(f, "\"profile\": \"%s\", \"visible_width\": %d, \"visible_height\": %d, ",
                             g_enhanced ? "enhanced_l8_16_9" : "reference_l8", vis_w(), VIS_H);
                std::fprintf(f, "\"audio_underruns\": %llu, \"audio_min_queued_ms\": %.1f, ",
                             (unsigned long long)g_audio.underruns(),
                             g_audio.min_padding() == UINT32_MAX ? -1.0 : 1000.0 * g_audio.min_padding() / g_audio.rate());
                {   // Test hook: dump the replay proof of this scripted run for the verifier.
                    wchar_t proof_path[MAX_PATH];
                    if (GetEnvironmentVariableW(L"STV_HOST_REPLAY_OUT", proof_path, MAX_PATH)) {
                        const auto proof = g_proof.encode(std::size_t(1) << 30);
                        FILE* pf = nullptr;
                        if (!proof.empty() && !_wfopen_s(&pf, proof_path, L"wb") && pf) {
                            std::fwrite(proof.data(), 1, proof.size(), pf);
                            std::fclose(pf);
                        }
                        std::fprintf(f, "\"replay_proof_bytes\": %zu, \"replay_proof_frames\": %llu, ",
                                     proof.size(), (unsigned long long)g_proof.frames());
                    }
                }
                u64 ram_hash = stv::host::ConfirmedAudioAudit::kOffset;
                u64 frame_hash = stv::host::ConfirmedAudioAudit::kOffset;
                for (u16 word : g_m.ram) stv::host::ConfirmedAudioAudit::hash_word(ram_hash, word, 2);
                for (u16 word : g_m.frame) stv::host::ConfirmedAudioAudit::hash_word(frame_hash, word, 2);
                std::fprintf(f, "\"observation_hash_algorithm\": \"fnv1a64_little_endian\", "
                             "\"raw_pcm_hash\": \"%016llx\", \"final_ram_hash\": \"%016llx\", "
                             "\"final_framebuffer_hash\": \"%016llx\", \"main_cycles\": %llu, \"sound_cycles\": %llu, "
                             "\"mixer_ym_samples\": %llu, ",
                             (unsigned long long)raw_pcm_hash, (unsigned long long)ram_hash,
                             (unsigned long long)frame_hash, (unsigned long long)g_m.c.cyc,
                             (unsigned long long)g_snd.cpu.cyc, (unsigned long long)g_mixer->ym_generated());
                const double elapsed = double(t.QuadPart - start_time.QuadPart) / double(freq.QuadPart);
                std::fprintf(f, "\"presentation_schema\": 2, \"presentation_mode\": \"%s\", "
                             "\"presentation_frames\": %llu, \"presentation_failures\": %llu, \"frame_captures\": %llu, "
                             "\"presentation_fps\": %.3f, \"presentation_window_visible\": %s, "
                             "\"content_width\": %d, \"content_height\": %d, \"content_rect\": [%ld, %ld, %ld, %ld], "
                             "\"presentation_client_width\": %d, \"presentation_client_height\": %d, "
                             "\"simulation_hz\": %.9f, \"simulation_rate_numerator\": %llu, \"simulation_rate_denominator\": %llu, "
                             "\"simulation_seconds\": %.9f, \"simulation_clock_rebases\": %llu, \"simulation_clock_suspensions\": %llu, \"simulation_max_late_ms\": %.3f, "
                             "\"audio_output_samples\": %llu, \"audio_output_ratio\": %.9f, ",
                             g_settings.sync_presentation() ? (g_compositor_failed ? "uncapped_compositor_error" : "compositor_synced") : "uncapped",
                             (unsigned long long)g_present.count, (unsigned long long)g_present.failures,
                             (unsigned long long)g_frame_captures, elapsed > 0 ? double(g_present.count) / elapsed : 0.0,
                             IsWindowVisible(g_wnd) && !IsIconic(g_wnd) ? "true" : "false",
                             g_frame_width, VIS_H, g_present.destination.left, g_present.destination.top,
                             g_present.destination.right, g_present.destination.bottom,
                             g_present.client_width, g_present.client_height,
                             elapsed > 0 ? double(frames_run) / elapsed : 0.0,
                             (unsigned long long)stv::host::FrameClock::kRateNumerator,
                             (unsigned long long)stv::host::FrameClock::kRateDenominator,
                             double(frames_run) * stv::host::FrameClock::kSeconds,
                             (unsigned long long)clock_rebases, (unsigned long long)clock_suspensions, simulation_max_late_ms,
                             (unsigned long long)g_audio.output_samples(), g_audio.output_ratio());
                std::fprintf(f, "\"audio_output_resyncs\": %llu, \"audio_dropped_source_samples\": %llu, \"audio_dropped_device_frames\": %llu, "
                             "\"audio_pending_limit_samples\": %u, \"audio_max_pending_samples\": %u, ",
                             (unsigned long long)g_audio.output_resyncs(),
                             (unsigned long long)g_audio.dropped_source_samples(),
                             (unsigned long long)g_audio.dropped_device_frames(),
                             g_audio.pending_limit_samples(), g_audio.maximum_pending_samples());
                std::fprintf(f, "\"compositor_waits\": %llu, \"compositor_wait_errors\": %llu, \"compositor_wait_max_ms\": %.3f, ",
                             (unsigned long long)g_compositor.completed(), (unsigned long long)g_compositor.errors(),
                             g_compositor.maximum_ms());
                std::fprintf(f, "\"circuit\": %u, \"wave\": %u, \"p1_lives\": %u, \"present_max_ms\": %.2f, \"present_ms_hist\": [",
                             g_m.rw(0x010EB610u), g_m.rw(0x010EB630u), g_m.rw(0x01086360u), g_present.maximum_ms);
                for (std::size_t i = 0; i < g_present.histogram.size(); ++i)
                    std::fprintf(f, "%s%llu", i ? ", " : "", (unsigned long long)g_present.histogram[i]);
                std::fputs("], \"host_stage_timing\": {", f);
                g_time_sim.write(f, "simulation", true);
                g_time_scene.write(f, "scene_and_effects", true);
                g_time_present.write(f, "presentation", false);
                std::fputs("},\"audio_output_timeline\":", f);
                g_audio_telemetry.write_json(f);
                std::fputs("}\n", f);
                std::fclose(f);
            }
            PostQuitMessage(0);
        }
    };
    // Unlocked active presentation has no timer wait. This timer handles paused,
    // hidden/clipped windows and deadlines during the optional compositor wait.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                close_lobby();
#if STV_HAS_EOS
                g_epic.finish();
#endif
                if (timer) CloseHandle(timer);
                g_compositor.shutdown();
                return int(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        service_window_focus();
        poll_pad_controls();
        service_lobby();
        service_leaderboard();
        const bool frontend_reset = service_frontend_requests();
        QueryPerformanceCounter(&now);
        if (frontend_reset || previous_net_mode != g_net.on) {
            audio = g_audio.ok();
            simulation_clock.reset(u64(now.QuadPart));
            ++clock_rebases;
            clock_paused = output_was_paused = false;
            previous_net_mode = g_net.on;
            // An explicit offline restart begins a new power-on; online starts stay paced.
            if (frontend_reset && (!g_host_solo || g_host_booting) && !g_net.on && !g_started) { boot_skip = boot_skip_allowed; boot_skip_frames = 0; intro_title_frames = 0; }
            else boot_skip = false;
            if (!boot_skip) g_host_booting = false;
        }
        (void)g_audio.service(GetTickCount64());
        audio_status();
        if (audio) { (void)g_audio.queued(); audio_status(); }
        observe_audio_output();
        QueryPerformanceCounter(&now); // a device reopen may take time; keep the existing deadline
        if (smoke_frames && double(now.QuadPart - start_time.QuadPart) / double(freq.QuadPart) >
                            30.0 + double(smoke_frames) / 50.0) {
            finish_report(true);
            continue;
        }
        if (g_net.on) {
            // The protocol ticks at exactly the arcade rate even while a shared
            // pause holds simulation. Extra presents never call advance/pump.
            if (clock_paused) { simulation_clock.reset(u64(now.QuadPart)); clock_paused = false; ++clock_rebases; }
            bool paused = g_paused || g_menu || g_net.session->paused_by_peer();
            sync_audio_pause(paused, output_was_paused);
            audio_status();
            bool ran = false;
            int budget = 4;
            while (budget-- > 0 && simulation_clock.due(u64(now.QuadPart)) && !g_net.ended) {
                net_tick(audio, output_was_paused);
                simulation_clock.consume();
                ran = true;
                QueryPerformanceCounter(&now);
            }
#if STV_HAS_EOS
            if (ran && g_epic_room) g_epic.flush();   // inputs leave now, not on the next loop
#endif
            // The peer's pause state may have changed in any owed tick. Use
            // that current state for output and waiting, not the batch's entry.
            paused = g_paused || g_menu || g_net.session->paused_by_peer();
            sync_audio_pause(paused, output_was_paused);
            audio_status();
            if (audio && !paused) { g_audio.pump(); audio_status(); }
            observe_audio_output();
            if (ran && g_net.mr) present_frame(*g_net.mr);
            QueryPerformanceCounter(&now);
            if (!g_net.ended && !simulation_clock.due(u64(now.QuadPart))) {
                g_camera_phase = paused ? 1.0f : float(std::clamp(1.0 - simulation_clock.remaining_seconds(u64(now.QuadPart)) / simulation_clock.kSeconds, 0.0, 1.0));
                const bool presented = present_current_frame();
                if (!presented || paused)
                    wait_seconds(timer, std::min(g_settings.sync_presentation()?0.008:0.001, simulation_clock.remaining_seconds(u64(now.QuadPart))),
                                 presentation_wait_event());
            }
            continue;
        }
        if (g_paused || g_menu || g_m.stop || !g_snd.fault.empty()) {
            // Explicit pause stops wall-clock accumulation. A CPU/render stall
            // does not enter this path and its owed simulation steps are kept.
            clock_paused = output_was_paused = true;
            g_display_fps = g_simulation_hz = 0;
            wait_seconds(timer, 0.008);
            continue;
        }
        if (clock_paused) {
            simulation_clock.reset(u64(now.QuadPart));
            ++clock_rebases;
            clock_paused = false;
        }
        if (output_was_paused) {
            if (audio) { (void)g_audio.discard_pending(); audio_status(); }
            output_was_paused = false;
        }
        if (boot_skip) {
            LARGE_INTEGER slice{}, at{};
            QueryPerformanceCounter(&slice);
            if (!boot_skip_frames) boot_skip_started = slice;
            bool finished = false;
            for (;;) {
                const u16 scene = game_scene_state(g_m);
                finished = startup_intro_ready(g_m,intro_title_frames) || scene == 0x10 || scene == 0x18 || scene == 0x50 ||
                           boot_skip_frames >= kBootSkipLimit || g_m.stop || !g_snd.fault.empty();
                if (finished) break;
                g_m.ports[0] = g_m.ports[1] = 0xffff;   // all controls released (active low)
                if (g_host_solo || g_net.on) g_proof.taint();
                if (g_host_solo) record_room_frame(0,0,u16(0xffff));
                else record_local_frame();
                run_one_frame();
                g_pcm.clear();                          // power-on sound is generated, not played
                if (g_frame_done) { ++boot_skip_frames; ++g_simulation_steps; }
                QueryPerformanceCounter(&at);
                if ((at.QuadPart - slice.QuadPart) * 1000 >= freq.QuadPart * 30) break;   // keep the window responsive
            }
            if (!finished) continue;
            boot_skip = false;
            g_host_booting = false;
            { char where[40]; std::snprintf(where, sizeof where, "state %02x background %08x", unsigned(game_scene_state(g_m)), unsigned(scene_background(g_m))); frontend_event("boot_skipped", boot_skip_frames, where); }
            {
                QueryPerformanceCounter(&at);
                char line[128];
                std::snprintf(line, sizeof line, "Power-on boot: %u frames in %llu ms.", boot_skip_frames,
                              (unsigned long long)((at.QuadPart - boot_skip_started.QuadPart) * 1000 / freq.QuadPart));
                write_player_log(line);
                log_startup_stage("first frame");
            }
            if (audio) { (void)g_audio.discard_pending(); audio_status(); }
            if (g_frame_done) present_frame(g_m);
            QueryPerformanceCounter(&now);
            simulation_clock.reset(u64(now.QuadPart));   // pacing starts at the first shown frame
            ++clock_rebases;
            output_was_paused = false;
        }
        // Offline only: an online peer must catch up to remain inside the rollback window.
        if (const u64 owed_since = simulation_clock.deadline();
            simulation_clock.drop_suspension(u64(now.QuadPart), u64(freq.QuadPart) * kSuspensionSeconds)) {
            const u64 late_ms = (u64(now.QuadPart) - owed_since) * 1000 / u64(freq.QuadPart);
            ++clock_rebases;
            ++clock_suspensions;
            frontend_event("clock_suspension_dropped", late_ms);
            if (audio) { (void)g_audio.discard_pending(); audio_status(); }
        }
        bool ran = false;
        int budget = 4; // responsiveness bound only; owed steps within kSuspensionSeconds are kept
        while (budget-- > 0 && simulation_clock.due(u64(now.QuadPart))) {
            simulation_max_late_ms = std::max(simulation_max_late_ms,
                1000.0 * double(u64(now.QuadPart) - simulation_clock.deadline()) / double(freq.QuadPart));
            LARGE_INTEGER sim_start{}, sim_end{};
            QueryPerformanceCounter(&sim_start);
            if (g_host_solo || g_net.on) g_proof.taint();   // room play is not a plain local run
            if (g_host_solo) {
                const u32 p1 = local_player_input();
                run_one_frame();
                stv::net::apply_inputs(g_m,p1,0);
                record_room_frame(p1,0);
            } else {
                sample_inputs();
                record_local_frame();
                run_one_frame();
            }
            QueryPerformanceCounter(&sim_end);
            if (smoke_frames) g_time_sim.add(sim_start, sim_end, u64(freq.QuadPart));
            if (g_frame_done) { ++frames_run; ++g_simulation_steps; }
            if (g_frame_done && !g_host_solo) leaderboard_after_frame();
            samples_made += g_pcm.size();
            if (smoke_frames) {
                // Hash original board PCM before gain, drift matching, queuing
                // or device conversion. Different mixer rates must not compare.
                for (float sample : g_pcm) {
                    u32 bits{};
                    std::memcpy(&bits, &sample, sizeof(bits));
                    stv::host::ConfirmedAudioAudit::hash_word(raw_pcm_hash, bits, 4);
                }
            }
            deliver_audio(audio);
            if (audio) {
                g_audio.pump();
                audio_status();
            }
            g_pcm.clear();
            simulation_clock.consume();
            ran = true;
            if (++frames_since_save >= 55 * 60) { save_cmos(); frames_since_save = 0; }
            QueryPerformanceCounter(&now);
            if (g_m.stop || !g_snd.fault.empty() || (smoke_frames && frames_run >= smoke_frames)) break;
        }
        if (audio) { g_audio.pump(); audio_status(); }
        observe_audio_output();
        if (g_m.stop) {
            g_status = "Stopped: " + (g_m.faults.empty() ? std::string("fault") : g_m.faults.back().what);
            InvalidateRect(g_wnd, nullptr, FALSE);
        }
        if (ran) {
            LARGE_INTEGER scene_start{}, scene_end{};
            QueryPerformanceCounter(&scene_start);
            present_frame(local_video_frame());
            QueryPerformanceCounter(&scene_end);
            if (smoke_frames) g_time_scene.add(scene_start, scene_end, u64(freq.QuadPart));
        }
        QueryPerformanceCounter(&now);
        if (!simulation_clock.due(u64(now.QuadPart)) || (smoke_frames && frames_run >= smoke_frames)) {
            g_camera_phase = float(std::clamp(1.0 - simulation_clock.remaining_seconds(u64(now.QuadPart)) / simulation_clock.kSeconds, 0.0, 1.0));
            LARGE_INTEGER paint_start{}, paint_end{};
            QueryPerformanceCounter(&paint_start);
            const bool presented = present_current_frame();
            QueryPerformanceCounter(&paint_end);
            if (smoke_frames && presented) g_time_present.add(paint_start, paint_end, u64(freq.QuadPart));
            if (smoke_frames && frames_run >= smoke_frames) { finish_report(); continue; }
            if (!presented)
                wait_seconds(timer, std::min(g_settings.sync_presentation()?0.008:0.001, simulation_clock.remaining_seconds(u64(now.QuadPart))),
                                 presentation_wait_event());
        }
        // No unlocked presentation cap or simulation-rate nudge. Under load we
        // catch up owed game steps before another redraw. Optional VSync waits
        // on its worker; simulation/audio never wait for the compositor.
    }
}
