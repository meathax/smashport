#pragma once
#include "base.hpp"
namespace stv {
// ROBODOOR.ASM native continuations. Gate numbers are original 1..4;
// gate 4 emits DOOR1 but has no animated object or KP counter.
struct SourceDoors {
    u8 timer[3]{}, view[3]{};
};
enum class DoorPhase:u8 {start,opening,holding,preclose,closing,top_finish,done};
struct DoorTask {u8 gate{1};u32 duration{};DoorPhase phase{DoorPhase::start};};
struct DoorRequest {bool create{},sound_only{};};
struct DoorEvent {u16 sleep{};u8 frame{};bool redraw{},sound{},done{};};
Status source_gate_request(SourceDoors&,unsigned gate,u32 duration,
                           bool first_circuit_closed_image,bool matching_process_exists,
                           DoorRequest&);
Status source_door_step(SourceDoors&,DoorTask&,bool halt,DoorEvent&);
const char* source_door_image(unsigned circuit,unsigned gate,unsigned frame);
constexpr u16 source_door_pid(unsigned gate) {return u16(224+gate);}
}
