#pragma once
// Host-only cosmetics. Input is a const RAM snapshot, never a Machine or CPU bus.
#include "stv/host/pc_settings.hpp"
#include "stv/gsp/object_lifecycle.hpp"
#include "stv/host/ray_scene.hpp"
#include "stv/host/room_cinema.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace stv::host {
enum class EffectKind : uint8_t { None, Projectile, Blood, Explosion, EnemyShot, FleshBreak, MetalBreak };
struct EffectObject { uint32_t slot{}, image{}; EffectKind kind{}; float x{}, y{}, vx{}, vy{}; uint16_t weapon{}; bool barrel{}; float muzzle_x{}, muzzle_y{}; bool raster_muzzle{}; };
struct EffectTarget { uint32_t slot{}; float x{},y{},w{},h{}; bool metal{}; uint16_t oid{}; int paletteBank{-1}; };
struct HudRect { float x,y,w,h; };
struct HudPart : HudRect { int paletteBank{-1}; bool backdrop{}; bool scorePanel{}; };
struct TankShadow { int x{},y{},w{},h{}; uint16_t pen{}; };
struct ArenaGeometry { float left{},top{},right{},bottom{},gateLeft{},gateRight{}; unsigned openGates{}; std::vector<HudRect> solids; HudRect floor{}; };
struct RoomLamp { float x{},y{},height{},radius{},r{},g{},b{},power{}; };
struct ShadowCaster {
    float x,y,w,h,foot,palette,player,material{};
};
struct StudioCue {
    float mountX{},mountY{},aimX{},aimY{},doorX{},doorY{};
    float bossX{},bossY{};
    unsigned bossParts{};
    bool exit{},boss{};
};
struct DeathPose { uint32_t process{}; bool dead{}, visible{}; float x{}, y{}, height{}; };
enum BodyFlags : unsigned { MachinePart=1, BossPart=2, BlastDeath=4, BrokenJoint=8 };
struct EnemyRemains { float x{},y{},scale{}; uint32_t oid{},seed{},room{},upper{},lower{},flags{}; float blastDx{},blastDy{}; };
struct EnemyImpact {float x{},y{},dx{},dy{};uint32_t oid{},flags{},room{},paletteBank{};uint16_t weapon{};};
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
    std::vector<gsp::VisibleMuzzle> visible_muzzles;
    bool roomIntro{};
    std::vector<TankShadow> tankShadows;
};
EffectScene observe_effect_scene(std::span<const uint16_t> ram, uint64_t frame, int width, int height, bool observe_objects=true);
enum class ArenaEffect { Shockwave=7, Brass=9, Muzzle=10, PlayerBurst=18, EnemyBlast=19, MachineWound=20 };
inline unsigned arena_effect_mask(const PcSettings& s) {
    return (s.shockwaves ? 128u : 0u) | (s.brass ? 512u : 0u) | (s.muzzle_flash ? 1024u : 0u) |
        (s.persistent_gore ? 2048u : 0u) | (s.destruction ? (8192u | (1u<<20)) : 0u) | (s.weapon_impacts ? 32768u : 0u) | (s.fluid_smoke ? 65536u : 0u) |
        ((s.blood || s.persistent_gore) ? 1u<<18 : 0u) | (s.explosive_debris ? 1u<<19 : 0u);
}
struct EffectSpawn { float x{},y{},kind{},count{},radius{},power{},pad0{},pad1{}; };
enum class ImpactTier : uint8_t { None, Small, Medium, Large };
// One cosmetic family per observed player projectile OID 0x4201..0x4206.
// Keep names identifier-neutral: the source labels for the six families are
// not reliable presentation metadata.
enum class WeaponFamily : uint8_t { Family1, Family2, Family3, Family4, Family5, Family6, Count };
enum class WeaponFxKind : uint8_t { Muzzle, Wake, Impact, Ricochet };
struct WeaponFxSignature {
    float muzzle{}, wake{}, impact{}, ricochet{};
    uint32_t color{};
};
inline constexpr std::array<WeaponFxSignature,size_t(WeaponFamily::Count)> weapon_fx_signatures{{
    {1.00f,.80f,.85f,.65f,0xffd6a0u},
    {1.18f,1.00f,.92f,.78f,0xffb44cu},
    {.92f,1.18f,1.00f,.86f,0xff8a3du},
    {1.42f,1.12f,1.38f,1.08f,0xff6336u},
    {.82f,1.35f,1.08f,.72f,0x8ceeffu},
    {1.10f,.95f,1.48f,1.15f,0xffb36cu}
}};
constexpr bool projectile_weapon_family(uint16_t weapon,WeaponFamily& family) {
    if(weapon<0x4201u || weapon>0x4206u)return false;
    family=static_cast<WeaponFamily>(weapon-0x4201u);return true;
}
struct ImpactDirectorState {
    // Final-presentation outputs only. Do not feed these values into game state,
    // collision, input, RNG, damage, or audio.
    ImpactTier tier{ImpactTier::None};
    float camera_trauma{};          // scalar in 0..1
    float zoom_punch{};             // additive final zoom factor; 0 is neutral
    float hit_flash{}, vignette{}, chromatic_split{}; // scalar in 0..1
};
struct WeaponFxEvent {
    float x{},y{},dx{},dy{},strength{};
    WeaponFamily family{WeaponFamily::Family1};
    WeaponFxKind kind{WeaponFxKind::Wake};
};
struct WeaponFxPacket {
    static constexpr size_t EventCapacity = 96;
    ImpactDirectorState director;
    std::array<WeaponFxEvent,EventCapacity> events{};
    uint32_t event_count{};
};
struct EffectPacket { std::vector<RoomLamp> dxrLights; // ray-traced gameplay lights, screen space
     float dt{}; bool reset{}; uint64_t serial{}; std::vector<EffectSpawn> spawns; std::vector<RayBox> boxes;
    bool in_arena{}; uint32_t room{}; uint64_t frame{}; int camera_x{},camera_y{};
    uint64_t roomAge{}; bool roomIntro{};
    std::vector<EnemyRemains> remains;
    std::vector<EffectTarget> foreground;
    std::vector<ShadowCaster> casters;
    std::vector<HudPart> hud;
    ArenaGeometry geometry;
    StudioCue studio;
    RoomCinemaCue cinema;
    std::array<RoomLamp,3> lamps{};
    WeaponFxPacket weaponFx;
    std::vector<TankShadow> tankShadows;
};
inline constexpr std::size_t MaxGameplayLights = 12;
class EnhanceEffects {
public:
    static constexpr size_t Capacity = 1024;
    void reset();
    void reset_room_cinema() { roomCinema_.reset(); }
    void use_gpu_simulation(bool value) { resident_=value; reset(); reset_room_cinema(); }
    const EffectPacket& packet() const { return packet_; }
    void advance(const EffectScene&, const PcSettings&, float audioPulse=0);
    void compose(std::span<uint32_t> pixels, int width, int height, const PcSettings&) const;
    size_t count() const { return particles_.size(); }
    size_t voxel_count() const;
    const std::string& gpu_error() const { return gpu_error_; }
private:
    enum class Kind : uint8_t { Spark, Blood, Debris, Smoke, Fire, Voxel };
    struct Particle { Kind kind; float x,y,z,vx,vy,vz,life,total,size; uint32_t color; };
    void emit(Kind, float, float, int, bool inert=false);
    void reset_weapon_fx();
    void advance_weapon_fx(const EffectScene&, float, const PcSettings&);
    void queue_weapon_fx(WeaponFxKind,WeaponFamily,float,float,float,float,float);
    void impact(ImpactTier tier, bool camera_motion=false);
    bool has_weapon_fx(WeaponFxKind,WeaponFamily,float,float) const;
    bool resident_{}; EffectPacket packet_{};
    float random();
    bool enabled(Kind, const PcSettings&) const;
    EffectScene previous_{};
    uint64_t roomStart_{},roomClockFrame_{};uint32_t roomClockRoom_{};bool roomClock_{};std::array<float,4> stepDistance_{};std::array<unsigned,2> nextFoot_{};
    bool primed_{};
    bool bossSmokeDeath_{};
    BossCinemaTracker roomCinema_;
    bool weaponFxObjectContinuity_{};
    uint32_t random_ = 0x51a7b10du;
    std::vector<Particle> particles_;
    mutable std::string gpu_error_;
    std::vector<uint64_t> contacts_;
};
}
