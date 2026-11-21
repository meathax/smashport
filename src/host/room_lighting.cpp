#include "stv/host/room_lighting.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace stv::host {
namespace {
constexpr float Pi = 3.14159265f;
// Global grade: darkness is the canvas; authored fixtures remain readable.
constexpr float AmbientScale = .68f, LightScale = 1.72f;
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
    {0x00001, "Arena One: gantry opener", MoodTheme::Steel, ScaffoldMotion::CrossSweep, {.62f,.80f,1}, .98f, 1.02f, .012f, .0f, 1.0f},
    {0x00002, "Ten Keys: crimson alarm", MoodTheme::Steel, ScaffoldMotion::OrbitSweep, {1,.22f,.12f}, .94f, 1.08f, .019f, .7f, .95f},
    {0x00003, "Power Vault: toxic spill", MoodTheme::Steel, ScaffoldMotion::Pulse, {.38f,1,.35f}, .92f, 1.13f, .034f, 1.6f, 1.05f},
    {0x00004, "Shrapnel Works: magenta cut", MoodTheme::Neon, ScaffoldMotion::Chase, {1,.26f,.70f}, .90f, 1.12f, .027f, 2.4f, 1.0f},
    {0x00005, "Bonus Prizes: live broadcast", MoodTheme::Stage, ScaffoldMotion::CrossSweep, {1,.78f,.42f}, 1.0f, 1.0f, .016f, .5f, 1.2f},
    {0x00006, "Shrapnel Hall: broken strip", MoodTheme::Steel, ScaffoldMotion::Beacon, {1,.55f,.18f}, .90f, 1.18f, .041f, 4.1f, .85f},
    {0x00007, "Total Carnage: orange rolling bank", MoodTheme::Steel, ScaffoldMotion::Scanline, {1,.42f,.10f}, .88f, 1.20f, .052f, 1.1f, 1.1f},
    {0x00008, "Crowd Control: cyan crossfire", MoodTheme::Neon, ScaffoldMotion::CrossSweep, {.20f,.88f,1}, .90f, 1.10f, .014f, 3.0f, 1.05f},
    {0x00009, "Tank Trouble: red chase", MoodTheme::Neon, ScaffoldMotion::Chase, {1,.18f,.10f}, .86f, 1.25f, .031f, 5.2f, 1.1f},
    {0x0000a, "Mutoid One: dissection white", MoodTheme::Mutoid, ScaffoldMotion::CrossSweep, {.88f,.92f,1}, .82f, 1.28f, .011f, .2f, 1.25f},
    {0x0000b, "First Rack: silver finale", MoodTheme::Stage, ScaffoldMotion::Pulse, {.75f,.86f,1}, .98f, 1.0f, .020f, 2.2f, 1.25f},

