#pragma once
#include "game_rules.hpp"
#include "process.hpp"
#include "waves.hpp"
namespace stv {
// Native lowering of ROBO.ASM OVERLORD/AP/ON1/DNE/SAD/RLSE and INIT_RAM.
// These are original game globals, not a host frame timer. WAVES/TV must supply
// EHALT/TV presence at their actual process boundaries when integrated.
struct WaveWorld {
    u16 circuit{},wave{1},player_status{1},ehalt{},halt{};
    u32 wave_yx{}; // SET_WAVE's packed source Y:X room offset.
    bool attract_mode{},tv_process_present{};
    Difficulty difficulty{};SpawnCaps maximums{};
    u16 enemy_count[6]{}; // HULK,TNK,FAT,ORB,GAL,RBTA (ZEROCNTS order)
    u16 done[5]{};        // HULK,ORB,TNK,RBTA,SNK (INIT_RAM order)
    u8 econ{},enon{},pzon{},gate_timer[3]{};
    u8 shk_on{},face_on{},boss_on{},toid_on{},wntout{},pctot{},endon{};
    u32 ramxy[50]{};u16 keepxy{}; // index, never an original or host pointer
};
struct WaveOverlord;
using InitialPopulation=Status(*)(WaveOverlord&,unsigned,u16,void*);
using WaveFlowService=Status(*)(WaveOverlord&,void*);
using WavePaletteService=Status(*)(WaveOverlord&,const PaletteCycleRequest&,void*);
struct WaveFlowHooks {
    InitialPopulation initial{}; // ON1 calls type handler once with ONSCRN count
    WaveSpawn release{};         // RLSE calls spawn handler; not success/consumption
    WaveFlowService cash{};       // O_ITEMS; only invoked for the source cash rooms
    WavePaletteService palette{}; // SETMAX: SPDRP/PAL1 request, when applicable
};
enum class WaveFlowPhase:u8 {start,initialize,wait_text,entry_delay,release,failed};
struct WaveOverlord {
    static constexpr u32 continuation=0x57415645u;
    WaveWorld world{};
    WaveRelease release{};
    WaveFlowPhase phase{WaveFlowPhase::start};
    u64 wakes{},initial_calls{},cash_calls{},initializations{};
    const char* error{};
    // Call exactly when the native OVERLORD process is runnable. The result is
    // consumed by MPROC's native dispatcher, not by a once-per-frame substitute.
    ProcessAction step(const WaveFlowHooks&,void* context);
    void init_ram();
};
}
