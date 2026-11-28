#include "stv/host/room_lighting.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <mutex>

namespace stv::host {
namespace {
constexpr float Pi = 3.14159265f;
// Global grade: darkness is the canvas; authored fixtures remain readable.
// Hard sci-fi TV studio grade: a high-key, cool-neutral LED soundstage. The
// floor is always lit; colour lives in controlled accents, not in darkness.
constexpr float AmbientScale = .76f, LightScale = 1.74f;
// Tone match (room_exposure): mean art multiplier per surface class with every
// enhancement on, calibrated against the original frames. The floor sits a touch
// above the art so the lamp pools read; walls and margins keep the art level.
// Ao/Lit are the measured mean occlusion and shadowing the analytic rig omits.
constexpr float ExposureFloorTarget = 1.13f, ExposureWallTarget = 1.13f, ExposureMarginTarget = 1.2f;
constexpr float ExposureFloorAo = 1.f, ExposureFloorLit = 1.5f, ExposureWallAo = .64f, ExposureMarginAo = .62f;
constexpr float ExposureNeutralize = .5f, ExposureChroma = .82f;
enum class Anim : uint8_t { Steady, Flicker, Pulse, Strobe, Broken, Sweep, Orbit, Hue };
struct Lamp {
    float u{}, v{}, z{}, radius{};
    float r{1}, g{1}, b{1}, intensity{1};
    Anim anim{Anim::Steady};
    float rate{}, phase{};
    float cone{}, inner{.5f};      // degrees; 0 is omnidirectional
    float aimU{.5f}, aimV{.5f}, sweepU{}, sweepV{};
    LightPattern pattern{LightPattern::None};
    float softness{6};
    float samples{1};
};
float hash1(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return float(x & 0xffffff) / float(0x1000000);
}
float valueNoise(float t) {
    const float i = std::floor(t), f = t - i, s = f * f * (3 - 2 * f);
    const uint32_t k = uint32_t(int32_t(i));
    return hash1(k) * (1 - s) + hash1(k + 1) * s;
}
std::array<float, 3> hue(float h) {
    h = h - std::floor(h);
    auto c = [&](float o) { return std::clamp(std::abs(std::fmod(h * 6 + o, 6.f) - 3) - 1, 0.f, 1.f); };
    return {.15f + .85f * c(0), .15f + .85f * c(4), .15f + .85f * c(2)};
}
Lamp omni(float u, float v, float z, float radius, float r, float g, float b, float intensity, float soft = 6,
          float samples = 1) {
    Lamp l;
    l.u = u; l.v = v; l.z = z; l.radius = radius; l.r = r; l.g = g; l.b = b; l.intensity = intensity;
    l.softness = soft; l.samples = samples;
    return l;
}
Lamp spot(float u, float v, float z, float radius, float r, float g, float b, float intensity, float cone,
          float aimU, float aimV, float inner = .55f) {
    Lamp l = omni(u, v, z, radius, r, g, b, intensity, 9, 2);
    l.cone = cone; l.inner = inner; l.aimU = aimU; l.aimV = aimV;
    return l;
}
Lamp animate(Lamp l, Anim a, float rate, float phase = 0, float sweepU = 0, float sweepV = 0) {
    l.anim = a; l.rate = rate; l.phase = phase; l.sweepU = sweepU; l.sweepV = sweepV;
    return l;
}
Lamp pattern(Lamp l, LightPattern p) { l.pattern = p; return l; }

// Retail room numbering: high word is the circuit (0..2), low word the wave.
// Themes follow the arena art observed in the 48-room widescreen audit sheets.
struct RoomKey { unsigned circuit, wave; };
RoomKey key(uint32_t room) { return {room >> 16, room & 0xffff}; }

// Authored retail selectors: 48 playable rooms plus terminal/bonus selectors.
// Profile entries are presentation-only and never address game RAM or ROM data.
constexpr RoomLightingProfile kRoomProfiles[] = {
    {0x00001, "Arena One: gantry opener", MoodTheme::Steel, ScaffoldMotion::CrossSweep, {.62f,.80f,1}, 0.586f, 1.01f, .012f, .0f, 1.05f},
    {0x00002, "Ten Keys: crimson alarm", MoodTheme::Steel, ScaffoldMotion::OrbitSweep, {1,.22f,.12f}, 0.568f, 1.05f, .019f, .7f, 1.05f},
    {0x00003, "Power Vault: toxic spill", MoodTheme::Steel, ScaffoldMotion::Pulse, {.38f,1,.35f}, 0.596f, 1.08f, .034f, 1.6f, 1.05f},
    {0x00004, "Shrapnel Works: magenta cut", MoodTheme::Neon, ScaffoldMotion::Chase, {1,.26f,.70f}, 0.826f, 1.07f, .027f, 2.4f, 1.05f},
    {0x00005, "Bonus Prizes: live broadcast", MoodTheme::Stage, ScaffoldMotion::CrossSweep, {1,.78f,.42f}, 1.400f, 1.00f, .016f, .5f, 1.20f},
    {0x00006, "Shrapnel Hall: broken strip", MoodTheme::Steel, ScaffoldMotion::Beacon, {1,.55f,.18f}, 0.538f, 1.11f, .041f, 4.1f, 1.05f},
    {0x00007, "Total Carnage: orange rolling bank", MoodTheme::Steel, ScaffoldMotion::Scanline, {1,.42f,.10f}, 0.576f, 1.12f, .052f, 1.1f, 1.10f},
    {0x00008, "Crowd Control: cyan crossfire", MoodTheme::Neon, ScaffoldMotion::CrossSweep, {.20f,.88f,1}, 0.731f, 1.06f, .014f, 3.0f, 1.05f},
    {0x00009, "Tank Trouble: red chase", MoodTheme::Neon, ScaffoldMotion::Chase, {1,.18f,.10f}, 0.798f, 1.15f, .031f, 5.2f, 1.10f},
    {0x0000a, "Mutoid One: dissection white", MoodTheme::Mutoid, ScaffoldMotion::CrossSweep, {.88f,.92f,1}, 0.866f, 1.17f, .011f, .2f, 1.25f},
    {0x0000b, "First Rack: silver finale", MoodTheme::Stage, ScaffoldMotion::Pulse, {.75f,.86f,1}, 1.0f, 1.00f, .020f, 2.2f, 1.25f},

    {0x10001, "Orbs: violet containment", MoodTheme::Neon, ScaffoldMotion::OrbitSweep, {.64f,.36f,1}, 0.836f, 1.07f, .018f, .4f, 1.10f},
    {0x10002, "Ten Keys Two: cyan lift", MoodTheme::Neon, ScaffoldMotion::CrossSweep, {.22f,.92f,1}, 0.846f, 1.06f, .015f, 1.4f, 1.05f},
    {0x10003, "Smash 'Em: pink pursuit", MoodTheme::Moonlit, ScaffoldMotion::Scanline, {1,.31f,.72f}, 0.711f, 1.10f, .045f, 2.5f, 1.05f},
    {0x10004, "Fire Power: toxic valves", MoodTheme::Toxic, ScaffoldMotion::Pulse, {.42f,1,.20f}, 1.048f, 1.12f, .037f, 3.7f, 1.15f},
    {0x10005, "Slaughter Line: magenta rails", MoodTheme::Neon, ScaffoldMotion::Chase, {1,.18f,.62f}, 0.897f, 1.11f, .024f, 4.8f, 1.05f},
    {0x10006, "Laser Death Zone: blue slicer", MoodTheme::Neon, ScaffoldMotion::Scanline, {.18f,.68f,1}, 0.831f, 1.16f, .061f, 5.1f, 1.10f},
    {0x10007, "Scarface: monitor storm", MoodTheme::Scarface, ScaffoldMotion::OrbitSweep, {.42f,1,.46f}, 0.790f, 1.19f, .026f, .8f, 1.30f},
    {0x10008, "Rowdy Droids: moon search", MoodTheme::Moonlit, ScaffoldMotion::CrossSweep, {.52f,.70f,1}, 0.859f, 1.05f, .013f, 1.9f, 1.05f},
    {0x10009, "Vacuum Clean: white hazard", MoodTheme::Moonlit, ScaffoldMotion::Beacon, {1,.72f,.46f}, 0.510f, 1.12f, .043f, 3.4f, 1.15f},
    {0x1000a, "Reserved metal selector", MoodTheme::Moonlit, ScaffoldMotion::Pulse, {.52f,.72f,1}, 0.750f, 1.00f, .029f, .0f, 1.05f},
    {0x1000b, "Metal Death: caustic bay", MoodTheme::Toxic, ScaffoldMotion::OrbitSweep, {.30f,1,.54f}, 0.745f, 1.13f, .020f, 2.1f, 1.15f},
    {0x1000c, "Watch Your Step: pit show", MoodTheme::Stage, ScaffoldMotion::CrossSweep, {1,.80f,.36f}, 1.400f, 1.03f, .017f, 4.4f, 1.20f},
    {0x1000d, "Total Carnage Two: cold violet", MoodTheme::Moonlit, ScaffoldMotion::Scanline, {.66f,.46f,1}, 0.806f, 1.10f, .047f, 1.7f, 1.05f},
    {0x1000e, "Defend Me: cyan apron", MoodTheme::Stage, ScaffoldMotion::Chase, {.26f,.90f,1}, 1.290f, 1.03f, .022f, 5.9f, 1.25f},
    {0x1000f, "Turtles: amber convoy", MoodTheme::Moonlit, ScaffoldMotion::OrbitSweep, {1,.63f,.24f}, 0.500f, 1.06f, .016f, 2.7f, 1.05f},
    {0x10010, "Chunks: magenta machinery", MoodTheme::Neon, ScaffoldMotion::Pulse, {1,.24f,.66f}, 0.799f, 1.12f, .035f, 4.0f, 1.15f},
    {0x10011, "These Are Fast: cobalt strobe", MoodTheme::Neon, ScaffoldMotion::Scanline, {.16f,.54f,1}, 0.954f, 1.17f, .068f, 1.2f, 1.10f},
    {0x10012, "Buffalo Herd: polar sweep", MoodTheme::Moonlit, ScaffoldMotion::CrossSweep, {.58f,.84f,1}, 0.744f, 1.06f, .015f, 3.3f, 1.05f},
    {0x10013, "Second Rack: blue-gold finale", MoodTheme::Stage, ScaffoldMotion::Pulse, {.56f,.76f,1}, 1.0f, 1.01f, .024f, 4.9f, 1.25f},

    {0x20001, "No Dice: jungle breach", MoodTheme::Jungle, ScaffoldMotion::OrbitSweep, {1,.60f,.18f}, 0.938f, 1.07f, .014f, .6f, 1.05f},
    {0x20002, "Temple Alert: braziers", MoodTheme::Temple, ScaffoldMotion::CrossSweep, {1,.56f,.16f}, 1.141f, 1.10f, .012f, 1.8f, 1.20f},
    {0x20003, "Scorpion Fever: amber pincer", MoodTheme::Jungle, ScaffoldMotion::Scanline, {1,.38f,.08f}, 0.981f, 1.12f, .048f, 3.1f, 1.05f},
    {0x20004, "Cobra Ahead: acid warning", MoodTheme::Jungle, ScaffoldMotion::Pulse, {.52f,1,.24f}, 0.813f, 1.13f, .034f, 4.2f, 1.15f},
    {0x20005, "Walls of Pain: gold teeth", MoodTheme::Temple, ScaffoldMotion::Chase, {1,.52f,.14f}, 0.574f, 1.08f, .029f, 5.4f, 1.15f},
    {0x20006, "Last Arena: furnace sun", MoodTheme::Temple, ScaffoldMotion::CrossSweep, {1,.72f,.30f}, 0.500f, 1.13f, .016f, 1.1f, 1.20f},
    {0x20007, "Cobra Death: twin crimson moons", MoodTheme::Cobra, ScaffoldMotion::OrbitSweep, {1,.20f,.08f}, 0.500f, 1.23f, .018f, .4f, 1.30f},
    {0x20008, "Turtle Guard: cyan canopy", MoodTheme::Jungle, ScaffoldMotion::Beacon, {.18f,.88f,1}, 0.928f, 1.10f, .036f, 2.0f, 1.05f},
    {0x20009, "Extra Sauce: hot magenta", MoodTheme::Jungle, ScaffoldMotion::Scanline, {1,.26f,.50f}, 0.968f, 1.14f, .056f, 3.3f, 1.05f},
    {0x2000a, "Reserved jungle selector", MoodTheme::Jungle, ScaffoldMotion::Pulse, {1,.60f,.20f}, 0.930f, 1.00f, .030f, .0f, 1.05f},
    {0x2000b, "Secret Rooms: hidden gold", MoodTheme::Temple, ScaffoldMotion::Beacon, {1,.76f,.30f}, 0.517f, 1.12f, .039f, 4.7f, 1.20f},
    {0x2000c, "Enjoy My Wealth: gold broadcast", MoodTheme::Stage, ScaffoldMotion::CrossSweep, {1,.74f,.28f}, 1.400f, 1.05f, .017f, 5.8f, 1.30f},
    {0x2000d, "No Turtles: cold furnace", MoodTheme::Jungle, ScaffoldMotion::OrbitSweep, {.45f,.72f,1}, 0.961f, 1.10f, .019f, 1.5f, 1.05f},
    {0x2000e, "Turtle Chunks: blue-gold chase", MoodTheme::Stage, ScaffoldMotion::Chase, {.42f,.82f,1}, 1.400f, 1.05f, .026f, 2.8f, 1.25f},
    {0x2000f, "Dynamite Cobra: ember grid", MoodTheme::Jungle, ScaffoldMotion::Scanline, {1,.30f,.08f}, 0.957f, 1.17f, .064f, 4.1f, 1.10f},
    {0x20010, "Buffalo Gun: temple target", MoodTheme::Temple, ScaffoldMotion::CrossSweep, {1,.58f,.22f}, 0.520f, 1.12f, .014f, 5.0f, 1.20f},
    {0x20011, "Total Carnage Three: violet rain", MoodTheme::Jungle, ScaffoldMotion::Scanline, {.72f,.36f,1}, 0.925f, 1.15f, .050f, 1.0f, 1.15f},
    {0x20012, "Secret Hall: amber breach", MoodTheme::Jungle, ScaffoldMotion::OrbitSweep, {1,.54f,.18f}, 0.853f, 1.11f, .018f, 3.6f, 1.10f},
    {0x20013, "Enough Keys: electric gold", MoodTheme::Stage, ScaffoldMotion::Chase, {1,.82f,.36f}, 1.400f, 1.06f, .028f, 4.9f, 1.30f},
    {0x20014, "Final Keys: red white show", MoodTheme::Stage, ScaffoldMotion::CrossSweep, {1,.22f,.14f}, 1.400f, 1.17f, .020f, 5.7f, 1.35f},
    {0x20015, "Mutoid Return: neon apocalypse", MoodTheme::Finale, ScaffoldMotion::OrbitSweep, {1,.12f,.64f}, 1.193f, 1.23f, .022f, .9f, 1.35f},
    {0x20016, "Pleasure Dome: spectral finale", MoodTheme::Dome, ScaffoldMotion::Pulse, {.68f,.36f,1}, 0.500f, 1.09f, .012f, 2.6f, 1.35f},
    {0x20017, "Ending screen: final afterglow", MoodTheme::Dome, ScaffoldMotion::CrossSweep, {1,.56f,.18f}, 0.700f, 1.06f, .018f, 4.0f, 1.20f},
};
constexpr RoomLightingProfile kUnknownProfile{0, "Unbound room", MoodTheme::Steel, ScaffoldMotion::CrossSweep,
                                                {.72f,.82f,1}, 1, 1, .015f, 0, 1};

std::vector<Lamp> themeLamps(uint32_t room) {
    const auto [circuit, wave] = key(room);
    std::vector<Lamp> l;
    switch (room_theme(room)) {
    case MoodTheme::Steel: {
        // Studio-floor fluorescent banks with a warm front flood.
        auto bank = omni(.22f, .28f, 105, 270, .72f, .84f, 1, 1.05f, 12, 2);
        l.push_back(wave == 6 ? animate(bank, Anim::Broken, 1, 3) : bank);
        bank.u = .78f; l.push_back(bank);
        l.push_back(omni(.5f, 1.1f, 70, 240, 1, .70f, .42f, .75f, 9, 2));
        if (wave == 1) l.push_back(spot(.5f, .30f, 150, 320, .9f, .95f, 1, .8f, 20, .5f, .55f));
        if (wave == 2 || wave == 9)
            l.push_back(animate(spot(.5f, -.04f, 46, 260, 1, .12f, .06f, 1.8f, 22, .5f, .45f), Anim::Orbit, .045f, 0, .42f, .35f));
        if (wave == 3) l.push_back(animate(omni(-.02f, .5f, 26, 190, .35f, 1, .30f, 1.2f), Anim::Flicker, .11f, 1.7f));
        if (wave == 7 || wave == 11) {
            l.push_back(animate(omni(-.01f, .5f, 30, 160, 1, .6f, .1f, 1.3f), Anim::Pulse, .09f, 0));
            l.push_back(animate(omni(1.01f, .5f, 30, 160, 1, .6f, .1f, 1.3f), Anim::Pulse, .09f, Pi));
        }

        break;
    }
    case MoodTheme::Neon: {
        const bool pinkCircuit = circuit == 1;
        auto strip = animate(omni(.22f, -.02f, 38, 230, 1, .22f, .72f, 1.25f, 10, 2), Anim::Pulse, .021f, 0);
        l.push_back(strip);
        strip.u = .78f; strip.phase = 2.2f; l.push_back(strip);
        l.push_back(omni(.5f, 1.05f, 26, 250, .2f, .85f, 1, 1.0f, 10, 2));
        l.push_back(animate(spot(.5f, .4f, 150, 360, 1, .75f, .95f, 1.3f, 18, .5f, .5f), Anim::Sweep, .012f, wave * .7f, .28f, .14f));
        if (pinkCircuit && (wave & 1))
            l.push_back(animate(omni(.02f, .9f, 30, 170, .62f, .3f, 1, 1.1f), Anim::Hue, .0035f, wave * .21f));
        if (!pinkCircuit || !(wave & 1))
            l.push_back(animate(omni(.98f, .1f, 30, 170, .62f, .3f, 1, 1.1f), Anim::Pulse, .05f, wave * 1.3f));
        break;
    }
    case MoodTheme::Moonlit: {
        l.push_back(omni(.05f, -.35f, 170, 540, .58f, .68f, 1, 1.05f, 18, 2));
        l.push_back(omni(.95f, 1.0f, 40, 230, .62f, .32f, 1, .75f, 8, 1));
        l.push_back(animate(omni(.5f, -.03f, 36, 150, 1, .62f, .25f, .95f), Anim::Flicker, .07f, wave * .9f));
        if (wave & 1) l.push_back(pattern(omni(.5f, .5f, 160, 320, .85f, .9f, 1, .65f, 8, 1), LightPattern::Fan));
        else l.push_back(animate(omni(.08f, .95f, 30, 170, .3f, .9f, 1, .9f), Anim::Broken, 1, wave * 2.f));
        break;
    }
    case MoodTheme::Toxic: {
        l.push_back(animate(omni(.2f, .72f, 10, 170, .40f, 1, .25f, 1.25f), Anim::Pulse, .05f, 0));
        l.push_back(animate(omni(.8f, .28f, 10, 170, .40f, 1, .25f, 1.25f), Anim::Pulse, .05f, 2.1f));
        l.push_back(pattern(animate(omni(.5f, .45f, 120, 320, .85f, .95f, .55f, .85f, 10, 2), Anim::Broken, 1, wave * 1.f),
                            LightPattern::Caustic));
        l.push_back(omni(.5f, 1.04f, 30, 160, 1, .6f, .2f, .75f));
        break;
    }
    case MoodTheme::Stage: {
        // A live broadcast: sweeping follow-spots and border chase lamps.
        const bool gold = circuit == 2;
        l.push_back(animate(spot(.1f, -.3f, 190, 560, 1, gold ? .82f : .97f, gold ? .45f : .9f, 1.9f, 16, .3f, .4f),
                            Anim::Sweep, .017f, wave * .37f, .30f, .28f));
        l.push_back(animate(spot(.9f, -.3f, 190, 560, .45f, .85f, 1, 1.7f, 16, .7f, .6f), Anim::Sweep, .021f,
                            2.1f + wave, .30f, .28f));
        l.push_back(animate(spot(.5f, 1.3f, 190, 560, 1, .4f, .8f, 1.6f, 16, .5f, .5f), Anim::Orbit, .013f,
                            wave * .5f, .35f, .30f));
        const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < 4; ++i)
            l.push_back(animate(omni(corners[i][0], corners[i][1], 18, 120, 1, .15f, .05f, 1.1f), Anim::Pulse, .11f,
                                i * Pi * .5f));
        break;
    }
    case MoodTheme::Jungle: {
        l.push_back(pattern(omni(.3f, -.5f, 230, 640, 1, .95f, .72f, .85f, 22, 2), LightPattern::Canopy));
        const float corners[4][2] = {{.04f, .06f}, {.96f, .06f}, {.04f, .97f}, {.96f, .97f}};
        for (int i = 0; i < 4; ++i)
            l.push_back(animate(omni(corners[i][0], corners[i][1], 34, 180, 1, .52f, .16f, 1.35f, 5, 1), Anim::Flicker,
                                .23f, float(i) * 2.3f + wave));
        break;
    }
    case MoodTheme::Temple: {
        l.push_back(animate(omni(.28f, .02f, 40, 210, 1, .58f, .2f, 1.4f, 6, 2), Anim::Flicker, .21f, wave * 1.f));
        l.push_back(animate(omni(.72f, .02f, 40, 210, 1, .58f, .2f, 1.4f, 6, 2), Anim::Flicker, .19f, wave * 1.f + 4));
        l.push_back(spot(.45f, .2f, 240, 440, 1, .9f, .65f, .85f, 26, .5f, .55f, .4f));
        l.push_back(animate(omni(.5f, 1.06f, 28, 170, 1, .6f, .25f, .6f), Anim::Flicker, .17f, 2));
        break;
    }
    case MoodTheme::Mutoid: {
        l.push_back(animate(spot(.3f, -.15f, 170, 440, .38f, .72f, 1, 1.1f, 20, .45f, .55f), Anim::Sweep, .008f, 0, .1f, .05f));
        l.push_back(animate(spot(.7f, -.15f, 170, 440, .38f, .72f, 1, 1.1f, 20, .55f, .55f), Anim::Sweep, .008f, 1.7f, .1f, .05f));
        l.push_back(animate(spot(.02f, -.02f, 44, 260, 1, .1f, .05f, 1.9f, 24, .3f, .5f), Anim::Orbit, .05f, 0, .3f, .3f));
        l.push_back(animate(spot(.98f, -.02f, 44, 260, 1, .1f, .05f, 1.9f, 24, .7f, .5f), Anim::Orbit, .05f, Pi, .3f, .3f));
        l.push_back(animate(omni(.5f, 1.05f, 18, 210, 1, .45f, .12f, 1.15f, 6, 1), Anim::Flicker, .2f, 0));
        break;
    }
    case MoodTheme::Scarface: {
        l.push_back(animate(omni(.2f, 1.15f, 50, 400, .45f, 1, .4f, 1.4f, 10, 2), Anim::Pulse, .03f, 0));
        l.push_back(omni(.85f, -.15f, 120, 400, .7f, .35f, 1, 1.2f, 10, 2));
        l.push_back(animate(omni(.5f, -.4f, 220, 640, .85f, .92f, 1, 3.0f, 14, 1), Anim::Strobe, 173, 0));
        l.push_back(animate(omni(.05f, .1f, 40, 160, .3f, .9f, 1, .85f), Anim::Broken, 1, 5));
        break;
    }
    case MoodTheme::Cobra: {
        l.push_back(animate(omni(.2f, .02f, 44, 230, 1, .55f, .18f, 1.5f, 6, 2), Anim::Flicker, .21f, 1));
        l.push_back(animate(omni(.8f, .02f, 44, 230, 1, .55f, .18f, 1.5f, 6, 2), Anim::Flicker, .19f, 5));
        l.push_back(animate(omni(.5f, .5f, 80, 320, 1, .15f, .1f, .95f, 8, 1), Anim::Pulse, .04f, 0));
        l.push_back(animate(spot(.5f, .1f, 240, 460, 1, .85f, .5f, 1.2f, 22, .5f, .55f, .4f), Anim::Sweep, .006f, 0, .12f, .05f));
        break;
    }
    case MoodTheme::Finale: {
        l.push_back(animate(spot(.15f, -.3f, 200, 560, 1, .12f, .64f, 1.8f, 13, .35f, .5f), Anim::Sweep, .034f, 0, .32f, .3f));
        l.push_back(animate(spot(.85f, -.3f, 200, 560, .12f, .58f, 1, 1.8f, 13, .65f, .5f), Anim::Sweep, .029f, 2, .32f, .3f));
        l.push_back(spot(.5f, -.2f, 210, 520, 1, .97f, .92f, 1.3f, 22, .5f, .55f));
        l.push_back(animate(omni(.5f, -.5f, 230, 680, .9f, .9f, 1, 2.6f, 14, 1), Anim::Strobe, 97, 31));
        l.push_back(omni(.95f, 1.1f, 40, 260, 1, .3f, .85f, 1.0f, 8, 1));
        break;
    }
    case MoodTheme::Dome: {
        for (int i = 0; i < 4; ++i) {
            const float u = i & 1 ? .85f : .15f, v = i & 2 ? 1.25f : -.25f;
            l.push_back(animate(spot(u, v, 190, 560, 1, 1, 1, .85f, 14, .5f, .5f), Anim::Hue, .004f, i * .25f, .32f, .3f));
        }
        l.push_back(pattern(omni(.5f, .45f, 150, 440, 1, 1, 1, .45f, 6, 1), LightPattern::Sparkle));
        break;
    }
    }
    return l;
}
std::vector<Lamp> scaffoldLamps(uint32_t room) {
    const auto &p = room_lighting_profile(room);
    // Broadcast LED movers: saturated, but cut with white so the floor reads.
    const float r = .8f * p.accent[0] + .2f, g = .8f * p.accent[1] + .2f, b = .8f * p.accent[2] + .2f;
    auto left = spot(.08f, -.34f, 205, 600, r, g, b, .95f * p.rig, 15, .35f, .50f, .38f);
    auto right = spot(.92f, -.34f, 205, 600, r * .65f + .25f, g * .65f + .25f, b * .65f + .25f,
                      .9f * p.rig, 15, .65f, .50f, .38f);
    auto wash = omni(.5f, -.08f, 82, 360, .44f + r * .24f, .44f + g * .24f, .44f + b * .24f,
                     .58f * p.rig, 13, 2);
    // Broadcast moving heads move on deliberate cues (~0.1-0.25 Hz at 60 fps),
    // mirrored left/right so the rig reads as one programmed show.
    const float rate = .010f + p.rate * .22f;
    switch (p.scaffold) {
    case ScaffoldMotion::CrossSweep:
        left = animate(left, Anim::Sweep, rate, p.phase, .30f, .10f);
        right = animate(right, Anim::Sweep, rate, p.phase + Pi, -.30f, .10f);
        break;
    case ScaffoldMotion::OrbitSweep:
        left = animate(left, Anim::Orbit, rate, p.phase, .24f, .18f);
        right = animate(right, Anim::Orbit, rate, p.phase + Pi, .24f, .18f);
        break;
    case ScaffoldMotion::Chase:
        left = animate(left, Anim::Pulse, rate, p.phase);
        right = animate(right, Anim::Pulse, rate, p.phase + Pi);
        wash = animate(wash, Anim::Pulse, rate * .5f, p.phase); // LED chase wash, no dropouts
        break;
    case ScaffoldMotion::Pulse:
        left = animate(left, Anim::Pulse, rate, p.phase);
        right = animate(right, Anim::Pulse, rate, p.phase + Pi);
        wash = animate(wash, Anim::Hue, .003f, p.phase);
        break;
    case ScaffoldMotion::Beacon:
        // Beacon: a searchlight raking the floor instead of a strobe hit.
        left = animate(left, Anim::Sweep, rate * .8f, p.phase, .40f, .22f);
        right = animate(right, Anim::Orbit, rate, p.phase, .36f, .18f);
        wash = animate(wash, Anim::Pulse, rate * .5f, p.phase); // LED chase wash, no dropouts
        break;
    case ScaffoldMotion::Scanline:
        left = animate(left, Anim::Sweep, rate, p.phase, .42f, .04f);
        right = animate(right, Anim::Sweep, rate, p.phase + 1.2f, -.42f, .04f);
        wash = animate(wash, Anim::Pulse, rate * 2, p.phase);
        break;
    }
    return {left, right, wash};
}

// Show spots: per-room coloured follow-spots on top of the scaffold movers. The
// palette follows the set's theme (a primary from the room accent, a partner
// colour from the theme); prize/broadcast sets run a full colour chase, boss
// sets push harder, faster saturated beams. room_exposure folds them into the
// room's tone match, so they add colour and contrast, never overall brightness.
std::array<float, 3> rotateHue(std::array<float, 3> c, float turn) {
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}), d = mx - mn;
    float h = 0;
    if (d > 1e-4f) {
        if (mx == c[0]) h = std::fmod((c[1] - c[2]) / d + 6, 6.f);
        else if (mx == c[1]) h = (c[2] - c[0]) / d + 2;
        else h = (c[0] - c[1]) / d + 4;
    }
    const auto r = hue(h / 6 + turn);
    // hue() keeps a .15 floor; restore the source saturation.
    const float s = mx > 0 ? d / mx : 0;
    return {mx * (1 - s + s * (r[0] - .15f) / .85f), mx * (1 - s + s * (r[1] - .15f) / .85f),
            mx * (1 - s + s * (r[2] - .15f) / .85f)};
}
std::vector<Lamp> showSpots(uint32_t room) {
    const auto &p = room_lighting_profile(room);
    const auto [circuit, wave] = key(room);
    const float seed = hash1(room * 2654435761u);
    const std::array<float, 3> accent = p.accent;
    std::array<float, 3> partner;
    float power = .62f, rate = .011f + p.rate * .18f, cone = 12;
    switch (room_theme(room)) {
    case MoodTheme::Steel: partner = {.55f, .86f, 1}; break;              // cold work-light against the accent
    case MoodTheme::Neon: partner = rotateHue(accent, .42f + seed * .16f); break; // complementary neon
    case MoodTheme::Moonlit: partner = {.62f, .52f, 1}; break;            // icy violet
    case MoodTheme::Toxic: partner = {.70f, .30f, 1}; break;              // acid green against ultraviolet
    case MoodTheme::Jungle: partner = {.45f, 1, .38f}; break;             // leaf green through the canopy
    case MoodTheme::Temple: partner = {1, .28f, .10f}; break;             // gold against ember red
    case MoodTheme::Mutoid: partner = {1, .10f, .06f}; power = .95f; rate *= 1.6f; cone = 10; break;
    case MoodTheme::Scarface: partner = {1, .16f, .60f}; power = .95f; rate *= 1.6f; cone = 10; break;
    case MoodTheme::Cobra: partner = {1, .70f, .12f}; power = .95f; rate *= 1.6f; cone = 10; break;
    case MoodTheme::Finale: partner = {.20f, .55f, 1}; power = 1.05f; rate *= 1.8f; cone = 10; break;
    case MoodTheme::Stage:
    case MoodTheme::Dome: {
        // Prize shows: four hue-chasing beams criss-crossing the floor.
        std::vector<Lamp> l;
        for (int i = 0; i < 4; ++i) {
            const float u = (i & 1) ? .82f : .18f, v = (i & 2) ? 1.25f : -.3f;
            l.push_back(animate(spot(u, v, 195, 560, 1, 1, 1, .62f * p.rig, 11, 1 - u, .5f, .45f), Anim::Hue,
                                .005f + .002f * float(i), seed + float(i) * .25f + float(circuit) * .13f,
                                (i & 1) ? -.34f : .34f, .30f));
        }
        return l;
    }
    }
    // Saturated beams, cut a little with white so the art under them still reads.
    auto mixW = [](const std::array<float, 3> &c, float w) {
        return std::array<float, 3>{c[0] * (1 - w) + w, c[1] * (1 - w) + w, c[2] * (1 - w) + w};
    };
    const auto a = mixW(accent, .12f), b = mixW(partner, .12f);
    const float side = seed < .5f ? 1.f : -1.f; // mirror the cross per room
    auto first = spot(.5f - .32f * side, -.32f, 200, 540, a[0], a[1], a[2], power * p.rig, cone, .5f + .18f * side, .5f, .42f);
    auto second = spot(.5f + .32f * side, -.32f, 200, 540, b[0], b[1], b[2], power * .9f * p.rig, cone, .5f - .18f * side, .55f, .42f);
    const bool orbit = (wave + circuit) & 1;
    first = animate(first, orbit ? Anim::Orbit : Anim::Sweep, rate, p.phase + seed * 6.f, .30f * side, .22f);
    second = animate(second, orbit ? Anim::Sweep : Anim::Orbit, rate * 1.13f, p.phase + 2.4f, -.30f * side, .22f);
    return {first, second};
}