    {0x10001, "Orbs: violet containment", MoodTheme::Neon, ScaffoldMotion::OrbitSweep, {.64f,.36f,1}, .92f, 1.12f, .018f, .4f, 1.1f},
    {0x10002, "Ten Keys Two: cyan lift", MoodTheme::Neon, ScaffoldMotion::CrossSweep, {.22f,.92f,1}, .90f, 1.10f, .015f, 1.4f, 1.05f},
    {0x10003, "Smash 'Em: pink pursuit", MoodTheme::Moonlit, ScaffoldMotion::Scanline, {1,.31f,.72f}, .88f, 1.16f, .045f, 2.5f, 1.0f},
    {0x10004, "Fire Power: toxic valves", MoodTheme::Toxic, ScaffoldMotion::Pulse, {.42f,1,.20f}, .84f, 1.20f, .037f, 3.7f, 1.15f},
    {0x10005, "Slaughter Line: magenta rails", MoodTheme::Neon, ScaffoldMotion::Chase, {1,.18f,.62f}, .88f, 1.18f, .024f, 4.8f, 1.0f},
    {0x10006, "Laser Death Zone: blue slicer", MoodTheme::Neon, ScaffoldMotion::Scanline, {.18f,.68f,1}, .82f, 1.26f, .061f, 5.1f, 1.1f},
    {0x10007, "Scarface: monitor storm", MoodTheme::Scarface, ScaffoldMotion::OrbitSweep, {.42f,1,.46f}, .78f, 1.32f, .026f, .8f, 1.3f},
    {0x10008, "Rowdy Droids: moon search", MoodTheme::Moonlit, ScaffoldMotion::CrossSweep, {.52f,.70f,1}, .90f, 1.08f, .013f, 1.9f, .95f},
    {0x10009, "Vacuum Clean: white hazard", MoodTheme::Moonlit, ScaffoldMotion::Beacon, {1,.72f,.46f}, .86f, 1.20f, .043f, 3.4f, 1.15f},
    {0x1000a, "Reserved metal selector", MoodTheme::Moonlit, ScaffoldMotion::Pulse, {.52f,.72f,1}, .92f, 1.0f, .029f, .0f, .9f},
    {0x1000b, "Metal Death: caustic bay", MoodTheme::Toxic, ScaffoldMotion::OrbitSweep, {.30f,1,.54f}, .84f, 1.22f, .020f, 2.1f, 1.15f},
    {0x1000c, "Watch Your Step: pit show", MoodTheme::Stage, ScaffoldMotion::CrossSweep, {1,.80f,.36f}, .94f, 1.05f, .017f, 4.4f, 1.2f},
    {0x1000d, "Total Carnage Two: cold violet", MoodTheme::Moonlit, ScaffoldMotion::Scanline, {.66f,.46f,1}, .86f, 1.17f, .047f, 1.7f, 1.0f},
    {0x1000e, "Defend Me: cyan apron", MoodTheme::Stage, ScaffoldMotion::Chase, {.26f,.90f,1}, .94f, 1.05f, .022f, 5.9f, 1.25f},
    {0x1000f, "Turtles: amber convoy", MoodTheme::Moonlit, ScaffoldMotion::OrbitSweep, {1,.63f,.24f}, .90f, 1.10f, .016f, 2.7f, 1.0f},
    {0x10010, "Chunks: magenta machinery", MoodTheme::Neon, ScaffoldMotion::Pulse, {1,.24f,.66f}, .84f, 1.20f, .035f, 4.0f, 1.15f},
    {0x10011, "These Are Fast: cobalt strobe", MoodTheme::Neon, ScaffoldMotion::Scanline, {.16f,.54f,1}, .80f, 1.28f, .068f, 1.2f, 1.1f},
    {0x10012, "Buffalo Herd: polar sweep", MoodTheme::Moonlit, ScaffoldMotion::CrossSweep, {.58f,.84f,1}, .90f, 1.10f, .015f, 3.3f, 1.0f},
    {0x10013, "Second Rack: blue-gold finale", MoodTheme::Stage, ScaffoldMotion::Pulse, {.56f,.76f,1}, .96f, 1.02f, .024f, 4.9f, 1.25f},

