#include "stv/host/enhance.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace stv::host;

int main() {
    PcSettings settings;
    settings.enhance=true;
    settings.particles=settings.blood=settings.debris=settings.smoke=settings.fire=false;
    settings.lighting=settings.voxels=settings.shockwaves=settings.tracers=settings.brass=false;
    settings.persistent_gore=settings.materials=settings.destruction=settings.weapon_impacts=false;
    settings.fluid_smoke=settings.explosive_debris=false;
    settings.muzzle_flash=settings.hardware_rt=false;

    EffectScene scene;
    scene.room=1;
    scene.width=410;
    scene.height=256;
    scene.valid=true;
    scene.in_arena=true;
    for(uint32_t slot=0;slot<48;++slot)
        scene.objects.push_back({slot,0,EffectKind::Projectile,float(24+(slot*7)%340),
            float(50+(slot*11)%150),float((slot&1)?3:-3),float((slot&2)?2:-2),uint16_t(0x4201+slot%6)});

    constexpr unsigned frames=30000;
    EnhanceEffects effects;
    const auto start=std::chrono::steady_clock::now();
    for(unsigned frame=1;frame<=frames;++frame){
        scene.frame=frame;
        for(auto& object:scene.objects){
            object.x=24.f+std::fmod(object.x+object.vx,340.f);
            object.y=50.f+std::fmod(object.y+object.vy,150.f);
        }
        effects.advance(scene,settings);
    }
    const auto elapsed=std::chrono::steady_clock::now()-start;
    const double microseconds=std::chrono::duration<double,std::micro>(elapsed).count();
    std::printf("%u frames, %.3f us/frame, 48 projectiles/frame\n",frames,microseconds/frames);
}
