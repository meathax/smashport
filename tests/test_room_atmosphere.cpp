#include "stv/host/enhance.hpp"
#include "stv/host/pc_menu.hpp"
#include "stv/host/pc_settings.hpp"
#include "stv/host/room_lighting.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>

using stv::host::EffectKind;
using stv::host::EffectObject;
using stv::host::EffectPacket;
using stv::host::EffectScene;
using stv::host::EnhanceEffects;
using stv::host::PcSettings;
using stv::host::RoomLamp;
using stv::host::RoomLightRig;

int main() {
    PcSettings settings;
    assert(settings.atmosphere >= 0 && settings.atmosphere <= 3);
    assert(settings.sprite_rim >= 0 && settings.sprite_rim <= 3);
    assert(settings.contact_shadows >= 0 && settings.contact_shadows <= 3);
    assert(settings.wet_materials >= 0 && settings.wet_materials <= 3);
    assert(settings.heat_distortion >= 0 && settings.heat_distortion <= 3);
    assert(settings.emissive_spill >= 0 && settings.emissive_spill <= 3);
    assert(settings.cinematic_grade >= 0 && settings.cinematic_grade <= 3);
    const auto packed = environment_levels(settings);
    assert(((packed >> 0) & 3) == unsigned(settings.atmosphere));
    assert(((packed >> 12) & 3) == unsigned(settings.cinematic_grade));
    assert(environment_effect_mask(PcSettings{}) == 0x1fc00000u);

    PcSettings empty;
    empty.atmosphere = empty.sprite_rim = empty.contact_shadows = empty.wet_materials = 0;
    empty.heat_distortion = empty.emissive_spill = empty.cinematic_grade = 0;
    assert(environment_effect_mask(empty) == 0);

    stv::host::PcMenu menu;
    stv::host::PcMenuContext context;
    menu.open(false);
    menu.enter(stv::host::PcMenuPage::Enhance);
    const auto rows = menu.rows(context);
    const auto has_row = [&](stv::host::PcMenuItem item) {
        return std::any_of(rows.begin(), rows.end(), [item](const auto& row) { return row.item == item; });
    };
    assert(has_row(stv::host::PcMenuItem::Atmosphere));
    assert(has_row(stv::host::PcMenuItem::SpriteRim));
    assert(has_row(stv::host::PcMenuItem::ContactShadows));
    assert(has_row(stv::host::PcMenuItem::WetMaterials));
    assert(has_row(stv::host::PcMenuItem::HeatDistortion));
    assert(has_row(stv::host::PcMenuItem::EmissiveSpill));
    assert(has_row(stv::host::PcMenuItem::CinematicGrade));

    const auto settings_file = std::filesystem::temp_directory_path() / "smash-room-atmosphere.ini";
    std::filesystem::remove(settings_file);
    settings.atmosphere = 3;
    settings.sprite_rim = 2;
    settings.contact_shadows = 1;
    settings.wet_materials = 3;
    settings.heat_distortion = 2;
    settings.emissive_spill = 1;
    settings.cinematic_grade = 3;
    std::string settings_error;
    assert(stv::host::save_pc_settings(settings_file, settings, settings_error));
    PcSettings restored;
    assert(stv::host::load_pc_settings(settings_file, restored, settings_error) ==
           stv::host::SettingsLoad::Loaded);
    assert(restored == settings);
    assert(std::filesystem::remove(settings_file));

    EffectScene scene;
    scene.valid = scene.in_arena = true;
    scene.frame = 120;
    scene.room = 1;
    scene.width = 410;
    scene.height = 256;
    scene.geometry.floor = {29, 46, 343, 195};
    scene.casters.push_back({40, 80, 16, 24, 104, 32, 1, 1});
    scene.objects = {
        {10, 0, EffectKind::Projectile, 40, 90},
        {11, 0, EffectKind::EnemyShot, 60, 100},
        {12, 0, EffectKind::Explosion, 80, 110},
    };
    settings.enhance = settings.hardware_rt = true;
    EnhanceEffects effects;
    effects.advance(scene, settings);
    assert(effects.packet().dxrLights.size() == 1);
    assert(effects.packet().dxrLights.front().x == 80);
    assert(effects.packet().dxrLights.front().power > 0);
    // Every encounter colors actual hostile shots, leaving player shots unlit.
    // Re-observing a frame must neither stack energy nor consume extra slots.
    std::array<std::array<float,3>,4> colors{};
    unsigned bossIndex=0;
    for (const auto room : {10u, 0x10007u, 0x20007u, 0x20015u}) {
        scene.room=room;
        effects.advance(scene, settings);
        const auto lights=effects.packet().dxrLights;
        assert(lights.size()==2 && lights[0].x==60 && lights[1].x==80);
        assert(lights[1].power*lights[1].radius > lights[0].power*lights[0].radius);
        colors[bossIndex++]={lights[0].r,lights[0].g,lights[0].b};
        effects.advance(scene, settings);
        assert(effects.packet().dxrLights.size()==2);
        assert(effects.packet().dxrLights[0].power==lights[0].power);
        ++scene.frame;
        effects.advance(scene, settings);
        assert(effects.packet().dxrLights[0].power!=lights[0].power);
    }
    for (size_t i=0;i<colors.size();++i)
        for (size_t j=i+1;j<colors.size();++j) assert(colors[i]!=colors[j]);
    for (unsigned i=0;i<40;++i)
        scene.objects.push_back({100+i,0,EffectKind::EnemyShot,float(i),100});
    effects.advance(scene, settings);
    assert(effects.packet().dxrLights.size()==stv::host::MaxGameplayLights);
    float barragePower=0;
    for(const auto& lamp:effects.packet().dxrLights)barragePower+=lamp.power;
    assert(barragePower<=5.001f);
    assert(std::any_of(effects.packet().dxrLights.begin(),effects.packet().dxrLights.end(),
        [](const auto& light){return light.x==80 && light.radius==165;}));
    settings.hardware_rt=false;
    effects.advance(scene, settings);
    assert(effects.packet().dxrLights.empty());
    settings.hardware_rt=true;
    scene.roomIntro = true;
    effects.advance(scene, settings);
    assert(effects.packet().casters.size() == 1);

    // Smoke styles and budgets must hold across every boss and a full barrage.
    for(const unsigned room:{10u,0x10007u,0x20007u,0x20015u}){
        EnhanceEffects smoke;smoke.use_gpu_simulation(true);
        PcSettings smokeSettings{};smokeSettings.enhance=smokeSettings.fluid_smoke=true;
        EffectScene smokeScene{};smokeScene.valid=smokeScene.in_arena=true;
        smokeScene.width=410;smokeScene.height=256;smokeScene.room=room;smokeScene.frame=1;
        smokeScene.studio.boss=true;smoke.advance(smokeScene,smokeSettings);
        smokeScene.frame++;
        for(unsigned i=0;i<96;++i)smokeScene.objects.push_back({100+i,0,EffectKind::Explosion,100.f+float(i%8),120});
        smoke.advance(smokeScene,smokeSettings);
        unsigned flows=0,cores=0;
        for(const auto& spawn:smoke.packet().spawns){
            if(spawn.kind==15 && spawn.pad0>=5){++flows;assert(spawn.pad0==(room==0x10007?6:room==0x20007?7:room==0x20015?8:5));}
            if(spawn.kind==16)cores+=unsigned(spawn.count);
        }
        assert(flows==8 && cores==24);
        smoke.advance(smokeScene,smokeSettings);assert(smoke.packet().spawns.empty());
        ++smokeScene.frame;smoke.advance(smokeScene,smokeSettings);
        for(const auto& spawn:smoke.packet().spawns)assert(spawn.kind!=16 && !(spawn.kind==15 && spawn.pad0>=5));
        smokeSettings.fluid_smoke=false;++smokeScene.frame;smokeScene.objects.clear();
        smokeScene.objects.push_back({999,0,EffectKind::Explosion,100,120});
        smoke.advance(smokeScene,smokeSettings);
        for(const auto& spawn:smoke.packet().spawns)assert(spawn.kind!=16 && !(spawn.kind==15 && spawn.pad0>=5));
    }
    EffectPacket packet;
    packet.room = 1;
    packet.frame = 100;
    packet.in_arena = true;
    packet.geometry.floor = {29, 46, 343, 195};
    for (int i = 0; i < 20; ++i)
        packet.dxrLights.push_back({40.f + i * 2, 80, 20, 80, 1, .5f, .1f, 1});
    RoomLightRig rig;
    const auto frame = rig.build(packet, 410, 0);
    assert(frame.lights.size() <= RoomLightRig::MaxLights);
    assert(frame.dynamicLights <= RoomLightRig::MaxDynamic);
    for (const auto value : frame.mood.ambient)
        assert(std::isfinite(value) && value >= 0 && value <= 1);
    assert(std::isfinite(frame.mood.gloss));
}
