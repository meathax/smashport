#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace stv::host {
// Widescreen presentation of the native 410-pixel pages (title, attract,
// operator, high scores): the original 4:3 image, centred, with black sides.
// No game artwork is shipped and nothing is synthesised around the ROM image.
// resident_effects.hlsl (sourceMode 2) draws the same black sides on the GPU.
constexpr int kWingLeft = 68, kNativeWidth = 410, kWideWidth = 547, kWingRows = 256;

inline bool extend_title(std::vector<std::uint32_t>& pixels, int& width, bool wide,
                         bool /*title_page*/ = false) {
    // Frame width is captured with the completed frame: every native 4:3 page
    // gets the same treatment; already-expanded gameplay is never padded.
    if (!wide || width != kNativeWidth || pixels.size() != std::size_t(kNativeWidth) * kWingRows) return false;
    std::vector<std::uint32_t> out(std::size_t(kWideWidth) * kWingRows, 0);
    for (int y = 0; y < kWingRows; ++y)
        std::copy_n(pixels.data() + std::size_t(y) * kNativeWidth, kNativeWidth,
                    out.data() + std::size_t(y) * kWideWidth + kWingLeft);
    pixels.swap(out);
    width = kWideWidth;
    return true;
}
}
