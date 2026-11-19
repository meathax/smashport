#include "stv/game_rules.hpp"
namespace stv {
namespace {
// Source: historicalsource/smashtv, ROBO.ASM:SETDIFF and adjacent named tables.
// The source has an extra table entry at index 10, but documents adjustments 0..9.
constexpr u16 free_table[11]={4,4,3,2,2,2,1,1,0,0,0};
constexpr i32 grunt_table[11]={-4,-3,-2,-1,0,0,1,3,4,6,6};
constexpr i32 tank_table[11]={-3,-2,-1,-1,0,0,1,2,3,4,4};
constexpr u16 tank2_table[11]={12,11,10,9,8,8,7,6,5,4,4};
constexpr u16 ring_table[11]={700,600,500,400,300,300,300,200,150,110,110};
constexpr u16 wound_table[11]={900,1400,2400,2900,3400,3800,3900,4400,4900,4900,4900};
constexpr u16 fat_hits[11]={1,3,6,8,9,10,12,15,18,20,20};
constexpr u16 tank_hits[11]={3,6,12,17,20,25,30,35,45,50,50};
// Source: ROBO.ASM:SETMAX/SET0/DOC1 and their tables. Unequal table lengths
// are intentional. Do NOT pad them to hide an unproved out-of-range wave.
constexpr u8 gal_types[]={0,1,1,0,1,0,1,0,1,1,0,0,0,1,1,1,1,1,1,0,0,0,0};
constexpr u8 robot_types[]={0,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1,1,0,1,0,0,0,0};
constexpr u8 hmax[]={2,12,11,25,32,20,20,25,29,24,32,32};
constexpr u8 tmax[]={1,1,1,1,1,1,1,1,1,1,1,1};
constexpr u8 fmax[]={2,3,3,2,7,10,6,6,11,8,5};
constexpr u8 omax[]={2,20,20,25,25,25,13,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25};
constexpr u8 rmax[]={2,8,20,23,18,18,18,18,15,13,18,15,23,18,18,18,21,16,18,18,25,25,25,25};
constexpr u8 rmax2[]={2,8,20,23,18,32,18,18,15,13,18,15,23,18,18,23,24,16,18,26,25,25,25,25};
constexpr u8 gmax[]={2,20,20,20,20,12,7,25,13,25,25,16,25,4,5,25,12,25,22,25};
constexpr u8 gmax2[]={2,8,10,10,10,12,7,15,13,15,15,16,15,4,5,8,12,15,12,15,16,15};
constexpr u8 hmax2[]={2,15,15,29,32,20,26,25,23,24,32,28,25,21,25,25,22,25,29,25,30,25};
}
Status set_difficulty(unsigned index,Difficulty& out){
 if(index>9)return Status::fail("SETDIFF documented adjustment range is 0..9",index);
 out={free_table[index],u16(grunt_table[index]),u16(tank_table[index]),tank2_table[index],ring_table[index],wound_table[index],fat_hits[index],tank_hits[index]};return {};
}
u32 one_player_adjust(u32 value,u32 reduction,u16 player_status){return player_status==3?value:value-reduction;}
u32 adjust_maximum(u32 value,u16 modifier){return value+u32(signed16(modifier));}
Status set_maximums(unsigned circuit,unsigned wave,u16 status,const Difficulty& difficulty,SpawnCaps& s,PaletteCycleRequest& effect){
 if(circuit>2)return Status::fail("Only source circuits 0..2 are mapped",circuit);
 unsigned limit=circuit==0?unsigned(sizeof fmax):circuit==1?unsigned(sizeof gmax):unsigned(sizeof hmax2);
 if(wave>=limit)return Status::fail("Wave exceeds the shortest source table; no padding permitted",wave);
 effect={};s.max_fat=5;s.gal_type=0;
 if(circuit==0){s.max_hulk=u8(adjust_maximum(hmax[wave],difficulty.grunt_mod));s.max_tank=tmax[wave];s.max_fat=fmax[wave];return {};}
 s.robot_type=robot_types[wave];
 if(circuit==1){s.max_robot=u8(adjust_maximum(one_player_adjust(rmax[wave],2,status),difficulty.grunt_mod));s.max_orb=omax[wave];s.max_gal=u8(one_player_adjust(gmax[wave],2,status));s.max_fat=5;s.max_tank=1;return {};}
 s.max_robot=u8(adjust_maximum(one_player_adjust(rmax2[wave],5,status),difficulty.grunt_mod));
 s.max_orb=u8(one_player_adjust(omax[wave],2,status));s.gal_type=gal_types[wave];
 if(s.gal_type)effect={true,32,45,3}; // SPDRP/PAL1 request only; palette service not yet connected.
 s.max_gal=u8(one_player_adjust(gmax2[wave],2,status));
 s.max_hulk=u8(adjust_maximum(one_player_adjust(hmax2[wave],5,status),difficulty.grunt_mod));
 s.max_tank=u8(wave==1?3:wave==11?4:1);return {};
}
void push_ring_away_from_boss(RingPush& p,u16 bx,u16 by){
 // ROBO.ASM:OFFRNG/GONEG/GOGO/GOGO1/GOGO2. Integer halves of 16.16 fields.
 i32 dx=signed16(bx)+20>signed16(p.leg_x)?-3:3;
 p.leg_x=u16(u32(p.leg_x)+u32(dx));p.torso_x=u16(u32(p.torso_x)+u32(dx));
 if(signed16(p.leg_y)>=0x182){i32 dy=signed16(by)+20>signed16(p.leg_y)?-3:3;p.leg_y=u16(u32(p.leg_y)+u32(dy));p.torso_y=u16(u32(p.torso_y)+u32(dy));}
}
bool advance_hulk_age(HulkAge& h){
 // HULKS.ASM:MV through C7, excludes DO_DIFF and process continuation integration.
 h.age=u16(u32(h.age&0x03ffu)-1u);
 if(h.age!=0 || h.level==10)return false;
 h.level=u16(u32(h.level)+1);h.age=h.reload;return true;
}
Status find_wave_record(Bytes records,u16 wanted,usize& offset,PackedWaveRecord& out){
 // HULKS.ASM:GET_WVADDR/HU1. Bound checks are host safety for invalid inputs.
 if(!wanted)return Status::fail("Zero is a wave terminator, not an enemy type");
 for(usize p=0;within(p,2,records.size);p+=12){u16 type=le16(records.data+p);if(!type)return Status::fail("Enemy type not present before wave terminator",p);if(!within(p,12,records.size))return Status::fail("Truncated six-word wave record",p);if(type==wanted){const u8* q=records.data+p;out={type,le16(q+2),le16(q+4),le16(q+6),le16(q+8),le16(q+10)};offset=p;return {};}}
 return Status::fail("Wave record table has no reachable terminator",records.size);
}
Status account_hulk_spawn(Buffer records,u16 hulk_type,HulkAccounting& a,const u16 player_x[2],HulkSpawnEffect& effect){
 // HULKS.ASM:SETUP_HULK, accounting tail only. RNG/age setup remain unported.
 usize at=0;PackedWaveRecord record;auto s=find_wave_record(records,hulk_type,at,record);if(!s)return s;
 a.count=u16(u32(a.count)+1);u8 target=u8(a.count&1);if(player_x[target]==0)target^=1;
 record.remaining=u16(u32(record.remaining)-1);put16(records.data+at+2,record.remaining);
 effect={target,record.remaining==0};if(effect.create_dexit)a.done=1;return {};
}
}
