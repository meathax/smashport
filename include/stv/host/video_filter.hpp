// Host-only presentation filters. Donor intent: cheviman/tmnt-port,
// ports/sdl2/main.c at 401c3694e5f89fa5844c384cfd82512803202cf9.
// See src/host/video_filter.cpp for the complete BSD-3-Clause notice.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace stv::host {
enum class VideoFilterMode { Nearest, Linear, Sharp, CRT, Modern, Guest, Royale, Lottes };
const char* video_filter_name(VideoFilterMode mode) noexcept;

// The caller supplies its aspect-correct destination rectangle. This class never
// changes game geometry, timing, palette or source pixels. Pixels are 0xAARRGGBB;
// source alpha is ignored and output alpha is opaque (GDI uses the RGB channels).
class VideoFilterBuffer {
public:
    static constexpr std::size_t kMaxPixels = 16u * 1024u * 1024u;
    static constexpr int kMaxDimension = 16384;
    bool render(const std::uint32_t* source, std::size_t source_pixels,
                int source_width, int source_height, int dest_width, int dest_height,
                VideoFilterMode mode, std::string& error);
    const std::vector<std::uint32_t>& pixels() const noexcept { return pixels_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

private:
    struct Axis { int first, second; std::uint16_t weight; };
    static std::vector<Axis> make_axis(int source, int dest, VideoFilterMode mode);
    void make_mask(int width, int height, int source_height);
    std::vector<Axis> x_, y_;
    std::vector<std::uint32_t> pixels_, row0_, row1_, mask_;
    int source_width_{}, source_height_{}, dest_width_{}, dest_height_{};
    int width_{}, height_{};
    VideoFilterMode mode_{VideoFilterMode::Nearest};
};
} // namespace stv::host
