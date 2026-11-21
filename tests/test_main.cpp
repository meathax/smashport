#include "stv/assets.hpp"
#include "stv/game_rules.hpp"
#include "stv/gameplay.hpp"
#include "stv/sound_commands.hpp"
#include "stv/input.hpp"
#include "stv/reference_timing.hpp"
#include "stv/coin_service.hpp"
#include "stv/switch_scan.hpp"
#include "stv/render.hpp"
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>
using namespace stv;
namespace {
u64 checks=0;unsigned groups=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x);}while(false)
void group(const char* name,const std::function<void()>& f){f();++groups;std::cout<<"PASS "<<name<<"\n";}
Bytes v(const std::vector<u8>& b){return {b.data(),b.size()};}Buffer b(std::vector<u8>& x){return {x.data(),x.size()};}
std::string sha_text(Bytes x){char s[65];hex(sha256(x).bytes,32,s);return s;}
u32 random_state=0x71912345;u32 rnd(){random_state=random_state*1664525u+1013904223u;return random_state;}
struct File {std::string name;std::vector<u8> data;};
struct HCountTrace {u16 value{};unsigned calls{};};
u16 traced_hcount(void* context){auto& trace=*static_cast<HCountTrace*>(context);++trace.calls;return trace.value;}
void append16(std::vector<u8>& z,u16 x){usize p=z.size();z.resize(p+2);put16(z.data()+p,x);}void append32(std::vector<u8>& z,u32 x){usize p=z.size();z.resize(p+4);put32(z.data()+p,x);}void text(std::vector<u8>& z,const std::string& s){z.insert(z.end(),s.begin(),s.end());}
// Minimal independent fixture writer: stored ZIP only, no parser code reused.
std::vector<u8> zip_fixture(const std::vector<File>& files){std::vector<u8> z;std::vector<u32> offsets;for(auto& f:files){offsets.push_back(u32(z.size()));append32(z,0x04034b50);append16(z,20);append16(z,0);append16(z,0);append16(z,0);append16(z,0);append32(z,crc32(v(f.data)));append32(z,u32(f.data.size()));append32(z,u32(f.data.size()));append16(z,u16(f.name.size()));append16(z,0);text(z,f.name);z.insert(z.end(),f.data.begin(),f.data.end());}
 u32 start=u32(z.size());for(usize i=0;i<files.size();++i){const auto& f=files[i];append32(z,0x02014b50);append16(z,20);append16(z,20);append16(z,0);append16(z,0);append16(z,0);append16(z,0);append32(z,crc32(v(f.data)));append32(z,u32(f.data.size()));append32(z,u32(f.data.size()));append16(z,u16(f.name.size()));append16(z,0);append16(z,0);append16(z,0);append16(z,0);append32(z,0);append32(z,offsets[i]);text(z,f.name);}
 u32 size=u32(z.size())-start;append32(z,0x06054b50);append16(z,0);append16(z,0);append16(z,u16(files.size()));append16(z,u16(files.size()));append32(z,size);append32(z,start);append16(z,0);return z;
}
void save(const std::filesystem::path& p,Bytes data){std::ofstream f(p,std::ios::binary);CHECK(f.good());f.write(reinterpret_cast<const char*>(data.data),static_cast<std::streamsize>(data.size));CHECK(f.good());}
}
int main(int argc,char** argv){try{
 group("fixed hash vectors",[]{CHECK(sha_text({nullptr,0})=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");const u8 abc[]={'a','b','c'};CHECK(sha_text({abc,3})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");char s[41];hex(sha1({abc,3}).bytes,20,s);CHECK(std::string(s)=="a9993e364706816aba3e25717850c26c9cd0d89d");const char* digits="123456789";CHECK(crc32({reinterpret_cast<const u8*>(digits),9})==0xcbf43926);std::vector<u8> million(1000000,'a');CHECK(sha_text(v(million))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");u8 raw[3];CHECK(unhex("00abFF",raw,3));CHECK(raw[0]==0&&raw[1]==0xab&&raw[2]==255);CHECK(!unhex("00abfg",raw,3));});
 group("packed bit fields and no adjacent writes",[]{for(unsigned width=1;width<=32;++width)for(unsigned start=0;start<96;++start){u8 mem[20],old[20];for(auto& x:mem)x=u8(rnd());copy(old,mem,sizeof mem);u32 value=rnd();CHECK(write_field({mem,sizeof mem},{start},width,value));for(unsigned bit=0;bit<160;++bit){unsigned expected=bit>=start&&bit<start+width?((value>>(bit-start))&1u):((old[bit/8]>>(bit%8))&1u);CHECK(((mem[bit/8]>>(bit%8))&1u)==expected);}u32 out=0;CHECK(read_field({mem,sizeof mem},{start},width,false,out));u32 mask=width==32?~0u:(1u<<width)-1;CHECK(out==(value&mask));CHECK(read_field({mem,sizeof mem},{start},width,true,out));i64 expected=value&mask;if(width<32 && (value&(1u<<(width-1))))expected-=i64(1)<<width;CHECK(out==u32(expected));}u8 mem[2]={0xaa,0xbb};u32 out=0x12345678;CHECK(!write_field({mem,2},{15},2,0));CHECK(mem[0]==0xaa&&mem[1]==0xbb);CHECK(!read_field({mem,2},{0},0,false,out));CHECK(out==0x12345678);CHECK(!read_field({mem,2},{0xffffffff},32,false,out));});
 group("defined arithmetic and flags against wide integer math",[]{for(unsigned i=0;i<100000;++i){u32 a=rnd(),c=rnd();auto add=add32(a,c),sub=sub32(a,c);u64 wide=u64(a)+c;i64 wide_signed=signed32(a)+signed32(c);CHECK(add.value==u32(wide));CHECK(add.flags.c==(wide>0xffffffffull));CHECK(add.flags.v==(wide_signed>2147483647ll||wide_signed<(-2147483647ll-1)));CHECK(sub.value==u32(u64(a)-c));CHECK(sub.flags.c==(a<c));i64 diff=signed32(a)-signed32(c);CHECK(sub.flags.v==(diff>2147483647ll||diff<(-2147483647ll-1)));}CHECK(add32(0x7fffffff,1).flags.v);CHECK(add32(~0u,1).flags.z&&add32(~0u,1).flags.c);CHECK(sub32(0x80000000,1).flags.v);CHECK(asr(0x80000000,32)==~0u);CHECK(asr(0x80000000,1)==0xc0000000);CHECK(sign_extend(0x80,8)==0xffffff80);CHECK(sign_extend(1,0)==0);CHECK(sign_extend(123,33)==123);});
 group("ROBO SETDIFF documented range and fixtures",[]{constexpr int grunt[10]={-4,-3,-2,-1,0,0,1,3,4,6};constexpr int rings[10]={700,600,500,400,300,300,300,200,150,110};for(unsigned i=0;i<10;++i){Difficulty d;CHECK(set_difficulty(i,d));CHECK(signed16(d.grunt_mod)==grunt[i]);CHECK(d.ring_time==rings[i]);}Difficulty d;CHECK(set_difficulty(4,d));CHECK(d.max_free==2&&d.fat_hits==9&&d.tank_hits==20&&d.cobra_kill==3400);auto old=d;CHECK(!set_difficulty(10,d));CHECK(equal(&old,&d,sizeof d));});
 group("ROBO ONEPADJ and ADJMAX modular behavior",[]{for(u16 s=0;s<8;++s){CHECK(one_player_adjust(2,5,s)==(s==3?2u:0xfffffffdu));}CHECK(adjust_maximum(2,0xfffc)==0xfffffffeu);CHECK(adjust_maximum(0xfffffffe,4)==2);});
 group("ROBO SETMAX preserves untouched fields and table bounds",[]{Difficulty d;CHECK(set_difficulty(4,d));SpawnCaps s{99,98,97,96,95,94,93,92};PaletteCycleRequest e;CHECK(set_maximums(0,0,1,d,s,e));CHECK(s.max_hulk==2&&s.max_tank==1&&s.max_fat==2);CHECK(s.max_orb==96&&s.max_robot==95&&s.max_gal==94&&s.robot_type==93&&s.gal_type==0);CHECK(set_maximums(1,5,1,d,s,e));CHECK(s.max_robot==16&&s.max_orb==25&&s.max_gal==10&&s.max_hulk==2);CHECK(set_maximums(2,1,1,d,s,e));CHECK(s.max_hulk==10&&s.max_robot==3&&s.max_gal==6&&s.max_tank==3&&s.max_orb==18);CHECK(e.start&&e.first==32&&e.last==45&&e.period_ticks==3);CHECK(set_maximums(2,11,3,d,s,e));CHECK(s.max_tank==4&&s.max_hulk==28);auto old=s;CHECK(!set_maximums(0,11,3,d,s,e));CHECK(equal(&s,&old,sizeof s));CHECK(!set_maximums(1,20,3,d,s,e));CHECK(!set_maximums(2,22,3,d,s,e));CHECK(!set_maximums(3,0,3,d,s,e));CHECK(set_difficulty(0,d));CHECK(set_maximums(0,0,1,d,s,e));CHECK(s.max_hulk==254);for(unsigned circuit=0;circuit<3;++circuit)for(unsigned wave=0;wave<(circuit==0?11u:circuit==1?20u:22u);++wave)for(unsigned difficulty=0;difficulty<10;++difficulty)for(u16 player=1;player<=3;++player){CHECK(set_difficulty(difficulty,d));CHECK(set_maximums(circuit,wave,player,d,s,e));}});
 group("ROBO OFFRNG paired objects and exact signed Y cutoff",[]{
  RingPush p{100,100,110,110};push_ring_away_from_boss(p,100,100);CHECK(p.leg_x==97&&p.torso_x==107&&p.leg_y==100&&p.torso_y==110);
  p={120,386,130,396};push_ring_away_from_boss(p,100,100);CHECK(p.leg_x==123&&p.torso_x==133&&p.leg_y==389&&p.torso_y==399);
  for(u16 y=385;y<=387;++y){p={120,y,130,u16(y+10)};push_ring_away_from_boss(p,100,400);CHECK(p.leg_y==(y<386?y:u16(y-3)));CHECK(p.torso_y==(y<386?u16(y+10):u16(y+7)));}
 p={0xffff,0xffff,0xffff,0xffff};push_ring_away_from_boss(p,0,0);CHECK(p.leg_x==0xfffc&&p.leg_y==0xffff);
 });
 group("SOUNDS generated source catalog and command tables",[]{
  CHECK(source_sound_table_count==110);
  const auto* shot=find_source_sound("SHOT");const auto* rifle=find_source_sound("RIFL");const auto* icon=find_source_sound("ICSND");
  CHECK(shot&&rifle&&icon&&shot->source_line>0&&shot->word_count==4);
  CHECK(shot->words[0]==0xf0d7&&shot->words[1]==5&&shot->words[2]==0x80c4&&shot->words[3]==0);
  CHECK(rifle->words[0]==0xf4d8&&icon->words[2]==0x8092);
  CHECK(sound_shot[0]==shot->words[0]&&sound_rifle[2]==rifle->words[2]&&sound_pickup[0]==icon->words[0]);
  CHECK(!find_source_sound("shot")&&!find_source_sound(nullptr));
  CHECK(find_source_sound("CVSDOFF")->words[0]==0xf8ef&&find_source_sound("MUSICOFF")->words[0]==0xfbec);
  for(usize i=0;i<source_sound_table_count;++i){
   const auto& table=source_sound_tables[i];CHECK(table.label&&table.words&&table.source_line&&table.word_count>=2&&table.word_count<=256);
   CHECK(table.words[0]>=0xf000&&table.words[table.word_count-1]==0);
   SoundCommands sound;SoundWrite writes[128];SoundTrace trace{writes,128};bool accepted=false;
   CHECK(sound.load(table.words,table.word_count,0,trace,accepted)&&accepted);
  }
  u16 level[5];CHECK(volume_table(0,level));const auto* volume=find_source_sound("VOLUME0");CHECK(volume&&volume->word_count==5);
  CHECK(equal(level,volume->words,sizeof(level)));
 });
 group("HULKS age mask, decrement, level cap and DO_DIFF request",[]{HulkAge h{0x401,2,100};CHECK(advance_hulk_age(h));CHECK(h.age==100&&h.level==3);h={1,10,70};CHECK(!advance_hulk_age(h));CHECK(h.age==0&&h.level==10);CHECK(!advance_hulk_age(h));CHECK(h.age==65535);h={1,11,6};CHECK(advance_hulk_age(h));CHECK(h.level==12);});
 group("HULKS wave search and spawn accounting subset",[]{u8 table[26]{};u16 words[12]={0x1111,2,0,6,4,30,0x2222,5,0,90,0,1320};for(unsigned i=0;i<12;++i)put16(table+i*2,words[i]);PackedWaveRecord r;usize offset=999;CHECK(find_wave_record({table,26},0x2222,offset,r));CHECK(offset==12&&r.remaining==5&&r.counter==1320);CHECK(!find_wave_record({table,26},0x3333,offset,r));CHECK(!find_wave_record({table,7},0x1111,offset,r));HulkAccounting a;HulkSpawnEffect e;u16 players[2]={100,0};CHECK(account_hulk_spawn({table,26},0x1111,a,players,e));CHECK(a.count==1&&e.seek_player==0&&!e.create_dexit&&le16(table+2)==1);CHECK(account_hulk_spawn({table,26},0x1111,a,players,e));CHECK(a.count==2&&e.create_dexit&&a.done==1&&le16(table+2)==0);CHECK(account_hulk_spawn({table,26},0x1111,a,players,e));CHECK(le16(table+2)==65535);});
 group("HULKS GET_VALID_XY random branch, carpet and RAMXY spacing",[]{
  constexpr u32 px=0x10000u;
  auto source_bounds=l8_source_hulk_spawn_bounds();
  CHECK(source_bounds.x_min==0x2a0000u&&source_bounds.x_max==0x1570000u&&source_bounds.y_min==0x210000u&&source_bounds.y_max==0xc10000u);
  const u32 expected[5][4]={{0,248,72,184},{152,512,72,184},{0,248,72,184},{136,248,68,255},{136,248,0,200}};
  for(u16 door=0;door<5;++door){SourceRectangle r;CHECK(source_red_carpet(door,r));CHECK(r.x_min==expected[door][0]*px&&r.x_max==expected[door][1]*px&&r.y_min==expected[door][2]*px&&r.y_max==expected[door][3]*px);}
  SourceRectangle untouched{1,2,3,4};CHECK(!source_red_carpet(5,untouched));CHECK(untouched.x_min==1&&untouched.y_max==4);
  SourceRng rng{1};HCountTrace hcount{0,0};SourceSpawnReservations reserved;SourceSpawnResult result{{0xdeadbeefu,0xbeefu},77};
  SourceSpawnBounds one_point{300*px-1,300*px,300*px-1,300*px};
  CHECK(source_get_valid_xy_random(rng,traced_hcount,&hcount,one_point,4,reserved,result));
  CHECK(result.attempts==1&&result.point.x==300*px&&result.point.y==300*px&&reserved.count==2&&reserved.points[1].x==300*px&&reserved.points[1].y==(300*px|300u)&&hcount.calls==2);
  SourceSpawnReservations carpet_reserved;SourceSpawnResult carpet_result{{0xdeadbeefu,0xbeefu},77};SourceRng carpet_rng{9};HCountTrace carpet_hcount{0,0};
  SourceSpawnBounds carpet_edge{136*px-1,136*px,68*px-1,68*px};
  auto carpet_failure=source_get_valid_xy_random(carpet_rng,traced_hcount,&carpet_hcount,carpet_edge,3,carpet_reserved,carpet_result,3);
  CHECK(!carpet_failure&&text_equal(carpet_failure.message,"GET_VALID_XY random branch exhausted native retry cap")&&carpet_failure.offset==3);
  CHECK(carpet_hcount.calls==6&&carpet_reserved.count==1&&carpet_reserved.points[0].x==60*px&&carpet_reserved.points[0].y==(60*px|60u)&&carpet_result.attempts==77&&carpet_result.point.x==0xdeadbeefu);
  SourceSpawnReservations overlap;overlap.points[1]={100*px,100*px};overlap.count=2;SourceSpawnResult overlap_result{};SourceRng overlap_rng{10};HCountTrace overlap_hcount{0,0};
  SourceSpawnBounds x_edge{126*px-1,126*px,100*px-1,100*px};
  CHECK(!source_get_valid_xy_random(overlap_rng,traced_hcount,&overlap_hcount,x_edge,4,overlap,overlap_result,2));
  CHECK(overlap_hcount.calls==4&&overlap.count==2);
  SourceSpawnBounds y_edge{126*px-1,126*px,128*px-1,128*px};SourceRng y_rng{11};HCountTrace y_hcount{0,0};
  CHECK(!source_get_valid_xy_random(y_rng,traced_hcount,&y_hcount,y_edge,4,overlap,overlap_result,1));
  CHECK(y_hcount.calls==2&&overlap.count==2);
  SourceSpawnBounds clear_edge{127*px-1,127*px,129*px-1,129*px};SourceRng clear_rng{12};HCountTrace clear_hcount{0,0};
  CHECK(source_get_valid_xy_random(clear_rng,traced_hcount,&clear_hcount,clear_edge,4,overlap,overlap_result));
  CHECK(overlap_result.attempts==1&&overlap_result.point.x==127*px&&overlap_result.point.y==129*px&&overlap.count==3);
  SourceSpawnReservations full;full.count=SourceSpawnReservations::capacity;SourceSpawnResult full_result{{7,8},9};SourceRng full_rng{13};HCountTrace full_hcount{0,0};
  auto full_failure=source_get_valid_xy_random(full_rng,traced_hcount,&full_hcount,one_point,4,full,full_result);
  CHECK(!full_failure&&full_failure.offset==SourceSpawnReservations::capacity&&full_hcount.calls==0&&full_result.attempts==9&&full_result.point.x==7);
  SourceSpawnReservations bad_door;SourceSpawnResult bad_result{};SourceRng bad_rng{14};HCountTrace bad_hcount{0,0};
  CHECK(!source_get_valid_xy_random(bad_rng,traced_hcount,&bad_hcount,one_point,5,bad_door,bad_result));CHECK(bad_hcount.calls==0&&bad_door.count==1);
 });
 group("HULKS GET_VALID_XY CTBL circle tables, source guards and MOVX/MOVY packing",[]{
  constexpr u16 tables[3][16][2]={
   {{0x50,0x28},{0x50,0x48},{0x50,0x68},{0x50,0x88},{0x42,0xa8},{0x3c,0xc8},{0x48,0xe8},{0x62,0xfc},{0x98,0x28},{0x98,0x48},{0x98,0x68},{0x98,0x88},{0xa6,0xa8},{0xac,0xc8},{0xa0,0xe8},{0x86,0xfc}},
   {{0x20,0xa4},{0x35,0xa4},{0x4a,0xa4},{0x5f,0x9c},{0x78,0x92},{0x95,0x92},{0xaa,0xa0},{0xb7,0xb7},{0x20,0xee},{0x35,0xee},{0x4a,0xee},{0x5f,0xf6},{0x78,0x100},{0x95,0x100},{0xaa,0xf8},{0xb7,0xe1}},
   {{0xd0,0xa4},{0xbb,0xa4},{0xa6,0xa4},{0x91,0x9c},{0x78,0x92},{0x5b,0x92},{0x46,0xa0},{0x39,0xb7},{0xd0,0xee},{0xbb,0xee},{0xa6,0xee},{0x91,0xf6},{0x78,0x100},{0x5b,0x100},{0x46,0xf8},{0x39,0xe1}}
  };
  constexpr u16 waves[6]={3,4,5,6,7,8};constexpr u8 table_for_wave[6]={2,0,0,1,0,2};
  for(unsigned w=0;w<6;++w){SourceSpawnReservations keepxy;
   for(u16 tmp=16;tmp>0;--tmp){
   SourceCirclePlacement out{};SourceCirclePlacementInput in{true,false,waves[w],0,tmp};
   CHECK(source_get_valid_xy_circle(in,keepxy,out));const u16 x=tables[table_for_wave[w]][tmp-1][0],y=tables[table_for_wave[w]][tmp-1][1];
   const u32 packed=(u32(y)<<16)|x;
   CHECK(out.selected&&out.tmp_after==tmp-1&&out.point_index==tmp-1);
   CHECK(out.pixel_x==x&&out.pixel_y==y&&out.a0_x_16_16==(u32(x)<<16));
   CHECK(out.a1_packed_xy==packed&&out.a3_keepxy==packed);
   CHECK(keepxy.count==unsigned(18-tmp)&&keepxy.points[keepxy.count-1].x==(u32(x)<<16)&&keepxy.points[keepxy.count-1].y==packed);
   }
  }
  const SourceCirclePlacementInput fallback[]={
   {false,false,3,0,16},{true,true,3,0,16},{true,false,2,0,16},
   {true,false,9,0,16},{true,false,3,2,16},{true,false,3,0,0}
  };
  for(const auto& in:fallback){SourceSpawnReservations keepxy;SourceCirclePlacement out{true,9,8,7,6,5,4,3};CHECK(source_get_valid_xy_circle(in,keepxy,out));CHECK(!out.selected&&out.tmp_after==0&&out.a3_keepxy==0&&keepxy.count==1);}
  SourceCirclePlacement unchanged{true,9,8,7,6,5,4,3};
  SourceSpawnReservations unchanged_keepxy;
  auto bad_wave=source_get_valid_xy_circle({true,false,11,0,1},unchanged_keepxy,unchanged);
  CHECK(!bad_wave&&text_equal(bad_wave.message,"GET_VALID_XY CTBL wave outside source table")&&bad_wave.offset==11);
  CHECK(unchanged.selected&&unchanged.tmp_after==9&&unchanged.a3_keepxy==3);
  auto bad_tmp=source_get_valid_xy_circle({true,false,3,0,17},unchanged_keepxy,unchanged);
  CHECK(!bad_tmp&&text_equal(bad_tmp.message,"GET_VALID_XY CTBL TMP outside 16-point table")&&bad_tmp.offset==17);
  CHECK(unchanged.selected&&unchanged.tmp_after==9&&unchanged.a3_keepxy==3);
  SourceSpawnReservations full_keepxy;full_keepxy.count=SourceSpawnReservations::capacity;SourceCirclePlacement full_out{true,8,7,6,5,4,3,2};
  auto full=source_get_valid_xy_circle({true,false,3,0,1},full_keepxy,full_out);
  CHECK(!full&&text_equal(full.message,"GET_VALID_XY OEXIT KEEPXY has no room")&&full.offset==SourceSpawnReservations::capacity);
  CHECK(full_keepxy.count==SourceSpawnReservations::capacity&&full_out.selected&&full_out.tmp_after==8&&full_out.a3_keepxy==2);
  SourceSpawnReservations invalid_keepxy;invalid_keepxy.count=0;
  auto invalid=source_get_valid_xy_circle({true,false,3,0,1},invalid_keepxy,full_out);
  CHECK(!invalid&&text_equal(invalid.message,"GET_VALID_XY OEXIT KEEPXY live count is invalid")&&invalid_keepxy.count==0);
 });
 group("HULKS GET_VALID_XY selects CTBL or GV and returns native A0/A1 registers",[]{
  SourceSpawnReservations circle_reservations;SourceRng circle_rng{1};HCountTrace circle_hcount{0,0};
  SourceGetValidXYRequest circle_request;circle_request.hulk_actor=true;circle_request.wave=3;circle_request.hulk_type=0;circle_request.tmp=16;
  SourceGetValidXYResult circle{};CHECK(source_get_valid_xy(circle_request,circle_rng,traced_hcount,&circle_hcount,circle_reservations,circle));
  CHECK(circle.branch==SourceSpawnBranch::circle);
  CHECK(circle.coordinates.a0_x_16_16==(0x39u<<16));
  CHECK(circle.coordinates.a1_value==circle.circle.a1_packed_xy&&circle.coordinates.a1_value==0x00e10039u);
  CHECK(circle.circle.point_index==15&&circle.tmp_after==15&&circle_reservations.count==2&&circle_hcount.calls==0&&circle_rng.seed==1);
  constexpr u32 pixel=0x10000u;SourceSpawnReservations random_reservations;SourceRng random_rng{1};HCountTrace random_hcount{0,0};
  SourceGetValidXYRequest random_request;random_request.wave=1;random_request.tmp=7;random_request.from_door=4;
  random_request.bounds={300*pixel-1,300*pixel,300*pixel-1,300*pixel};
  SourceGetValidXYResult random{};CHECK(source_get_valid_xy(random_request,random_rng,traced_hcount,&random_hcount,random_reservations,random));
  CHECK(random.branch==SourceSpawnBranch::random&&random.coordinates.a0_x_16_16==300*pixel&&random.coordinates.a1_value==300*pixel);
  CHECK(random.tmp_after==7&&random.random.attempts==1&&random_hcount.calls==2&&random_reservations.count==2);
 });
 group("HULKS OHULK WAVEYX offsets preserve attract and fixed-point source semantics",[]{
  constexpr u32 base_x=0x005f213du,base_y=0x00239d7eu;
  SourceOhulkCoordinates out{1,2};
  CHECK(source_apply_ohulk_waveyx(base_x,base_y,false,0x015201d8u,out));
  CHECK(out.a0_x_16_16==0x0237213du&&out.a1_value==0x01759d7eu);
  CHECK(source_apply_ohulk_waveyx(base_x,base_y,true,0x015201d8u,out));
  CHECK(out.a0_x_16_16==base_x&&out.a1_value==base_y);
  CHECK(source_apply_ohulk_waveyx(0x39000000u,0x00e10039u,false,0x015201d8u,out));
  CHECK(out.a0_x_16_16==0x3ad80000u&&out.a1_value==0x02330039u);
  CHECK(source_apply_ohulk_waveyx(0xffffffffu,0xffffffffu,false,0x00010001u,out));
  CHECK(out.a0_x_16_16==0x0000ffffu&&out.a1_value==0x0000ffffu);
 });
 group("SOUNDS SHOT exact strobes, timer, repeats disabled",[]{SoundCommands s;SoundWrite w[128];SoundTrace t{w,128};bool accepted;CHECK(s.load(sound_shot,4,9,t,accepted)&&accepted);CHECK(t.count==2&&w[0].bus_value==0xfdc4&&w[1].bus_value==0xffc4);CHECK(s.channels[0].priority==0xf0d7&&s.channels[0].timer==5);for(unsigned i=0;i<4;++i)CHECK(s.process_tick(t));CHECK(s.channels[0].priority==0xf0d7);CHECK(s.process_tick(t));CHECK(s.channels[0].priority==0&&s.channels[0].period==5&&s.channels[0].cursor==3);for(unsigned i=0;i<100;++i)CHECK(s.process_tick(t));CHECK(t.count==2);});
 group("SOUNDS priority preemption, equal inhibit, busy flag",[]{SoundCommands s;SoundWrite w[128];SoundTrace t{w,128};bool a;CHECK(s.load(sound_rifle,4,0,t,a)&&a);CHECK(s.load(sound_rifle,4,0,t,a)&&!a);CHECK(s.load(sound_shot,4,0,t,a)&&!a);const u16 high[]={0xf0f0,1,0x80aa,0};CHECK(s.load(high,4,0,t,a)&&a);CHECK(s.process_tick(t));const u16 busy[]={0xf800,1,0x8001,0};CHECK(s.load(busy,4,0,t,a)&&a);CHECK(s.load(high,4,0,t,a)&&!a);CHECK(s.process_tick(t));CHECK(s.load(high,4,0,t,a)&&a);});
 group("SOUNDS actual 0x4000 zero-time threshold and eight-bit payload",[]{SoundCommands s;SoundWrite w[128];SoundTrace t{w,128};bool a;const u16 script[]={0xf001,2,0x4abc,0x4041,0x8042,0};CHECK(s.load(script,6,0,t,a)&&a);CHECK(t.count==6&&w[0].bus_value==0xfdbc&&w[2].bus_value==0xfd41&&w[4].bus_value==0xfd42);CHECK(s.channels[0].timer==2&&s.channels[0].cursor==5);CHECK(s.process_tick(t));CHECK(s.process_tick(t));CHECK(s.channels[0].priority==0);});
 group("SOUNDS same-tick channel visit order",[]{SoundCommands s;SoundWrite w[128];SoundTrace t{w,128};bool a;const u16 c0[]={0xf010,2,0x8040,0x8041,0},c3[]={0xf310,2,0x8050,0x8051,0};CHECK(s.load(c0,5,0,t,a)&&a);CHECK(s.load(c3,5,0,t,a)&&a);CHECK(s.process_tick(t));CHECK(s.process_tick(t));CHECK(t.count==8&&w[4].channel==0&&w[6].channel==3&&w[4].tick==2&&w[6].tick==2);});
 group("SOUNDS period reuse, load suppression, clear and reset separation",[]{SoundCommands s;SoundWrite w[128];SoundTrace t{w,128};bool a;CHECK(s.load(sound_shot,4,0,t,a));const u16 reuse[]={0xf0ee,0x8001,0};CHECK(s.load(reuse,3,0,t,a)&&a);CHECK(s.channels[0].period==5&&s.channels[0].timer==5);s.suppressed=1;CHECK(s.load(nullptr,0,0,t,a)&&!a);CHECK(s.process_tick(t));CHECK(s.channels[0].timer==4);s.clear_database();CHECK(s.suppressed==1&&s.ticks==1&&s.channels[0].timer==0);usize old=t.count;CHECK(s.board_reset(true,t));CHECK(t.count==old+6&&w[old].bus_value==0xfe00&&w[old+1].bus_value==0xffff&&w[old+4].channel==3);});
 group("SOUNDS bounded scripts and transactional trace overflow",[]{SoundCommands s;SoundWrite w[8];SoundTrace t{w,1};bool a;CHECK(!s.load(sound_shot,4,0,t,a));CHECK(!a&&t.count==0&&s.channels[0].priority==0);const u16 bad[]={0xf0ee,1,0x8001};CHECK(!s.load(bad,3,0,t,a));t.capacity=2;const u16 script[]={0xf001,1,0x8001,0x8002,0};CHECK(s.load(script,5,0,t,a)&&a);CHECK(!s.process_tick(t));CHECK(s.ticks==0&&s.channels[0].timer==1&&t.count==2);t.capacity=8;CHECK(s.process_tick(t));CHECK(s.ticks==1&&t.count==4);SoundTrace invalid{nullptr,100,0};CHECK(!s.send(1,0,invalid));});
 group("SOUNDS original volume table endpoints",[]{u16 out[5];CHECK(volume_table(0,out));CHECK(out[0]==0xf3ee&&out[1]==1&&out[2]==0x402f&&out[3]==0x806f&&out[4]==0);CHECK(volume_table(15,out));CHECK(out[2]==0x4020&&out[3]==0x8060);CHECK(volume_table(99,out));CHECK(out[2]==0x4020);});
 group("host eight-way sticks, hysteresis and independent players",[]{StickSettings settings;CHECK(validate_stick_settings(settings));DigitalStick p1,p2;CHECK(p1.sample(0,0,settings)==0);CHECK(p1.sample(32767,0,settings)==right);CHECK(p1.sample(8000,0,settings)==right);CHECK(p1.sample(6999,0,settings)==0);CHECK(p1.sample(8000,0,settings)==0);CHECK(p1.sample(-32768,-32768,settings)==(left|up));CHECK(p2.sample(0,32767,settings)==down);CHECK(p1.direction==(left|up));CHECK(p1.sample(32767,32767,settings)==(right|down));CHECK(p1.sample(0,-32768,settings)==up);CHECK(digital_directions(true,true,true,false)==left);CHECK(digital_directions(true,false,false,true)==(up|right));settings.release=10000;CHECK(!validate_stick_settings(settings));CHECK(p1.sample(32767,0,settings)==0);});
 group("two-player controller assignment bounds and uniqueness",[]{ControllerAssignment a;CHECK(a.validate());CHECK(a.xinput_slot[0]==0&&a.xinput_slot[1]==1);CHECK(!a.assign(1,0));CHECK(a.xinput_slot[1]==1);CHECK(a.assign(0,3));CHECK(a.xinput_slot[0]==3&&a.xinput_slot[1]==1);CHECK(!a.assign(0,4));CHECK(!a.assign(2,2));CHECK(a.validate());});
 group("named two-player keyboard actions reject conflicts transactionally",[]{KeyboardBindings b;CHECK(b.validate());CHECK(b.keys[0][unsigned(InputAction::move_up)]=='W'&&b.keys[0][unsigned(InputAction::start)]=='1'&&b.keys[1][unsigned(InputAction::coin)]=='6');CHECK(!b.assign(1,InputAction::start,'1'));CHECK(b.keys[1][unsigned(InputAction::start)]=='2');CHECK(b.assign(1,InputAction::start,'Q'));CHECK(!b.assign(2,InputAction::coin,'Z'));CHECK(!b.assign(0,InputAction::coin,0));CHECK(b.validate());});
 group("host rational clock is invariant to presentation partitioning",[]{TickClock a,c;CHECK(a.configure(1000000,60000,1001));CHECK(c.configure(1000000,60000,1001));u64 n=0,total=0;for(unsigned i=0;i<10000;++i){CHECK(a.advance(10000,n));total+=n;}CHECK(c.advance(100000000,n));CHECK(total==n&&a.phase==c.phase&&total==5994);CHECK(!a.configure(0,1,1));CHECK(!a.advance(~u64(0),n));TickClock empty;CHECK(!empty.advance(1,n));
  TickClock l8;CHECK(l8.configure(1000000000,L8ReferenceTiming::pixel_clock_hz,L8ReferenceTiming::pixels_per_frame));CHECK(l8.advance(18279249,n)&&n==0);CHECK(l8.advance(1,n)&&n==1);CHECK(l8.advance(18279250*9ull,n)&&n==9);});
 group("MAIN DIRQ1 every-eighth coin service preserves CTIMLP A0 byte write",[]{
  SourceCoinService service;bool started=false;
  CHECK(service.begin_validation(0,started)&&started&&service.flags[0]==30);
  CHECK(service.begin_validation(0,started)&&!started&&service.flags[0]==30);
  for(unsigned i=1;i<8;++i)CHECK(!service.end_screen()&&service.irqled==i&&service.flags[0]==30);
  CHECK(service.end_screen()&&service.irqled==0&&service.flags[0]==0);
  service.reset();CHECK(service.begin_validation(0,started)&&started);CHECK(service.begin_validation(1,started)&&started);
  for(unsigned i=0;i<8;++i)service.end_screen();
  CHECK(service.flags[0]==0&&service.flags[1]==0);
  service.reset();CHECK(service.begin_validation(0,started)&&started);CHECK(service.begin_validation(3,started)&&started);
  service.fraction_timer=0x0130;
  for(unsigned i=0;i<8;++i)service.end_screen();
  CHECK(service.flags[0]==0&&service.flags[3]==30&&service.fraction_timer==0x002f);
  service.reset();CHECK(!service.begin_validation(4,started)&&!started&&service.flags[0]==0);
 });
 group("MAIN SWSCAN active-low 110 edge and distinct widget reads",[]{SwitchScan s;CHECK(s.sample(~0u,~0u)==0);CHECK(s.sample(~0u,~0u)==0);CHECK(s.sample(0x12345678u,~(1u<<18))==(1u<<18));CHECK(s.joystick_raw==0x12345678u);CHECK(s.previous==~(1u<<18)&&s.older==~0u);CHECK(s.sample(~0u,~(1u<<18))==0);CHECK(s.sample(~0u,~0u)==0);CHECK(s.sample(~0u,~0u)==0);CHECK(s.sample(~0u,~(1u<<18))==(1u<<18));SwitchScan fresh;CHECK(fresh.sample(~0u,~(1u<<21))==0);CHECK(fresh.sample(~0u,~0u)==0);CHECK(fresh.sample(~0u,~0u)==0);CHECK(fresh.sample(~0u,~(1u<<21))==(1u<<21));});
 group("MAIN SWSCAN RAM switch stack ordering, capacity and scan boundary",[]{
  SwitchStack stack;u8 bit=255;
  CHECK(stack.push((1u<<31)|(1u<<18)|1u)==3&&stack.depth==3);
  CHECK(!stack.pop(false,bit)&&bit==255&&stack.depth==3);
  CHECK(stack.pop(true,bit)&&bit==0);
  CHECK(stack.pop(true,bit)&&bit==18);
  CHECK(stack.pop(true,bit)&&bit==31);
  CHECK(!stack.pop(true,bit));
  CHECK(stack.push(~0u)==SwitchStack::capacity&&stack.depth==SwitchStack::capacity);
  CHECK(stack.push(1u)==0);
  for(unsigned expected=16;expected<32;++expected){CHECK(stack.pop(true,bit)&&bit==expected);}
  CHECK(stack.depth==0);
  CHECK(stack.push(1u<<2)==1&&stack.push(1u<<4)==1);
  CHECK(stack.pop(true,bit)&&bit==4);
  CHECK(stack.pop(true,bit)&&bit==2);
  CHECK(sizeof(yunit_switch_routes)/sizeof(yunit_switch_routes[0])==32);
  for(unsigned i=0;i<16;++i)CHECK(text_equal(yunit_switch_routes[i].routine,"DUMDIE"));
  CHECK(text_equal(yunit_switch_routes[16].routine,"LCOIN")&&text_equal(yunit_switch_routes[16].process_id,"LC_PID"));
  CHECK(text_equal(yunit_switch_routes[17].routine,"RCOIN")&&text_equal(yunit_switch_routes[17].process_id,"RC_PID"));
  CHECK(text_equal(yunit_switch_routes[18].routine,"P1START")&&text_equal(yunit_switch_routes[18].process_id,"P1SWPID"));
  CHECK(text_equal(yunit_switch_routes[21].routine,"P2START")&&text_equal(yunit_switch_routes[21].process_id,"P2SWPID"));
  CHECK(text_equal(yunit_switch_routes[22].routine,"SERVICE")&&text_equal(yunit_switch_routes[24].routine,"FREEZE"));
  CHECK(text_equal(yunit_switch_routes[25].routine,"XCOIN"));
  for(unsigned i=26;i<32;++i)CHECK(text_equal(yunit_switch_routes[i].routine,"DUMDIE"));
  SourceWidgetStream stream;InputFrame first,second;
  CHECK(stream.advance(first,second));
  first.boundary=second.boundary=1;CHECK(stream.advance(first,second));
  first.boundary=second.boundary=2;
  second.player[0].coin=true;second.player[1].start=true;
  CHECK(stream.advance(first,second));
  CHECK(stream.scan.joystick_raw==~0u&&stream.stack.depth==2&&stream.next_boundary==3);
  CHECK(stream.stack.pop(true,bit)&&bit==16);
  CHECK(stream.stack.pop(true,bit)&&bit==21);
  CHECK(!stream.advance(first,second)&&stream.next_boundary==3&&stream.stack.depth==0);
  first.boundary=second.boundary=3;second.player[1].shooting=0x10;
  CHECK(!stream.advance(first,second)&&stream.next_boundary==3&&stream.stack.depth==0);
  second.player[1].shooting=0;CHECK(stream.advance(first,second)&&stream.stack.depth==0);
  stream.reset(100);CHECK(stream.next_boundary==100&&stream.stack.depth==0&&stream.scan.previous==0);
 });
 group("two-slot InputFrame encodes original active-low widget ports",[]{InputFrame in;in.boundary=1050*2;in.player[0]={u8(up|right),u8(left),false,true};in.player[1]={u8(down|left),u8(right),true,false};WidgetPorts ports;CHECK(encode_widget(in,ports));CHECK(ports.controls==u16(~u16(0x8649))&&ports.coins_starts==u16(~u16(0x21)));CHECK(ports.longword()==(u32(ports.coins_starts)<<16|ports.controls));SwitchScan scan;CHECK(scan.sample(~0u,~0u)==0);CHECK(scan.sample(~0u,~0u)==0);CHECK(scan.sample(ports.longword(),ports.longword())==(0x0021'8649u));InputFrame invalid=in;invalid.player[1].shooting=0x10;WidgetPorts unchanged=ports;CHECK(!encode_widget(invalid,ports));CHECK(ports.controls==unchanged.controls&&ports.coins_starts==unchanged.coins_starts);});
 group("host indexed raster clipping, flips, transparency and stride",[]{const u32 palette[4]={0,0x112233,0x223344,0x334455};const u8 src[6]={1,0,2,3,2,1};for(int fx=0;fx<2;++fx)for(int fy=0;fy<2;++fy)for(int ox=-3;ox<5;++ox)for(int oy=-2;oy<4;++oy){u32 out[30],expected[30];for(unsigned i=0;i<30;++i)out[i]=expected[i]=0xdeadbeef;CHECK(draw_indexed({out,5,4,7,30},{{src,6},3,2,3},palette,4,{ox,oy,bool(fx),bool(fy),true}));for(int y=0;y<4;++y)for(int x=0;x<5;++x){int ix=x-ox,iy=y-oy;if(ix>=0&&ix<3&&iy>=0&&iy<2){if(fx)ix=2-ix;if(fy)iy=1-iy;auto index=src[iy*3+ix];if(index)expected[y*7+x]=palette[index];}}CHECK(equal(out,expected,sizeof out));}});
 group("host raster malformed data cannot partially write",[]{u32 out[4]={7,7,7,7},palette[2]={0,42};u8 src[4]={1,1,1,2};CHECK(!draw_indexed({out,2,2,2,4},{{src,4},2,2,2},palette,2,{}));CHECK(out[0]==7&&out[3]==7);CHECK(!draw_indexed({out,2,2,2,3},{{src,4},2,2,2},palette,2,{}));});
 group("ZIP stored content, central/local consistency and CRC",[]{auto zip=std::make_unique<ZipArchive>();auto z=zip_fixture({{"dir/a.bin",{1,2,3}}, {"b.bin",{9,8}}});CHECK(zip->open(v(z)));CHECK(zip->count==2);u8 out[3];CHECK(zip->extract(0,{out,3}));CHECK(out[0]==1&&out[2]==3);CHECK(!zip->extract(0,{out,2}));z[zip->entries[0].data_offset]^=1;CHECK(zip->open(v(z)));CHECK(!zip->extract(0,{out,3}));});
 group("ZIP unsafe paths, duplicate aliases, malformed local headers",[]{auto zip=std::make_unique<ZipArchive>();for(const auto& name:{"../evil","/absolute","C:\\evil","a/../bad","a\\..\\bad"}){auto z=zip_fixture({{name,{1}}});CHECK(!zip->open(v(z)));}auto z=zip_fixture({{"A/file",{1}},{"a\\FILE",{2}}});CHECK(!zip->open(v(z)));z=zip_fixture({{"good",{1,2}}});z[30]='x';CHECK(!zip->open(v(z)));});
 group("DEFLATE stored block and strict output/trailing checks",[]{u8 raw[]={1,3,0,0xfc,0xff,'a','b','c'},out[4];usize written=0;CHECK(inflate_raw({raw,8},{out,3},written));CHECK(written==3&&out[2]=='c');CHECK(!inflate_raw({raw,8},{out,4},written));CHECK(!inflate_raw({raw,7},{out,3},written));u8 reserved[]={7};CHECK(!inflate_raw({reserved,1},{out,3},written));});
 group("ZIP mutation safety corpus (not proof of rejection completeness)",[]{auto zip=std::make_unique<ZipArchive>();auto original=zip_fixture({{"a.bin",std::vector<u8>(256,42)},{"b.bin",{1,2,3}}});for(unsigned i=0;i<3000;++i){auto z=original;unsigned changes=1+rnd()%5;for(unsigned j=0;j<changes;++j)z[rnd()%z.size()]^=u8(rnd()|1);if(i%4==0)z.resize(rnd()%z.size());auto s=zip->open(v(z));if(s){for(unsigned j=0;j<zip->count;++j){std::vector<u8> out(zip->entries[j].size);(void)zip->extract(j,b(out));}}++checks;}});
 group("synthetic importer package, interleaving, reload, provenance, determinism",[&]{
  constexpr u32 test_profile=0x54455354;RomSpec spec[reference_rom_count];char digest[reference_rom_count][41];std::vector<File> files;
  for(unsigned i=0;i<reference_rom_count;++i){std::vector<u8> data(31+i*7);for(unsigned j=0;j<data.size();++j)data[j]=u8((i+1)*13+j*7);hex(sha1(v(data)).bytes,20,digest[i]);spec[i]=l8_reference_roms[i];spec[i].size=u32(data.size());spec[i].crc=crc32(v(data));spec[i].sha1_text=digest[i];files.push_back({"renamed/fixture-"+std::to_string(i)+".bin",data});}
  auto z=zip_fixture(files);auto zip=std::make_unique<ZipArchive>();CHECK(zip->open(v(z)));ImportReport report;std::vector<u8> scratch(rom_scratch_size),package(asset_package_size),other(asset_package_size);CHECK(scan_roms(*zip,spec,b(scratch),report));CHECK(report.found==14&&report.missing_mask==0);CHECK(make_package(*zip,spec,test_profile,report,b(scratch),b(package)));CHECK(verify_package(v(package),spec,test_profile,b(scratch)));CHECK(!verify_package(v(package),l8_reference_roms,retail_l8_reference_id,b(scratch)));
  for(unsigned i=0;i<2;++i)for(unsigned j=0;j<files[i].data.size();++j)CHECK(package[package_header_size+0xc0000+i+j*2]==files[i].data[j]);
  usize sound_base=package_header_size+main_region_size+graphics_region_size;for(unsigned i=11;i<14;++i)for(unsigned j=0;j<files[i].data.size();++j)CHECK(package[sound_base+spec[i].offset+j]==package[sound_base+spec[i].reload+j]);
  char manifest[32768];TextWriter writer{manifest,sizeof manifest};CHECK(write_manifest(*zip,spec,test_profile,report,v(package),writer));CHECK(std::string(manifest).find("source compatibility UNVERIFIED")!=std::string::npos);
  if(argc==2){std::filesystem::create_directories(argv[1]);save(std::filesystem::path(argv[1])/"synthetic-input.zip",v(z));save(std::filesystem::path(argv[1])/"synthetic-package.stvdata",v(package));save(std::filesystem::path(argv[1])/"synthetic-manifest.json",{reinterpret_cast<const u8*>(manifest),writer.size});}
  std::reverse(files.begin(),files.end());z=zip_fixture(files);CHECK(zip->open(v(z)));CHECK(scan_roms(*zip,spec,b(scratch),report));CHECK(make_package(*zip,spec,test_profile,report,b(scratch),b(other)));CHECK(equal(package.data(),other.data(),package.size()));
  package.back()^=1;CHECK(!verify_package(v(package),spec,test_profile,b(scratch)));package.back()^=1;
  files.pop_back();z=zip_fixture(files);CHECK(zip->open(v(z)));CHECK(!scan_roms(*zip,spec,b(scratch),report));CHECK(report.found==13&&report.missing_mask==1);
  files.push_back(files.front());files.back().name="duplicate-content.bin";z=zip_fixture(files);CHECK(zip->open(v(z)));CHECK(!scan_roms(*zip,spec,b(scratch),report));
 });
 group("text writer bounds and JSON byte escaping",[]{char tiny[4];TextWriter t{tiny,4};t.text("1234");CHECK(t.overflow&&t.size==3);char out[256];TextWriter w{out,256};w.quoted("a\"b\\c\n\xff");CHECK(std::string(out)=="\"a\\\"b\\\\c\\u000a\\u00ff\"");});
 std::cout<<"RESULT: "<<groups<<" groups passed; "<<checks<<" assertions/checks; no emulated CPU or original-ROM differential run.\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
