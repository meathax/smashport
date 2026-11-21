// Losing the foreground never pauses or changes the game. Held input is
// dropped until released, and a player pause or menu is left exactly as it was.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
static bool test_foreground_ours = true, test_minimized = false, test_visible = true;
static SHORT test_key_state = 0;
static HWND test_window = reinterpret_cast<HWND>(std::uintptr_t(1));
HWND test_foreground() { return test_foreground_ours ? test_window : reinterpret_cast<HWND>(std::uintptr_t(2)); }
BOOL test_iconic(HWND) { return test_minimized; }
BOOL test_is_visible(HWND) { return test_visible; }
SHORT test_key(int vk) { return vk == 'W' ? test_key_state : 0; }
#define GetForegroundWindow test_foreground
#define IsIconic test_iconic
#define IsWindowVisible test_is_visible
#define GetAsyncKeyState test_key
#define wWinMain unused_host_entry
#include "../src/host/win_main.cpp"
#undef wWinMain
#undef GetAsyncKeyState
#undef IsWindowVisible
#undef IsIconic
#undef GetForegroundWindow

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (false)

namespace {
void reset() {
    g_wnd = test_window;
    test_foreground_ours = true; test_minimized = false; test_visible = true; test_key_state = 0;
    g_menu = g_mode_menu = g_connecting = false;
    g_paused = false; g_keyboard_blocked = false; g_window_inactive = false;
    SetEnvironmentVariableW(L"STV_HOST_NOPAUSE", nullptr);
    service_window_focus();
}
}  // namespace

int main() {
    reset();
    service_window_focus();
    CHECK(!g_paused && !g_window_inactive && !g_keyboard_blocked);

    // No way of leaving the window pauses the game; each drops held input.
    struct Leave { const char* name; void (*go)(); } leaves[] = {
        {"WM_KILLFOCUS", [] { wndproc(g_wnd, WM_KILLFOCUS, 0, 0); }},
        {"WM_ACTIVATE inactive", [] { wndproc(g_wnd, WM_ACTIVATE, WA_INACTIVE, 0); }},
        {"WM_ACTIVATEAPP false", [] { wndproc(g_wnd, WM_ACTIVATEAPP, FALSE, 0); }},
        {"main loop: another window is foreground", [] { test_foreground_ours = false; service_window_focus(); }},
        {"main loop: minimized", [] { test_minimized = true; service_window_focus(); }},
        {"main loop: hidden", [] { test_visible = false; service_window_focus(); }},
    };
    for (const auto& leave : leaves) {
        reset();
        leave.go();
        if (g_paused || !g_keyboard_blocked) std::printf("  wrong for: %s\n", leave.name);
        CHECK(!g_paused);
        CHECK(g_keyboard_blocked);
    }

    // Never pauses at any point of a leave-and-return cycle (the focus messages in Windows order).
    reset();
    wndproc(g_wnd, WM_KILLFOCUS, 0, 0);
    wndproc(g_wnd, WM_ACTIVATE, WA_INACTIVE, 0);
    wndproc(g_wnd, WM_ACTIVATEAPP, FALSE, 0);
    test_foreground_ours = false;
    service_window_focus();
    CHECK(!g_paused && g_window_inactive);
    test_foreground_ours = true;
    wndproc(g_wnd, WM_ACTIVATEAPP, TRUE, 0);
    wndproc(g_wnd, WM_ACTIVATE, WA_ACTIVE, 0);
    wndproc(g_wnd, WM_SETFOCUS, 0, 0);
    service_window_focus();
    service_window_focus();
    CHECK(!g_paused && !g_window_inactive && g_keyboard_blocked);

    // Held keys stay ignored after returning until released, then input is live again.
    g_keys[P1_UP] = 'W';
    u16 in0 = 1, in1 = 1;
    test_key_state = SHORT(0x8000);
    read_controls(in0, in1);
    CHECK(in0 == 0 && g_keyboard_blocked);
    test_key_state = 0;
    read_controls(in0, in1);
    CHECK(!g_keyboard_blocked);
    test_key_state = SHORT(0x8000);
    read_controls(in0, in1);
    CHECK(in0 != 0);

    // The game keeps running while away: only a player's own pause or menu holds it.
    reset();
    wndproc(g_wnd, WM_KILLFOCUS, 0, 0);
    wndproc(g_wnd, WM_KEYDOWN, 'P', 0);
    CHECK(g_paused);                       // P still pauses
    reset();
    g_menu = true; g_paused = true;
    wndproc(g_wnd, WM_KILLFOCUS, 0, 0);
    test_foreground_ours = false; service_window_focus();
    test_foreground_ours = true; service_window_focus();
    CHECK(g_menu && g_paused);             // left exactly as it was

    // A bare Alt tap must not open the system-menu modal loop.
    reset();
    CHECK(wndproc(g_wnd, WM_SYSCOMMAND, SC_KEYMENU, 0) == 0);
    CHECK(!g_paused);

    // Minimize, drag, menu loop and suspend are plain system messages: no pause.
    reset();
    wndproc(g_wnd, WM_SIZE, SIZE_MINIMIZED, 0);
    wndproc(g_wnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    wndproc(g_wnd, WM_ENTERSIZEMOVE, 0, 0);
    wndproc(g_wnd, WM_ENTERMENULOOP, 0, 0);
    wndproc(g_wnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    CHECK(!g_paused);

    // No window handle yet (creation): nothing to judge.
    reset();
    g_wnd = nullptr;
    service_window_focus();
    CHECK(!g_paused && !g_window_inactive);
    g_wnd = test_window;

    std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
