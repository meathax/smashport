#include "stv/host/sdl_pads.hpp"
#include <SDL3/SDL_main.h>

int main(int, char**) {
    using namespace stv::host;
    SdlPads pads;
    if (!pads.ready()) return 1;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    const auto first = SDL_AttachVirtualJoystick(&desc);
    const auto second = SDL_AttachVirtualJoystick(&desc);
    if (!first || !second) return 2;
    auto* device = SDL_OpenJoystick(second);
    if (!device || !SDL_SetJoystickVirtualAxis(device, SDL_GAMEPAD_AXIS_RIGHTY, -22000) ||
        !SDL_SetJoystickVirtualButton(device, SDL_GAMEPAD_BUTTON_SOUTH, true)) return 3;
    pads.refresh();
    auto p2 = pads.read(1);
    if (!pads.read(0).state.connected || !p2.state.connected || p2.ry != 22000 ||
        !(p2.state.buttons & PadA)) return 4;
    if (!SDL_DetachVirtualJoystick(first)) return 5;
    pads.refresh();
    if (pads.read(0).state.connected || pads.read(1).ry != 22000) return 6;
    SDL_CloseJoystick(device);
    return SDL_DetachVirtualJoystick(second) ? 0 : 7;
}