// Hard sci-fi soundstage key rig shared by every arena: an overhead grid of
// cool-white LED softboxes (their reflections read on the polished floor), a
// hard top-back rim light, and a cyan LED line along the rear wall. The theme
// and scaffold rigs layer colour on top of this even, high-key base.
std::vector<Lamp> studioKeyRig(uint32_t room) {
    const auto &p = room_lighting_profile(room);
    // Accent bleeds only faintly into the key; it stays recognisably white.
    const float ar = .92f + p.accent[0] * .06f, ag = .95f + p.accent[1] * .04f, ab = 1.f;
    auto rig = std::vector<Lamp>{
        omni(.27f, .36f, 230, 470, ar, ag, ab, .34f, 30, 2),
        omni(.73f, .36f, 230, 470, ar, ag, ab, .34f, 30, 2),
        omni(.50f, .80f, 210, 430, 1, .97f, .93f, .24f, 28, 2), // warm-neutral audience-side fill
        spot(.50f, -.55f, 260, 560, .80f, .90f, 1, .36f, 40, .5f, .62f, .40f),
        omni(.50f, .00f, 14, 300, .45f + p.accent[0] * .15f, .82f + p.accent[1] * .1f, 1, .42f, 16, 1),
    };
    // The profile's ambient value is the room's calibrated exposure: it scales
    // the shared key rig with the base fill so each set sits at its own level.
    for (auto &lamp : rig) lamp.intensity *= p.ambient;
    return rig;
}

