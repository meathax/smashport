#include "stv/source_actor.hpp"

namespace stv {

Status source_ohulk_initialize(NativeProcesses& processes, NativeObjects& objects,
                               const SourceOhulkInitialization& input,
                               SourceOhulkInitializationResult& out) {
    if (input.process >= process_capacity || !input.art || !input.accounting ||
        !input.player_object_x || !input.player_torsos || !input.rng ||
        !input.read_hcount || !input.wave_records.data)
        return Status::fail("OHULK native owner is missing a required source service");
    if (!processes.cells[input.process].active ||
        processes.cells[input.process].id != source_hulk_process_id)
        return Status::fail("OHULK initialization requires an active HULKPID process",
                            input.process);

    // This is the post-GET_VALID_XY body. The higher-level source_ohulk_start
    // owns the earlier CFLAGS.STAND write and placement call.
    SourceOhulkCoordinates coordinates{};
    auto status = source_apply_ohulk_waveyx(input.placement.a0_x_16_16,
                                            input.placement.a1_value,
                                            input.attract_mode, input.wave_yx,
                                            coordinates);
    if (!status) return status;

    ObjectHandle object = no_object;
    status = source_ohulk_beginobj2(processes, objects, input.process,
                                    *input.art, input.hulk_type,
                                    coordinates, object);
    if (!status) return status;

    SourceHulkSetupResult setup{};
    status = source_ohulk_setup_hulk(processes, input.process, objects,
                                     input.wave_records, *input.accounting,
                                     input.player_object_x, *input.rng,
                                     input.read_hcount, input.hcount_context,
                                     setup);
    if (!status) return status;

    status = source_hulk_do_diff(processes.cells[input.process]);
    if (!status) return status;

    SourceHulkJustSkResult initial_seek{};
    status = source_hulk_just_sk(processes, input.process, objects,
                                 input.player_torsos, *input.rng,
                                 input.read_hcount, input.hcount_context,
                                 initial_seek);
    if (!status) return status;

    status = source_hulk_get_stand(processes, objects, input.process,
                                   input.hulk_type);
    if (!status) return status;

    // OHULK ends with JRUC LUP_TOP, not SLEEP. Dispatch its immediate native
    // continuation now so the first source sleep (HALT/BUYIN/WAY) owns timing.
    SourceHulkLupTopWorld world{};
    world.halt = input.halt;
    world.hulk_down = input.hulk_down;
    world.ehalt = input.ehalt;
    world.hulk_type = input.hulk_type;
    world.player_torsos = input.player_torsos;
    world.rng = input.rng;
    world.read_hcount = input.read_hcount;
    world.hcount_context = input.hcount_context;
    world.player_processes = input.player_processes;
    world.hulk_count = &input.accounting->count;
    world.secondary_art = input.secondary_art;
    ProcessAction next_action{};
    status = source_hulk_lup_top(processes, objects, input.process,
                                 world, next_action);
    if (!status) return status;

    SourceOhulkInitializationResult result{};
    result.object = object;
    result.coordinates = coordinates;
    result.setup = setup;
    result.initial_seek = initial_seek;
    result.next_action = next_action;
    out = result;
    return {};
}

Status source_ohulk_start(NativeProcesses& processes, NativeObjects& objects,
                          const SourceOhulkStartInput& input,
                          SourceOhulkStartResult& out) {
    const ProcessHandle handle=input.actor.process;
    if(handle>=process_capacity||!processes.cells[handle].active||
       processes.cells[handle].id!=source_hulk_process_id)
        return Status::fail("OHULK entry requires an active HULKPID process",handle);

    // HULKS.ASM writes CFLAGS.STAND before GET_VALID_XY. Retain that write if
    // placement or any later source service fails.
    auto status=source_ohulk_mark_standing(processes.cells[handle]);
    if(!status)return status;
    if(!input.reservations)
        return Status::fail("GET_VALID_XY requires the room-owned KEEPXY/RAMXY reservations");
    if(!input.actor.rng)
        return Status::fail("GET_VALID_XY requires the room-owned UTIL RANDOM state");

    SourceGetValidXYRequest request=input.placement;
    request.hulk_actor=true;
    request.attract_mode=input.actor.attract_mode;
    request.hulk_type=input.actor.hulk_type;
    SourceGetValidXYResult placement{};
    status=source_get_valid_xy(request,*input.actor.rng,input.actor.read_hcount,
                               input.actor.hcount_context,*input.reservations,
                               placement);
    if(!status)return status;

    SourceOhulkInitialization actor=input.actor;
    actor.placement=placement.coordinates;
    SourceOhulkInitializationResult initialized{};
    status=source_ohulk_initialize(processes,objects,actor,initialized);
    if(!status)return status;

    SourceOhulkStartResult result{};
    result.placement=placement;
    result.actor=initialized;
    out=result;
    return {};
}

}
