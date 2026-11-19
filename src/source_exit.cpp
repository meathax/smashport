#include "stv/source_exit.hpp"

namespace stv {
namespace {
constexpr u16 exact_process_mask=0xffffu;
constexpr u16 cobra_processes[]={279,280,281,282,223}; // COBPID*, then LORDPID
constexpr u16 icon_cleanup_processes[]={232,262}; // ICONPID, CSHPID
}

ProcessAction source_dexit_step(NativeProcesses& processes,ProcessHandle handle,
                                void* context){
    if(handle>=process_capacity||!processes.cells[handle].active||
       processes.cells[handle].id!=source_dexit_process_id)
        return ProcessAction::fail("DEXIT requires its active process ID zero");
    const u32 state=processes.cells[handle].continuation;
    if(state==source_exit_state::dexit_entry)
        return ProcessAction::wait(120,source_exit_state::dexit_after_120);
    if(state==source_exit_state::dexit_after_120){
        // ENDWAVE.ASM issues five ordered KILALL calls with mask FFFFH.
        for(u16 id:cobra_processes)processes.kill_matching(id,exact_process_mask);
        return ProcessAction::wait(10,source_exit_state::dexit_after_10);
    }
    if(state==source_exit_state::dexit_after_10){
        // CREATE ARWPID,KILIC precedes reading GAMSTATE in the source.
        ProcessHandle arrow=no_process;
        auto status=processes.create(source_arrow_process_id,
                                     source_exit_state::kilic_entry,nullptr,arrow);
        if(!status)return ProcessAction::fail(status.message);
        if(!context)
            return ProcessAction::fail("DEXIT requires GAMSTATE after ARWPID/KILIC creation");
        const auto& source=*static_cast<const SourceDexitContext*>(context);
        if(source.game_state==source_game_state_inamode)
            return ProcessAction::suicide(); // DEXIT.NOSCRLL: DIE
        return ProcessAction::fail("DEXIT non-attract DX2 requires room/object and exit-gate services");
    }
    return ProcessAction::fail("DEXIT continuation is not translated");
}

ProcessAction source_kilic_step(NativeProcesses& processes,ProcessHandle handle,
                                void*){
    if(handle>=process_capacity||!processes.cells[handle].active||
       processes.cells[handle].id!=source_arrow_process_id)
        return ProcessAction::fail("KILIC requires its active ARWPID process");
    const u32 state=processes.cells[handle].continuation;
    if(state==source_exit_state::kilic_entry)
        return ProcessAction::wait(600,source_exit_state::kilic_after_600);
    if(state==source_exit_state::kilic_after_600){
        // ENDWAVE.ASM:KILIC kills icon and cash dispatchers, then DIE.
        for(u16 id:icon_cleanup_processes)processes.kill_matching(id,exact_process_mask);
        return ProcessAction::suicide();
    }
    return ProcessAction::fail("KILIC continuation is not translated");
}
}
