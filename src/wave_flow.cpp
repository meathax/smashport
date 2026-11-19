#include "stv/wave_flow.hpp"
namespace stv {
void WaveOverlord::init_ram() {
    // ROBO INIT_RAM: notably does NOT clear ENON, ECON, enemy counts or HALT.
    for(auto& d:world.done)d=0;
    for(auto& t:world.gate_timer)t=0;
    world.shk_on=world.face_on=world.boss_on=world.toid_on=0;
    world.wntout=world.pctot=world.pzon=world.endon=0;
    ++initializations;
}
ProcessAction WaveOverlord::step(const WaveFlowHooks& hooks,void* context) {
    ++wakes;
    auto fail=[&](const char* why){phase=WaveFlowPhase::failed;error=why;return ProcessAction::fail(why);};
    auto sleep=[&](u16 n){return ProcessAction::wait(n,continuation);};
    switch(phase) {
    case WaveFlowPhase::start:
        phase=WaveFlowPhase::initialize;return sleep(3);
    case WaveFlowPhase::initialize: {
        world.ehalt=1;
        PaletteCycleRequest effect;
        auto s=set_maximums(world.circuit,world.wave,world.player_status,
                            world.difficulty,world.maximums,effect);
        if(!s)return fail(s.message);
        if(effect.start) {
            if(!hooks.palette)return fail("OVERLORD SETMAX requests untranslated COLCYC service");
            s=hooks.palette(*this,effect,context);if(!s)return fail(s.message);
        }
        for(auto& n:world.enemy_count)n=0;
        world.econ=world.enon=world.pzon=0;
        for(auto& xy:world.ramxy)xy=0;
        world.ramxy[0]=0x003c003cu;world.keepxy=1;
        unsigned selected=world.wave;
        if(world.attract_mode&&world.wave==1) {world.maximums.max_hulk=30;selected=9;}
        s=release.load(world.circuit,selected);if(!s)return fail(s.message);
        // The source creates initial children in table order. Those children run
        // later in MPROC's same dispatch and own SETUP_* population consumption.
        for(unsigned i=0;i<release.count;++i) {
            const u16 n=release.records[i].initial;
            if(!n)continue;
            if(!hooks.initial)return fail("OVERLORD ON1 needs an implemented initial-population handler");
            ++initial_calls;s=hooks.initial(*this,i,n,context);if(!s)return fail(s.message);
        }
        const bool cash=world.circuit==0?world.wave==5:(world.wave==12||world.wave==14);
        if(cash) {
            if(!hooks.cash)return fail("OVERLORD cash room requires untranslated O_ITEMS service");
            ++cash_calls;s=hooks.cash(*this,context);if(!s)return fail(s.message);
        }
        phase=WaveFlowPhase::wait_text;return sleep(1);
    }
    case WaveFlowPhase::wait_text:
        if(world.ehalt!=2)return sleep(1);
        if(world.tv_process_present)world.halt=1;
        phase=WaveFlowPhase::entry_delay;return sleep(120);
    case WaveFlowPhase::entry_delay:
        world.ehalt=world.tv_process_present?1:0;
        init_ram();phase=WaveFlowPhase::release;
        // SAD0 falls straight into RLSE: no extra SLEEP 3 before its first scan.
        [[fallthrough]];
    case WaveFlowPhase::release: {
        // Original HALT skips the handler table completely.
        if(!world.halt) {
            if(!hooks.release)return fail("OVERLORD RLSE needs an implemented spawn handler");
            auto s=release.scan(false,hooks.release,context);if(!s)return fail(s.message);
        }
        return sleep(3);
    }
    case WaveFlowPhase::failed:return ProcessAction::fail(error?error:"OVERLORD fault");
    }
    return fail("Invalid native OVERLORD continuation");
}
}
