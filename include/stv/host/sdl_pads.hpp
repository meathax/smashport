#pragma once

#include "stv/host/pad.hpp"
#include <SDL3/SDL.h>
#include <utility>

namespace stv::host {
struct AnalogPad {
    PadSnapshot state;
    int lx{}, ly{}, rx{}, ry{};
};

// Hold devices by instance ID so disconnecting P1 does not renumber P2.
class SdlPads {
public:
    SdlPads() : ready_(SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {}
    ~SdlPads() {
        for (auto* pad : pads_) if (pad) SDL_CloseGamepad(pad);
        if (ready_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    SdlPads(const SdlPads&) = delete;
    SdlPads& operator=(const SdlPads&) = delete;
    bool ready() const { return ready_; }

    void refresh() {
        if (!ready_) return;
        SDL_UpdateGamepads();
        for (auto& pad : pads_) {
            if (pad && !SDL_GamepadConnected(pad)) {
                SDL_CloseGamepad(pad);
                pad = nullptr;
            }
        }
        if (pads_[0] && pads_[1]) return;
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i) {
            if ((pads_[0] && SDL_GetGamepadID(pads_[0]) == ids[i]) ||
                (pads_[1] && SDL_GetGamepadID(pads_[1]) == ids[i])) continue;
            auto& slot = pads_[0] ? pads_[1] : pads_[0];
            slot = SDL_OpenGamepad(ids[i]);
            if (pads_[0] && pads_[1]) break;
        }
        SDL_free(ids);
    }

    AnalogPad read(int slot) const {
        auto* pad = pads_[slot];
        if (!pad) return {};
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

private:
    bool ready_;
    std::array<SDL_Gamepad*, 2> pads_{};
};
} // namespace stv::host
