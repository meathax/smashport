#pragma once
// Passive image/scene evidence at the same vblank boundary as native snapshots.
// No bus reads, simulation changes, viewport transforms or input mutations.
#include "stv/gsp/scene.hpp"
#include <fstream>
#include <string>

namespace stv::scene_audit {
class Observer {
public:
    explicit Observer(const std::string& path) : out_(path, std::ios::binary) {
        out_ << "{\"schema\":\"stv.scene.frames.v1\",\"phase\":\"vblank_begin\","
                "\"image_hash\":\"fnv1a32_rgb888_little_endian\"}\n";
    }
    bool good() const { return out_.good(); }
    bool sample(stv::gsp::u32 frame, const stv::gsp::Machine& m) {
        using namespace stv::gsp;
        const u32 width = scene_view_width(m);
        const u32 canvas = L8_NATIVE_VIEW_WIDTH + m.vis_extra;
        if (width > canvas || canvas + VIS_MINX > m.vram_w ||
            m.frame.size() < std::size_t(m.vram_w) * SCREEN_H) return false;
        u32 hash = 2166136261u;
        u32 outside = 0;
        for (int y = VIS_MINY; y <= VIS_MAXY; ++y) {
            const auto row = std::size_t(y) * m.vram_w + VIS_MINX;
            for (u32 x = 0; x < canvas; ++x) {
                const u16 pen = m.frame[row+x];
                if (x >= width) { if (pen != 0) ++outside; continue; }
                const u32 rgb = m.pens[pen & 0x0fff];
                for (unsigned byte = 0; byte != 3; ++byte)
                    hash = (hash ^ ((rgb >> (byte*8)) & 0xffu)) * 16777619u;
            }
        }
        const auto word = [&](u32 a) { return m.ram[(a-0x01000000u)>>4]; };
        out_ << "{\"frame\":" << frame << ",\"gamstate\":" << game_scene_state(m)
             << ",\"background\":" << scene_background(m)
             << ",\"view_width\":" << width << ",\"canvas_width\":" << canvas
             << ",\"screen_right\":" << word(0x010002c0u) << ",\"screen_bottom\":" << word(0x010002d0u)
             << ",\"outside_nonzero_indices\":" << outside << ",\"image_hash\":" << hash
             << ",\"circuit\":" << word(0x010eb610u) << ",\"wave\":" << word(0x010eb630u)
             << ",\"world_x\":" << int(stv::gsp::s16(word(0x01000280u)))
             << ",\"world_y\":" << int(stv::gsp::s16(word(0x01000290u))) << "}\n";
        return good();
    }
private:
    std::ofstream out_;
};
} // namespace stv::scene_audit
