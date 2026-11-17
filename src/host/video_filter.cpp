/*
 * Presentation filter intent and CRT mask adapted from cheviman/tmnt-port:
 * ports/sdl2/main.c, commit 401c3694e5f89fa5844c384cfd82512803202cf9,
 * lines 131-179 and 468-529. SDL texture passes are replaced by bounded CPU
 * buffers; independent axis factors preserve Smash TV's non-square pixels.
 *
 * BSD 3-Clause License
 *
 * Copyright (c) 2026, Sebastian Toyos
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "stv/host/video_filter.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define STV_FILTER_SSE2 1
#endif

namespace stv::host {
namespace {
std::uint32_t blend(std::uint32_t a, std::uint32_t b, unsigned weight) noexcept {
    if (!weight || a == b) return a | 0xff000000u;
    if (weight == 256) return b | 0xff000000u;
    std::uint32_t out = 0xff000000u;
    for (unsigned shift : {0u, 8u, 16u}) {
        const unsigned c = (((a >> shift) & 255u) * (256 - weight) +
                            ((b >> shift) & 255u) * weight + 128u) >> 8;
        out |= c << shift;
    }
    return out;
}
void blend_rows(std::uint32_t* dest, const std::uint32_t* a,
                const std::uint32_t* b, int width, unsigned weight) noexcept {
    if (!weight || a == b) { std::memcpy(dest, a, std::size_t(width) * 4); return; }
    if (weight == 256) { std::memcpy(dest, b, std::size_t(width) * 4); return; }
    int x = 0;
#ifdef STV_FILTER_SSE2
    const __m128i zero = _mm_setzero_si128();
    const __m128i wa = _mm_set1_epi16(short(256 - weight)), wb = _mm_set1_epi16(short(weight));
    const __m128i round = _mm_set1_epi16(128);
    for (; x + 4 <= width; x += 4) {
        const __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + x));
        const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x));
        const __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(va, zero), wa),
                                       _mm_mullo_epi16(_mm_unpacklo_epi8(vb, zero), wb));
        const __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(va, zero), wa),
                                       _mm_mullo_epi16(_mm_unpackhi_epi8(vb, zero), wb));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dest + x),
            _mm_packus_epi16(_mm_srli_epi16(_mm_add_epi16(lo, round), 8),
                             _mm_srli_epi16(_mm_add_epi16(hi, round), 8)));
    }
#endif
    for (; x < width; ++x) dest[x] = blend(a[x], b[x], weight);
}
void apply_mask(std::uint32_t* dest, const std::uint32_t* mask, std::size_t size) noexcept {
    std::size_t x = 0;
#ifdef STV_FILTER_SSE2
    const __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi16(128);
    for (; x + 4 <= size; x += 4) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dest + x));
        const __m128i m = _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask + x));
        __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(v, zero), _mm_unpacklo_epi8(m, zero)), round);
        __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(v, zero), _mm_unpackhi_epi8(m, zero)), round);
        lo = _mm_srli_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), 8);
        hi = _mm_srli_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), 8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dest + x), _mm_packus_epi16(lo, hi));
    }
#endif
    for (; x < size; ++x) {
        std::uint32_t out = 0xff000000u;
        for (unsigned shift : {0u, 8u, 16u})
            out |= (((((dest[x] >> shift) & 255u) * ((mask[x] >> shift) & 255u)) + 127u) / 255u) << shift;
        dest[x] = out;
    }
}
bool dimensions(int width, int height, std::size_t& count) noexcept {
    if (width <= 0 || height <= 0 || width > VideoFilterBuffer::kMaxDimension ||
        height > VideoFilterBuffer::kMaxDimension) return false;
    const std::uint64_t pixels = std::uint64_t(width) * std::uint64_t(height);
    if (pixels > VideoFilterBuffer::kMaxPixels) return false;
    count = std::size_t(pixels);
    return true;
}
} // namespace

const char* video_filter_name(VideoFilterMode mode) noexcept {
    switch (mode) {
    case VideoFilterMode::Nearest: return "Nearest";
    case VideoFilterMode::Linear: return "Linear";
    case VideoFilterMode::Sharp: return "Sharp";
    case VideoFilterMode::CRT: return "CRT (basic)";
    case VideoFilterMode::Modern: return "Modern clean (GPU)";
    case VideoFilterMode::Guest: return "CRT Guest Advanced";
    case VideoFilterMode::Royale: return "CRT Royale";
    case VideoFilterMode::Lottes: return "CRT Lottes";
    }
    return "Unknown";
}

std::vector<VideoFilterBuffer::Axis> VideoFilterBuffer::make_axis(int source, int dest, VideoFilterMode mode) {
    std::vector<Axis> axis(static_cast<std::size_t>(dest));
    const int whole = (mode == VideoFilterMode::Sharp || mode == VideoFilterMode::CRT)
                    ? std::max(1, dest / source) : 1;
    const int intermediate = source * whole; // bounded by source/dest dimensions
    for (int x = 0; x < dest; ++x) {
        if (mode == VideoFilterMode::Nearest) {
            const int sample = int((std::int64_t(2 * x + 1) * source) / (2 * dest));
            axis[x] = {std::min(sample, source - 1), std::min(sample, source - 1), 0};
        } else {
            const double position = std::clamp((x + 0.5) * intermediate / dest - 0.5, 0.0, double(intermediate - 1));
            const int first = int(std::floor(position)), second = std::min(first + 1, intermediate - 1);
            const unsigned weight = unsigned(std::clamp(std::lround((position - first) * 256.0), 0l, 256l));
            axis[x] = {first / whole, second / whole, std::uint16_t(weight)};
        }
    }
    return axis;
}

void VideoFilterBuffer::make_mask(int width, int height, int source_height) {
    mask_.resize(std::size_t(width) * height);
    constexpr double pi = 3.14159265358979;
    for (int y = 0; y < height; ++y) {
        const double row = (y + 0.5) * source_height / height;
        const double scan = 1.0 - 0.45 * (1.0 - std::sin(pi * (row - std::floor(row))));
        const double dy = 2.0 * y / height - 1.0;
        for (int x = 0; x < width; ++x) {
            const double dx = 2.0 * x / width - 1.0;
            const double light = scan * (1.0 - 0.18 * dx * dx * dy * dy);
            const int phosphor = x % 3;
            const auto channel = [light, phosphor](int selected) {
                return std::uint32_t(255.0 * light * (phosphor == selected ? 1.0 : 0.80));
            };
            mask_[std::size_t(y) * width + x] = 0xff000000u | (channel(0) << 16) | (channel(1) << 8) | channel(2);
        }
    }
}

bool VideoFilterBuffer::render(const std::uint32_t* source, std::size_t source_pixels,
                              int source_width, int source_height, int dest_width, int dest_height,
                              VideoFilterMode mode, std::string& error) {
    error.clear();
    width_ = height_ = 0;
    std::size_t input_count = 0, output_count = 0;
    if (!source || !dimensions(source_width, source_height, input_count) ||
        !dimensions(dest_width, dest_height, output_count) || source_pixels < input_count) {
        error = "Video filter requires valid nonempty dimensions and complete source pixels (maximum 16M pixels).";
        return false;
    }
    if (mode != VideoFilterMode::Nearest && mode != VideoFilterMode::Linear &&
        mode != VideoFilterMode::Sharp && mode != VideoFilterMode::CRT) {
        error = "Unknown video filter.";
        return false;
    }
    if (!pixels_.empty()) {
        const auto begin = reinterpret_cast<std::uintptr_t>(source);
        const auto end = begin + input_count * sizeof(std::uint32_t);
        const auto own_begin = reinterpret_cast<std::uintptr_t>(pixels_.data());
        const auto own_end = own_begin + pixels_.size() * sizeof(std::uint32_t);
        if (end < begin || (begin < own_end && end > own_begin)) {
            error = "Video filter source overlaps its destination buffer.";
            return false;
        }
    }
    try {
        if (source_width_ != source_width || source_height_ != source_height ||
            dest_width_ != dest_width || dest_height_ != dest_height || mode_ != mode) {
            x_ = make_axis(source_width, dest_width, mode);
            y_ = make_axis(source_height, dest_height, mode);
            pixels_.resize(output_count);
            row0_.resize(dest_width);
            row1_.resize(dest_width);
            if (mode == VideoFilterMode::CRT) make_mask(dest_width, dest_height, source_height);
            else mask_.clear();
            source_width_ = source_width; source_height_ = source_height;
            dest_width_ = dest_width; dest_height_ = dest_height; mode_ = mode;
        }
        const auto horizontal = [&](int y, std::vector<std::uint32_t>& row) {
            const auto* input = source + std::size_t(y) * source_width;
            for (int x = 0; x < dest_width; ++x) {
                const auto& a = x_[x];
                row[x] = blend(input[a.first], input[a.second], a.weight);
            }
        };
        int first_row = -1, second_row = -1;
        for (int y = 0; y < dest_height; ++y) {
            const auto& a = y_[y];
            if (first_row != a.first) {
                if (second_row == a.first) { row0_.swap(row1_); std::swap(first_row, second_row); }
                else { horizontal(a.first, row0_); first_row = a.first; }
            }
            const std::uint32_t* second = row0_.data();
            if (a.second != a.first) {
                if (second_row != a.second) { horizontal(a.second, row1_); second_row = a.second; }
                second = row1_.data();
            }
            blend_rows(pixels_.data() + std::size_t(y) * dest_width, row0_.data(), second, dest_width, a.weight);
        }
        if (mode == VideoFilterMode::CRT) apply_mask(pixels_.data(), mask_.data(), output_count);
        width_ = dest_width; height_ = dest_height;
        return true;
    } catch (const std::bad_alloc&) {
        source_width_ = source_height_ = dest_width_ = dest_height_ = 0;
        error = "Not enough memory for the selected video filter.";
        return false;
    }
}
} // namespace stv::host
