#pragma once
#include "base.hpp"
namespace stv {
struct SourceRng {u32 seed{};u32 random(u16 horizontal_count);u32 randu(u32 limit,u16 horizontal_count);u32 range(u32 lower,u32 upper,u16 horizontal_count);};
struct SourcePosition {u32 x{},y{},vx{},vy{};};
using SourceHCountRead=u16(*)(void* context);
struct SourceSpawnBounds {u32 x_min{},x_max{},y_min{},y_max{};}; // 16.16 source coordinates; RANGRAND excludes min and includes max; GV draws Y before X
struct SourceRectangle {u32 x_min{},x_max{},y_min{},y_max{};}; // inclusive, 16.16
// RAMXY entry as TCK consumes it: x is the X half shifted to 16.16; y is the
// whole OEXIT-packed [Y:X] long, which the source compares against 16.16 Y.
struct SourceSpawnPoint {u32 x{},y{};};
struct SourceSpawnReservations {
    static constexpr unsigned capacity=50; // ROBO.ASM MAX_ONSC / RAMXY
    SourceSpawnPoint points[capacity]{{0x003c0000u,0x003c003cu}}; // source [60,60] seed
    u8 count{1}; // live entries; the next entry is the source zero terminator
};
struct SourceSpawnResult {SourceSpawnPoint point{};unsigned attempts{};};
// Optional diagnostic: the X of each rejected GV candidate (the A0 value a
// retry re-enters GET_VALID_XY with). Not source state.
struct SourceSpawnRejections {u32* x{};unsigned capacity{},count{};};
struct SourceCirclePlacementInput {bool hulk_actor{true};bool attract_mode{};u16 wave{};u16 hulk_type{};u16 tmp{};};
struct SourceCirclePlacement {
    bool selected{};
    u16 tmp_after{};
    u8 point_index{};
    u16 pixel_x{},pixel_y{};
    u32 a0_x_16_16{},a1_packed_xy{},a3_keepxy{};
};
// Native OHULK A0/A1 register pair. A0 is 16.16 X. A1 is the source register
// value as returned: random placement is 16.16 Y, while CTBL carries packed Y:X.
struct SourceOhulkCoordinates {u32 a0_x_16_16{},a1_value{};};
enum class SourceSpawnBranch:u8 {random,circle};
struct SourceGetValidXYRequest {
    bool hulk_actor{true},attract_mode{};
    u16 wave{1},hulk_type{},tmp{16},from_door{};
    SourceSpawnBounds bounds{0x002a0000u,0x01570000u,0x00210000u,0x00c10000u}; // X [2Ah,157h], Y [21h,C1h]
    unsigned attempt_limit{1024};
    SourceSpawnRejections* rejections{};
};
struct SourceGetValidXYResult {
    SourceSpawnBranch branch{SourceSpawnBranch::random};
    SourceOhulkCoordinates coordinates{};
    SourceCirclePlacement circle{};
    SourceSpawnResult random{};
    u16 tmp_after{};
};
SourceSpawnBounds l8_source_hulk_spawn_bounds();
Status source_red_carpet(u16 from_door,SourceRectangle&);
// HULKS.ASM GET_VALID_XY CTBL circle branch only. False selection means the
// original code falls through to GV; invalid source states fail by name.
Status source_get_valid_xy_circle(const SourceCirclePlacementInput&,SourceSpawnReservations&,SourceCirclePlacement&);
// OHULK's post-GET_VALID_XY WAVEYX addition. Preserve the native A0/A1
// register representation; A3's packed KEEPXY reservation is separate.
Status source_apply_ohulk_waveyx(u32 base_x_16_16,u32 base_a1_value,
                                bool attract_mode,u32 wave_yx,SourceOhulkCoordinates&);
// HULKS.ASM GET_VALID_XY random (GV) branch only; FROMDR ownership and HCOUNT
// timing source remain with the caller.
Status source_get_valid_xy_random(SourceRng&,SourceHCountRead,void* hcount_context,
                                  const SourceSpawnBounds&,u16 from_door,
                                  SourceSpawnReservations&,SourceSpawnResult&,
                                  unsigned attempt_limit=1024,
                                  SourceSpawnRejections* rejections=nullptr);
// Source branch selector joining GET_VALID_XY's CTBL and GV paths. HCOUNT is
// an explicit input to the GV path; the caller still owns its exact timing.
Status source_get_valid_xy(const SourceGetValidXYRequest&,SourceRng&,
                           SourceHCountRead,void* hcount_context,
                           SourceSpawnReservations&,SourceGetValidXYResult&);
struct PlayerBounds {u32 left{},right{},top{},bottom{};};
// ROBO UPDATE_PLYRXY and STUFF_VEL. Direction numbers are the original unusual
// 1,2,4,5,6,7,8,9 scheme, not standard compass indices.
u16 player_direction(u8 original_move_bits,const SourcePosition&,const PlayerBounds&,bool in_intro);
Status player_velocity(u16 direction,u16& shoes,SourcePosition& legs,SourcePosition& torso);
u16 firing_direction(u8 original_shoot_bits);
struct BulletSetup {SourcePosition position{};u16 age{},flags{},id{};u8 compass_direction{};const char* image{};};
Status regular_bullet_setup(u16 direction,const SourcePosition& torso,BulletSetup&);
struct HulkSpeeds {u32 horizontal{},vertical{},diagonal_x{},diagonal_y{};};
struct HulkStandingFrame {const char* image{};u16 flags{};};
// HULKS.ASM:SPINFRMS/SPINFRMS2/SPINFLGS, indexed by source DIR 1..8.
Status hulk_standing_frame(u16 hulk_type,u16 direction,HulkStandingFrame& out);
Status hulk_speeds(u16 level,HulkSpeeds&);
u16 turn_hulk(u16 current,u16 target);
u16 hulk_direction_to(i32 animation_x,i32 object_y,i32 target_x,i32 target_y);
Status hulk_velocity(u16 direction,const HulkSpeeds&,SourcePosition&);
void advance_position(SourcePosition&);
}

namespace stv {
// SCORE.ASM BCDADD and SCRADD2, without unrelated DO_EXTRA presentation effects.
u32 source_bcd_add(u32 score,u32 amount);
struct SourceScore {u32 packed_bcd{};u32 next_extra{0x1500000};};
struct ScoreEffects {bool extra_life_requested{};};
ScoreEffects source_score_add(SourceScore&,u32 packed_amount,u16 player_status,bool attract_mode);
}
