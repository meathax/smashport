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
        // Under load the loop can run up to four owed frames before one
        // present. Keep interpolating that combined scroll instead of snapping.
        const uint64_t gap = scene.frame - frame;
        const int reach = 16 * int(gap <= 4 ? gap : 0);
        if (active && scene.in_arena && width == scene.width && scene.frame > frame && gap <= 4 &&
            std::abs(scene.camera_x - x) <= reach && std::abs(scene.camera_y - y) <= reach) {
            dx = scene.camera_x - x;
            dy = scene.camera_y - y;
        }
        x = scene.camera_x; y = scene.camera_y; width = scene.width;
        frame = scene.frame; active = scene.in_arena;
    }
};
} // namespace stv::host
