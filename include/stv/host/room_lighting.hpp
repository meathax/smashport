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
enum class ScaffoldMotion : uint8_t { CrossSweep, OrbitSweep, Chase, Pulse, Beacon, Scanline };

struct RoomLightingProfile {
    uint32_t room{};
    const char *name{};
    MoodTheme theme{};
    ScaffoldMotion scaffold{};
    std::array<float, 3> accent{1, 1, 1};
    float ambient{1};       // multiplier on the theme's dark base
    float intensity{1};     // room-light multiplier, never an actor-speed/control value
    float rate{.015f};      // scaffold animation rate
    float phase{};
    float rig{1};           // virtual show-lighting wash, independent of scaffold art
};

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
const RoomLightingProfile &room_lighting_profile(uint32_t room);
size_t room_lighting_profile_count();

struct RoomLightingLook {
    uint32_t room{};
    RoomCinemaCue cinema{};
};

// Tone match between the ray-traced rig and the original art. The arcade art is
// already painted under its own light, so the irradiance that multiplies it must
// average about 1 per surface, or the floor blows out while the walls sink.
// scale: floor, rear wall, margins (sides/front); balance: rgb that removes most
// of the rig's average colour cast. Derived from the static room lamps averaged
// over their animation: explosions, boss cues and lamp motion still read.
struct RoomExposure {
    std::array<float, 4> scale{1, 1, 1, 1}; // w: share of the lamps' colour kept
    std::array<float, 4> balance{1, 1, 1, 0};
};
RoomExposure room_exposure(uint32_t room, float floorWidth, float floorHeight);

struct LightingFrame {
    RoomMood mood{};
    RoomExposure exposure{}, previousExposure{};
    std::vector<GpuLight> lights;
    std::array<float, 4> floor{}; // anchored current floor, light-map space
    unsigned staticLights{}, previousRoomLights{}, studioLights{}, dynamicLights{};
    std::array<float, 4> previousFloor{};
    RoomMood previousMood{};
    unsigned gates{}, previousGates{};
    float leftGate{}, previousLeftGate{}, lightTime{}, previousLightTime{};
    bool previousVisible{}, holdingOrigin{};
    RoomLightingLook look{}, previousLook{};
    // Volumetric in-scatter samples per half-resolution column (0 = off).
    unsigned volumetricSteps{};
};

class RoomLightRig {
  public:
    static constexpr size_t MaxRoomLights = 32;
    static constexpr size_t MaxLights = 2 * MaxRoomLights;
    static constexpr size_t MaxDynamic = MaxGameplayLights;
    void reset() { current_ = {}; previous_ = {}; }
    // width/offset describe the light map: offset is the 4:3 wing shift.
    LightingFrame build(const EffectPacket &, int width, int offset);

  private:
    struct Anchor {
        uint32_t room{};
        float x{}, y{}, w{}, h{};
        bool valid{};
    };
    struct Snapshot {
        Anchor anchor{};
        RoomMood mood{};
        std::array<GpuLight, MaxRoomLights> lights{};
        size_t count{};
        float cameraX{}, cameraY{};
        unsigned gates{};
        float leftGate{};
        uint64_t frame{};
        RoomLightingLook look{};
    };
    Snapshot current_, previous_;
};
} // namespace stv::host