std::vector<Lamp> roomLamps(uint32_t room) {
    const auto &profile = room_lighting_profile(room);
    auto lamps = studioKeyRig(room);
    auto theme = themeLamps(room);
    for (auto &lamp : theme) {
        // Precision hardware: no dying tubes or open flame on this stage. Keep
        // the motion, but as controlled LED behaviour.
        if (lamp.anim == Anim::Broken) lamp.anim = Anim::Steady;
        if (lamp.anim == Anim::Strobe) lamp.intensity *= .62f;
        // Smooth programmed cues: compress anything faster than ~0.3 Hz.
        if ((lamp.anim == Anim::Pulse || lamp.anim == Anim::Sweep || lamp.anim == Anim::Orbit) && lamp.rate > .03f)
            lamp.rate = .03f + (lamp.rate - .03f) * .35f;
    }
    const size_t keyCount = lamps.size();
    lamps.insert(lamps.end(), theme.begin(), theme.end());
    for (size_t i = keyCount; i < lamps.size(); ++i) {
        auto &lamp = lamps[i];
        const float mix = .08f + .025f * float(i % 3);
        lamp.r = lamp.r * (1 - mix) + profile.accent[0] * mix;
        lamp.g = lamp.g * (1 - mix) + profile.accent[1] * mix;
        lamp.b = lamp.b * (1 - mix) + profile.accent[2] * mix;
        lamp.intensity *= profile.intensity;
        lamp.phase += profile.phase;
    }
    auto scaffold = scaffoldLamps(room);
    lamps.insert(lamps.end(), scaffold.begin(), scaffold.end());
    auto spots = showSpots(room);
    lamps.insert(lamps.end(), spots.begin(), spots.end());
    // Leave exposure headroom for the boss rig and simultaneous attack lights.
    if(room==10 || room==0x10007 || room==0x20007 || room==0x20015)
        for(auto& lamp:lamps)lamp.intensity*=.72f;
    return lamps;
}

