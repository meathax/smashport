#pragma once
#include "objects.hpp"
namespace stv {
// Native translations of COLL2 SHORT_PLYR/SHORT2 and ROBO KILL_PLYR,
// TORSO_1/DELYDET, DOV. Caller owns graphics, processes, and service requests.
enum class PlayerDeathKind:u8 {none,spin,shock,chunks};
struct PlayerLife {
    u16 dead{}, immunity{40}, reserves{2};
    u8 number{1},countdown{};
    u32 deaths{},respawns{};
};
struct PlayerHitEffects {bool accepted{},clear_early_hulk_difficulty{},request_buyin{};};
Status source_short_player_hit(const NativeSprite& legs,const NativeSprite& killer,u16 killer_id,bool& hit);
PlayerHitEffects source_player_hit(PlayerLife&,u16 killer_id,bool shortened_box_hit,bool halt,bool attract,unsigned circuit,unsigned wave);
void source_torso_immunity_step(PlayerLife&);
PlayerDeathKind source_player_death_kind(u16 killer_id,bool face_on);
Status source_player_respawn(PlayerLife&);
PlayerBounds source_player_bounds(unsigned boundary_type,u32 world_x=0,u32 world_y=0);
// ROBOATT LDRS/RDRS fall through into LDRS2/RDRS2 before their terminator.
struct SourceMove {u16 bits,ticks;};
struct SourceMovement {const char* label;const SourceMove* moves;u16 count;u32 line;};
extern const SourceMovement source_movements[];
extern const unsigned source_movement_count;
const SourceMovement* find_source_movement(const char* label);
}
