#pragma once
// Host-only cosmetics. Input is a const RAM snapshot, never a Machine or CPU bus.
#include "stv/host/pc_settings.hpp"
#include "stv/gsp/object_lifecycle.hpp"
#include "stv/host/ray_scene.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace stv::host {
enum class EffectKind : uint8_t { None, Projectile, Blood, Explosion, EnemyShot, FleshBreak, MetalBreak };
struct EffectObject { uint32_t slot{}, image{}; EffectKind kind{}; float x{}, y{}, vx{}, vy{}; uint16_t weapon{}; bool barrel{}; float muzzle_x{}, muzzle_y{}; };
struct EffectTarget { uint32_t slot{}; float x{},y{},w{},h{}; bool metal{}; uint16_t oid{}; int paletteBank{-1}; };
struct HudRect { float x,y,w,h; };
struct HudPart : HudRect { int paletteBank{-1}; };
struct ArenaGeometry { float left{},top{},right{},bottom{},gateLeft{},gateRight{}; unsigned openGates{}; std::vector<HudRect> solids; HudRect floor{}; };
struct RoomLamp { float x{},y{},height{},radius{},r{},g{},b{},power{}; };
struct ShadowCaster {
    float x,y,w,h,foot,palette,player,material{};
};
struct StudioCue {float mountX{},mountY{},aimX{},aimY{},doorX{},doorY{}; bool exit{},boss{};};
struct DeathPose { uint32_t process{}; bool dead{}, visible{}; float x{}, y{}, height{}; };
enum BodyFlags : unsigned { MachinePart=1, BossPart=2, BlastDeath=4, BrokenJoint=8 };
struct EnemyRemains { float x{},y{},scale{}; uint32_t oid{},seed{},room{},upper{},lower{},flags{}; float blastDx{},blastDy{}; };
struct EnemyImpact {float x{},y{},dx{},dy{};uint32_t oid{},flags{},room{},paletteBank{};};
class EnemyRemainsTracker {
public:
    void observe(std::span<const uint16_t> ram, gsp::ObjectEvent event, uint32_t node, uint64_t cycle,
                 std::span<const uint8_t> gfx={},std::span<const uint32_t> pens={},uint32_t partner=0);
    std::vector<EnemyRemains> take(uint32_t room);
    std::vector<EnemyImpact> take_impacts(uint32_t room);
private:
    struct Slot {
        bool inserted{}, blasted{};
        EnemyRemains body{};
        float blastDx{},blastDy{};
        uint64_t contactExpires{};
    };
    struct BombOrigin { float x{},y{}; uint32_t room{}; bool valid{}; };
    std::array<BombOrigin,2> smartBombs_{};
    std::array<Slot,gsp::object_slots> slots_{};
    std::vector<EnemyRemains> retired_;
    std::vector<EnemyImpact> impacts_;
};
struct EffectScene {
    uint64_t frame{}; uint32_t room{}; int width{}, height{}, camera_x{}, camera_y{};
    bool valid{}; std::vector<EffectObject> objects;
    std::vector<RayBox> blockers; // artistic sprite proxies, never collision geometry
    std::vector<EffectTarget> targets;
    bool in_arena{};
    std::vector<ShadowCaster> casters;
    std::vector<HudPart> hud;
    ArenaGeometry geometry;
    StudioCue studio;
    std::array<RoomLamp,3> lamps{};
    std::array<DeathPose,2> players{};
    std::vector<EnemyRemains> remains;
    std::vector<EnemyImpact> impacts;
    bool roomIntro{};
};
EffectScene observe_effect_scene(std::span<const uint16_t> ram, uint64_t frame, int width, int height, bool observe_objects=true);
enum class ArenaEffect { Shockwave=7, Tracer=8, Brass=9, Muzzle=10, PlayerBurst=18, EnemyBlast=19, MachineWound=20 };
inline unsigned arena_effect_mask(const PcSettings& s) {
    return (s.shockwaves ? 128u : 0u) | (s.tracers ? 256u : 0u) | (s.brass ? 512u : 0u) | (s.muzzle_flash ? 1024u : 0u) |
        (s.persistent_gore ? 2048u : 0u) | (s.destruction ? (8192u | (1u<<20)) : 0u) | (s.weapon_impacts ? 32768u : 0u) | (s.fluid_smoke ? 65536u : 0u) |
        ((s.blood || s.persistent_gore) ? 1u<<18 : 0u) | (s.explosive_debris ? 1u<<19 : 0u);
}
struct EffectSpawn { float x{},y{},kind{},count{},radius{},power{},pad0{},pad1{}; };
struct EffectPacket { std::vector<RoomLamp> dxrLights; // ray-traced gameplay lights, screen space
     float dt{}; bool reset{}; unsigned mask{}; uint64_t serial{}; std::vector<EffectSpawn> spawns; std::vector<RayBox> boxes;
    bool in_arena{}; uint32_t room{}; uint64_t frame{}; int camera_x{},camera_y{};
    uint64_t roomAge{}; bool roomIntro{};
    std::vector<EnemyRemains> remains;
    std::vector<EffectTarget> foreground;
    std::vector<ShadowCaster> casters;
    std::vector<HudPart> hud;
    ArenaGeometry geometry;
    StudioCue studio;
    std::array<RoomLamp,3> lamps{};
};
class EnhanceEffects {
public:
    static constexpr size_t Capacity = 1024;
    void reset();
    void use_gpu_simulation(bool value) { resident_=value; reset(); }
    const EffectPacket& packet() const { return packet_; }
    void advance(const EffectScene&, const PcSettings&);
    void compose(std::span<uint32_t> pixels, int width, int height, const PcSettings&) const;
    size_t count() const { return particles_.size(); }
    size_t voxel_count() const;
    const std::string& gpu_error() const { return gpu_error_; }
private:
    enum class Kind : uint8_t { Spark, Blood, Debris, Smoke, Fire, Voxel };
    struct Particle { Kind kind; float x,y,z,vx,vy,vz,life,total,size; uint32_t color; };
    void emit(Kind, float, float, int, bool inert=false);
    bool resident_{}; EffectPacket packet_{};
    float random();
    bool enabled(Kind, const PcSettings&) const;
    EffectScene previous_{};
    uint64_t roomStart_{};std::array<float,4> stepDistance_{};std::array<unsigned,2> nextFoot_{};
    bool primed_{};
    uint32_t random_ = 0x51a7b10du;
    std::vector<Particle> particles_;
    mutable std::string gpu_error_;
    std::vector<uint64_t> contacts_;
};
}