// Show studio at a circuit start, left of wave one: warm show spots, marquee
// chase and a searchlight raking the crowd. World anchored like any room.
std::vector<Lamp> studioLamps() {
    return {
        animate(spot(-.45f, .15f, 200, 540, 1, .85f, .6f, 1.4f, 24, -.45f, .6f), Anim::Sweep, .015f, 0, .25f, .1f),
        animate(spot(-1.0f, .15f, 200, 540, 1, .85f, .6f, 1.4f, 24, -1.0f, .6f), Anim::Sweep, .015f, 2, .25f, .1f),
        animate(omni(-.72f, -.25f, 40, 280, 1, .3f, .15f, 1.25f, 6, 1), Anim::Pulse, .12f, 0),
        omni(-.6f, 1.2f, 60, 320, .3f, .5f, 1, .95f, 10, 1),
        animate(spot(-.3f, 1.6f, 200, 600, .9f, .95f, 1, 1.3f, 9, -.8f, 1.1f), Anim::Sweep, .02f, 1, .5f, .15f),
    };
}

GpuLight realize(const Lamp &lamp, float fx, float fy, float fw, float fh, float t) {
    const float scale = fw / 480.f;
    GpuLight g;
    g.x = fx + lamp.u * fw;
    g.y = fy + lamp.v * fh;
    g.z = lamp.z;
    // Wall fixtures hang just inside the arena, never inside the wall proxies
    // (rear 44 high, sides 30, front lip 14) that cast their shadows.
    if (g.z < 48 && g.y < fy + 5 && g.y > fy - 40) g.y = fy + 5;
    if (g.z < 34 && g.y >= fy && g.y <= fy + fh) {
        if (g.x < fx + 5 && g.x > fx - 40) g.x = fx + 5;
        if (g.x > fx + fw - 5 && g.x < fx + fw + 40) g.x = fx + fw - 5;
    }
    if (g.z < 18 && g.y > fy + fh - 5 && g.y < fy + fh + 30) g.y = fy + fh - 5;
    g.radius = lamp.radius * std::max(.6f, scale);
    g.r = lamp.r; g.g = lamp.g; g.b = lamp.b;
    float intensity = lamp.intensity;
    const float phase = t * lamp.rate + lamp.phase;
    float aimU = lamp.aimU, aimV = lamp.aimV;
    switch (lamp.anim) {
    case Anim::Steady: break;
    case Anim::Flicker:
        // LED flame-effect practicals: a gentle shimmer, never a guttering torch.
        intensity *= .86f + .14f * (.6f * valueNoise(phase) + .4f * valueNoise(phase * 2.7f + 13.f));
        g.x += (valueNoise(phase * 1.3f + 7) - .5f) * 1.2f;
        g.z += (valueNoise(phase * 1.1f + 3) - .5f) * 1.5f;
        break;
    // LED fixtures breathe; they do not black out between beats.
    case Anim::Pulse: intensity *= .55f + .45f * (.5f + .5f * std::sin(phase)); break;
    case Anim::Strobe: {
        const float period = std::max(8.f, lamp.rate);
        const float local = std::fmod(t + lamp.phase, period);
        intensity *= local < 2 ? 1.f : local < 5 ? .35f : local < 7 ? .8f : 0.f;
        break;
    }
    case Anim::Broken: {
        const uint32_t step = uint32_t(t / 3.f) + uint32_t(lamp.phase * 977);
        const bool drop = hash1(step) < .09f || (hash1(step / 23) < .18f && hash1(step * 7) < .5f);
        intensity *= drop ? .12f : .92f + .08f * hash1(uint32_t(t) * 31u);
        break;
    }
    case Anim::Sweep:
        aimU += lamp.sweepU * std::sin(phase);
        aimV += lamp.sweepV * std::sin(phase * .71f + lamp.phase * 1.3f);
        break;
    case Anim::Orbit:
        aimU += lamp.sweepU * std::cos(phase);
        aimV += lamp.sweepV * std::sin(phase);
        break;
    case Anim::Hue: {
        const auto c = hue(phase);
        g.r = c[0]; g.g = c[1]; g.b = c[2];
        if (lamp.sweepU != 0 || lamp.sweepV != 0) {
            aimU += lamp.sweepU * std::sin(t * .019f + lamp.phase * 6.2f);
            aimV += lamp.sweepV * std::cos(t * .015f + lamp.phase * 4.1f);
        }
        break;
    }
    }
    g.intensity = intensity * LightScale;
    if (lamp.cone > 0) {
        float dx = fx + aimU * fw - g.x, dy = fy + aimV * fh - g.y, dz = -g.z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        g.dx = dx / len; g.dy = dy / len; g.dz = dz / len;
        g.cosOuter = std::cos(lamp.cone * Pi / 180.f);
        g.cosInner = std::cos(lamp.cone * lamp.inner * Pi / 180.f);
    }
    g.softness = lamp.softness * std::max(.6f, scale);
    g.pattern = float(lamp.pattern);
    g.samples = lamp.samples;
    return g;
}
bool reaches(const GpuLight &g, float width, float height) {
    return g.x + g.radius > 0 && g.x - g.radius < width && g.y + g.radius > 0 && g.y - g.radius < height + 40;
}
} // namespace

