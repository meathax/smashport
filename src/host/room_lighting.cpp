#include "stv/host/room_lighting.hpp"
#include <algorithm>
#include <cmath>

namespace stv::host {
namespace {
constexpr float Pi = 3.14159265f;
// Global grade: a darker room base lets the placed lights carve pools and shadows.
constexpr float AmbientScale = .74f, LightScale = 1.6f;
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
        l.push_back(animate(spot(.3f, -.15f, 170, 440, 1, .97f, .92f, 1.5f, 20, .45f, .55f), Anim::Sweep, .008f, 0, .1f, .05f));
        l.push_back(animate(spot(.7f, -.15f, 170, 440, 1, .97f, .92f, 1.5f, 20, .55f, .55f), Anim::Sweep, .008f, 1.7f, .1f, .05f));
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
        l.push_back(animate(spot(.15f, -.3f, 200, 560, 1, .12f, .1f, 1.8f, 13, .35f, .5f), Anim::Sweep, .034f, 0, .32f, .3f));
        l.push_back(animate(spot(.85f, -.3f, 200, 560, 1, .12f, .1f, 1.8f, 13, .65f, .5f), Anim::Sweep, .029f, 2, .32f, .3f));
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
    const auto [circuit, wave] = key(room);
    if (circuit == 0) {
        if (wave == 10) return MoodTheme::Mutoid;
        if (wave == 5) return MoodTheme::Stage;
        if (wave == 4 || wave == 8 || wave == 9) return MoodTheme::Neon;
        return MoodTheme::Steel;
    }
    if (circuit == 1) {
        if (wave == 7) return MoodTheme::Scarface;
        if (wave == 12 || wave == 14) return MoodTheme::Stage;
        if (wave == 4 || wave == 11) return MoodTheme::Toxic;
        if (wave == 1 || wave == 2 || wave == 5 || wave == 6 || wave == 16 || wave == 17) return MoodTheme::Neon;
        return MoodTheme::Moonlit;
    }
    if (wave == 22) return MoodTheme::Dome;
    if (wave == 21) return MoodTheme::Finale;
    if (wave == 7) return MoodTheme::Cobra;
    if (wave == 12 || wave == 14 || wave == 19 || wave == 20) return MoodTheme::Stage;
    if (wave == 5 || wave == 6 || wave == 11 || wave == 16) return MoodTheme::Temple;
    return MoodTheme::Jungle;
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
    case MoodTheme::Mutoid: m.ambient = {.46f, .40f, .46f}; m.gloss = .65f; m.actorFloor = .74f; m.occlusion = .65f; break;
    case MoodTheme::Scarface: m.ambient = {.42f, .50f, .44f}; m.gloss = .60f; m.actorFloor = .74f; m.occlusion = .65f; break;
    case MoodTheme::Cobra: m.ambient = {.48f, .41f, .33f}; m.gloss = .40f; m.actorFloor = .76f; m.occlusion = .60f; break;
    case MoodTheme::Finale: m.ambient = {.40f, .34f, .46f}; m.gloss = .70f; m.actorFloor = .74f; m.occlusion = .65f; break;
    case MoodTheme::Dome: m.ambient = {.50f, .46f, .56f}; m.gloss = .45f; m.actorFloor = .84f; m.occlusion = .40f; break;
    }
    for (auto &a : m.ambient) a *= AmbientScale;
    return m;
}

LightingFrame RoomLightRig::build(const EffectPacket &p, int width, int offset) {
    LightingFrame frame;
    const auto f = p.geometry.floor;
    frame.mood = room_mood(p.room);
    if (f.w <= 0 || f.h <= 0) return frame;
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
    emit(themeLamps(p.room), current_, frame.staticLights);
    // The departing room stays lit for the length of the pan (state 0x18) only.
    if (!p.roomIntro) previous_ = {};
    if (previous_.valid && previous_.room != current_.room) {
        const float sx = previous_.x - float(p.camera_x) + float(offset), sy = previous_.y - float(p.camera_y);
        if (sx + previous_.w + 200 > 0 && sx - 200 < W && sy + previous_.h + 200 > 0 && sy - 200 < H)
            emit(themeLamps(previous_.room), previous_, frame.previousRoomLights);
        else
            previous_ = {};
    }
    if ((p.room & 0xffff) == 1) emit(studioLamps(), current_, frame.studioLights);
    // Gameplay lights: projectiles, enemy shots and explosions, strongest first.
    std::vector<RoomLamp> dynamic = p.dxrLights;
    std::stable_sort(dynamic.begin(), dynamic.end(),
                     [](const RoomLamp &a, const RoomLamp &b) { return a.power * a.radius > b.power * b.radius; });
    for (const auto &d : dynamic) {
        if (frame.dynamicLights >= MaxDynamic || frame.lights.size() >= MaxLights) break;
        if (!(d.power > 0) || !(d.radius > 0)) continue;
        GpuLight g;
        g.x = d.x + float(offset); g.y = d.y; g.z = d.height; g.radius = d.radius;
        g.r = d.r; g.g = d.g; g.b = d.b; g.intensity = d.power;
        g.softness = 2; g.samples = 1;
        if (!reaches(g, W, H)) continue;
        frame.lights.push_back(g);
        ++frame.dynamicLights;
    }
    return frame;
}
} // namespace stv::host
