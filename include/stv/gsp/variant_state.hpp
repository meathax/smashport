#pragma once
// Weapon/power-up variant machine state (enhanced_l8_16_9, variant_mode != 0).
// Plain data: serialized byte-for-byte by src/gsp/state.cpp after the enhanced
// input records, only when variants are enabled or this state is not empty.
// Everything here is derived from guest state at retail hook points
// (docs/WEAPON_VARIANTS.md); nothing reads host time or host randomness.
#include <array>
#include <cstdint>
#include "stv/gsp/variants.hpp"

namespace stv::gsp {

enum class VariantSpawnKind : std::uint8_t {
    None = 0, Blast, Patch, Trail, Fragment, Shake, SmartBomb, Cash, CloneHelper,
};

enum class VariantChild : std::uint8_t {
    None = 0,
    Blast,      // pod process that explodes on its first wake (cluster, mortar, shockwave, bulldozer)
    Patch,      // napalm floor patch: stationary piercing pod with a long fuse
    Trail,      // afterburner flame: stationary pod with a short fuse
    Fragment,   // shrapnel: short-lived spray pellet
    Thrown,     // a ThrownBlades saw (orbit/throw state in the per-process fields)
};

struct VariantPlayer {
    std::uint8_t weapon{};        // Variant of the current weapon pickup
    std::uint8_t weapon_type{};   // WPN_TYP the weapon variant belongs to
    std::uint8_t gun{};           // HotLead / RicochetRounds (default gun)
    std::uint8_t blade{};
    std::uint8_t ring{};
    std::uint8_t ring_pending{};  // next RINGS process for this player takes `ring`
    std::uint8_t shoe{};
    std::uint8_t extra_pending{}; // Clone / LastStand from the current EXTRA pickup
    std::uint8_t last_stand{};    // banked LastStand (max 1)
    std::uint8_t clone{};         // 1 while a Clone helper is out
    std::uint8_t fire_loop{};     // CREATE re-entry counter (spread / seekers / mortar)
    std::uint8_t fire_count{};    // Wall of Lead cadence
    std::uint8_t throw_rr{};      // next ThrownBlades saw to launch
    std::uint8_t throw_cooldown{};
    std::uint8_t damage_count{};  // deterministic reduced-damage cadence against bosses
    std::uint8_t gold_drops{};
    std::uint8_t double_tap_circuit{}; // circuit+1 of the last DoubleTap re-detonation
    std::uint8_t grace{};         // LastStand: frames the saved player cannot be killed
    std::uint16_t gun_timer{};
    std::uint16_t clone_timer{};
    std::uint16_t double_tap_timer{};
    std::uint16_t gold_timer{};
    std::uint16_t ring_timer{};
    std::uint16_t trail_timer{};
    std::uint16_t pad3{};
    std::uint32_t double_tap_obj{};
};
static_assert(sizeof(VariantPlayer) == 36);

struct VariantSpawn {            // queued from a retail hook, created at the next process dispatch
    std::uint8_t kind{}, variant{}, owner{}, pad{};   // kind: VariantSpawnKind
    std::uint16_t age{}, pad2{};
    std::int32_t x{}, y{};        // world pixels
    std::int32_t vx{}, vy{};      // 16.16 velocity (fragments)
};

struct VariantState {
    static constexpr std::uint32_t kVersion = 1;
    std::uint32_t version{kVersion};
    std::uint16_t stun_timer{};
    std::uint16_t spawn_count{};
    std::array<std::uint8_t, 420> icon{};        // per foreground object slot: rolled icon variant
    std::array<std::uint16_t, 420> obj_mark{};   // per object: low 16 bits of the frame it was last plowed
    std::array<std::uint8_t, 240> proc_var{};    // per process slot: owning Variant
    std::array<std::uint8_t, 240> proc_kind{};   // VariantChild
    std::array<std::uint8_t, 240> proc_owner{};  // player 1/2
    std::array<std::uint8_t, 240> proc_flags{};  // bit0 bounced, bit1 split, bit2 throwing
    std::array<std::uint16_t, 240> proc_aux{};   // fan index / pierce count / life / throw phase
    std::array<std::uint16_t, 240> proc_last{};  // last pierced object slot + 1
    std::array<std::int32_t, 240> proc_x{}, proc_y{}, proc_vx{}, proc_vy{};
    std::array<VariantPlayer, 2> player{};
    std::array<VariantSpawn, 24> spawn{};
    std::array<std::uint32_t, 4 * 420> queue_node{}; // display record -> object node (tinted DMA)
};
static_assert(sizeof(VariantState) == 14396, "VariantState is serialized byte-for-byte; bump kVersion on layout changes");

// Pickup roll (pure): icon subtype (ICONS.ASM ICINIT index - 1), VariantMode,
// boss room flag and a 64-bit hash of guest state. Returns Variant::None or a
// variant of that pickup.
Variant variant_roll(unsigned icon_subtype, std::uint8_t mode, bool boss_room, std::uint64_t hash);
// The two variants a pickup subtype can carry (None when it carries none).
void variant_pair(unsigned icon_subtype, Variant& first, Variant& second);

} // namespace stv::gsp
