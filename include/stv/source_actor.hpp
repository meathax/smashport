#pragma once

#include "stv/objects.hpp"
#include "stv/source_assets.hpp"

namespace stv {

// Inputs supplied by the native room owner at the boundaries that precede
// HULKS.ASM:OHULK (GET_VALID_XY and SET_WAVE/WAVEYX). This routine deliberately
// does not invent those services or their timing.
struct SourceOhulkInitialization {
    ProcessHandle process{no_process};
    const EditorImages* art{};
    u16 hulk_type{};
    SourceOhulkCoordinates placement{}; // Native A0/A1; A1 may be CTBL-packed Y:X
    bool attract_mode{};
    u32 wave_yx{};
    Buffer wave_records{};
    HulkAccounting* accounting{};
    const u16* player_object_x{}; // PLYROBJS[].OXPOS as consumed by SETUP_HULK
    const u16* player_torsos{};   // native handles for PLYROBJS[0..1]
    SourceRng* rng{};
    SourceHCountRead read_hcount{};
    void* hcount_context{};
    u16 halt{}, hulk_down{}, ehalt{};
    const ProcessHandle* player_processes{};
    const EditorImages* secondary_art{};
};

struct SourceOhulkInitializationResult {
    ObjectHandle object{no_object};
    SourceOhulkCoordinates coordinates{};
    SourceHulkSetupResult setup{};
    SourceHulkJustSkResult initial_seek{};
    ProcessAction next_action{};
};

struct SourceOhulkStartInput {
    SourceOhulkInitialization actor{};
    SourceGetValidXYRequest placement{};
    SourceSpawnReservations* reservations{};
};

struct SourceOhulkStartResult {
    SourceGetValidXYResult placement{};
    SourceOhulkInitializationResult actor{};
};

// Runs the source-ordered OHULK body after GET_VALID_XY has returned, then
// enters the measured LUP_TOP continuation in the same scheduler visit. The
// caller consumes next_action exactly as NativeProcesses::dispatch would.
Status source_ohulk_initialize(NativeProcesses&, NativeObjects&,
                               const SourceOhulkInitialization&,
                               SourceOhulkInitializationResult&);

// Full native OHULK entry from CFLAGS.STAND through GET_VALID_XY, WAVEYX,
// object/setup routines and the first LUP_TOP source sleep. Caller-owned beam
// timing and room globals remain explicit; missing services fail by name.
Status source_ohulk_start(NativeProcesses&, NativeObjects&,
                          const SourceOhulkStartInput&,
                          SourceOhulkStartResult&);

}
