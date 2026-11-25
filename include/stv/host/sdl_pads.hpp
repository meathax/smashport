#pragma once

#include "stv/host/pad.hpp"
#include <SDL3/SDL.h>
#include <string>
#include <utility>

namespace stv::host {
struct AnalogPad {
    PadSnapshot state;
    int lx{}, ly{}, rx{}, ry{};
};

// Hold devices by instance ID so disconnecting P1 does not renumber P2.
// Devices SDL recognises as gamepads (XInput, Xbox, PlayStation, Switch and
// every controller in SDL's mapping database) use the standard gamepad layout.
// Any other stick-type controller (generic DirectInput/HID pads with no mapping)
// is opened as a raw joystick with a conventional layout, so it still moves,
// fires and navigates, and every input stays rebindable. An optional
// gamecontrollerdb.txt beside the executable adds community mappings.
class SdlPads {
public:
    SdlPads() : ready_(init()) {
        if (!ready_) return;
        if (const char* base = SDL_GetBasePath()) {
            const std::string db = std::string(base) + "gamecontrollerdb.txt";
            if (SDL_GetPathInfo(db.c_str(), nullptr)) SDL_AddGamepadMappingsFromFile(db.c_str());
        }
    }
    ~SdlPads() {
        for (auto& device : devices_) close(device);
        if (ready_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    SdlPads(const SdlPads&) = delete;
    SdlPads& operator=(const SdlPads&) = delete;
    bool ready() const { return ready_; }

    void refresh() {
        if (!ready_) return;
        SDL_UpdateGamepads();
        SDL_UpdateJoysticks();
        for (auto& device : devices_) {
            const bool alive = device.pad ? SDL_GamepadConnected(device.pad)
                             : device.joystick ? SDL_JoystickConnected(device.joystick) : true;
            if (!alive) close(device);
        }
        if (devices_[0].id && devices_[1].id) return;
        int count = 0;
        auto* ids = SDL_GetJoysticks(&count);
        for (int i = 0; ids && i < count; ++i) {
            if (devices_[0].id == ids[i] || devices_[1].id == ids[i]) continue;
            auto& slot = devices_[0].id ? devices_[1] : devices_[0];
            if (SDL_IsGamepad(ids[i])) {
                if ((slot.pad = SDL_OpenGamepad(ids[i]))) slot.id = ids[i];
            } else if (auto* joystick = SDL_OpenJoystick(ids[i])) {
                if (generic_pad(joystick)) { slot.joystick = joystick; slot.id = ids[i]; }
                else SDL_CloseJoystick(joystick); // wheels, pedals, sensors: not a pad
            }
            if (devices_[0].id && devices_[1].id) break;
        }
        SDL_free(ids);
    }

    AnalogPad read(int slot) const {
        const auto& device = devices_[slot];
        if (device.pad) return read_gamepad(device.pad);
        if (device.joystick) return read_joystick(device.joystick);
        return {};
    }

    // A raw joystick qualifies as a pad when it has two sticks' worth of axes or
    // a hat plus a stick, and enough buttons to confirm/back/fire.
    static bool generic_pad(SDL_Joystick* joystick) {
        const int axes = SDL_GetNumJoystickAxes(joystick), hats = SDL_GetNumJoystickHats(joystick);
        const int buttons = SDL_GetNumJoystickButtons(joystick);
        const auto type = SDL_GetJoystickType(joystick);
        if (type == SDL_JOYSTICK_TYPE_WHEEL || type == SDL_JOYSTICK_TYPE_FLIGHT_STICK ||
            type == SDL_JOYSTICK_TYPE_THROTTLE) return false;
        return buttons >= 4 && (axes >= 2 || hats >= 1);
    }

private:
    static bool init() {
        // The host owns focus gating; SDL has no window of its own here.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        return SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }
    struct Device {
        SDL_JoystickID id = 0;
        SDL_Gamepad* pad = nullptr;
        SDL_Joystick* joystick = nullptr;
    };
    static void close(Device& device) {
        if (device.pad) SDL_CloseGamepad(device.pad);
        if (device.joystick) SDL_CloseJoystick(device.joystick);
        device = {};
    }

    static AnalogPad read_gamepad(SDL_Gamepad* pad) {
        AnalogPad sample;
        sample.state.connected = true;
        constexpr std::pair<SDL_GamepadButton, std::uint32_t> buttons[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_UP, PadUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PadDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PadLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PadRight},
            {SDL_GAMEPAD_BUTTON_SOUTH, PadA}, {SDL_GAMEPAD_BUTTON_EAST, PadB},
            {SDL_GAMEPAD_BUTTON_WEST, PadX}, {SDL_GAMEPAD_BUTTON_NORTH, PadY},
            {SDL_GAMEPAD_BUTTON_START, PadStart}, {SDL_GAMEPAD_BUTTON_BACK, PadBack},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, PadL3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PadR3},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PadLB}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PadRB},
        };
        for (const auto& [button, bit] : buttons)
            if (SDL_GetGamepadButton(pad, button)) sample.state.buttons |= bit;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 3855) sample.state.buttons |= PadLT;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 3855) sample.state.buttons |= PadRT;
        sample.lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        sample.ly = -int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        sample.rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        sample.ry = -int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        return sample;
    }

    // Unmapped HID pad. Axes follow the HID usage order X, Y, Z, Rx, Ry, Rz:
    // four-axis pads put the right stick on Z/Rz (2, 3); five or more axes are
    // Xbox-style with the right stick on Rx/Ry (3, 4). Twelve or more buttons
    // use the common USB pad order (8 select, 9 start, 10/11 stick clicks),
    // otherwise the DirectInput Xbox order (6 back, 7 start, 8/9 stick clicks).
    static AnalogPad read_joystick(SDL_Joystick* joystick) {
        AnalogPad sample;
        sample.state.connected = true;
        const int axes = SDL_GetNumJoystickAxes(joystick);
        const int count = SDL_GetNumJoystickButtons(joystick);
        const auto axis = [&](int index) { return index < axes ? int(SDL_GetJoystickAxis(joystick, index)) : 0; };
        sample.lx = axis(0);
        sample.ly = -axis(1);
        const int right = axes >= 5 ? 3 : 2;
        if (axes >= 4) { sample.rx = axis(right); sample.ry = -axis(right + 1); }
        constexpr std::uint32_t xbox[] = {PadA, PadB, PadX, PadY, PadLB, PadRB, PadBack, PadStart, PadL3, PadR3};
        constexpr std::uint32_t usb[] = {PadA, PadB, PadX, PadY, PadLB, PadRB, PadLT, PadRT,
                                         PadBack, PadStart, PadL3, PadR3};
        const auto* layout = count >= 12 ? usb : xbox;
        const int mapped = count >= 12 ? 12 : 10;
        for (int i = 0; i < mapped && i < count; ++i)
            if (SDL_GetJoystickButton(joystick, i)) sample.state.buttons |= layout[i];
        if (SDL_GetNumJoystickHats(joystick) > 0) {
            const auto hat = SDL_GetJoystickHat(joystick, 0);
            if (hat & SDL_HAT_UP) sample.state.buttons |= PadUp;
            if (hat & SDL_HAT_DOWN) sample.state.buttons |= PadDown;
            if (hat & SDL_HAT_LEFT) sample.state.buttons |= PadLeft;
            if (hat & SDL_HAT_RIGHT) sample.state.buttons |= PadRight;
        }
        return sample;
    }

    bool ready_;
    std::array<Device, 2> devices_{};
};
} // namespace stv::host
