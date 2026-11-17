#pragma once
#include "process.hpp"

namespace stv {
namespace source_exit_state {
constexpr u32 dexit_entry=0x45584901u;
constexpr u32 dexit_after_120=0x45584902u;
constexpr u32 dexit_after_10=0x45584903u;
constexpr u32 kilic_entry=0x4b494c01u;
constexpr u32 kilic_after_600=0x4b494c02u;
}
constexpr u16 source_game_state_inamode=0x0030u; // ROBO.EQU:INAMODE
constexpr u16 source_dexit_process_id=0;
constexpr u16 source_arrow_process_id=224; // ROBO.EQU:ARWPID
struct SourceDexitContext {u16 game_state{};};

// ENDWAVE.ASM:DEXIT's timed process cleanup and attract-mode terminal path.
// The context is the caller's current source GAMSTATE value; process state is
// represented only by native continuation tokens and remains snapshot-safe.
ProcessAction source_dexit_step(NativeProcesses&,ProcessHandle,void* context);
// ENDWAVE.ASM:KILIC, created as ARWPID by DEXIT.
ProcessAction source_kilic_step(NativeProcesses&,ProcessHandle,void* context);
}
