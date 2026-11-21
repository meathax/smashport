#include "stv/host/enhance.hpp"
#include "stv/host/pc_menu.hpp"
#include "stv/host/pc_settings.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

using namespace stv::host;

namespace {
EffectScene scene(uint64_t frame) {
    EffectScene value;
    value.frame=frame;
    value.room=1;
    value.width=410;
    value.height=256;
    value.valid=true;
    value.in_arena=true;
    return value;
}

const PcMenuRow* menu_row(const std::vector<PcMenuRow>& rows,PcMenuItem item) {
    for(const auto& row:rows)if(row.item==item)return &row;
    return nullptr;
}
}

int main(int argc,char** argv) {
    PcSettings settings;
    settings.enhance=true;
    settings.particles=settings.blood=settings.debris=settings.smoke=settings.fire=false;
    settings.hardware_rt=true;

    EnhanceEffects effects;
    auto lights=scene(1);
    lights.objects.push_back({1,0,EffectKind::Projectile,40,80,4,0,0x4201});
    lights.objects.push_back({2,0,EffectKind::EnemyShot,50,90,4,0,0x8500});
    lights.objects.push_back({3,0,EffectKind::Explosion,60,100,0,0,0});
    effects.advance(lights,settings);
    assert(effects.packet().dxrLights.size()==1);
    assert(effects.packet().dxrLights.front().x==60 && effects.packet().dxrLights.front().y==100);

    auto impact=scene(2);
    impact.impacts.push_back({60,100,4,0,0x9901,MachinePart,1,0,0x4204});
    effects.advance(impact,settings);
    assert(effects.packet().weaponFx.director.tier==ImpactTier::Medium);
    assert(effects.packet().weaponFx.director.camera_trauma>.4f);
    assert(effects.packet().weaponFx.event_count==1);
    assert(effects.packet().weaponFx.events[0].kind==WeaponFxKind::Impact);
    assert(effects.packet().weaponFx.events[0].family==WeaponFamily::Family4);

    auto trajectories=scene(3);
    trajectories.objects.push_back({1,0,EffectKind::Projectile,44,80,4,0,0x4201});
    settings.temporal_trails=3;
    effects.advance(trajectories,settings);
    for(uint64_t frame=4;frame<10;++frame){
        trajectories.frame=frame;
        trajectories.objects.front().x+=4;
        effects.advance(trajectories,settings);
    }
    assert(effects.packet().weaponFx.history_count==0);
    assert(std::none_of(effects.packet().weaponFx.events.begin(),
        effects.packet().weaponFx.events.begin()+effects.packet().weaponFx.event_count,
        [](const WeaponFxEvent& event){return event.kind==WeaponFxKind::Wake;}));
    settings.temporal_trails=0;
    trajectories.frame=10;
    effects.advance(trajectories,settings);
    assert(effects.packet().weaponFx.history_count==0);

    auto crowded=scene(11);
    for(uint32_t slot=0;slot<96;++slot)
        crowded.objects.push_back({slot+100,0,EffectKind::Projectile,float(20+slot*3),80,4,0,0x4201});
    effects.advance(crowded,settings);
    assert(effects.packet().weaponFx.event_count==WeaponFxPacket::EventCapacity);
    assert(std::any_of(effects.packet().weaponFx.events.begin(),
        effects.packet().weaponFx.events.begin()+effects.packet().weaponFx.event_count,
        [](const WeaponFxEvent& event){return event.kind==WeaponFxKind::Muzzle;}));
    assert(std::none_of(effects.packet().weaponFx.events.begin(),
        effects.packet().weaponFx.events.begin()+effects.packet().weaponFx.event_count,
        [](const WeaponFxEvent& event){return event.kind==WeaponFxKind::Wake;}));

    PcMenu menu;
    menu.open(true);
    menu.enter(PcMenuPage::Enhance);
    PcMenuContext context;
    const auto rows=menu.rows(context);
    assert(menu_row(rows,PcMenuItem::CombatFeedback));
    context.combat_feedback=false;
    const auto disabledRows=menu.rows(context);
    const auto* camera=menu_row(disabledRows,PcMenuItem::CameraShake);
    assert(camera && !camera->enabled);
    assert(std::string(camera->unavailable_reason)=="Enable Combat feedback");
    context.combat_feedback=true;
    context.reduce_motion=true;
    const auto reducedRows=menu.rows(context);
    const auto* shake=menu_row(reducedRows,PcMenuItem::CameraShake);
    assert(shake && !shake->enabled);
    assert(!menu_row(reducedRows,PcMenuItem::Tracers));
    assert(!menu_row(reducedRows,PcMenuItem::TemporalTrails));

    PcSettings saved;
    saved.combat_feedback=3;
    saved.camera_shake=2;
    saved.zoom_punch=0;
    saved.hit_flash=2;
    saved.edge_effects=3;
    saved.weapon_signatures=0;
    saved.temporal_trails=3;
    saved.reduce_shake=saved.reduce_flashing=saved.reduce_motion=true;
    const std::filesystem::path file=argc>1?argv[1]:"combat-settings-fixture.cfg";
    std::filesystem::remove(file);
    std::string error;
    assert(save_pc_settings(file,saved,error));
    PcSettings loaded;
    assert(load_pc_settings(file,loaded,error)==SettingsLoad::Loaded);
    assert(loaded==saved);
    {
        std::ofstream invalid(file,std::ios::trunc);
        invalid<<"version=1\ncombat_feedback=4\nname=PLAYER\n";
    }
    assert(load_pc_settings(file,loaded,error)==SettingsLoad::Invalid);
    std::filesystem::remove(file);
}
