#pragma once
#include "source_actor.hpp"
#include "wave_flow.hpp"

namespace stv {

constexpr u16 source_lord_process_id=223; // ROBO.EQU:LORDPID
// Native continuation token for HULKS.ASM:OHULK. L8 CREATE passes the wake
// PC 0xFFE3B8E0; this token is a native state ID, never a PC.
constexpr u32 source_hulk_entry_ohulk=0x484c0000u;
// HULKS.ASM:HTYP, indexed by @WAVE for circuits 2 and 3 (ONSCRN_HULKS).
Status source_onscreen_hulk_type(u16 circuit,u16 wave,u16 current_type,u16& out);

// HCOUNT is the TMS34010 beam counter. No measured native timing model exists
// yet, so the room owner reads it only from an explicit stream. Reading past
// the supplied values latches `exhausted` and the room fails at that tick
// instead of inventing a value.
struct SourceHCountStream {
    const u16* values{};
    u32 count{},position{};
    bool exhausted{};
};
u16 source_hcount_stream_read(void* stream);

// Values the room cannot yet derive itself. Each is an explicit input with a
// named origin; there is no default that silently makes a room playable.
struct SourceRoomStart {
    u32 rand{};                      // UTIL.ASM:RAND at room entry
    const u16* hcount{};             // HCOUNT reads, in source read order
    u32 hcount_count{};
    u16 from_door{};                 // ENDWAVE.ASM:FROMDR
    // PLYROBJS torso positions (16.16). These are fixed stand-ins until the
    // player processes are owned by the room; they are never actors.
    bool player_present[2]{};
    u32 player_x[2]{},player_y[2]{};
};

// Native owner of one retail room's MPROC process graph, starting at WAVES'
// CREATE LORDPID,OVERLORD. Currently connected: OVERLORD AP/ON1 with the
// ONSCRN_HULKS initial handler, OHULK entry and its LUP_TOP continuations.
// Every other TYP_TBL/TYP_TBL2 handler, WAVES' EHALT=2 text release, DEXIT,
// tanks, collisions and player processes stop at named boundaries.
struct SourceRoom {
    static constexpr unsigned wave_ram_bytes=8*12+2; // WAVE_RAM: 8 records + terminator
    NativeProcesses processes{};
    NativeObjects objects{};
    WaveOverlord overlord{};
    SourceRng rng{};
    SourceHCountStream hcount{};
    u16 tmp{},hulk_type{},from_door{};
    u8 wave_ram[wave_ram_bytes]{};
    ObjectHandle player_torsos[2]{no_object,no_object};
    ProcessHandle lord{no_process};
    const EditorImages* regular_art{};
    const EditorImages* snake_art{};
    u64 ticks{},onscreen_hulk_calls{},hulk_starts{};
    const char* boundary{};
    // Diagnostic only (excluded from digest): rejected GV candidate X values.
    u32 rejected_x[32]{};
    SourceSpawnRejections rejections{};
    bool started{};

    // Return to the unstarted state in place (the pools are large).
    void clear();
    Status start(const WaveOverlord& selected_room,const EditorImages& regular,
                 const EditorImages& snake,const SourceRoomStart&);
    // One MPROC PRCDSP pass with one elapsed interrupt tick.
    Status tick();
    Status validate() const;
    Sha256 digest() const;

    // Internal source services used by the native dispatcher.
    Status onscreen_hulks(u16 count);
    ProcessAction hulk_step(ProcessHandle);
    ProcessAction lord_step(ProcessHandle);
    void wave_ram_from_release();
    Status release_from_wave_ram();
    void reservations_from_world(SourceSpawnReservations&) const;
    Status world_from_reservations(const SourceSpawnReservations&);
};

}
