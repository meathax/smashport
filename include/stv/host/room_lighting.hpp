#pragma once
// Presentation-only lighting rigs for the hardware ray-traced path. Every rig is
// anchored to the observed room floor in world space (floor + camera), so the
// entry pan, room-to-room scrolls and the settled room show the same lights.
// Nothing here reads or writes game state; input is the const EffectPacket.
#include <array>
#include <cstdint>
#include <vector>
#include "stv/host/enhance.hpp"

namespace stv::host {
// 64-byte record shared with dxr_lighting.hlsl (four float4 rows).
struct GpuLight {
    float x{}, y{}, z{}, radius{};       // screen-space X/Y (light-map coordinates), height, reach
    float r{}, g{}, b{}, intensity{};
    float dx{}, dy{}, dz{}, cosOuter{-2}; // spot axis; cosOuter <= -1 is omnidirectional
    float cosInner{1}, softness{}, pattern{}, samples{};
};
static_assert(sizeof(GpuLight) == 64);

enum class MoodTheme : uint8_t { Steel, Neon, Moonlit, Toxic, Stage, Jungle, Temple, Mutoid, Scarface, Cobra, Finale, Dome };
enum class LightPattern : uint8_t { None, Canopy, Fan, Sparkle, Caustic };

struct RoomMood {
    MoodTheme theme{};
    std::array<float, 3> ambient{};  // multiplies the floor art
    float gloss{};                   // floor specular strength
    float actorFloor{};              // minimum sprite lighting, keeps actors readable
    float occlusion{};               // ambient contact-shadow strength
};

MoodTheme room_theme(uint32_t room);
const char *theme_name(MoodTheme);
RoomMood room_mood(uint32_t room);

struct LightingFrame {
    RoomMood mood{};
    std::vector<GpuLight> lights;
    std::array<float, 4> floor{}; // anchored current floor, light-map space
    unsigned staticLights{}, previousRoomLights{}, studioLights{}, dynamicLights{};
};

class RoomLightRig {
  public:
    static constexpr size_t MaxLights = 32;
    static constexpr size_t MaxDynamic = 12;
    void reset() { current_ = previous_ = {}; }
    // width/offset describe the light map: offset is the 4:3 wing shift.
    LightingFrame build(const EffectPacket &, int width, int offset);

  private:
    struct Anchor {
        uint32_t room{};
        float x{}, y{}, w{}, h{};
        bool valid{};
    };
    Anchor current_, previous_;
};
} // namespace stv::host