MoodTheme room_theme(uint32_t room) {
    return room_lighting_profile(room).theme;
}
const char *theme_name(MoodTheme theme) {
    static constexpr const char *names[] = {"steel studio", "neon club", "moonlit hall", "toxic works", "live stage",
                                            "jungle torches", "golden temple", "Mutoid Man", "Scarface", "Cobra",
                                            "final broadcast", "Pleasure Dome"};
    return names[size_t(theme)];
}
RoomMood room_mood(uint32_t room) {
    RoomMood m;
    m.theme = room_theme(room);
    switch (m.theme) {
    // Studio grade: every set sits under the same cool-neutral (~6000 K) LED
    // key; the theme only tints it. Polished resin floors carry the softbox
    // reflections, and the fill keeps contact shadows crisp but never murky.
    case MoodTheme::Steel: m.ambient = {.70f, .74f, .82f}; m.gloss = .74f; m.actorFloor = .88f; m.occlusion = .40f; break;
    case MoodTheme::Neon: m.ambient = {.68f, .66f, .78f}; m.gloss = .82f; m.actorFloor = .86f; m.occlusion = .42f; break;
    case MoodTheme::Moonlit: m.ambient = {.62f, .68f, .80f}; m.gloss = .76f; m.actorFloor = .86f; m.occlusion = .42f; break;
    case MoodTheme::Toxic: m.ambient = {.62f, .72f, .68f}; m.gloss = .66f; m.actorFloor = .86f; m.occlusion = .42f; break;
    case MoodTheme::Stage: m.ambient = {.74f, .75f, .82f}; m.gloss = .86f; m.actorFloor = .88f; m.occlusion = .40f; break;
    case MoodTheme::Jungle: m.ambient = {.72f, .72f, .70f}; m.gloss = .42f; m.actorFloor = .88f; m.occlusion = .40f; break;
    case MoodTheme::Temple: m.ambient = {.74f, .72f, .68f}; m.gloss = .56f; m.actorFloor = .88f; m.occlusion = .40f; break;
    case MoodTheme::Mutoid: m.ambient = {.62f, .66f, .76f}; m.gloss = .82f; m.actorFloor = .84f; m.occlusion = .48f; break;
    case MoodTheme::Scarface: m.ambient = {.60f, .70f, .68f}; m.gloss = .80f; m.actorFloor = .84f; m.occlusion = .48f; break;
    case MoodTheme::Cobra: m.ambient = {.70f, .66f, .62f}; m.gloss = .64f; m.actorFloor = .84f; m.occlusion = .46f; break;
    case MoodTheme::Finale: m.ambient = {.62f, .60f, .74f}; m.gloss = .84f; m.actorFloor = .84f; m.occlusion = .48f; break;
    case MoodTheme::Dome: m.ambient = {.70f, .68f, .78f}; m.gloss = .72f; m.actorFloor = .90f; m.occlusion = .36f; break;
    }
    const auto &profile = room_lighting_profile(room);
    for (auto &a : m.ambient) a *= AmbientScale * profile.ambient;
    return m;
}
const RoomLightingProfile &room_lighting_profile(uint32_t room) {
    for (const auto &profile : kRoomProfiles) if (profile.room == room) return profile;
    return kUnknownProfile;
}
size_t room_lighting_profile_count() { return std::size(kRoomProfiles); }

