#pragma once
#include "stv/gsp/machine.hpp"
#include "stv/host/enhance.hpp"
#include "stv/gsp/sprite_ink.hpp"
#include <array>
#include <cmath>

namespace stv::host {
namespace firing_detail {
using stv::gsp::Machine;
using stv::gsp::u16; using stv::gsp::u32; using stv::gsp::s16;
// Enhanced shots already carry the launch pixel. Classic and enemy shots
// retain retail placement and use the owner's silhouette for the flash.
using stv::gsp::sprite_ink;

inline bool firing_point(const Machine& m, const EffectObject& shot, int camera_x, int camera_y, float& x, float& y) {
    using stv::gsp::object_pool_base; using stv::gsp::object_stride; using stv::gsp::object_slots;
    const u32 offset = shot.slot - object_pool_base;
    if (offset >= object_slots * object_stride || offset % object_stride) return false;
    const auto& spawn = m.spawn_anchor[offset / object_stride];
    if (!spawn.valid) return false;
    x = float(spawn.x - camera_x);
    y = float(spawn.y - camera_y);
    const float speed = std::hypot(shot.vx, shot.vy);
    if (speed < .1f) return true;
    if (m.enhanced && shot.kind == EffectKind::Projectile) return true;
    // Owners: displayed non-shot objects whose sprite contains the spawn anchor.
    std::array<u32, 8> owners{};
    std::size_t count = 0;
    auto word = [&](u32 a) { return m.ram[(a - 0x01000000u) >> 4]; };
    auto longword = [&](u32 a) { return u32(word(a)) | (u32(word(a + 16)) << 16); };
    for (u32 node = longword(0x01000040u), guard = 0; node && guard < object_slots && count < owners.size(); node = longword(node), ++guard) {
        if (node == shot.slot) continue;
        if (node < object_pool_base || (node-object_pool_base)%object_stride ||
            (node-object_pool_base)/object_stride>=object_slots) break;
        const u16 oid = word(node + 0x150);
        if (shot.kind==EffectKind::Projectile && (oid&0xff00)!=0x4100)continue;
        if ((oid & 0xff00) == 0x4200 || oid == 0x8500) continue;
        const int w = word(node + 0xf0), h = word(node + 0x100);
        const int ox = s16(word(node + 0x70)), oy = s16(word(node + 0x90));
        if (spawn.x >= ox - 2 && spawn.x < ox + w + 2 && spawn.y >= oy - 2 && spawn.y < oy + h + 2) owners[count++] = node;
    }
    if (!count) return true;
    const float dx = shot.vx / speed, dy = shot.vy / speed;
    float last = -1;
    for (float t = 0; t <= 24.f; t += .5f) {
        const int wx = int(std::floor(spawn.x + .5f + dx * t)), wy = int(std::floor(spawn.y + .5f + dy * t));
        bool ink = false;
        for (std::size_t i = 0; i < count && !ink; ++i) ink = sprite_ink(m, owners[i], wx, wy);
        if (ink) last = t;
        else if (last >= 0 && t - last > 1.5f) break;   // left the barrel
    }
    if (last >= 0) {
        // Same pixel-centre convention as the sampling above: the first
        // transparent pixel past the last ink pixel on the shot's path.
        x = std::floor(spawn.x + .5f + dx * (last + .5f)) - camera_x;
        y = std::floor(spawn.y + .5f + dy * (last + .5f)) - camera_y;
    }
    return true;
}

} // namespace firing_detail
using firing_detail::firing_point;
using firing_detail::sprite_ink;
} // namespace stv::host
