#pragma once
#include "stv/host/enhance.hpp"
#include <algorithm>
#include <cstdlib>

namespace stv::host {
struct CameraMotion {
    uint64_t frame{};
    int x{}, y{}, width{}, dx{}, dy{};
    bool active{};
    // Place the latest picture at the interpolated camera position. Actor
    // animation and input feedback use the newest frame at every phase.
    float offset(int delta, float phase) const noexcept {
        return delta * (std::clamp(phase, 0.0f, 1.0f) - 1.0f);
    }
    void observe(const EffectScene& scene) {
        dx = dy = 0;
        if (active && scene.in_arena && width == scene.width && frame + 1 == scene.frame &&
            std::abs(scene.camera_x - x) <= 16 && std::abs(scene.camera_y - y) <= 16) {
            dx = scene.camera_x - x;
            dy = scene.camera_y - y;
        }
        x = scene.camera_x; y = scene.camera_y; width = scene.width;
        frame = scene.frame; active = scene.in_arena;
    }
};
} // namespace stv::host