    {0x20001, "No Dice: jungle breach", MoodTheme::Jungle, ScaffoldMotion::OrbitSweep, {1,.60f,.18f}, .88f, 1.12f, .014f, .6f, 1.05f},
    {0x20002, "Temple Alert: braziers", MoodTheme::Temple, ScaffoldMotion::CrossSweep, {1,.56f,.16f}, .86f, 1.16f, .012f, 1.8f, 1.2f},
    {0x20003, "Scorpion Fever: amber pincer", MoodTheme::Jungle, ScaffoldMotion::Scanline, {1,.38f,.08f}, .84f, 1.20f, .048f, 3.1f, 1.05f},
    {0x20004, "Cobra Ahead: acid warning", MoodTheme::Jungle, ScaffoldMotion::Pulse, {.52f,1,.24f}, .82f, 1.22f, .034f, 4.2f, 1.15f},
    {0x20005, "Walls of Pain: gold teeth", MoodTheme::Temple, ScaffoldMotion::Chase, {1,.52f,.14f}, .88f, 1.14f, .029f, 5.4f, 1.15f},
    {0x20006, "Last Arena: furnace sun", MoodTheme::Temple, ScaffoldMotion::CrossSweep, {1,.72f,.30f}, .82f, 1.22f, .016f, 1.1f, 1.2f},
    {0x20007, "Cobra Death: twin crimson moons", MoodTheme::Cobra, ScaffoldMotion::OrbitSweep, {1,.20f,.08f}, .76f, 1.38f, .018f, .4f, 1.3f},
    {0x20008, "Turtle Guard: cyan canopy", MoodTheme::Jungle, ScaffoldMotion::Beacon, {.18f,.88f,1}, .86f, 1.16f, .036f, 2.0f, 1.05f},
    {0x20009, "Extra Sauce: hot magenta", MoodTheme::Jungle, ScaffoldMotion::Scanline, {1,.26f,.50f}, .82f, 1.24f, .056f, 3.3f, 1.0f},
    {0x2000a, "Reserved jungle selector", MoodTheme::Jungle, ScaffoldMotion::Pulse, {1,.60f,.20f}, .92f, 1.0f, .030f, .0f, .9f},
    {0x2000b, "Secret Rooms: hidden gold", MoodTheme::Temple, ScaffoldMotion::Beacon, {1,.76f,.30f}, .82f, 1.20f, .039f, 4.7f, 1.2f},
    {0x2000c, "Enjoy My Wealth: gold broadcast", MoodTheme::Stage, ScaffoldMotion::CrossSweep, {1,.74f,.28f}, .96f, 1.08f, .017f, 5.8f, 1.3f},
    {0x2000d, "No Turtles: cold furnace", MoodTheme::Jungle, ScaffoldMotion::OrbitSweep, {.45f,.72f,1}, .84f, 1.16f, .019f, 1.5f, 1.0f},
    {0x2000e, "Turtle Chunks: blue-gold chase", MoodTheme::Stage, ScaffoldMotion::Chase, {.42f,.82f,1}, .94f, 1.08f, .026f, 2.8f, 1.25f},
    {0x2000f, "Dynamite Cobra: ember grid", MoodTheme::Jungle, ScaffoldMotion::Scanline, {1,.30f,.08f}, .80f, 1.28f, .064f, 4.1f, 1.1f},
    {0x20010, "Buffalo Gun: temple target", MoodTheme::Temple, ScaffoldMotion::CrossSweep, {1,.58f,.22f}, .82f, 1.20f, .014f, 5.0f, 1.2f},
    {0x20011, "Total Carnage Three: violet rain", MoodTheme::Jungle, ScaffoldMotion::Scanline, {.72f,.36f,1}, .80f, 1.25f, .050f, 1.0f, 1.15f},
    {0x20012, "Secret Hall: amber breach", MoodTheme::Jungle, ScaffoldMotion::OrbitSweep, {1,.54f,.18f}, .84f, 1.18f, .018f, 3.6f, 1.1f},
    {0x20013, "Enough Keys: electric gold", MoodTheme::Stage, ScaffoldMotion::Chase, {1,.82f,.36f}, .94f, 1.10f, .028f, 4.9f, 1.3f},
    {0x20014, "Final Keys: red white show", MoodTheme::Stage, ScaffoldMotion::CrossSweep, {1,.22f,.14f}, .84f, 1.28f, .020f, 5.7f, 1.35f},
    {0x20015, "Mutoid Return: neon apocalypse", MoodTheme::Finale, ScaffoldMotion::OrbitSweep, {1,.12f,.64f}, .76f, 1.38f, .022f, .9f, 1.35f},
    {0x20016, "Pleasure Dome: spectral finale", MoodTheme::Dome, ScaffoldMotion::Pulse, {.68f,.36f,1}, .88f, 1.15f, .012f, 2.6f, 1.35f},
    {0x20017, "Ending screen: final afterglow", MoodTheme::Dome, ScaffoldMotion::CrossSweep, {1,.56f,.18f}, .84f, 1.10f, .018f, 4.0f, 1.2f},
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
        l.push_back(spot(.45f, .2f, 240, 440, 1, .9f, .65f, 1.15f, 26, .5f, .55f, .4f));
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
    const float r = p.accent[0], g = p.accent[1], b = p.accent[2];
    auto left = spot(.08f, -.34f, 205, 600, r, g, b, .95f * p.rig, 15, .35f, .50f, .38f);
    auto right = spot(.92f, -.34f, 205, 600, r * .65f + .25f, g * .65f + .25f, b * .65f + .25f,
                      .9f * p.rig, 15, .65f, .50f, .38f);
    auto wash = omni(.5f, -.08f, 82, 360, .44f + r * .24f, .44f + g * .24f, .44f + b * .24f,
                     .58f * p.rig, 13, 2);
    switch (p.scaffold) {
    case ScaffoldMotion::CrossSweep:
        left = animate(left, Anim::Sweep, p.rate, p.phase, .30f, .10f);
        right = animate(right, Anim::Sweep, p.rate, p.phase + Pi, -.30f, .10f);
        break;
    case ScaffoldMotion::OrbitSweep:
        left = animate(left, Anim::Orbit, p.rate, p.phase, .24f, .18f);
        right = animate(right, Anim::Orbit, p.rate, p.phase + Pi, .24f, .18f);
        break;
    case ScaffoldMotion::Chase:
        left = animate(left, Anim::Pulse, p.rate, p.phase);
        right = animate(right, Anim::Pulse, p.rate, p.phase + Pi);
        wash = animate(wash, Anim::Broken, 1, p.phase);
        break;
    case ScaffoldMotion::Pulse:
        left = animate(left, Anim::Pulse, p.rate, p.phase);
        right = animate(right, Anim::Pulse, p.rate, p.phase + Pi);
        wash = animate(wash, Anim::Hue, .003f, p.phase);
        break;
    case ScaffoldMotion::Beacon:
        left = animate(left, Anim::Strobe, 97, p.phase * 17);
        right = animate(right, Anim::Orbit, p.rate, p.phase, .36f, .18f);
        wash = animate(wash, Anim::Broken, 1, p.phase);
        break;
    case ScaffoldMotion::Scanline:
        left = animate(left, Anim::Sweep, p.rate, p.phase, .42f, .04f);
        right = animate(right, Anim::Sweep, p.rate, p.phase + 1.2f, -.42f, .04f);
        wash = animate(wash, Anim::Pulse, p.rate * 2, p.phase);
        break;
    }
    return {left, right, wash};
}

