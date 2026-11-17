// Presentation scaling for SmashTV.exe (PLAN P8). The arcade pixel is not square:
// MAME shows the 410x256 visible area at 4:3, so one pixel is 5:6 (width:height).
// Integer modes scale each axis by a whole number (nearest neighbour), so every
// source pixel becomes an exact sx x sy block.
#pragma once

#include <cmath>
#include <initializer_list>

namespace stv::host {

enum class ScaleMode { IntegerExact, IntegerFill, Smooth };

struct Scale {
    int w, h;       // output size in device pixels
    int sx, sy;     // integer factors (0 in Smooth mode)
};

// Pixel aspect of the source (width/height of one pixel).
constexpr double kPixelAspect = 5.0 / 6.0;

inline Scale compute_scale(int cw, int ch, int vw, int vh, ScaleMode mode) {
    if (mode == ScaleMode::IntegerExact) {
        // factors in an exact 5:6 ratio
        int best = 0;
        for (int k = 1; 5 * k * vw <= cw && 6 * k * vh <= ch; ++k) best = k;
        if (best) return {5 * best * vw, 6 * best * vh, 5 * best, 6 * best};
        mode = ScaleMode::IntegerFill;
    }
    if (mode == ScaleMode::IntegerFill) {
        // Largest whole factors with at most 5% aspect error. If none fits,
        // use the existing smooth fallback rather than distort a small display.
        Scale bestS{0, 0, 0, 0};
        for (int sy = 1; sy * vh <= ch; ++sy)
            for (int sx = 1; sx * vw <= cw; ++sx) {
                const double err = std::fabs((double(sx) / sy) / kPixelAspect - 1.0);
                if (err > 0.05 + 1e-12) continue;
                if (sx * vw * sy * vh > bestS.w * bestS.h) bestS = {sx * vw, sy * vh, sx, sy};
            }
        if (bestS.sx) return bestS;
    }
    // smooth: largest size with the exact display aspect
    const double aspect = double(vw) * kPixelAspect / vh;
    int w = cw, h = int(cw / aspect);
    if (h > ch) { h = ch; w = int(ch * aspect); }
    return {w, h, 0, 0};
}

}  // namespace stv::host
