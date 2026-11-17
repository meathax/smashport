#pragma once
#include "gameplay.hpp"
#include "waves.hpp"
namespace stv {
// Original TANK.ASM kernels. These are NOT a complete scheduled tank actor.
struct TankSpawnRequest {bool create{},write_enon{};u8 enon{};};
TankSpawnRequest tank_spawn_request(u16 halted,u16 count,u8 maximum,u16 circuit);
unsigned tank_gate_from_random(u32 random_output); // RANGRAND(1,4) => 2..4
struct TankGateRead {bool resolved{};u8 value{};};
// Executed MAME 0.289 L8 witness: index-4 long at 0xffe3b270 is
// 0x18915610; the following byte read returned zero at PC 0xffe40e50.
// This is reference-profile evidence, not a claim about a physical PCB.
inline constexpr TankGateRead l8_reference_drinf_index4{true,0};
struct TankEntry {bool rejected{},set_econ{};u16 object_id{},direction{},distance{},gate_duration{};SourcePosition position{};};
Status tank_entry(unsigned gate,TankGateRead,u8 maximum,bool occupied,TankEntry&);
bool tank_door_contains(unsigned gate,u16 object_id,u8 maximum,u32 x,u32 y,u32 waveyx);
struct TankTurn {u16 direction{};bool increment{};};
Status tank_turn(u16 direction,u16 seek,TankTurn&);
const char* tank_stand_image(u16 direction,bool turret,u16& flags);
const char* tank_run_script(u16 direction); // null is original WALK_LIST zero, not an invented animation
Status tank_speeds(u16 level,bool reverse,HulkSpeeds&);
Status tank_velocity16(u16 direction,const HulkSpeeds&,SourcePosition&);
u16 tank_seek_from_random(u32 random_output); // may equal current: GET_SK then rerolls
u16 tank_distance_from_random(u32 random_output);
u16 tank_heading_code(u32 x_velocity,u32 y_velocity); // original GETHDR, NOT atan2
Status tank_convert_heading(u16 code,u16& direction); // preserves CVRT -> CVRT2 fallthrough at 16
Status tank_cannonball(u16 direction,const SourcePosition& turret,const SourcePosition& body,SourcePosition& out);
struct TankAccounting {u16 count{},done{},level{};};
Status tank_account_spawn(WaveRelease&,unsigned,TankAccounting&); // SETUP_TNK tail; no DEXIT
struct TankHit {bool ignored{},destroy{},flash{};u8 absorbed{};};
Status tank_hit(bool turret,u8 has_driver,u8 absorbed,unsigned weapon,u16 tank_hits,bool mutoid,TankHit&);
}
