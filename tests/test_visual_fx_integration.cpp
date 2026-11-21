#include "stv/host/enhance.hpp"
#include "stv/host/pc_menu.hpp"
#include "stv/host/pc_settings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include "resident_effects_shader.hpp"

using namespace stv::host;

namespace {
bool has_row(const std::vector<PcMenuRow>& rows, PcMenuItem item) {
    return std::any_of(rows.begin(), rows.end(),
        [item](const PcMenuRow& row) { return row.item == item; });
}

void require_shader_compiles(const char* entry) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(effectsShader, std::strlen(effectsShader),
        "resident_effects", nullptr, nullptr, entry, "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) {
        std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        errors->Release();
    }
    assert(SUCCEEDED(result));
    if (code) code->Release();
}
}

int main(int argc, char** argv) {
    PcSettings settings;
    settings.reduce_shake = settings.reduce_flashing = settings.reduce_motion = true;
    enable_all_visual_effects(settings);
    assert(settings.enhance);
    assert(settings.room_cinema == 3 && settings.temporal_trails == 0);
    assert(!settings.tracers);
    assert(settings.atmosphere == 3 && settings.cinematic_grade == 3);
    assert(settings.reduce_shake && settings.reduce_flashing && settings.reduce_motion);
    assert(!settings.hardware_rt);

    const unsigned environment = environment_effect_mask(settings);
    assert((environment & ((1u << 22) - 1)) == 0);
    assert(environment == 0x1fc00000u);
    assert((environment & HardwareRayTracingEffectMask) == 0);
    assert((environment & RoomCinemaEffectMask) == 0);
    assert((HardwareRayTracingEffectMask & RoomCinemaEffectMask) == 0);

    PcMenu menu;
    menu.open(true);
    menu.enter(PcMenuPage::Enhance);
    PcMenuContext menu_context;
    menu_context.combat_feedback = true;
    const auto rows = menu.rows(menu_context);
    const PcMenuItem expected[]{
        PcMenuItem::Atmosphere, PcMenuItem::SpriteRim, PcMenuItem::ContactShadows,
        PcMenuItem::WetMaterials, PcMenuItem::HeatDistortion, PcMenuItem::EmissiveSpill,
        PcMenuItem::CinematicGrade, PcMenuItem::RoomCinema, PcMenuItem::CombatFeedback,
        PcMenuItem::CameraShake, PcMenuItem::ZoomPunch, PcMenuItem::HitFlash,
        PcMenuItem::EdgeEffects, PcMenuItem::WeaponSignatures,
        PcMenuItem::ReduceShake, PcMenuItem::ReduceFlashing, PcMenuItem::ReduceMotion
    };
    std::set<PcMenuItem> ids;
    for (PcMenuItem item : expected) {
        assert(has_row(rows, item));
        assert(ids.insert(item).second);
        assert(static_cast<unsigned>(item) < PcMenuItemCount);
    }
    for (PcMenuItem item : {PcMenuItem::CameraShake, PcMenuItem::HitFlash,
                            PcMenuItem::EdgeEffects, PcMenuItem::WeaponSignatures}) {
        assert(std::any_of(rows.begin(), rows.end(), [item](const PcMenuRow& row) {
            return row.item == item && row.enabled;
        }));
    }
    assert(!has_row(rows, PcMenuItem::Tracers));
    assert(!has_row(rows, PcMenuItem::TemporalTrails));

    EffectScene scene;
    scene.valid = scene.in_arena = true;
    scene.frame = 100;
    scene.room = 1;
    scene.width = 410;
    scene.height = 256;
    scene.objects = {
        {1, 0, EffectKind::Projectile, 40, 80},
        {2, 0, EffectKind::EnemyShot, 60, 90},
        {3, 0, EffectKind::Explosion, 80, 100},
    };
    settings.hardware_rt = true;
    EnhanceEffects effects;
    effects.advance(scene, settings);
    assert(effects.packet().dxrLights.size() == 1);
    assert(effects.packet().dxrLights.front().x == 80);

    const std::filesystem::path file = argc > 1 ? argv[1] : "visual-fx-settings-fixture.cfg";
    std::filesystem::remove(file);
    std::string error;
    assert(save_pc_settings(file, settings, error));
    PcSettings restored;
    assert(load_pc_settings(file, restored, error) == SettingsLoad::Loaded);
    assert(restored == settings);

    {
        std::ofstream legacy(file, std::ios::trunc);
        legacy << "version=1\nname=PLAYER\n";
    }
    assert(load_pc_settings(file, restored, error) == SettingsLoad::Loaded);
    assert(restored.room_cinema == 0);
    {
        std::ofstream incomplete(file, std::ios::trunc);
        incomplete << "version=3\nroom_cinema=2\nname=PLAYER\n";
    }
    assert(load_pc_settings(file, restored, error) == SettingsLoad::Invalid);
    std::filesystem::remove(file);

    const char* entries[] = {
        "integrate", "spawn", "binParticles", "composite",
        "markHud", "markReceiver", "stepSmoke", "stampBodies"
    };
    for (const char* entry : entries) require_shader_compiles(entry);
}