bool is_boss_target(uint32_t room, uint16_t oid) {
    const unsigned type = (oid >> 8) & 31;
    if (room == 0x0000a || room == 0x20015) return type == 24;
    if (room == 0x10007) return type == 25;
    if (room == 0x20007) return type == 10;
    return false;
}

void add_boss_lights(LightingFrame &frame, const EffectPacket &p, int offset) {
    float left = 100000, top = 100000, right = -100000, bottom = -100000;
    unsigned count = 0;
    for (const auto &target : p.foreground) {
        if (!is_boss_target(p.room, target.oid)) continue;
        left = std::min(left, target.x); top = std::min(top, target.y);
        right = std::max(right, target.x + target.w); bottom = std::max(bottom, target.y + target.h);
        ++count;
    }
    if (!count || frame.dynamicLights + 4 > RoomLightRig::MaxDynamic) return;
    const auto &profile = room_lighting_profile(p.room);
    const float cx = (left + right) * .5f, cy = (top + bottom) * .5f;
    const float phase = float(p.frame) * profile.rate;
    std::array<float, 3> keyTint{1, .95f, .88f}, rimTint=profile.accent;
    std::array<float, 3> underTint=profile.accent, edgeTint{.30f, .55f, 1};
    if (p.room == 0x10007) {
        keyTint = {1, 1, .94f}; rimTint = {.32f, 1, .42f};
        underTint = {.12f, .92f, .38f}; edgeTint = {.62f, .36f, 1};
    } else if (p.room == 0x20007) {
        keyTint = {1, .86f, .62f}; rimTint = {1, .16f, .08f};
        underTint = {1, .46f, .12f}; edgeTint = {.46f, .92f, 1};
    } else if (p.room == 0x20015) {
        keyTint = {1, .78f, .94f}; rimTint = {1, .12f, .64f};
        underTint = {.48f, .18f, 1}; edgeTint = {.12f, .78f, 1};
    } else {
        keyTint = {.90f, .94f, 1}; rimTint = {1, .18f, .12f};
        underTint = {.16f, .62f, 1}; edgeTint = {.38f, .58f, 1};
    }
    auto spot = [&](float x, float y, float z, float radius, float intensity,
                    const std::array<float, 3> &color, float cone, float softness) {
        GpuLight light;
        light.x = x + float(offset); light.y = y; light.z = z; light.radius = radius;
        light.r = color[0]; light.g = color[1]; light.b = color[2]; light.intensity = intensity;
        const float dx = cx + float(offset) - light.x, dy = cy - light.y, dz = -light.z;
        const float len = std::max(.001f, std::sqrt(dx * dx + dy * dy + dz * dz));
        light.dx = dx / len; light.dy = dy / len; light.dz = dz / len;
        light.cosOuter = std::cos(cone * Pi / 180);
        light.cosInner = std::cos(cone * .42f * Pi / 180);
        light.softness = softness; light.samples = 2;
        frame.lights.push_back(light); ++frame.dynamicLights;
    };
    auto point = [&](float x, float y, float z, float radius, float intensity,
                     const std::array<float, 3> &color, float softness) {
        GpuLight light;
        light.x = x + float(offset); light.y = y; light.z = z; light.radius = radius;
        light.r = color[0]; light.g = color[1]; light.b = color[2]; light.intensity = intensity;
        light.softness = softness; light.samples = 1;
        frame.lights.push_back(light); ++frame.dynamicLights;
    };
    const float span = std::max(24.f, right - left);
    const unsigned quality = std::min(p.cinema.quality, RoomCinemaQualityHigh);
    const float cue = .58f * (quality ? (.70f + .12f * float(quality)) *
        (.72f + .20f * p.cinema.bossPhase +
         .25f * std::max(p.cinema.bossIntro, p.cinema.deathPower)) : 1.f);
    spot(cx + std::sin(phase) * span * .24f, top - 36, 145, 350,
         (2.4f + .35f * std::sin(phase * .7f)) * cue, keyTint, 22, 11);
    point(cx - std::sin(phase + Pi) * span * .34f, bottom + 4, 30, 225,
          (1.55f + .22f * std::sin(phase * 1.7f + profile.phase)) * cue, underTint, 8);
    spot(cx + std::cos(phase * .8f) * span * .48f, top - 4, 95, 285,
         (1.25f + .28f * std::sin(phase * 1.3f + 2)) * cue, rimTint, 31, 9);
    point(cx - std::cos(phase * .9f + .7f) * span * .52f, cy, 54, 190,
          (.92f + .34f * std::sin(phase * 2.1f + 4)) * cue, edgeTint, 6);
}

namespace {
// Unshadowed diffuse irradiance of one light (dxr_lighting.hlsl shadeLight
// without visibility, gobo or specular).
std::array<float, 3> lampIrradiance(const GpuLight &L, const float *P, const float *N) {
    float dx = L.x - P[0], dy = L.y - P[1], dz = L.z - P[2];
    const float dist = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), .25f);
    dx /= dist; dy /= dist; dz /= dist;
    const float x = dist / std::max(L.radius, 1e-3f);
    if (x >= 1) return {0, 0, 0};
    const float x2 = x * x, win = std::clamp(1 - x2 * x2, 0.f, 1.f);
    float att = L.intensity * win * win / (1 + x2 * 5);
    if (L.cosOuter > -1) {
        const float c = -(dx * L.dx + dy * L.dy + dz * L.dz);
        const float t = std::clamp((c - L.cosOuter) / std::max(L.cosInner - L.cosOuter, 1e-4f), 0.f, 1.f);
        att *= t * t * (3 - 2 * t);
    }
    const float wrap = std::clamp((N[0] * dx + N[1] * dy + N[2] * dz + .12f) / 1.12f, 0.f, 1.f);
    return {L.r * att * wrap, L.g * att * wrap, L.b * att * wrap};
}
float luma3(const std::array<float, 3> &c) { return c[0] * .2126f + c[1] * .7152f + c[2] * .0722f; }
} // namespace