std::vector<Lamp> roomLamps(uint32_t room) {
    const auto &profile = room_lighting_profile(room);
    auto lamps = themeLamps(room);
    for (size_t i = 0; i < lamps.size(); ++i) {
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
    // Leave exposure headroom for the boss rig and simultaneous attack lights.
    if(room==10 || room==0x10007 || room==0x20007 || room==0x20015)
        for(auto& lamp:lamps)lamp.intensity*=.62f;
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
        intensity *= .74f + .26f * (.6f * valueNoise(phase) + .4f * valueNoise(phase * 2.7f + 13.f));
        g.x += (valueNoise(phase * 1.3f + 7) - .5f) * 1.2f;
        g.z += (valueNoise(phase * 1.1f + 3) - .5f) * 1.5f;
        break;
    case Anim::Pulse: intensity *= .3f + .7f * (.5f + .5f * std::sin(phase)); break;
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
    case MoodTheme::Steel: m.ambient = {.56f, .60f, .70f}; m.gloss = .55f; m.actorFloor = .80f; m.occlusion = .50f; break;
    case MoodTheme::Neon: m.ambient = {.54f, .45f, .62f}; m.gloss = .70f; m.actorFloor = .78f; m.occlusion = .55f; break;
    case MoodTheme::Moonlit: m.ambient = {.42f, .47f, .66f}; m.gloss = .60f; m.actorFloor = .78f; m.occlusion = .55f; break;
    case MoodTheme::Toxic: m.ambient = {.40f, .50f, .38f}; m.gloss = .45f; m.actorFloor = .78f; m.occlusion = .55f; break;
    case MoodTheme::Stage: m.ambient = {.62f, .60f, .74f}; m.gloss = .75f; m.actorFloor = .76f; m.occlusion = .60f; break;
    case MoodTheme::Jungle: m.ambient = {.52f, .45f, .32f}; m.gloss = .12f; m.actorFloor = .80f; m.occlusion = .50f; break;
    case MoodTheme::Temple: m.ambient = {.54f, .47f, .34f}; m.gloss = .35f; m.actorFloor = .80f; m.occlusion = .50f; break;
    case MoodTheme::Mutoid: m.ambient = {.43f, .38f, .46f}; m.gloss = .65f; m.actorFloor = .74f; m.occlusion = .65f; break;
    case MoodTheme::Scarface: m.ambient = {.42f, .50f, .44f}; m.gloss = .60f; m.actorFloor = .74f; m.occlusion = .65f; break;
    case MoodTheme::Cobra: m.ambient = {.48f, .41f, .33f}; m.gloss = .40f; m.actorFloor = .76f; m.occlusion = .60f; break;
    case MoodTheme::Finale: m.ambient = {.40f, .34f, .46f}; m.gloss = .70f; m.actorFloor = .74f; m.occlusion = .65f; break;
    case MoodTheme::Dome: m.ambient = {.50f, .46f, .56f}; m.gloss = .45f; m.actorFloor = .84f; m.occlusion = .40f; break;
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

LightingFrame RoomLightRig::build(const EffectPacket &p, int width, int offset) {
    LightingFrame frame;
    frame.lights.reserve(MaxLights);
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
    if (!current_.valid || current_.room != p.room || std::abs(current_.x - wx) > .5f ||
        std::abs(current_.y - wy) > .5f || current_.w != f.w) {
        if (current_.valid && (current_.room != p.room || std::abs(current_.x - wx) > 8 || std::abs(current_.y - wy) > 8))
            previous_ = current_;
        current_ = {p.room, wx, wy, f.w, f.h, true};
    }
    frame.floor = {wx - float(p.camera_x) + float(offset), wy - float(p.camera_y), f.w, f.h};
    const float t = float(p.frame);
    const float W = float(width), H = 256.f;
    auto emit = [&](const std::vector<Lamp> &lamps, const Anchor &a, unsigned &counter) {
        const float sx = a.x - float(p.camera_x) + float(offset), sy = a.y - float(p.camera_y);
        for (const auto &lamp : lamps) {
            if (frame.lights.size() >= MaxLights - MaxDynamic) return;
            auto g = realize(lamp, sx, sy, a.w, a.h, t);
            if (!reaches(g, W, H) || g.intensity <= .002f) continue;
            frame.lights.push_back(g);
            ++counter;
        }
    };
    emit(roomLamps(p.room), current_, frame.staticLights);
    // Retire the departing rig only after its room leaves the lighted viewport.
    // State 0x18 can end before the camera has exposed the final wall, and later
    // scripted camera movement must not switch off a still-visible room.
    if (previous_.valid && previous_.room != current_.room) {
        const float sx = previous_.x - float(p.camera_x) + float(offset), sy = previous_.y - float(p.camera_y);
        if (sx + previous_.w + 200 > 0 && sx - 200 < W && sy + previous_.h + 200 > 0 && sy - 200 < H)
            emit(roomLamps(previous_.room), previous_, frame.previousRoomLights);
        else
            previous_ = {};
    }
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
        emit(studio, current_, frame.studioLights);
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
        if (frame.dynamicLights >= gameplayLightLimit || frame.lights.size() >= MaxLights) break;
        if (!(d.power > 0) || !(d.radius > 0)) continue;
        GpuLight g;
        g.x = d.x + float(offset); g.y = d.y; g.z = d.height; g.radius = d.radius;
        g.r = d.r; g.g = d.g; g.b = d.b; g.intensity = d.power;
        g.softness = 2; g.samples = 1;
        if (!reaches(g, W, H)) continue;
        frame.lights.push_back(g);
        ++frame.dynamicLights;
    }
    auto emitCinema = [&](float x, float y, float z, float radius, float r, float g, float b, float power) {
        if (frame.dynamicLights >= MaxDynamic || frame.lights.size() >= MaxLights ||
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
    return frame;
}
} // namespace stv::host
