// Weapon/power-up variants (enhanced_l8_16_9 only). Shared contract between the
// gameplay owner (src/gsp/variants*.cpp) and the host presentation
// (pickup notification, options menu). Reference profile never consults this.
#pragma once
#include <cstdint>

namespace stv::gsp {

// The only variant rate the player uses: Widescreen always plays variants at
// the balanced ("rare") rate; Arcade (reference profile) never has them.
inline constexpr std::uint8_t kWidescreenVariantMode = 1;

// Options-menu setting. Off keeps the retail pickup behaviour byte-for-byte.
enum class VariantMode : std::uint8_t { Off = 0, Rare = 1, Frequent = 2 };

// Stable ids: persisted in snapshots/replays, never renumber.
enum class Variant : std::uint8_t {
    None = 0,
    // Default gun (rolled from the NONSTOP pickup)
    HotLead, RicochetRounds,
    // Spread shot (SPRAY)
    WallOfLead, ShrapnelSpread,
    // Grenade gun (GRENADE)
    ClusterGrenade, NapalmGrenade,
    // Rocket gun (POD)
    TunnelRocket, TwinSeekers,
    // Boss-room lobber (WPN5 / boss grenade weapon)
    MortarBarrage, QuakeShell,
    // Saw blades (BLADE)
    BuzzStorm, ThrownBlades,
    // Room-killing smart bomb (SMART)
    DoubleTap, GoldRushBomb,
    // Force field (RING)
    Reflector, ShockwaveShield,
    // Speed shoes (SHOES)
    Afterburner, BulldozerBoots,
    // Extra man (PLAYER)
    Clone, LastStand,
    Count
};

struct VariantInfo {
    const char* name;     // shown in the pickup notification
    const char* blurb;    // one-line effect description (notification subtitle)
    std::uint32_t tint;   // 0xRRGGBB recolour applied to the variant's shots/icon
    bool very_rare;       // rolls at the very-rare rate instead of the rare rate
};

inline constexpr VariantInfo kVariantInfo[] = {
    {"",                 "",                                          0x000000, false},
    {"HOT LEAD",         "Piercing incendiary rounds",                0xFF7A1A, false},
    {"RICOCHET ROUNDS",  "Bullets bounce off walls",                  0x29E6FF, false},
    {"WALL OF LEAD",     "Nine-way spread",                           0xFF2E2E, false},
    {"SHRAPNEL SPREAD",  "Pellets burst into shards",                 0xFFF07A, false},
    {"CLUSTER GRENADE",  "Every blast drops four more",               0x4CFF4C, false},
    {"NAPALM GRENADE",   "Leaves the floor burning",                  0xFF8C00, false},
    {"TUNNEL ROCKET",    "Pierces the whole arena",                   0x3D7BFF, false},
    {"TWIN SEEKERS",     "Paired homing rockets",                     0xB04DFF, false},
    {"MORTAR BARRAGE",   "Three-shell line strike",                   0xFF3B3B, false},
    {"QUAKE SHELL",      "Stuns the whole screen",                    0xE8E8E8, true },
    {"BUZZ STORM",       "Six blades, wide orbit",                    0xFF3030, false},
    {"THROWN BLADES",    "Throw boomerang blades",                    0xFFC81E, false},
    {"DOUBLE TAP",       "Detonates twice",                           0xFF2020, true },
    {"GOLD RUSH BOMB",   "Every kill pays out",                       0xFFD700, true },
    {"REFLECTOR",        "Enemy shots bounce back",                   0x30FF6A, false},
    {"SHOCKWAVE SHIELD", "Ends with a blast ring",                    0xFFFFFF, false},
    {"AFTERBURNER",      "Speed and a scorching trail",               0xFF4A10, false},
    {"BULLDOZER BOOTS",  "Run straight through grunts",               0x9AA4B0, false},
    {"CLONE",            "A mirror twin fights too",                  0x8FD8FF, true },
    {"LAST STAND",       "Cheat death once",                          0xFFD24A, false},
};
static_assert(sizeof(kVariantInfo) / sizeof(kVariantInfo[0]) == std::size_t(Variant::Count));

inline const VariantInfo& variant_info(Variant v) {
    return kVariantInfo[std::size_t(v) < std::size_t(Variant::Count) ? std::size_t(v) : 0];
}

} // namespace stv::gsp