RoomExposure room_exposure(uint32_t room, float fw, float fh) {
    if (!(fw > 0) || !(fh > 0)) return {};
    static std::mutex lock;
    static std::map<std::array<uint32_t, 3>, RoomExposure> cache;
    const std::array<uint32_t, 3> key{room, uint32_t(fw * 4), uint32_t(fh * 4)};
    {
        std::lock_guard guard(lock);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    // Average the lamp animation over ~25 s of show time.
    constexpr int Times = 16;
    std::vector<std::vector<GpuLight>> rigs;
    for (int k = 0; k < Times; ++k) {
        std::vector<GpuLight> rig;
        for (const auto &lamp : roomLamps(room)) {
            if (rig.size() >= RoomLightRig::MaxRoomLights - RoomLightRig::MaxDynamic) break;
            auto g = realize(lamp, 0, 0, fw, fh, float(k) * 97.f);
            if (g.intensity > .002f) rig.push_back(g);
        }
        rigs.push_back(std::move(rig));
    }
    auto mean = [&](auto &&point, int nu, int nv) {
        std::array<float, 3> sum{};
        for (int v = 0; v < nv; ++v)
            for (int u = 0; u < nu; ++u) {
                float P[3], N[3];
                point((float(u) + .5f) / float(nu), (float(v) + .5f) / float(nv), P, N);
                for (const auto &rig : rigs)
                    for (const auto &L : rig) {
                        const auto e = lampIrradiance(L, P, N);
                        for (int c = 0; c < 3; ++c) sum[c] += e[c];
                    }
            }
        for (auto &c : sum) c /= float(nu * nv * Times);
        return sum;
    };
    const auto floorLight = mean([&](float u, float v, float *P, float *N) {
        P[0] = u * fw; P[1] = v * fh; P[2] = 0; N[0] = 0; N[1] = 0; N[2] = 1;
    }, 16, 10);
    const auto wallLight = mean([&](float u, float v, float *P, float *N) {
        P[0] = -30 + u * (fw + 60); P[1] = .6f; P[2] = v * 44; N[0] = 0; N[1] = 1; N[2] = 0;
    }, 16, 4);
    const auto marginLight = mean([&](float u, float v, float *P, float *N) {
        // Side strips (two thirds of the samples) and the front lip.
        N[0] = 0; N[1] = 0; N[2] = 1;
        if (v < .66f) {
            P[0] = u < .5f ? -30 + u * 60 : fw + (u - .5f) * 60; P[1] = v / .66f * fh; P[2] = 0;
        } else {
            P[0] = -30 + u * (fw + 60); P[1] = fh + (v - .66f) / .34f * 15 + 14; P[2] = 14.4f;
        }
    }, 16, 6);
    const auto ambient = room_mood(room).ambient;
    auto total = [&](const std::array<float, 3> &light, float ao, float lit) {
        return std::array<float, 3>{ambient[0] * ao + light[0] * lit, ambient[1] * ao + light[1] * lit,
                                    ambient[2] * ao + light[2] * lit};
    };
    const auto floorE = total(floorLight, ExposureFloorAo, ExposureFloorLit);
    const auto wallE = total(wallLight, ExposureWallAo, 1);
    const auto marginE = total(marginLight, ExposureMarginAo, 1);
    RoomExposure out;
    const float y = luma3(floorE);
    for (int c = 0; c < 3; ++c) out.balance[c] = std::pow(y / std::max(floorE[c], 1e-3f), ExposureNeutralize);
    auto balanced = [&](const std::array<float, 3> &e) {
        return luma3({e[0] * out.balance[0], e[1] * out.balance[1], e[2] * out.balance[2]});
    };
    out.scale[0] = std::clamp(ExposureFloorTarget / std::max(balanced(floorE), 1e-3f), .2f, 2.f);
    out.scale[1] = std::clamp(ExposureWallTarget / std::max(balanced(wallE), 1e-3f), .2f, 3.f);
    out.scale[2] = std::clamp(ExposureMarginTarget / std::max(balanced(marginE), 1e-3f), .2f, 3.f);
    // Boss and prize sets keep more of their beams' colour (their spots are also
    // faster and more numerous); the exposure itself stays matched to the art.
    const auto theme = room_theme(room);
    const bool show = theme == MoodTheme::Stage || theme == MoodTheme::Dome;
    const bool boss = theme == MoodTheme::Mutoid || theme == MoodTheme::Scarface || theme == MoodTheme::Cobra ||
                      theme == MoodTheme::Finale;
    out.scale[3] = std::min(1.f, ExposureChroma + (show || boss ? .12f : 0.f));
    // Measured per-theme residual (all-effects 2P route captures, 28 Nov 2026):
    // the analytic rig omits gobo averages, specular and haze, which differ by
    // set dressing. Floor, wall, margin multipliers in MoodTheme order.
    static constexpr float themeFix[12][3] = {
        {1.02f, 1.09f, 1.04f}, // Steel
        {.99f, .99f, 1.04f},   // Neon
        {.94f, .96f, .99f},    // Moonlit
        {.70f, 1.09f, 1.f},    // Toxic
        {.58f, .87f, .78f},    // Stage (prize / broadcast)
        {1.13f, 1.05f, 1.07f}, // Jungle
        {1.12f, 1.02f, .97f},  // Temple
        {.98f, .93f, .98f},    // Mutoid
        {.97f, 1.08f, 1.03f},  // Scarface
        {1.15f, 1.07f, 1.04f}, // Cobra
        {1.02f, .92f, .82f},   // Finale
        {.86f, 1.01f, .81f},   // Dome
    };
    for (int k = 0; k < 3; ++k) out.scale[k] = std::clamp(out.scale[k] * themeFix[size_t(theme)][k], .2f, 3.f);
    std::lock_guard guard(lock);
    cache[key] = out;
    return out;
}

LightingFrame RoomLightRig::build(const EffectPacket &p, int width, int offset) {
    LightingFrame frame;
    frame.lights.reserve(MaxLights);
    // The particle system requests a reset on every intro frame.
    if (!p.in_arena || (current_.anchor.valid && p.frame < current_.frame)) reset();
    const auto f = p.geometry.floor;
    frame.mood = room_mood(p.room);
    if (f.w <= 0 || f.h <= 0) return frame;
    const unsigned cinemaQuality = std::min(p.cinema.quality, RoomCinemaQualityHigh);
    const float cinemaScale = float(cinemaQuality) / float(RoomCinemaQualityHigh);
    if (cinemaQuality && p.studio.boss) {
        const auto tint = room_cinema_tint(p.room);
        const float strength = cinemaScale * std::clamp(
            .30f + p.cinema.bossPhase * .55f + p.cinema.deathPower * .15f, 0.f, 1.f);
        for (size_t i = 0; i < frame.mood.ambient.size(); ++i)
            frame.mood.ambient[i] *= 1.f + (tint[i] - 1.f) * strength;
    }
    if (cinemaQuality) {
        const float ambience = 1.f + p.cinema.audioPulse * (.025f + .035f * cinemaScale);
        for (auto &a : frame.mood.ambient) a *= ambience;
        frame.mood.gloss *= 1.f + p.cinema.audioPulse * .025f * cinemaScale;
    }
    // World anchor: WAVES.SET_WAVE origin minus camera, plus the camera.
    float wx = f.x + float(p.camera_x), wy = f.y + float(p.camera_y);
    // The circuit-start studio holds before WAVEYX is written (origin 0, floor at
    // its room margins). The pan that follows places wave one 605 pixels right of
    // the held camera, so anchor there and the rig is continuous through the pan.
    // Circuits 2/3 hold with the previous circuit's stale origin instead (lesson 54691).
    if (p.roomIntro && (p.room & 0xffff) == 1) {
        const float left = f.w > 400 ? 33.f : 29.f;
        if (wy == 46.f && wx < 40.f) {
            wx += float(p.camera_x) + 605.f;
            wy += float(p.camera_y);
        } else if (p.camera_x == -137 && f.w > 400) {
            wx = float(p.camera_x) + 605.f + left;
            wy = float(p.camera_y) + 46.f;
        }
    }
    const auto &anchor = current_.anchor;
    const bool moved = std::abs(anchor.x - wx) > 8 || std::abs(anchor.y - wy) > 8;
    const bool pendingOrigin = anchor.valid && anchor.room != p.room && !moved &&
                               (p.roomIntro || p.frame > current_.frame);
    if (anchor.valid && moved && (anchor.room != p.room || p.roomIntro))
        previous_ = current_;
    else if (!pendingOrigin && anchor.valid && anchor.room != p.room)
        previous_ = {};

    auto appendSnapshot = [&](const Snapshot &snapshot) {
        const float dx = snapshot.cameraX - float(p.camera_x) + float(offset);
        const float dy = snapshot.cameraY - float(p.camera_y);
        for (size_t i = 0; i < snapshot.count; ++i) {
            auto lamp = snapshot.lights[i];
            lamp.x += dx;
            lamp.y += dy;
            frame.lights.push_back(lamp);
        }
    };
    auto screenLook = [&](const Snapshot &snapshot) {
        auto look = snapshot.look;
        const float dx = snapshot.cameraX - float(p.camera_x) + float(offset);
        const float dy = snapshot.cameraY - float(p.camera_y);
        look.cinema.bossX += dx;
        look.cinema.bossY += dy;
        look.cinema.deathX += dx;
        look.cinema.deathY += dy;
        return look;
    };
    // The selector can publish before WAVEYX. Keep the old rig through that
    // interval, then start the new rig once it has its own world position.
    if (pendingOrigin) {
        frame.floor = {anchor.x - float(p.camera_x) + float(offset),
                       anchor.y - float(p.camera_y), anchor.w, anchor.h};
        frame.mood = current_.mood;
        frame.exposure = room_exposure(current_.anchor.room, current_.anchor.w, current_.anchor.h);
        frame.gates = current_.gates;
        frame.leftGate = current_.leftGate;
        frame.lightTime = float(current_.frame % 1000003);
        frame.look = screenLook(current_);
        frame.holdingOrigin = true;
        appendSnapshot(current_);
        frame.staticLights = unsigned(frame.lights.size());
        return frame;
    }
    current_.anchor = {p.room, wx, wy, f.w, f.h, true};
    frame.floor = {wx - float(p.camera_x) + float(offset), wy - float(p.camera_y), f.w, f.h};
    frame.exposure = room_exposure(p.room, f.w, f.h);
    const float t = float(p.frame);
    const float W = float(width), H = 256.f;
    auto emit = [&](const std::vector<Lamp> &lamps, const Anchor &a, unsigned &counter) {
        const float sx = a.x - float(p.camera_x) + float(offset), sy = a.y - float(p.camera_y);
        for (const auto &lamp : lamps) {
            if (frame.lights.size() >= MaxRoomLights - MaxDynamic) return;
            auto g = realize(lamp, sx, sy, a.w, a.h, t);
            if (g.intensity <= .002f) continue;
            frame.lights.push_back(g);
            ++counter;
        }
    };
    emit(roomLamps(p.room), current_.anchor, frame.staticLights);
    if ((p.room & 0xffff) == 1) {
        auto studio = studioLamps();
        const auto &profile = room_lighting_profile(p.room);
        for (auto &lamp : studio) {
            lamp.r = lamp.r * .8f + profile.accent[0] * .2f;
            lamp.g = lamp.g * .8f + profile.accent[1] * .2f;
            lamp.b = lamp.b * .8f + profile.accent[2] * .2f;
            lamp.intensity *= profile.intensity;
            lamp.phase += profile.phase;
        }
        emit(studio, current_.anchor, frame.studioLights);
    }
    add_boss_lights(frame, p, offset);
    // Explosions outrank boss-room shot spill; retain slots for cinema cues.
    std::array<RoomLamp, MaxGameplayLights> dynamic{};
    const auto dynamicCount = std::min(p.dxrLights.size(), dynamic.size());
    std::copy_n(p.dxrLights.begin(), dynamicCount, dynamic.begin());
    std::stable_sort(dynamic.begin(), dynamic.begin() + dynamicCount,
                     [](const RoomLamp &a, const RoomLamp &b) { return a.power * a.radius > b.power * b.radius; });
    const bool bossCinemaLight = cinemaQuality >= RoomCinemaQualityMedium &&
        (p.cinema.bossIntro > .01f || p.cinema.deathPower > .01f);
    const unsigned audioCinemaLights = cinemaQuality >= RoomCinemaQualityMedium &&
        p.cinema.audioPulse > .015f ? (cinemaQuality == RoomCinemaQualityHigh ? 2u : 1u) : 0u;
    const unsigned reservedCinemaLights = std::min<unsigned>(
        MaxDynamic, (bossCinemaLight ? 1u : 0u) + audioCinemaLights);
    const unsigned gameplayLightLimit = MaxDynamic - reservedCinemaLights;
    for (size_t i = 0; i < dynamicCount; ++i) {
        const auto &d = dynamic[i];
        if (frame.dynamicLights >= gameplayLightLimit || frame.lights.size() >= MaxRoomLights) break;
        if (!(d.power > 0) || !(d.radius > 0)) continue;
        GpuLight g;
        g.x = d.x + float(offset); g.y = d.y; g.z = d.height; g.radius = d.radius;
        g.r = d.r; g.g = d.g; g.b = d.b; g.intensity = d.power;
        // A small area source: the adaptive disk sampler keeps this at two
        // rays outside penumbrae, and explosions get soft-edged shadows.
        g.softness = 4; g.samples = 2;
        // Like wall fixtures, a light inside a wall proxy (rear 44 high, sides
        // 30, front lip 14) would have every shadow ray blocked: keep it in the arena.
        {
            const float fx = frame.floor[0], fy = frame.floor[1], fw = frame.floor[2], fh = frame.floor[3];
            if (fw > 0 && fh > 0) {
                if (g.z < 48 && g.y < fy + 5 && g.y > fy - 40) g.y = fy + 5;
                if (g.z < 34 && g.y >= fy && g.y <= fy + fh) {
                    if (g.x < fx + 5 && g.x > fx - 40) g.x = fx + 5;
                    if (g.x > fx + fw - 5 && g.x < fx + fw + 40) g.x = fx + fw - 5;
                }
                if (g.z < 18 && g.y > fy + fh - 5 && g.y < fy + fh + 30) g.y = fy + fh - 5;
            }
        }
        if (!reaches(g, W, H)) continue;
        frame.lights.push_back(g);
        ++frame.dynamicLights;
    }
    auto emitCinema = [&](float x, float y, float z, float radius, float r, float g, float b, float power) {
        if (frame.dynamicLights >= MaxDynamic || frame.lights.size() >= MaxRoomLights ||
            !(power > 0) || !(radius > 0)) return;
        GpuLight light;
        light.x = x + float(offset); light.y = y; light.z = z; light.radius = radius;
        light.r = r; light.g = g; light.b = b; light.intensity = power;
        light.softness = 5.f + 2.f * cinemaScale;
        light.samples = cinemaQuality == RoomCinemaQualityHigh ? 2.f : 1.f;
        if (!reaches(light, W, H)) return;
        frame.lights.push_back(light);
        ++frame.dynamicLights;
    };
    if (bossCinemaLight) {
        const float death = p.cinema.deathPower;
        const float power = (p.cinema.bossIntro * .65f + p.cinema.bossPower * .9f +
                             p.cinema.bossPhase * .08f + death * .95f) * cinemaScale;
        const auto tint = room_cinema_tint(p.room);
        emitCinema(p.cinema.bossX ? p.cinema.bossX : frame.floor[0] + frame.floor[2] * .5f,
                   p.cinema.bossY ? p.cinema.bossY : frame.floor[1] + 40.f,
                   72.f + death * 40.f, 230.f + death * 150.f, tint[0], tint[1], tint[2], power);
    }
    if (audioCinemaLights) {
        const auto tint = room_cinema_tint(p.studio.boss ? p.room : 0);
        const float power = p.cinema.audioPulse * (.35f + .12f * cinemaScale);
        emitCinema(frame.floor[0] + frame.floor[2] * .18f, frame.floor[1] + 10.f,
                   42.f, 185.f, tint[0], tint[1], tint[2], power);
        if (audioCinemaLights == 2)
            emitCinema(frame.floor[0] + frame.floor[2] * .82f, frame.floor[1] + 10.f,
                       42.f, 185.f, tint[2], tint[1], tint[0], power * .9f);
    }
    current_.mood = frame.mood;
    current_.gates = frame.gates = p.geometry.openGates;
    current_.leftGate = frame.leftGate = p.roomIntro ?
        std::clamp((W - frame.floor[0]) / f.w, 0.f, 1.f) : float(frame.gates & 1);
    current_.frame = p.frame;
    frame.lightTime = float(p.frame % 1000003);
    current_.look = {p.room, p.cinema};
    if (!p.cinema.bossX) current_.look.cinema.bossX = frame.floor[0] - float(offset) + f.w * .5f;
    if (!p.cinema.bossY) current_.look.cinema.bossY = frame.floor[1] + f.h * .35f;
    frame.look = screenLook(current_);
    current_.cameraX = float(p.camera_x);
    current_.cameraY = float(p.camera_y);
    current_.count = frame.lights.size();
    for (size_t i = 0; i < current_.count; ++i) {
        current_.lights[i] = frame.lights[i];
        current_.lights[i].x -= float(offset);
    }
    const auto &old = previous_.anchor;
    if (old.valid) {
        const float sx = old.x - float(p.camera_x) + float(offset);
        const float sy = old.y - float(p.camera_y);
        if (sx + old.w + 30 > 0 && sx - 30 < W && sy + old.h + 15 > 0 && sy - 44 < H) {
            frame.previousFloor = {sx, sy, old.w, old.h};
            frame.previousMood = previous_.mood;
            frame.previousExposure = room_exposure(old.room, old.w, old.h);
            frame.previousGates = previous_.gates;
            frame.previousLeftGate = previous_.leftGate;
            frame.previousLightTime = float(previous_.frame % 1000003);
            frame.previousLook = screenLook(previous_);
            frame.previousVisible = true;
            appendSnapshot(previous_);
            frame.previousRoomLights = unsigned(previous_.count);
        } else {
            previous_ = {};
        }
    }
    return frame;
}
} // namespace stv::host
