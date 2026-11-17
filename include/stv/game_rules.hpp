#pragma once
#include "base.hpp"
namespace stv {
// Translated subset of the browser-inspected archive. Not a retail revision claim.
struct Difficulty {
 u16 max_free{},grunt_mod{},tank_diff{},tank_diff2{},ring_time{},cobra_kill{},fat_hits{},tank_hits{};
};
Status set_difficulty(unsigned adjustment,Difficulty& out);
struct SpawnCaps {
 u8 max_hulk{},max_tank{},max_fat{},max_orb{},max_robot{},max_gal{};
 u16 robot_type{},gal_type{};
};
struct PaletteCycleRequest {bool start{};u16 first{},last{},period_ticks{};};
Status set_maximums(unsigned circuit,unsigned wave,u16 player_status,
                   const Difficulty& difficulty,SpawnCaps& in_out,PaletteCycleRequest& effect);
u32 one_player_adjust(u32 value,u32 reduction,u16 player_status);
u32 adjust_maximum(u32 value,u16 grunt_mod);
struct RingPush {u16 leg_x{},leg_y{},torso_x{},torso_y{};};
void push_ring_away_from_boss(RingPush& player,u16 boss_x,u16 boss_y);
struct HulkAge {u16 age{},level{},reload{};};
// true means the original code calls DO_DIFF; that callee is not implemented here.
bool advance_hulk_age(HulkAge& age);
struct PackedWaveRecord {u16 type{},remaining{},level{},rate{},on_screen{},counter{};};
Status find_wave_record(Bytes records,u16 wanted,usize& byte_offset,PackedWaveRecord& out);
struct HulkAccounting {u16 count{},done{};};
struct HulkSpawnEffect {u8 seek_player{};bool create_dexit{};};
Status account_hulk_spawn(Buffer records,u16 hulk_type,HulkAccounting& state,
                          const u16 player_object_x[2],HulkSpawnEffect& effect);
}
