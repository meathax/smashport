#pragma once
#include "process.hpp"
#include "gameplay.hpp"
#include "game_rules.hpp"

namespace stv {
struct EditorImages;
constexpr u16 source_hulk_process_id=236; // ROBO.EQU:HULKPID
constexpr u16 source_hulk_wave_type=6; // ROBO.EQU:HULK
constexpr u16 source_buyin_process_id=216; // ROBO.EQU:BUYINPID
// Native token for the first traced OHULK SLEEP 2 return. L8 A7=0xFFE3D6A0
// points at a branch to LUP_TOP; this token is a native state ID, never a PC.
constexpr u32 source_hulk_resume_lup_top=0x484c0001u;
// Native return sites for the two HULKS.ASM batting FRANIM calls. They are
// stable continuation IDs, never translated or host addresses.
constexpr u32 source_hulk_resume_bat_first=0x484c0002u;
constexpr u32 source_hulk_resume_bat_second=0x484c0003u;
// MPROC.EQU process-local stack bounds. A12 is represented as an offset in
// the native process cell, not the original CPU virtual address.
constexpr u16 source_process_stack_data_offset=0x02a0u;
constexpr u16 source_process_stack_end_offset=0x05a0u;

struct NativeObjects;

// HULKS.ASM fields are bit offsets relative to MPROC.EQU:PDATA. The native
// process data byte array is the pointer-free backing store for those bits.
enum class SourceHulkField:u8 {
    age, direction, distance, control_flags, seek_direction, level,
    horizontal_speed, diagonal_x_speed, vertical_speed, diagonal_y_speed,
    keep_age, oscillation, seek_player, batting
};

Status source_hulk_pdata_read(const NativeProcess&,SourceHulkField,u32&);
Status source_hulk_pdata_write(NativeProcess&,SourceHulkField,u32);
// MPROC.ASM:PRCSLP maps the source register image and sleep count into a
// native dispatcher action without retaining source/host pointers.
Status source_hulk_prcslp(NativeProcesses&,ProcessHandle,u16 delay,
                          const SourceProcessRegisters&,ProcessAction&);
// Initial OHULK sleep after BEGINOBJ2/SETUP_HULK. A8 is resolved from the
// process-linked native object. The remaining words are captured source
// values; A10 may be a source address and must stay opaque until translated.
struct SourceHulkInitialYieldContext {u32 a9{},a10{},a11{};u16 stack_offset{source_process_stack_end_offset};};
Status source_hulk_sleep_after_setup(NativeProcesses&,const NativeObjects&,
                                     ProcessHandle,u16 delay,
                                     const SourceHulkInitialYieldContext&,
                                     ProcessAction&);
// HULKS.ASM:DOLOGIC BUYINPID/EXISTP guard. The caller reaches this only after
// the source HALT/IBAT/CFLAGS/HULK_DN/EHALT decisions; a match takes DONE_SPIN
// and sleeps two ticks back to LUP_TOP, while no match falls through.
struct SourceHulkBuyinGate {bool sleep_two{};ProcessHandle process{no_process};};
Status source_hulk_buyin_gate(const NativeProcesses&,ProcessHandle,
                              SourceHulkBuyinGate&);
// HULKS.ASM:GET_STAND selects SPINFRMS/SPINFRMS2 and calls NDSP1.ASM:ANI
// on the process-linked native object. hulk_type is the captured HULKTYP word.
Status source_hulk_get_stand(NativeProcesses&,NativeObjects&,ProcessHandle,
                             u16 hulk_type);
// HULKS.ASM:GET_RUN chooses WALK_LIST[A/M/F] by type, level and direction,
// stores a stable native animation cursor in the A9 register slot, and clears
// CFLAGS.STAND. It replaces only the source image pointer representation.
Status source_hulk_get_run(NativeProcess&,u16 hulk_type);
struct SourceHulkDohulkStep {enum Kind:u8 {frame,script_restarted} kind{frame};ProcessAction action{};};
// HULKS.ASM:DOHULK / UTIL.ASM:FRANIM processes one L8 animation frame or,
// on the end marker, executes GET_RUN and reports the immediate LUP_TOP loop.
Status source_hulk_dohulk(NativeProcesses&,NativeObjects&,ProcessHandle,
                          u16 hulk_type,SourceHulkDohulkStep&,
                          const EditorImages* secondary_bank=nullptr);
// HULKS.ASM:DO_DIFF stores the four level-derived fixed-point speeds.
Status source_hulk_do_diff(NativeProcess&);
struct SourceHulkMvAgeResult {bool level_changed{};u16 age{};};
// The source-ordered HAGE/LEVEL prefix of HULKS.ASM:MV. A level transition
// commits HAGE=0 and the incremented LEVEL before DO_DIFF, then reloads HAGE
// from KPAGE only after that service succeeds.
Status source_hulk_mv_age(NativeProcess&,SourceHulkMvAgeResult&);
// HULKS.ASM:GET_DIS selects the source LEVTBL delay through one HCOUNT read.
Status source_hulk_get_dis(NativeProcess&,SourceRng&,SourceHCountRead,
                           void* hcount_context,u16& distance);
// HULKS.ASM:SETUP_DIR uses ROBO.ASM:PLYROBJS (P1/P2 torso objects).
struct SourceHulkDirectionResult {u8 player_slot{};u16 direction{};};
Status source_hulk_setup_dir(NativeProcesses&,ProcessHandle,const NativeObjects&,
                             const u16 player_torsos[2],
                             SourceHulkDirectionResult&);
// HULKS.ASM:JUST_SK preserves its source call order: GET_DIS (including its
// HCOUNT/RNG and DIS writes), then SETUP_DIR. A later SETUP_DIR boundary
// failure does not roll back the earlier source effects.
struct SourceHulkJustSkResult {u16 distance{};SourceHulkDirectionResult target{};};
Status source_hulk_just_sk(NativeProcesses&,ProcessHandle,const NativeObjects&,
                           const u16 player_torsos[2],SourceRng&,
                           SourceHCountRead,void* hcount_context,
                           SourceHulkJustSkResult&);
struct SourceHulkLupTopWorld {
    u16 halt{},hulk_down{},ehalt{},hulk_type{};
    const u16* player_torsos{}; // PLYROBJS[0..1]; source seek-target objects
    SourceRng* rng{}; // source UTIL.ASM:RANDOM state for completed MV seek decisions
    SourceHCountRead read_hcount{};
    void* hcount_context{};
    const ProcessHandle* player_processes{}; // ROBO.ASM:PLYRPRCS[0..1]; gang visibility bounds
    u16* hulk_count{}; // HULK_CNT owner required by the HULK_DN -> REALO path
    const EditorImages* secondary_art{}; // L8 animation frames split across IMG banks
};
// Native lowering of the measured LUP_TOP -> LP5 -> DOLOGIC entry and the
// no-match stand/facing path. MV includes HAGE/LEVEL, one-step spin turns,
// distance countdown/FRANIM, ordinary seeking and the gang on-screen gate.
// DEXIT scheduling remains owned by the room/wave controller.
Status source_hulk_lup_top(NativeProcesses&,NativeObjects&,ProcessHandle,
                           const SourceHulkLupTopWorld&,ProcessAction&);
// HULKS.ASM:INTO consumes the caller's A0 direction and writes its
// PDATA-speed-selected velocity to the linked object's velocity words.
// No position or timer changes.
Status source_hulk_into(NativeProcesses&,ProcessHandle,NativeObjects&,u16 direction);

struct SourceHulkSetupResult {
    u16 age_table_index{},hage{},level{},count{},remaining{};
    u8 seek_player{};
    bool create_dexit{};
};

// HULKS.ASM:OHULK's standing bit, before GET_VALID_XY/IH1/BEGINOBJ2.
Status source_ohulk_mark_standing(NativeProcess&);

// HULKS.ASM:OHULK tail + SETUP_HULK. Called only after BEGINOBJ2 linked the
// native object to this process. The source RANGRAND consumes an explicit
// HCOUNT sample. CREATE 0,DEXIT is returned as an effect for its owner.
Status source_ohulk_setup_hulk(NativeProcesses&,ProcessHandle,const NativeObjects&,Buffer wave_records,
                               HulkAccounting&,const u16 player_object_x[2],
                               SourceRng&,SourceHCountRead,void* hcount_context,
                               SourceHulkSetupResult&);
}
