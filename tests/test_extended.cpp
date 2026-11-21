#include "stv/process.hpp"
#include "stv/gameplay.hpp"
#include "stv/animation.hpp"
#include "stv/source_assets.hpp"
#include "stv/objects.hpp"
#include "stv/source_actor.hpp"
#include "stv/source_exit.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace stv;
static unsigned groups=0;static u64 checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x);}while(false)
template<class F>void group(const char* name,F fn){fn();++groups;std::cout<<"PASS "<<name<<'\n';}
struct Log{std::vector<unsigned> ids;ProcessHandle victim{no_process};};
struct HCountFixture{u16 value{};unsigned reads{};};
static u16 read_hcount(void* context){auto& fixture=*static_cast<HCountFixture*>(context);++fixture.reads;return fixture.value;}
static ProcessAction record(NativeProcesses& p,ProcessHandle h,void* ptr){auto& l=*static_cast<Log*>(ptr);l.ids.push_back(p.cells[h].id);return ProcessAction::wait(1,p.cells[h].continuation);}
struct ExitDispatchContext {SourceDexitContext dexit{};};
static ProcessAction exit_dispatch(NativeProcesses& processes,ProcessHandle handle,void* context){
 const auto& process=processes.cells[handle];
 if(process.id==source_dexit_process_id)return source_dexit_step(processes,handle,context?&static_cast<ExitDispatchContext*>(context)->dexit:nullptr);
 if(process.id==source_arrow_process_id)return source_kilic_step(processes,handle,context);
 return ProcessAction::wait(0x7fff,process.continuation);
}
static std::vector<u8> read(const char* file){std::ifstream f(file,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Missing supplied-source fixture pack");auto size=f.tellg();if(size<0||size>64*1024*1024)throw std::runtime_error("Bad source fixture size");std::vector<u8>b(static_cast<usize>(size));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()));if(!f)throw std::runtime_error("Fixture read failed");return b;}
int main(int argc,char** argv){try{if(argc!=2)throw std::runtime_error("Pass original source.stva fixture");auto bytes=read(argv[1]);auto pack=std::make_unique<SourcePack>();CHECK(pack->open({bytes.data(),bytes.size()}));
 group("L8 HULKS DOLOGIC BUYINPID guard selects EXISTP match or falls through",[]{
  NativeProcesses processes;processes.initialize();ProcessHandle hulk;
  CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,hulk));
  SourceHulkBuyinGate gate{true,0};
  CHECK(source_hulk_buyin_gate(processes,hulk,gate));CHECK(!gate.sleep_two&&gate.process==no_process);
  ProcessHandle first_buyin,second_buyin;
  CHECK(processes.create(source_buyin_process_id,0x42554931u,nullptr,first_buyin));
  CHECK(processes.create(source_buyin_process_id,0x42554932u,nullptr,second_buyin));
  CHECK(source_hulk_buyin_gate(processes,hulk,gate));
  CHECK(gate.sleep_two&&gate.process==second_buyin); // ACTIVE order: newest match first.
  ProcessHandle unrelated;CHECK(processes.create(0x1234,0x1234,nullptr,unrelated));
  const auto before=gate;CHECK(!source_hulk_buyin_gate(processes,unrelated,gate));
  CHECK(gate.sleep_two==before.sleep_two&&gate.process==before.process);
  CHECK(!source_hulk_buyin_gate(processes,no_process,gate));
 });
 group("MPROC initial free chain, same-scan creation and LIFO insertion",[]{auto p=std::make_unique<NativeProcesses>();p->initialize();CHECK(p->validate());ProcessHandle h;CHECK(p->create(1,1,nullptr,h));CHECK(h==0);Log l;auto step=[](NativeProcesses& p,ProcessHandle h,void* x){auto& l=*static_cast<Log*>(x);auto& c=p.cells[h];l.ids.push_back(c.id);if(c.id==1){ProcessHandle a,b;if(!p.create(2,1,nullptr,a)||!p.create(3,1,nullptr,b))return ProcessAction::fail("create failed");}return ProcessAction::suicide();};CHECK(p->dispatch(step,&l));CHECK((l.ids==std::vector<unsigned>{1,3,2}));CHECK(p->active_count==0&&p->validate());});
 group("MPROC signed PTIME, single decrement despite five accrued interrupts",[]{auto p=std::make_unique<NativeProcesses>();p->initialize();ProcessHandle h;Log l;CHECK(p->create(1,1,nullptr,h));p->cells[h].time=3;CHECK(p->dispatch(record,&l,5));CHECK(l.ids.empty()&&p->cells[h].time==2&&p->time_temp==5);CHECK(p->dispatch(record,&l,1));CHECK(l.ids.empty());CHECK(p->dispatch(record,&l,1));CHECK(l.ids.size()==1);for(u16 n:{u16(0),u16(1),u16(0x8000),u16(0xffff)}){p->cells[h].time=n;l.ids.clear();CHECK(p->dispatch(record,&l));CHECK(l.ids.size()==1);}p->cells[h].time=0x7fff;l.ids.clear();CHECK(p->dispatch(record,&l));CHECK(l.ids.empty()&&p->cells[h].time==0x7ffe);});
 group("MPROC KILL skips caller, masked kill/EXIST exclude current",[]{auto p=std::make_unique<NativeProcesses>();p->initialize();ProcessHandle a,b,c;CHECK(p->create(0x1234,1,nullptr,a));CHECK(p->create(0x12ab,1,nullptr,b));CHECK(p->create(0x9900,1,nullptr,c));CHECK(p->exist(0x1200,0xff00)==b);Log l;auto step=[](NativeProcesses& p,ProcessHandle h,void* x){auto& log=*static_cast<Log*>(x);log.ids.push_back(p.cells[h].id);CHECK(p.kill(h));CHECK(p.cells[h].active);if(p.cells[h].id==0x9900){CHECK(p.kill_matching(0x1200,0xff00)==2);CHECK(p.exist(0x9900,0xffff)==no_process);}return ProcessAction::wait(1,1);};CHECK(p->dispatch(step,&l));CHECK(l.ids.size()==1&&p->active_count==1&&p->validate());});
 group("MPROC stale private bytes, capacity, XFER and native call stack",[]{auto p=std::make_unique<NativeProcesses>();p->initialize();ProcessHandle h,old;CHECK(p->create(1,1,nullptr,h));old=h;p->cells[h].data[75]=0xa9;CHECK(p->kill(h));u32 regs[4]={1,2,3,4};CHECK(p->create(9,44,regs,h));CHECK(h==old&&p->cells[h].data[75]==0xa9);CHECK(p->transfer(h,45,regs));CHECK(p->cells[h].time==1&&p->cells[h].saved[3]==4);for(unsigned i=0;i<16;++i)CHECK(p->call(h,100+i));CHECK(!p->call(h,200));u32 ret;for(unsigned i=16;i>0;--i){CHECK(p->return_from_call(h,ret));CHECK(ret==99+i);}CHECK(!p->return_from_call(h,ret));for(unsigned i=1;i<process_capacity;++i)CHECK(p->create(u16(i),1,nullptr,h));CHECK(!p->create(8,1,nullptr,h));CHECK(p->active_count==240&&p->validate());});
 group("MPROC infinite create/suicide chains fail explicitly",[]{auto p=std::make_unique<NativeProcesses>();p->initialize();ProcessHandle h;CHECK(p->create(1,1,nullptr,h));auto step=[](NativeProcesses& p,ProcessHandle,void*){ProcessHandle child;CHECK(p.create(1,1,nullptr,child));return ProcessAction::suicide();};CHECK(!p->dispatch(step,nullptr,1,64));CHECK(!p->dispatching&&p->validate());});
 group("ENDWAVE DEXIT attract path preserves 120/10/600 waits and ordered process cleanup",[]{
  NativeProcesses processes;processes.initialize();ExitDispatchContext context;context.dexit.game_state=source_game_state_inamode;
  ProcessHandle dexit;CHECK(processes.create(source_dexit_process_id,source_exit_state::dexit_entry,nullptr,dexit));
  constexpr u16 ids[]={279,280,281,282,223,279,232,262,201};ProcessHandle actors[sizeof(ids)/sizeof(ids[0])]{};
  for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);++i){CHECK(processes.create(ids[i],0x52535400u,nullptr,actors[i]));processes.cells[actors[i]].time=0x7fff;}
  CHECK(processes.dispatch(exit_dispatch,&context));CHECK(processes.cells[dexit].time==120);
  for(unsigned i=0;i<119;++i)CHECK(processes.dispatch(exit_dispatch,&context));
  CHECK(processes.cells[dexit].time==1&&processes.cells[actors[0]].active&&processes.cells[actors[4]].active);
  CHECK(processes.dispatch(exit_dispatch,&context));
  CHECK(processes.cells[dexit].time==10);
  // KILALL prepends each killed slot to MPROC's free chain. Its head therefore
  // exposes both the source's per-ID call order and EXISTP-style active order.
  ProcessHandle freed=processes.free_head;constexpr ProcessHandle ordered_free[]={5,4,3,2,1,6,10};
  for(ProcessHandle expected:ordered_free){CHECK(freed==expected);freed=processes.cells[freed].next;}
  for(unsigned i=0;i<9;++i)CHECK(processes.dispatch(exit_dispatch,&context));
  CHECK(processes.cells[dexit].time==1);
  CHECK(!processes.cells[actors[0]].active&&!processes.cells[actors[1]].active&&
        !processes.cells[actors[2]].active&&!processes.cells[actors[3]].active&&
        !processes.cells[actors[4]].active&&!processes.cells[actors[5]].active);
  CHECK(processes.cells[actors[6]].active&&processes.cells[actors[7]].active&&processes.cells[actors[8]].active);
  CHECK(processes.dispatch(exit_dispatch,&context));
  CHECK(!processes.cells[dexit].active&&processes.cells[actors[6]].active&&
        processes.cells[actors[7]].active&&processes.cells[actors[8]].active);
  ProcessHandle arrow=processes.exist(source_arrow_process_id,0xffffu);
  CHECK(arrow!=no_process&&processes.cells[arrow].continuation==source_exit_state::kilic_after_600&&processes.cells[arrow].time==600);
  for(unsigned i=0;i<599;++i)CHECK(processes.dispatch(exit_dispatch,&context));
  CHECK(processes.cells[arrow].active&&processes.cells[arrow].time==1&&
        processes.cells[actors[6]].active&&processes.cells[actors[7]].active);
  CHECK(processes.dispatch(exit_dispatch,&context));
  CHECK(!processes.cells[arrow].active&&!processes.cells[actors[6]].active&&
        !processes.cells[actors[7]].active&&processes.cells[actors[8]].active&&processes.validate());

  NativeProcesses gameplay;gameplay.initialize();ExitDispatchContext live;live.dexit.game_state=0;
  ProcessHandle live_dexit;CHECK(gameplay.create(source_dexit_process_id,source_exit_state::dexit_entry,nullptr,live_dexit));
  CHECK(gameplay.dispatch(exit_dispatch,&live));
  for(unsigned i=0;i<120;++i)CHECK(gameplay.dispatch(exit_dispatch,&live));
  for(unsigned i=0;i<9;++i)CHECK(gameplay.dispatch(exit_dispatch,&live));
  auto untranslated=gameplay.dispatch(exit_dispatch,&live);
  CHECK(!untranslated&&untranslated.message&&std::string(untranslated.message).find("DEXIT non-attract DX2")!=std::string::npos);
  CHECK(gameplay.cells[live_dexit].active&&gameplay.exist(source_arrow_process_id,0xffffu)!=no_process);
 });
 group("L8 OHULK maps BEGINOBJ2 and PRCSLP state through a native two-tick wake",[&]{
  EditorImages art;CHECK(art.open(pack->find("FINGRNT.IMG")->bytes));
  NativeObjects objects;objects.initialize();
  NativeProcesses processes;processes.initialize();ProcessHandle actor;
  const u32 initial[4]={0x21,0x10,0x80001234,6};
  CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,initial,actor));
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::control_flags,0x0100));
  CHECK(source_ohulk_mark_standing(processes.cells[actor]));
  SourceOhulkCoordinates start{0x0237213du,0x01759d7eu};ObjectHandle linked_object;
  CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,start,linked_object));
  CHECK(linked_object==0&&processes.cells[actor].saved[0]==linked_object);
  u8 wave[14]{};put16(wave,source_hulk_wave_type);put16(wave+2,2);put16(wave+4,0);
  HulkAccounting accounting{};u16 player_x[2]={300,500};HCountFixture hcount{0x1234,0};
  u32 setup_seed=1;while(SourceRng{setup_seed}.range(0,10,hcount.value)!=1)++setup_seed;
  SourceRng setup_rng{setup_seed};SourceHulkSetupResult setup{};
  CHECK(source_ohulk_setup_hulk(processes,actor,objects,{wave,sizeof wave},accounting,
                                player_x,setup_rng,read_hcount,&hcount,setup));
  CHECK(setup.age_table_index==1&&setup.hage==300&&setup.level==0&&hcount.reads==1);
  CHECK(source_hulk_do_diff(processes.cells[actor]));
  SourceHulkInitialYieldContext captured{0x10,0xfffba320u,6,source_process_stack_end_offset};
  SourceProcessRegisters expected{source_hulk_resume_lup_top,linked_object,captured.a9,captured.a10,captured.a11,captured.stack_offset};
  struct Wake {SourceProcessRegisters expected{};const NativeObjects* objects{};unsigned calls{};};Wake wake{};
  const auto step=[](NativeProcesses& p,ProcessHandle h,void* context){
   auto& state=*static_cast<Wake*>(context);
   if(state.calls++==0){
    ProcessAction sleep{};
    CHECK(source_hulk_sleep_after_setup(p,*state.objects,h,2,
        {state.expected.a9,state.expected.a10,state.expected.a11,u16(state.expected.a12)},sleep));
    CHECK(sleep.kind==ProcessAction::sleep&&sleep.delay==2&&sleep.next==source_hulk_resume_lup_top);
    return sleep;
   }
   CHECK(p.cells[h].time==0);
   SourceProcessRegisters restored{};CHECK(source_process_registers_read(p.cells[h],restored));
   CHECK(restored.a7==state.expected.a7&&restored.a8==state.expected.a8&&restored.a9==state.expected.a9&&
         restored.a10==state.expected.a10&&restored.a11==state.expected.a11&&restored.a12==state.expected.a12);
   CHECK(restored.a8< object_capacity);
   const NativeObject& object=state.objects->cells[ObjectHandle(restored.a8)];
   CHECK(object.allocated&&object.listed&&object.process==h&&object.id==source_hulk_object_id);
   return ProcessAction::wait(1,restored.a7);
  };
  wake.expected=expected;wake.objects=&objects;
  ProcessHandle orphan;CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,orphan));
  const NativeProcess orphan_before=processes.cells[orphan];
  ProcessAction untouched=ProcessAction::wait(7,0x52534d04);const ProcessAction untouched_before=untouched;
  CHECK(!source_hulk_sleep_after_setup(processes,objects,orphan,2,captured,untouched));
  CHECK(untouched.kind==untouched_before.kind&&untouched.delay==untouched_before.delay&&untouched.next==untouched_before.next);
  CHECK(equal(&orphan_before,&processes.cells[orphan],sizeof orphan_before));CHECK(processes.kill(orphan));
  auto bad_stack=captured;bad_stack.stack_offset=source_process_stack_data_offset-1;
  const NativeProcess actor_before=processes.cells[actor];
  CHECK(!source_hulk_sleep_after_setup(processes,objects,actor,2,bad_stack,untouched));
  CHECK(equal(&actor_before,&processes.cells[actor],sizeof actor_before));
  CHECK(processes.dispatch(step,&wake));CHECK(wake.calls==1&&processes.cells[actor].time==2);
  SourceProcessRegisters sleeping{};CHECK(source_process_registers_read(processes.cells[actor],sleeping));
  CHECK(sleeping.a7==expected.a7&&sleeping.a8==expected.a8&&sleeping.a9==expected.a9&&
        sleeping.a10==expected.a10&&sleeping.a11==expected.a11&&sleeping.a12==expected.a12);
  CHECK(processes.dispatch(step,&wake));CHECK(wake.calls==1&&processes.cells[actor].time==1);
  CHECK(processes.dispatch(step,&wake));CHECK(wake.calls==2&&processes.cells[actor].time==1);
  const auto before=processes.cells[actor];auto invalid=expected;invalid.a7=0;
  ProcessAction unchanged=ProcessAction::wait(7,0x52534d04);const auto unchanged_action=unchanged;
  CHECK(!source_hulk_prcslp(processes,actor,2,invalid,unchanged));
  CHECK(unchanged.kind==unchanged_action.kind&&unchanged.delay==unchanged_action.delay&&unchanged.next==unchanged_action.next);
  CHECK(!source_hulk_prcslp(processes,actor,0,expected,unchanged));
  CHECK(equal(&before,&processes.cells[actor],sizeof before));
  CHECK(processes.transfer(actor,0x52534d03,nullptr));
  CHECK(processes.cells[actor].saved_a12==0);
  CHECK(processes.validate());
 });
 group("ROBO all input combinations, conflicting directions and source boundaries",[]{PlayerBounds b{10u<<16,90u<<16,20u<<16,80u<<16};SourcePosition p{50u<<16,50u<<16,0,0};constexpr u16 expected[16]={0,1,2,3,7,8,9,9,4,5,6,7,9,9,9,9};for(unsigned i=0;i<16;++i){CHECK(player_direction(u8(i),p,b,false)==expected[i]);CHECK(firing_direction(u8(i))==expected[i]);}p.x=b.right;CHECK(player_direction(12,p,b,false)==0);CHECK(player_direction(12,p,b,true)==9);p.y=b.bottom;CHECK(player_direction(3,p,b,false)==0);p.x=b.left;CHECK(player_direction(4,p,b,false)==0);p.y=b.top;CHECK(player_direction(1,p,b,false)==0);});
 group("ROBO exact diagonal/cardinal velocities and shoes decrement only on call",[]{SourcePosition l{},t{};u16 shoe=0;CHECK(player_velocity(5,shoe,l,t));CHECK(l.vx==0xfa00&&l.vy==0u-0xfa00&&t.vx==l.vx);shoe=1;CHECK(player_velocity(9,shoe,l,t));CHECK(shoe==0&&l.vx==0u-0x18000&&l.vy==0x18000);CHECK(player_velocity(9,shoe,l,t));CHECK(l.vx==0u-0xfa00);CHECK(!player_velocity(10,shoe,l,t));l={~0u,0,1,~0u};advance_position(l);CHECK(l.x==0&&l.y==~0u);});
 group("SHOTS regular bullet tables preserve odd -5 fixed-point left offset",[]{SourcePosition t{100u<<16,100u<<16,1,2};BulletSetup b;CHECK(regular_bullet_setup(7,t,b));CHECK(b.position.x==(100u<<16)-5);CHECK(b.position.y==105u<<16);CHECK(b.position.vx==0u-0x72000&&b.position.vy==0&&b.age==60&&b.id==0x4201);CHECK(regular_bullet_setup(5,t,b));CHECK(b.position.vx==0x72000&&b.position.vy==0u-0x72000&&b.flags==0x412);CHECK(!regular_bullet_setup(3,t,b));CHECK(!regular_bullet_setup(0,t,b));});
 group("HULKS source speed truncation, directional turns and tangent boundaries",[]{HulkSpeeds s;CHECK(hulk_speeds(0,s));CHECK(s.horizontal==0x8000&&s.vertical==0x7000&&s.diagonal_x==(0x8000/10)*8);CHECK(hulk_speeds(10,s));CHECK(s.horizontal==0x1c000);CHECK(!hulk_speeds(0xffff,s));CHECK(turn_hulk(1,8)==8&&turn_hulk(8,1)==1&&turn_hulk(1,5)==2&&turn_hulk(5,1)==4);CHECK(hulk_direction_to(0,0,0,-10)==0);CHECK(hulk_direction_to(0,0,10,0)==2);CHECK(hulk_direction_to(0,0,0,10)==4);CHECK(hulk_direction_to(0,0,-10,0)==6);CHECK(hulk_direction_to(0,0,10,-10)==1);CHECK(hulk_direction_to(0,0,-10,-10)==7);});
 group("HULKS GET_STAND maps SPINFRMS tables and applies ANI to its linked object",[&]{
  constexpr const char* regular[8]={"GB12W2A","GB10W3A","GB9W4A","GB8W3A","GB6W5A","GB8W3A","GB9W4A","GB10W3A"};
  constexpr const char* special[8]={"SNG12A","SNG10A","SNG9A","SNG8A","SNG6A","SNG8A","SNG9A","SNG10A"};
  constexpr u16 flags[8]={2,0x12,0x12,0x12,2,2,2,2};
  for(u16 type:{u16(0),u16(2)})for(u16 dir=1;dir<=8;++dir){
   HulkStandingFrame table{};CHECK(hulk_standing_frame(type,dir,table));
   CHECK(name_equal(table.image,type==2?special[dir-1]:regular[dir-1]));CHECK(table.flags==flags[dir-1]);
  }
  HulkStandingFrame unchanged{"KEEP",0x7777};CHECK(!hulk_standing_frame(0,0,unchanged));
  CHECK(name_equal(unchanged.image,"KEEP")&&unchanged.flags==0x7777);
  EditorImages regular_art,special_art;CHECK(regular_art.open(pack->find("FINGRNT.IMG")->bytes));
  CHECK(special_art.open(pack->find("NSNAK.IMG")->bytes));
  for(u16 type:{u16(0),u16(2)}){
   const EditorImages& art=type==2?special_art:regular_art;
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();ProcessHandle actor;
   CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   ObjectHandle object;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,type,{0x0237213du,0x01759d7eu},object));
   u32 anchor_x=0,anchor_y=0;CHECK(sprite_anchor(objects.cells[object].sprite,anchor_x,anchor_y));
   for(u16 dir=1;dir<=8;++dir){
    CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::direction,dir));
    CHECK(source_hulk_get_stand(processes,objects,actor,type));
    HulkStandingFrame frame{};CHECK(hulk_standing_frame(type,dir,frame));unsigned expected_image=0;
    CHECK(resolve_animation_image(art,frame.image,expected_image));
    CHECK(objects.cells[object].sprite.image==expected_image&&objects.cells[object].sprite.flags==frame.flags);
    u32 x=0,y=0;CHECK(sprite_anchor(objects.cells[object].sprite,x,y));CHECK(x==anchor_x&&y==anchor_y);
   }
   auto before=objects.cells[object].sprite;
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::direction,0));
   CHECK(!source_hulk_get_stand(processes,objects,actor,type));
   CHECK(equal(&before,&objects.cells[object].sprite,sizeof before));
   CHECK(processes.validate()&&objects.validate());
  }
 });
 group("L8 OHULK initial DO_DIFF/JUST_SK/GET_STAND tail follows source order",[&]{
  EditorImages art;CHECK(art.open(pack->find("FINGRNT.IMG")->bytes));
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
  ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,
      {565u<<16,391u<<16},hulk));
  ObjectHandle p1;CHECK(objects.allocate(p1));
  objects.cells[p1].sprite.position.x=662u<<16;
  objects.cells[p1].sprite.position.y=463u<<16;
  const u16 player_torsos[2]={p1,no_object};
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::direction,3));
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::level,0));
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::seek_player,0));

  // OHULK executes DO_DIFF, JUST_SK (GET_DIS then SETUP_DIR), GET_STAND.
  CHECK(source_hulk_do_diff(processes.cells[actor]));
  u32 value=0;CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::diagonal_x_speed,value)&&value==0x6660);
  CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::diagonal_y_speed,value)&&value==0x5998);
  HCountFixture hcount{0x59a3};SourceRng rng{0x12345678};SourceRng expected_rng=rng;
  const u16 expected_distance=u16(expected_rng.range(1,11,hcount.value));
  SourceHulkJustSkResult result{0xbeef,{1,9}};
  CHECK(source_hulk_just_sk(processes,actor,objects,player_torsos,rng,
                            read_hcount,&hcount,result));
  CHECK(hcount.reads==1&&rng.seed==expected_rng.seed);
  CHECK(result.distance==expected_distance&&result.target.player_slot==0&&result.target.direction==4);
  CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::distance,value)&&value==expected_distance);
  CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::seek_direction,value)&&value==4);
  CHECK(source_hulk_get_stand(processes,objects,actor,0));
  unsigned expected_image=0;CHECK(resolve_animation_image(art,"GB9W4A",expected_image));
  CHECK(objects.cells[hulk].sprite.image==expected_image&&objects.cells[hulk].sprite.flags==0x12);
  CHECK(processes.validate()&&objects.validate());

  // Source order is observable on a later boundary failure: GET_DIS has
  // already consumed HCOUNT and committed its PDATA/RNG result.
  NativeProcesses partial_processes;partial_processes.initialize();NativeObjects partial_objects;partial_objects.initialize();
  ProcessHandle partial_actor;CHECK(partial_processes.create(source_hulk_process_id,0x4f48554cu,nullptr,partial_actor));
  ObjectHandle partial_hulk;CHECK(source_ohulk_beginobj2(partial_processes,partial_objects,partial_actor,art,0,
      {565u<<16,391u<<16},partial_hulk));
  CHECK(source_hulk_pdata_write(partial_processes.cells[partial_actor],SourceHulkField::level,0));
  HCountFixture partial_hcount{0x59a3};SourceRng partial_rng{0x12345678};
  SourceHulkJustSkResult unchanged{0xbeef,{1,9}};
  CHECK(!source_hulk_just_sk(partial_processes,partial_actor,partial_objects,nullptr,
                             partial_rng,read_hcount,&partial_hcount,unchanged));
  CHECK(partial_hcount.reads==1&&partial_rng.seed!=0x12345678);
  CHECK(source_hulk_pdata_read(partial_processes.cells[partial_actor],SourceHulkField::distance,value)&&value>=2&&value<=11);
  CHECK(unchanged.distance==0xbeef&&unchanged.target.player_slot==1&&unchanged.target.direction==9);
 });
 group("L8 HULKS LUP_TOP routes HALT, BUYIN and measured stand/facing path",[&]{
  EditorImages art;CHECK(art.open(pack->find("FINGRNT.IMG")->bytes));
  const auto make_actor=[&](NativeProcesses& processes,NativeObjects& objects,
                            ProcessHandle& actor,ObjectHandle& hulk,ObjectHandle& p1){
   processes.initialize();objects.initialize();
   CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,{565u<<16,391u<<16},hulk));
   CHECK(objects.allocate(p1));
   objects.cells[p1].sprite.position.x=662u<<16;
   objects.cells[p1].sprite.position.y=463u<<16;
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::level,0));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::direction,3));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::seek_direction,4));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::seek_player,0));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::control_flags,0x0100));
   CHECK(source_hulk_do_diff(processes.cells[actor]));
  };
  {
   NativeProcesses processes;NativeObjects objects;ProcessHandle actor;ObjectHandle hulk,p1;
   make_actor(processes,objects,actor,hulk,p1);const u16 torsos[2]={p1,no_object};
   SourceHulkLupTopWorld world{0,0,1,0,torsos};ProcessAction action{};
   CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   CHECK(action.kind==ProcessAction::sleep&&action.delay==2&&action.next==source_hulk_resume_lup_top);
   u32 value=0;CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::direction,value)&&value==4);
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::control_flags,value)&&value==0x0101);
   unsigned image=0;CHECK(resolve_animation_image(art,"GB8W3A",image));
   CHECK(objects.cells[hulk].sprite.image==image&&objects.cells[hulk].sprite.flags==0x12);
   CHECK(objects.cells[hulk].sprite.position.vx==0&&objects.cells[hulk].sprite.position.vy==0);

   // Once already facing SK_DIR, source DOLOGIC calls SETUP_DIR then DO_DIFF
   // before the same DONE_SPIN two-tick wait.
   CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   CHECK(action.kind==ProcessAction::sleep&&action.delay==2);
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::seek_direction,value)&&value==4);
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::horizontal_speed,value)&&value==0x8000);
  }
  {
   NativeProcesses processes;NativeObjects objects;ProcessHandle actor;ObjectHandle hulk,p1;
   make_actor(processes,objects,actor,hulk,p1);ProcessHandle buyin;
   CHECK(processes.create(source_buyin_process_id,0x4255494eu,nullptr,buyin));
   const u16 torsos[2]={p1,no_object};SourceHulkLupTopWorld world{0,0,1,0,torsos};
   const NativeProcess before=processes.cells[actor];const NativeSprite sprite=objects.cells[hulk].sprite;
   ProcessAction action{};auto bat_start=source_hulk_lup_top(processes,objects,actor,world,action);
   if(!bat_start)throw std::runtime_error(bat_start.message?bat_start.message:"BAT start failed");
   CHECK(action.kind==ProcessAction::sleep&&action.delay==2);
   CHECK(equal(&before,&processes.cells[actor],sizeof before));
   CHECK(equal(&sprite,&objects.cells[hulk].sprite,sizeof sprite));
  }
  {
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   SourceHulkLupTopWorld halted{1,0,1,0,nullptr};ProcessAction action{};
   CHECK(source_hulk_lup_top(processes,objects,actor,halted,action));
   CHECK(action.kind==ProcessAction::sleep&&action.delay==3&&action.next==source_hulk_resume_lup_top);
  }
  {
  NativeProcesses processes;NativeObjects objects;ProcessHandle actor;ObjectHandle hulk,p1;
  make_actor(processes,objects,actor,hulk,p1);
  objects.cells[hulk].sprite.position.vx=0x12345678;
  objects.cells[hulk].sprite.position.vy=0x87654321;
  const auto sprite=objects.cells[hulk].sprite;
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::batting,1));
   const auto batting=processes.cells[actor];ProcessAction action{ProcessAction::fault,9,0xabcdef01u,{}};
   const auto unchanged=action;SourceHulkLupTopWorld world{0,0,1,0,nullptr};
   CHECK(!source_hulk_lup_top(processes,objects,actor,world,action));
   CHECK(action.kind==unchanged.kind&&action.delay==unchanged.delay&&action.next==unchanged.next);
   CHECK(equal(&batting,&processes.cells[actor],sizeof batting));
   CHECK(objects.cells[hulk].sprite.position.vx==0&&objects.cells[hulk].sprite.position.vy==0);
   CHECK(objects.cells[hulk].sprite.image==sprite.image&&objects.cells[hulk].sprite.flags==sprite.flags);
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::batting,0));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::control_flags,0x010c));
   SourceHulkLupTopWorld doorway{0,0,1,0,nullptr};
   CHECK(source_hulk_lup_top(processes,objects,actor,doorway,action));
   u32 value=0;CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::control_flags,value)&&value==0x0101);
   CHECK(action.kind==ProcessAction::sleep&&action.delay==2);
 }
 });
 group("L8 HULKS LP5 HULK_DN runs REALO in object/count/DIE order",[&]{
  EditorImages art;const auto* entry=pack->find("FINGRNT.IMG");CHECK(entry&&art.open(entry->bytes));
  const auto make_realo_actor=[&](NativeProcesses& processes,NativeObjects& objects,
                                 ProcessHandle& actor,ObjectHandle& hulk){
   processes.initialize();objects.initialize();
   CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,{565u<<16,391u<<16},hulk));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::control_flags,0x0100));
  };
  NativeProcesses processes;NativeObjects objects;
  ProcessHandle actor;ObjectHandle hulk;make_realo_actor(processes,objects,actor,hulk);
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::control_flags,0x0104));
  u16 count=1;SourceHulkLupTopWorld world{};world.hulk_down=1;world.hulk_count=&count;
  ProcessAction action{};CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
  CHECK(action.kind==ProcessAction::die&&count==0);
  CHECK(objects.count==0&&!objects.cells[hulk].allocated&&!objects.cells[hulk].listed);
  CHECK(objects.validate()&&processes.cells[actor].active); // DIE is returned to the owner
  CHECK(processes.kill(actor)&&!processes.cells[actor].active);

  NativeProcesses missing_processes;missing_processes.initialize();NativeObjects missing_objects;missing_objects.initialize();
  ProcessHandle missing_actor;ObjectHandle missing_hulk;
  make_realo_actor(missing_processes,missing_objects,missing_actor,missing_hulk);
  CHECK(source_hulk_pdata_write(missing_processes.cells[missing_actor],SourceHulkField::control_flags,0x0104));
  SourceHulkLupTopWorld missing_world{};missing_world.hulk_down=1;
  ProcessAction unchanged{ProcessAction::sleep,7,0xabcdef01u,{}};const auto before=unchanged;
  CHECK(!source_hulk_lup_top(missing_processes,missing_objects,missing_actor,missing_world,unchanged));
  CHECK(unchanged.kind==before.kind&&unchanged.delay==before.delay&&unchanged.next==before.next);
  CHECK(missing_objects.count==1&&missing_objects.cells[missing_hulk].allocated);

 });
 group("L8 HULKS IBAT chooses source swing, repeats both FRANIM passes and executes BATDN",[&]{
  const auto run_case=[&](u16 hulk_type,const char* bank_name,
                          i32 target_x,i32 target_y,const char* expected_label,
                          u16 player_number,const char* secondary_name=nullptr){
   EditorImages art;const auto* entry=pack->find(bank_name);CHECK(entry&&art.open(entry->bytes));
   EditorImages secondary;const EditorImages* secondary_ptr=nullptr;
   if(secondary_name){const auto* alternate=pack->find(secondary_name);CHECK(alternate&&secondary.open(alternate->bytes));secondary_ptr=&secondary;}
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,
       source_hulk_resume_lup_top,nullptr,actor));
   ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,hulk_type,
       {200u<<16,160u<<16},hulk));
   ObjectHandle p1,p2;CHECK(objects.allocate(p1));CHECK(objects.allocate(p2));
   objects.cells[p1].sprite.position.x=player_number==1?u32(target_x)<<16:300u<<16;
   objects.cells[p1].sprite.position.y=player_number==1?u32(target_y)<<16:260u<<16;
   objects.cells[p2].sprite.position.x=player_number==2?u32(target_x)<<16:100u<<16;
   objects.cells[p2].sprite.position.y=player_number==2?u32(target_y)<<16:80u<<16;
   const u16 torsos[2]={p1,p2};
   NativeProcess& process=processes.cells[actor];
   CHECK(source_hulk_pdata_write(process,SourceHulkField::direction,4));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::level,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,0x0100));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::batting,player_number));
   CHECK(source_hulk_do_diff(process));
   u32 anchor_x=0,anchor_y=0;CHECK(sprite_anchor(objects.cells[hulk].sprite,anchor_x,anchor_y));
   objects.cells[hulk].sprite.position.vx=0x12345678;
   objects.cells[hulk].sprite.position.vy=0x87654321;
   SourceHulkLupTopWorld world{};world.ehalt=1;world.hulk_type=hulk_type;
   world.player_torsos=torsos;world.secondary_art=secondary_ptr;
   ProcessAction action{};auto bat_start=source_hulk_lup_top(processes,objects,actor,world,action);
   if(!bat_start)throw std::runtime_error(bat_start.message?bat_start.message:"BAT start failed");
   const SourceAnimation* script=nullptr;u16 cursor=0;
   CHECK(decode_source_animation_cursor(process.saved[1],script,cursor));
   CHECK(name_equal(script->label,expected_label)&&cursor==1&&script->count>0);
   CHECK(process.saved[2]!=0&&objects.cells[hulk].sprite.position.vx==0&&
         objects.cells[hulk].sprite.position.vy==0);
   const auto check_frame=[&](u16 frame){
    const auto& sprite=objects.cells[hulk].sprite;unsigned image=0;
    CHECK(resolve_animation_image(*sprite.art,script->frames[frame].image,image));
    CHECK(sprite.image==image);u32 x=0,y=0;CHECK(sprite_anchor(sprite,x,y));
    CHECK(x==anchor_x&&y==anchor_y);
   };
   check_frame(0);
   if(secondary_ptr&&art.find(script->frames[0].image)<0)
    CHECK(objects.cells[hulk].sprite.art==secondary_ptr);
   CHECK(action.kind==ProcessAction::sleep&&
         action.delay==u16(script->frames[0].control&255u)&&
         action.next==source_hulk_resume_bat_first);

   // FRANIM A1=1 retains its source return across each per-frame sleep.
   for(u16 frame=1;frame<script->count;++frame){
    process.continuation=action.next;
    auto bat_step=source_hulk_lup_top(processes,objects,actor,world,action);
    if(!bat_step)throw std::runtime_error(bat_step.message?bat_step.message:"BAT frame failed");
    CHECK(action.next==source_hulk_resume_bat_first);
    CHECK(decode_source_animation_cursor(process.saved[1],script,cursor)&&cursor==u16(frame+1));
    check_frame(frame);
   }
   process.continuation=action.next;
   CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   CHECK(action.next==source_hulk_resume_bat_second);
   CHECK(decode_source_animation_cursor(process.saved[1],script,cursor)&&cursor==1);
   check_frame(0); // second pass starts immediately
   for(u16 frame=1;frame<script->count;++frame){
    process.continuation=action.next;
    CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
    CHECK(action.next==source_hulk_resume_bat_second);
    CHECK(decode_source_animation_cursor(process.saved[1],script,cursor)&&cursor==u16(frame+1));
    check_frame(frame);
   }
   process.continuation=action.next;
   CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   u32 value=0;
   CHECK(source_hulk_pdata_read(process,SourceHulkField::batting,value)&&value==0);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==0x0100);
   CHECK(decode_source_animation_cursor(process.saved[1],script,cursor)&&cursor==0);
   CHECK(action.kind==ProcessAction::sleep&&action.delay==1&&
         action.next==source_hulk_resume_lup_top);
   CHECK(objects.cells[hulk].sprite.position.vx==0x6660&&
         objects.cells[hulk].sprite.position.vy==0x5998);
  };
  run_case(0,"FINGRNT.IMG",250,240,"BAT4O",1);
  run_case(0,"FINGRNT.IMG",150,240,"BAT8O",1);
  run_case(0,"FINGRNT.IMG",250,100,"BAT2O",1);
  run_case(0,"FINGRNT.IMG",150,100,"BAT10O",2);
  run_case(2,"NSNAK.IMG",250,240,"BAT4OS",1);
  run_case(2,"NSNAK.IMG",150,240,"BAT8OS",1);
  run_case(2,"NSNAK.IMG",250,100,"BAT2OS",1);
  run_case(2,"NSNAK.IMG",150,100,"BAT10OS",2);
  run_case(1,"NSNAK.IMG",250,240,"BAT4OS",1,"FINGRNT.IMG");
  run_case(1,"NSNAK.IMG",150,240,"BAT8OS",1,"FINGRNT.IMG");
 });
 group("L8 HULKS GET_RUN and DOHULK advance pointer-free FRANIM cursors",[&]{
  EditorImages regular;CHECK(regular.open(pack->find("FINGRNT.IMG")->bytes));
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
  ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,regular,0,{565u<<16,391u<<16},hulk));
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::level,3));
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::direction,3));
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::control_flags,0x0101));
  CHECK(source_hulk_get_run(processes.cells[actor],0));
  const SourceAnimation* script=nullptr;u16 cursor=0;
  CHECK(decode_source_animation_cursor(processes.cells[actor].saved[1],script,cursor));
  CHECK(name_equal(script->label,"TM_PWK_RGT")&&cursor==0);
  u32 flags=0;CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::control_flags,flags)&&flags==0x0100);
  for(u16 i=0;i<script->count;++i){
   SourceHulkDohulkStep step{};CHECK(source_hulk_dohulk(processes,objects,actor,0,step));
   CHECK(step.kind==SourceHulkDohulkStep::frame&&step.action.kind==ProcessAction::sleep);
   CHECK(step.action.delay==u16(script->frames[i].control&255u)&&step.action.next==source_hulk_resume_lup_top);
   CHECK(decode_source_animation_cursor(processes.cells[actor].saved[1],script,cursor)&&cursor==u16(i+1));
   unsigned image=0;CHECK(resolve_animation_image(regular,script->frames[i].image,image));
   CHECK(objects.cells[hulk].sprite.image==image);
  }
  SourceHulkDohulkStep ended{};CHECK(source_hulk_dohulk(processes,objects,actor,0,ended));
  CHECK(ended.kind==SourceHulkDohulkStep::script_restarted);
  CHECK(decode_source_animation_cursor(processes.cells[actor].saved[1],script,cursor)&&
        name_equal(script->label,"TM_PWK_RGT")&&cursor==0);

  // LP5/DOHULK loops to LUP_TOP without sleeping at the list terminator, so
  // the same call processes the new script's first frame before yielding.
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::control_flags,0x0104));
  SourceHulkLupTopWorld world{0,0,1,0,nullptr};ProcessAction action{};
  CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
  CHECK(action.kind==ProcessAction::sleep&&action.delay==u16(script->frames[0].control&255u));
  CHECK(decode_source_animation_cursor(processes.cells[actor].saved[1],script,cursor)&&cursor==1);
  unsigned first_image=0;CHECK(resolve_animation_image(regular,script->frames[0].image,first_image));
  CHECK(objects.cells[hulk].sprite.image==first_image);

  EditorImages alternate;CHECK(alternate.open(pack->find("NSNAK.IMG")->bytes));
  NativeProcesses snake_processes;snake_processes.initialize();NativeObjects snake_objects;snake_objects.initialize();
  ProcessHandle snake_actor;CHECK(snake_processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,snake_actor));
  ObjectHandle snake;CHECK(source_ohulk_beginobj2(snake_processes,snake_objects,snake_actor,alternate,2,{565u<<16,391u<<16},snake));
  CHECK(source_hulk_pdata_write(snake_processes.cells[snake_actor],SourceHulkField::level,6));
  CHECK(source_hulk_pdata_write(snake_processes.cells[snake_actor],SourceHulkField::direction,8));
  CHECK(source_hulk_pdata_write(snake_processes.cells[snake_actor],SourceHulkField::control_flags,1));
  CHECK(source_hulk_get_run(snake_processes.cells[snake_actor],2));
  CHECK(decode_source_animation_cursor(snake_processes.cells[snake_actor].saved[1],script,cursor));
  CHECK(name_equal(script->label,"T2F_PWK_UPLFT")&&cursor==0);
  SourceHulkDohulkStep snake_step{};CHECK(source_hulk_dohulk(snake_processes,snake_objects,snake_actor,2,snake_step));
  CHECK(snake_step.kind==SourceHulkDohulkStep::frame&&snake_step.action.delay==u16(script->frames[0].control&255u));

  const NativeProcess before=snake_processes.cells[snake_actor];
  CHECK(source_hulk_pdata_write(snake_processes.cells[snake_actor],SourceHulkField::direction,0));
  const NativeProcess invalid=snake_processes.cells[snake_actor];
  CHECK(!source_hulk_get_run(snake_processes.cells[snake_actor],2));
  CHECK(equal(&invalid,&snake_processes.cells[snake_actor],sizeof invalid));
  CHECK(before.saved[1]!=0&&snake_objects.validate()&&objects.validate());
 });
 group("L8 HULKS MV HAGE/LEVEL prefix matches first-frame trace and source order",[]{
  NativeProcesses processes;processes.initialize();ProcessHandle actor;
  CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
  NativeProcess& hulk=processes.cells[actor];SourceHulkMvAgeResult result{true,0xbeef};u32 value=0;
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::level,0));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::keep_age,300));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::horizontal_speed,0x11111111));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::diagonal_x_speed,0x22222222));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::vertical_speed,0x33333333));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::diagonal_y_speed,0x44444444));
  struct FirstMvAge{u16 before,after;};
  constexpr FirstMvAge traced_first_mv[]={{0x012c,0x012b},{0x01a4,0x01a3},{0x005a,0x0059},{0x00dc,0x00db}};
  for(const auto& sample:traced_first_mv){
   CHECK(source_hulk_pdata_write(hulk,SourceHulkField::age,sample.before));
   CHECK(source_hulk_mv_age(hulk,result));
   CHECK(!result.level_changed&&result.age==sample.after);
   CHECK(source_hulk_pdata_read(hulk,SourceHulkField::age,value)&&value==sample.after);
   CHECK(source_hulk_pdata_read(hulk,SourceHulkField::level,value)&&value==0);
   CHECK(source_hulk_pdata_read(hulk,SourceHulkField::horizontal_speed,value)&&value==0x11111111);
  }

  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::age,1));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::level,2));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::keep_age,0x005a));
  CHECK(source_hulk_mv_age(hulk,result));
  HulkSpeeds speeds{};CHECK(hulk_speeds(3,speeds));
  CHECK(result.level_changed&&result.age==0x005a);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::level,value)&&value==3);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::age,value)&&value==0x005a);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::horizontal_speed,value)&&value==speeds.horizontal);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::diagonal_x_speed,value)&&value==speeds.diagonal_x);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::vertical_speed,value)&&value==speeds.vertical);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::diagonal_y_speed,value)&&value==speeds.diagonal_y);

  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::age,1));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::level,10));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::horizontal_speed,0xabcdef01));
  CHECK(source_hulk_mv_age(hulk,result));
  CHECK(!result.level_changed&&result.age==0);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::level,value)&&value==10);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::horizontal_speed,value)&&value==0xabcdef01);

  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::age,0x0401));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::level,0));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::keep_age,0x0063));
  CHECK(source_hulk_mv_age(hulk,result));
  CHECK(result.level_changed&&result.age==0x0063);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::level,value)&&value==1);

  // The source commits HAGE=0 and LEVEL++ before DO_DIFF faults on this
  // invalid state; the four old speed words and the caller result stay intact.
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::age,1));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::level,0x8000));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::keep_age,90));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::horizontal_speed,0x11111111));
  const SourceHulkMvAgeResult unchanged=result;
  CHECK(!source_hulk_mv_age(hulk,result));
  CHECK(result.level_changed==unchanged.level_changed&&result.age==unchanged.age);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::age,value)&&value==0);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::level,value)&&value==0x8001);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::horizontal_speed,value)&&value==0x11111111);

  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::age,0));
  CHECK(source_hulk_pdata_write(hulk,SourceHulkField::level,4));
  CHECK(source_hulk_mv_age(hulk,result));
  CHECK(!result.level_changed&&result.age==0xffff);
  CHECK(source_hulk_pdata_read(hulk,SourceHulkField::level,value)&&value==4);
 });
 group("L8 HULKS MV dispatches age, DIS, FRANIM and one-step spin turns",[&]{
  EditorImages art;CHECK(art.open(pack->find("FINGRNT.IMG")->bytes));
  {
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,
                                                 {565u<<16,391u<<16},hulk));
   NativeProcess& process=processes.cells[actor];
   CHECK(source_hulk_pdata_write(process,SourceHulkField::age,0x012c));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::level,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::distance,2));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::direction,4));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_direction,5));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,0));
   CHECK(source_hulk_do_diff(process));CHECK(source_hulk_get_run(process,0));
   const SourceAnimation* run=find_animation("HULKS.ASM","T_PWK_DNRGT");CHECK(run&&run->count);
   SourceHulkLupTopWorld world{};world.hulk_type=0;world.ehalt=0;
   ProcessAction action{};CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   u32 value=0;CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==0x012b);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::distance,value)&&value==1);
   CHECK(action.kind==ProcessAction::sleep&&action.delay==u16(run->frames[0].control&255u));
   const SourceAnimation* cursor_animation=nullptr;u16 cursor=0;
   CHECK(decode_source_animation_cursor(process.saved[1],cursor_animation,cursor));
   CHECK(cursor_animation==run&&cursor==1);
   unsigned expected_image=0;CHECK(resolve_animation_image(art,run->frames[0].image,expected_image));
   CHECK(objects.cells[hulk].sprite.image==expected_image);

   // On DIS expiry, the source stores zero before entering the reseek path.
   // Without an HCOUNT/player service the native boundary must remain named,
   // while the earlier HAGE and DIS writes remain committed.
   CHECK(!source_hulk_lup_top(processes,objects,actor,world,action));
   CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==0x012a);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::distance,value)&&value==0);
  }
  {
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,
                                                 {565u<<16,391u<<16},hulk));
   NativeProcess& process=processes.cells[actor];
   CHECK(source_hulk_pdata_write(process,SourceHulkField::age,0x005a));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::level,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::direction,4));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_direction,5));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,1));
   SourceHulkLupTopWorld world{};world.hulk_type=0;world.ehalt=0;
   ProcessAction action{};CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   u32 value=0;CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==0x0059);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::direction,value)&&value==5);
   CHECK(action.kind==ProcessAction::sleep&&action.delay==3&&
         action.next==source_hulk_resume_lup_top);
   HulkStandingFrame frame{};CHECK(hulk_standing_frame(0,5,frame));
   unsigned expected_image=0;CHECK(resolve_animation_image(art,frame.image,expected_image));
   CHECK(objects.cells[hulk].sprite.image==expected_image&&
         objects.cells[hulk].sprite.flags==frame.flags&&objects.validate());
  }
  {
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,
                                                 {565u<<16,391u<<16},hulk));
   ObjectHandle p1;CHECK(objects.allocate(p1));
   objects.cells[p1].sprite.position.x=662u<<16;
   objects.cells[p1].sprite.position.y=463u<<16;
   NativeProcess& process=processes.cells[actor];
   CHECK(source_hulk_pdata_write(process,SourceHulkField::age,0x012c));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::level,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::distance,1));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::direction,4));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_direction,5));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_player,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,0));
   CHECK(source_hulk_do_diff(process));
   const u16 torsos[2]={p1,no_object};
   SourceHulkLupTopWorld world{};world.hulk_type=0;world.ehalt=0;
   SourceRng rng{0x12345678};world.rng=&rng;HCountFixture hcount{0x59a3};
   world.read_hcount=read_hcount;world.hcount_context=&hcount;world.player_torsos=torsos;
   SourceRng expected_rng{0x12345678};
   const u16 expected_distance=u16(expected_rng.range(1,11,0x59a3));
   ProcessAction action{};CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   u32 value=0;CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==0x012b);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::distance,value)&&value==expected_distance);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::seek_direction,value)&&value==4);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==1);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::oscillation,value)&&value==2);
   CHECK(hcount.reads==1&&rng.seed==expected_rng.seed);
   CHECK(action.kind==ProcessAction::sleep&&action.delay==3&&
         action.next==source_hulk_resume_lup_top);
   CHECK(objects.cells[hulk].sprite.position.vx==0x6660&&
         objects.cells[hulk].sprite.position.vy==0x5998);
  }
  {
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,
                                                 {565u<<16,391u<<16},hulk));
   ObjectHandle p1;CHECK(objects.allocate(p1));
   objects.cells[p1].sprite.position.x=662u<<16;
   objects.cells[p1].sprite.position.y=463u<<16;
   NativeProcess& process=processes.cells[actor];
   CHECK(source_hulk_pdata_write(process,SourceHulkField::age,0x005a));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::level,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::distance,9));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::direction,5));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_direction,5));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_player,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,1));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::oscillation,0));
   CHECK(source_hulk_do_diff(process));
   const u16 torsos[2]={p1,no_object};
   SourceHulkLupTopWorld world{};world.hulk_type=0;world.ehalt=0;
   SourceRng rng{0x87654321};world.rng=&rng;HCountFixture hcount{0x159a};
   world.read_hcount=read_hcount;world.hcount_context=&hcount;world.player_torsos=torsos;
   ProcessAction action{};CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   u32 value=0;CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==0x0059);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::seek_direction,value)&&value==4);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==1);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::oscillation,value)&&value==1);
   CHECK(hcount.reads==1&&rng.seed!=0x87654321);
   CHECK(action.kind==ProcessAction::sleep&&action.delay==1);
  }
 });
 group("L8 HULKS gang DIS expiry retries offscreen and retargets only inside PLYRPRCS bounds",[&]{
  EditorImages art;CHECK(art.open(pack->find("FINGRNT.IMG")->bytes));
  auto run_case=[&](u32 x,u32 y,bool visible,bool use_second_process){
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,source_hulk_resume_lup_top,nullptr,actor));
   ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,
                                                 {200u<<16,160u<<16},hulk));
   objects.cells[hulk].sprite.position.x=x;objects.cells[hulk].sprite.position.y=y;
   ProcessHandle player;CHECK(processes.create(0x10,0x504c5931u,nullptr,player));
   const PlayerBounds bounds{150u<<16,250u<<16,120u<<16,220u<<16};
   auto& player_process=processes.cells[player];
   CHECK(write_field({player_process.data,sizeof player_process.data},{320},32,bounds.top));
   CHECK(write_field({player_process.data,sizeof player_process.data},{352},32,bounds.bottom));
   CHECK(write_field({player_process.data,sizeof player_process.data},{384},32,bounds.left));
   CHECK(write_field({player_process.data,sizeof player_process.data},{416},32,bounds.right));
   ObjectHandle torso;CHECK(objects.allocate(torso));
   objects.cells[torso].sprite.position.x=230u<<16;
   objects.cells[torso].sprite.position.y=180u<<16;
   const ProcessHandle player_processes[2]={use_second_process?no_process:player,
                                             use_second_process?player:no_process};
   const u16 player_torsos[2]={torso,no_object};
   auto& process=processes.cells[actor];
   CHECK(source_hulk_pdata_write(process,SourceHulkField::age,0x012c));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::level,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::distance,1));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::direction,4));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_direction,1));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_player,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,0x0010));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::oscillation,0));
   CHECK(source_hulk_do_diff(process));CHECK(source_hulk_get_run(process,0));
   const SourceAnimation* run=nullptr;u16 cursor=0;
   CHECK(decode_source_animation_cursor(process.saved[1],run,cursor));CHECK(run&&run->count);
   SourceHulkLupTopWorld world{};world.ehalt=0;world.hulk_type=0;
   world.player_torsos=player_torsos;world.player_processes=player_processes;
   SourceRng rng{0x64329187};world.rng=&rng;const u32 initial_rng=rng.seed;
   HCountFixture hcount{0x1563};world.read_hcount=read_hcount;world.hcount_context=&hcount;
   SourceRng expected_rng{initial_rng};
   const u16 expected_distance=u16(expected_rng.range(1,11,hcount.value));
   ProcessAction action{};CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   u32 value=0;CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==0x012b);
   if(!visible){
    CHECK(source_hulk_pdata_read(process,SourceHulkField::distance,value)&&value==5);
    CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==0x0010);
    CHECK(rng.seed==initial_rng&&hcount.reads==0);
    CHECK(action.kind==ProcessAction::sleep&&
          action.delay==u16(run->frames[0].control&255u));
   }else{
    CHECK(source_hulk_pdata_read(process,SourceHulkField::distance,value)&&value==expected_distance);
    CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==1);
    CHECK(source_hulk_pdata_read(process,SourceHulkField::oscillation,value)&&value==2);
    CHECK(rng.seed==expected_rng.seed&&hcount.reads==1);
    CHECK(action.kind==ProcessAction::sleep&&action.delay==3&&
          action.next==source_hulk_resume_lup_top);
   }
  };
  run_case(149u<<16,160u<<16,false,false); // left is below the inclusive boundary
  run_case(150u<<16,120u<<16,true,false);  // left/top edges are inclusive
  run_case(250u<<16,120u<<16,true,true);   // right edge inclusive; PLYRPRCS[1] fallback
  run_case(200u<<16,220u<<16,false,false); // bottom edge is exclusive
 });
 group("L8 resumed Hulk INTO diagonal writes match the native HULKS kernel",[]{HulkSpeeds speeds;CHECK(hulk_speeds(0,speeds));CHECK(speeds.diagonal_x==0x6660&&speeds.diagonal_y==0x5998);struct Sample{u16 direction;u32 vx,vy;};constexpr Sample samples[]={{4,0x6660,0x5998},{4,0x6660,0x5998},{6,0xffff99a0,0x5998},{2,0x6660,0xffffa668}};for(const auto& sample:samples){SourcePosition position{};CHECK(hulk_velocity(sample.direction,speeds,position));CHECK(position.vx==sample.vx&&position.vy==sample.vy);}});
 group("L8 resumed Hulk GETXY seeks match sampled actor and target coordinates",[]{struct Sample{i32 hulk_x,hulk_y,target_x,target_y;u16 seek;};constexpr Sample samples[]={{0x231+4,0x187,0x296,0x1cf,4},{0x25e + 4,0x198,0x296,0x1cf,4},{0x31e + 9,0x18e,0x296,0x1cf,6},{0x23d+6,0x21b,0x296,0x1cf,2}};for(const auto& sample:samples)CHECK(u16(hulk_direction_to(sample.hulk_x,sample.hulk_y,sample.target_x,sample.target_y)+1)==sample.seek);});
 group("L8 resumed Hulk WAY steps match observed spin and facing writes",[]{struct Sample{u16 facing,seek,observed;};constexpr Sample samples[]={{4,5,5},{4,7,5},{5,7,6},{6,7,7}};for(const auto& sample:samples)CHECK(turn_hulk(sample.facing,sample.seek)==sample.observed);});
 group("UTIL RANDOM stored seed before carry/rotation; independent wide formula",[]{for(u32 seed:{0u,1u,0x3fffffffu,0x40000000u,0x7fffffffu,0x80000000u,0xbfffffffu,0xffffffffu})for(unsigned rot=0;rot<64;++rot){SourceRng r{seed};u32 shifted=u32(u64(seed)*2);bool top=seed>=0x80000000u,next=(seed&0x40000000u)!=0;if(top==next)shifted|=2;u32 carry_result=shifted+u32(top);u64 rotated=(u64(carry_result)<<(rot%32))|(u64(carry_result)>>(32-(rot%32)));CHECK(r.random(u16(rot))==u32(rotated));CHECK(r.seed==shifted);}SourceRng r{0x12345678};for(unsigned i=0;i<1000;++i){u32 x=r.range(1,11,u16(i));CHECK(x>=2&&x<=11);}});
 group("Source pack and all native IMG validation",[&]{CHECK(pack->count==58);unsigned total=0;for(unsigned i=0;i<pack->count;++i){std::string name=pack->entries[i].name;if(name.ends_with(".IMG")){EditorImages a;CHECK(a.open(pack->entries[i].bytes));total+=a.count;}}CHECK(total==1969);auto corrupt=bytes;corrupt.back()^=1;auto bad=std::make_unique<SourcePack>();CHECK(!bad->open({corrupt.data(),corrupt.size()}));CHECK(bad->count==0);});
 group("All 14 background pairs/51 modules; invalid scratch references surfaced",[&]{auto bg=std::make_unique<EditorBackground>();unsigned modules=0,bad=0;for(unsigned i=0;i<pack->count;++i){std::string n=pack->entries[i].name;if(!n.ends_with(".BDD"))continue;auto layout=pack->find((n.substr(0,n.size()-4)+".BDB").c_str());CHECK(layout);CHECK(bg->open(pack->entries[i].bytes,layout->bytes));modules+=bg->module_count;bad+=bg->invalid_palette_placements;}CHECK(modules==51&&bad==4);});
 group("Generated source scripts and ANI rounded width anchor invariance",[&]{EditorImages player,hulk,snake,bullet;CHECK(player.open(pack->find("PLYRS2.IMG")->bytes));CHECK(hulk.open(pack->find("FINGRNT.IMG")->bytes));CHECK(snake.open(pack->find("NSNAK.IMG")->bytes));CHECK(bullet.open(pack->find("BMB.IMG")->bytes));CHECK(source_animation_count==138);unsigned frames=0;for(unsigned i=0;i<source_animation_count;++i){auto& a=source_animations[i];const bool alternate_bat=name_equal(a.label,"BAT8OS")||name_equal(a.label,"BAT4OS")||name_equal(a.label,"BAT10OS")||name_equal(a.label,"BAT2OS");auto bank=name_equal(a.module,"ROBO.ASM")?&player:name_equal(a.module,"SHOTS.ASM")?&bullet:(name_equal(a.label,"BLDOTS")?&player:(a.label[1]=='2'||alternate_bat?&snake:&hulk));NativeSprite s{bank,0,0,2,{100u<<16,100u<<16,0,0}};u32 ax,ay;CHECK(sprite_anchor(s,ax,ay));for(unsigned f=0;f<a.count;++f){auto result=apply_source_frame(s,a.frames[f]);if(!result)throw std::runtime_error(std::string(a.module)+":"+a.label+":"+a.frames[f].image+": "+result.message);CHECK(result);u32 x,y;CHECK(sprite_anchor(s,x,y));CHECK(x==ax&&y==ay);++frames;}}CHECK(frames==742);});
 group("L8 OHULK WAVEYX fractions and IH1 BEGINOBJ2 match captured first actor",[&]{
  EditorImages hulk;CHECK(hulk.open(pack->find("FINGRNT.IMG")->bytes));
  SourceOhulkCoordinates position;CHECK(source_apply_ohulk_waveyx(0x005f213du,0x00239d7eu,false,0x015201d8u,position));
  CHECK(position.a0_x_16_16==0x0237213du&&position.a1_value==0x01759d7eu);
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  ProcessHandle process;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,process));
  ObjectHandle actor;CHECK(source_ohulk_beginobj2(processes,objects,process,hulk,0,position,actor));
  CHECK(actor==0&&objects.count==1&&objects.cells[actor].allocated&&objects.cells[actor].listed);
  const auto& object=objects.cells[actor];CHECK(object.process==process&&object.id==0x8600&&object.z==159);
  CHECK(object.sprite.flags==0x8012&&object.sprite.image==unsigned(hulk.find("GB6W5A")));
  CHECK(object.sprite.position.x==0x022e213du&&object.sprite.position.y==0x01899d7eu);
  CHECK(processes.cells[process].saved[0]==actor);
  u32 anchor_x,anchor_y;CHECK(sprite_anchor(object.sprite,anchor_x,anchor_y));
  CHECK(anchor_x==position.a0_x_16_16&&anchor_y==position.a1_value);
  CHECK(processes.validate()&&objects.validate());
  ProcessHandle wrong;CHECK(processes.create(source_hulk_process_id-1,1,nullptr,wrong));
  ObjectHandle rejected=0;CHECK(!source_ohulk_beginobj2(processes,objects,wrong,hulk,0,position,rejected));
  CHECK(rejected==no_object&&objects.count==1&&processes.cells[wrong].saved[0]==0);
  EditorImages snake_hulk;CHECK(snake_hulk.open(pack->find("NSNAK.IMG")->bytes));
  ProcessHandle alternate;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,alternate));
  CHECK(source_ohulk_beginobj2(processes,objects,alternate,snake_hulk,2,position,rejected));
  CHECK(rejected==1&&objects.count==2&&objects.cells[rejected].sprite.image==unsigned(snake_hulk.find("SNG6A")));
  CHECK(objects.cells[rejected].id==source_hulk_object_id&&processes.cells[alternate].saved[0]==rejected);
  CHECK(processes.validate()&&objects.validate());
 });
 group("L8 OHULK ordered initialization reaches its first source-owned sleep",[&]{
  EditorImages hulk;CHECK(hulk.open(pack->find("FINGRNT.IMG")->bytes));
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  ObjectHandle player;CHECK(objects.allocate(player));
  objects.cells[player].sprite.position.x=662u<<16;
  objects.cells[player].sprite.position.y=463u<<16;
  const u16 player_torsos[2]={player,no_object};
  const u16 player_x[2]={662,0};
  ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
  u8 wave[14]{};put16(wave,source_hulk_wave_type);put16(wave+2,4);put16(wave+4,0);
  HulkAccounting accounting{};HCountFixture hcount{0x1234,0};
  u32 seed=1;while(SourceRng{seed}.range(0,10,hcount.value)!=1)++seed;
  SourceRng rng{seed};
  SourceOhulkInitialization input{};
  input.process=actor;input.art=&hulk;input.hulk_type=0;
  input.placement={0x005f213du,0x00239d7eu};
  input.wave_yx=0x015201d8u;input.wave_records={wave,sizeof wave};
  input.accounting=&accounting;input.player_object_x=player_x;
  input.player_torsos=player_torsos;input.rng=&rng;
  input.read_hcount=read_hcount;input.hcount_context=&hcount;
  input.ehalt=1;
  SourceOhulkInitializationResult result{};
  CHECK(source_ohulk_initialize(processes,objects,input,result));
  CHECK(result.object==1&&objects.cells[result.object].process==actor&&
        objects.cells[result.object].id==source_hulk_object_id);
  CHECK(result.coordinates.a0_x_16_16==0x0237213du&&
        result.coordinates.a1_value==0x01759d7eu);
  u32 anchor_x=0,anchor_y=0;
  CHECK(sprite_anchor(objects.cells[result.object].sprite,anchor_x,anchor_y));
  CHECK(anchor_x==0x0237213du&&anchor_y==0x01759d7eu);
  CHECK(result.setup.age_table_index==1&&result.setup.hage==300&&
        result.setup.level==0&&result.setup.count==1&&
        result.setup.remaining==3&&!result.setup.create_dexit);
  CHECK(accounting.count==1&&hcount.reads==2);
  CHECK(le16(wave+2)==3);
  u32 direction=0,seek=0,level=0,age=0,flags=0;
  CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::direction,direction));
  CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::seek_direction,seek));
  CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::level,level));
  CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::age,age));
  CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::control_flags,flags));
  CHECK(direction==(seek==3?3u:turn_hulk(3,u16(seek)))&&seek==result.initial_seek.target.direction);
  HulkStandingFrame standing{};unsigned standing_image=0;
  CHECK(hulk_standing_frame(0,u16(direction),standing));
  CHECK(resolve_animation_image(hulk,standing.image,standing_image));
  CHECK(objects.cells[result.object].sprite.image==standing_image);
  CHECK(level==0&&age==300&&(flags&1u));
  CHECK(result.next_action.kind==ProcessAction::sleep&&result.next_action.delay==2&&
        result.next_action.next==source_hulk_resume_lup_top);
  ProcessHandle impostor;CHECK(processes.create(source_hulk_process_id-1,0x4f48554cu,nullptr,impostor));
  const NativeProcess impostor_before=processes.cells[impostor];
  SourceOhulkInitializationResult untouched{};untouched.object=0x4321;
  input.process=impostor;
  CHECK(!source_ohulk_initialize(processes,objects,input,untouched));
  CHECK(equal(&impostor_before,&processes.cells[impostor],sizeof impostor_before));
 CHECK(untouched.object==0x4321&&objects.count==2&&accounting.count==1&&le16(wave+2)==3);
  CHECK(processes.validate()&&objects.validate());
 });
 group("L8 first-room OHULK BEGINOBJ2 matches all four captured object coordinates",[&] {
  struct Sample {u32 returned_x,returned_y,anchor_x,anchor_y,object_x,object_y;};
  static constexpr Sample samples[] = {
   {0x005f213du,0x00239d7eu,0x0237213du,0x01759d7eu,0x022e213du,0x01899d7eu},
   {0x008cc8eau,0x0034e483u,0x0264c8eau,0x0186e483u,0x025bc8eau,0x019ae483u},
   {0x014fb404u,0x002ae0f9u,0x0327b404u,0x017ce0f9u,0x031eb404u,0x0190e0f9u},
   {0x006c9eacu,0x00bd1c8du,0x02449eacu,0x020f1c8du,0x023b9eacu,0x02231c8du},
  };
  EditorImages hulk;CHECK(hulk.open(pack->find("FINGRNT.IMG")->bytes));
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  for(unsigned i=0;i<sizeof samples/sizeof samples[0];++i) {
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
   SourceOhulkCoordinates coordinates{};
   CHECK(source_apply_ohulk_waveyx(samples[i].returned_x,samples[i].returned_y,
                                   false,0x015201d8u,coordinates));
   CHECK(coordinates.a0_x_16_16==samples[i].anchor_x&&coordinates.a1_value==samples[i].anchor_y);
   ObjectHandle handle=no_object;
   CHECK(source_ohulk_beginobj2(processes,objects,actor,hulk,0,coordinates,handle));
   const auto& object=objects.cells[handle];
   CHECK(object.process==actor&&object.id==source_hulk_object_id&&object.listed);
   CHECK(object.sprite.position.x==samples[i].object_x&&object.sprite.position.y==samples[i].object_y);
   CHECK(processes.cells[actor].saved[0]==handle);
  }
  CHECK(processes.active_count==4&&objects.count==4&&processes.validate()&&objects.validate());
 });
 group("L8 OHULK entry marks standing before placement then starts a source actor",[&]{
  EditorImages hulk;CHECK(hulk.open(pack->find("FINGRNT.IMG")->bytes));
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  ObjectHandle player;CHECK(objects.allocate(player));
  objects.cells[player].sprite.position.x=662u<<16;objects.cells[player].sprite.position.y=463u<<16;
  const u16 player_torsos[2]={player,no_object},player_x[2]={662,0};
  ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::control_flags,0x0100));
  u8 wave[14]{};put16(wave,source_hulk_wave_type);put16(wave+2,4);put16(wave+4,0);
  HulkAccounting accounting{};SourceRng rng{1};HCountFixture hcount{0,0};
  SourceOhulkStartInput input{};input.actor.process=actor;input.actor.art=&hulk;input.actor.hulk_type=0;
  input.actor.wave_yx=0x015201d8u;input.actor.wave_records={wave,sizeof wave};input.actor.accounting=&accounting;
  input.actor.player_object_x=player_x;input.actor.player_torsos=player_torsos;input.actor.rng=&rng;
  input.actor.read_hcount=read_hcount;input.actor.hcount_context=&hcount;input.actor.ehalt=1;
  input.placement.wave=1;input.placement.tmp=16;input.placement.from_door=4;
  constexpr u32 pixel=0x10000u;input.placement.bounds={300*pixel-1,300*pixel,300*pixel-1,300*pixel};
  SourceSpawnReservations full;full.count=SourceSpawnReservations::capacity;input.reservations=&full;
  SourceOhulkStartResult untouched{};untouched.actor.object=0x4321;
  auto failed=source_ohulk_start(processes,objects,input,untouched);
  u32 flags=0;CHECK(!failed&&source_hulk_pdata_read(processes.cells[actor],SourceHulkField::control_flags,flags));
  CHECK(flags==0x0101&&untouched.actor.object==0x4321&&objects.count==1&&accounting.count==0&&le16(wave+2)==4);

  SourceSpawnReservations reservations;input.reservations=&reservations;
  SourceOhulkStartResult result{};CHECK(source_ohulk_start(processes,objects,input,result));
  CHECK(result.placement.branch==SourceSpawnBranch::random&&result.placement.tmp_after==16);
  CHECK(result.placement.coordinates.a0_x_16_16==300*pixel&&result.placement.coordinates.a1_value==300*pixel);
  CHECK(result.actor.coordinates.a0_x_16_16==u32(772)*pixel&&result.actor.coordinates.a1_value==u32(638)*pixel);
  u32 ax=0,ay=0;CHECK(sprite_anchor(objects.cells[result.actor.object].sprite,ax,ay));
  CHECK(ax==result.actor.coordinates.a0_x_16_16&&ay==result.actor.coordinates.a1_value);
  CHECK(result.actor.setup.count==1&&result.actor.setup.remaining==3&&accounting.count==1&&le16(wave+2)==3);
  CHECK(reservations.count==2&&reservations.points[1].x==300*pixel&&reservations.points[1].y==(300*pixel|300u));
  CHECK(hcount.reads==4&&result.actor.next_action.kind==ProcessAction::sleep&&
        result.actor.next_action.delay==2&&result.actor.next_action.next==source_hulk_resume_lup_top);

  ProcessHandle circle_actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,circle_actor));
  SourceRng circle_rng{1};HCountFixture circle_hcount{0,0};
  input.actor.process=circle_actor;input.actor.rng=&circle_rng;
  input.actor.hcount_context=&circle_hcount;input.placement.wave=3;input.placement.tmp=16;
  SourceSpawnReservations circle_reservations;input.reservations=&circle_reservations;
  SourceOhulkStartResult circle_result{};CHECK(source_ohulk_start(processes,objects,input,circle_result));
  CHECK(circle_result.placement.branch==SourceSpawnBranch::circle&&circle_result.placement.circle.point_index==15);
  CHECK(circle_result.placement.coordinates.a0_x_16_16==0x00390000u);
  CHECK(circle_result.placement.coordinates.a1_value==0x00e10039u);
  CHECK(circle_result.actor.coordinates.a0_x_16_16==0x02110000u&&
        circle_result.actor.coordinates.a1_value==0x02330039u);
  u32 circle_anchor_x=0,circle_anchor_y=0;
  CHECK(sprite_anchor(objects.cells[circle_result.actor.object].sprite,circle_anchor_x,circle_anchor_y));
  CHECK(circle_anchor_x==0x02110000u&&circle_anchor_y==0x02330039u);
  CHECK(circle_reservations.count==2&&circle_reservations.points[1].x==0x00390000u&&
        circle_reservations.points[1].y==0x00e10039u&&circle_hcount.reads==2);
  CHECK(processes.validate()&&objects.validate());
 });
 group("HULKS PDATA field access matches source widths and sampled L8 state",[]{
  NativeProcesses processes;processes.initialize();ProcessHandle actor;
  CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
  auto& process=processes.cells[actor];u32 value=0;
  CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,0x0100));
  CHECK(process.data[6]==0x00&&process.data[7]==0x01);
  CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==0x0100);
  CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,value|1u));
  CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==0x0101);
  CHECK(source_hulk_pdata_write(process,SourceHulkField::age,0x012c));
  CHECK(process.data[0]==0x2c&&process.data[1]==0x01);
  CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==0x012c);
  CHECK(source_hulk_pdata_write(process,SourceHulkField::horizontal_speed,0x12345678));
  CHECK(process.data[12]==0x78&&process.data[13]==0x56&&process.data[14]==0x34&&process.data[15]==0x12);
  CHECK(source_hulk_pdata_read(process,SourceHulkField::horizontal_speed,value)&&value==0x12345678);
  CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_player,0x1234));
  CHECK(source_hulk_pdata_read(process,SourceHulkField::seek_player,value)&&value==0x34);
  const auto prior=process;CHECK(!source_hulk_pdata_read(process,static_cast<SourceHulkField>(255),value));
  CHECK(!source_hulk_pdata_write(process,static_cast<SourceHulkField>(255),1));
  CHECK(equal(&prior.data,&process.data,sizeof process.data));
  ProcessHandle non_hulk;CHECK(processes.create(source_hulk_process_id-1,1,nullptr,non_hulk));
  CHECK(!source_hulk_pdata_write(processes.cells[non_hulk],SourceHulkField::age,0x12c));
  CHECK(processes.validate());
 });
 group("L8 OHULK standing and SETUP_HULK initialize linked process state",[&]{
  struct Sample{u16 index,hage;};constexpr Sample samples[]={{1,300},{5,420},{10,90},{3,220}};
  EditorImages hulk;CHECK(hulk.open(pack->find("FINGRNT.IMG")->bytes));
  const SourceOhulkCoordinates position{0x0237213du,0x01759d7eu};
  {
   NativeProcesses processes;processes.initialize();ProcessHandle actor;
   CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::level,0));
   CHECK(source_hulk_do_diff(processes.cells[actor]));u32 value=0;
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::horizontal_speed,value)&&value==0x8000);
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::diagonal_x_speed,value)&&value==0x6660);
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::vertical_speed,value)&&value==0x7000);
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::diagonal_y_speed,value)&&value==0x5998);
  }
  for(const auto& sample:samples){
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
   auto& process=processes.cells[actor];CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,0x0100));
   CHECK(source_ohulk_mark_standing(process));u32 value=0;
   CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==0x0101);
   ObjectHandle object;CHECK(source_ohulk_beginobj2(processes,objects,actor,hulk,0,position,object));
   u8 wave[14]{};put16(wave,source_hulk_wave_type);put16(wave+2,2);put16(wave+4,7);put16(wave+6,6);put16(wave+8,4);put16(wave+10,30);
   HulkAccounting accounting{};u16 player_x[2]={300,500};
   u32 seed=1;bool found=false;
   for(;seed<1000000;++seed){SourceRng probe{seed};if(probe.range(0,10,0x1234)==sample.index){found=true;break;}}
   CHECK(found);SourceRng rng{seed},expected_rng=rng;CHECK(expected_rng.range(0,10,0x1234)==sample.index);
   HCountFixture hcount{0x1234,0};SourceHulkSetupResult result{};
   CHECK(source_ohulk_setup_hulk(processes,actor,objects,{wave,sizeof wave},accounting,player_x,rng,read_hcount,&hcount,result));
   CHECK(hcount.reads==1&&result.age_table_index==sample.index&&result.hage==sample.hage);
   CHECK(rng.seed==expected_rng.seed&&accounting.count==1&&accounting.done==0);
   CHECK(result.level==7&&result.remaining==1&&result.seek_player==1&&!result.create_dexit);
   CHECK(le16(wave+2)==1&&objects.cells[object].process==actor&&objects.validate());
   CHECK(source_hulk_pdata_read(process,SourceHulkField::direction,value)&&value==3);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::seek_direction,value)&&value==5);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==0);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::batting,value)&&value==0);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==sample.hage);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::keep_age,value)&&value==sample.hage);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::seek_player,value)&&value==1);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::level,value)&&value==7);
   CHECK(source_hulk_do_diff(process));HulkSpeeds expected_speeds;CHECK(hulk_speeds(7,expected_speeds));
   CHECK(source_hulk_pdata_read(process,SourceHulkField::horizontal_speed,value)&&value==expected_speeds.horizontal);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::diagonal_x_speed,value)&&value==expected_speeds.diagonal_x);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::vertical_speed,value)&&value==expected_speeds.vertical);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::diagonal_y_speed,value)&&value==expected_speeds.diagonal_y);
   HCountFixture dis_hcount{0x5678,hcount.reads};SourceRng expected_dis_rng=rng;
   const u16 expected_distance=u16(expected_dis_rng.range(1,6,dis_hcount.value));u16 distance=0;
   CHECK(source_hulk_get_dis(process,rng,read_hcount,&dis_hcount,distance));
   CHECK(dis_hcount.reads==2&&distance==expected_distance&&rng.seed==expected_dis_rng.seed);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::distance,value)&&value==distance);
   CHECK(processes.validate());
  }
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
  ObjectHandle object;CHECK(source_ohulk_beginobj2(processes,objects,actor,hulk,0,position,object));
  u8 wave[14]{};put16(wave,source_hulk_wave_type);put16(wave+2,1);put16(wave+4,3);
  HulkAccounting accounting{};u16 player_x[2]={300,0};SourceRng rng{0x12345678};
  HCountFixture hcount{4,0};SourceHulkSetupResult result{9,8,7,6,5,4,true};
  const auto before=processes.cells[actor];const auto rng_before=rng;const auto accounting_before=accounting;
  CHECK(!source_ohulk_setup_hulk(processes,actor,objects,{wave,sizeof wave},accounting,player_x,rng,nullptr,&hcount,result));
  CHECK(hcount.reads==0&&rng.seed==rng_before.seed&&accounting.count==accounting_before.count&&accounting.done==accounting_before.done&&le16(wave+2)==1);
  CHECK(equal(processes.cells[actor].data,before.data,sizeof before.data));CHECK(result.age_table_index==9&&result.hage==8&&result.level==7&&result.count==6&&result.remaining==5&&result.seek_player==4&&result.create_dexit);
  result={};
  CHECK(source_ohulk_setup_hulk(processes,actor,objects,{wave,sizeof wave},accounting,player_x,rng,read_hcount,&hcount,result));
  CHECK(result.count==1&&result.seek_player==0&&result.remaining==0&&result.create_dexit);
  CHECK(accounting.count==1&&accounting.done==1&&le16(wave+2)==0);
  u32 value=0;CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::seek_player,value)&&value==0);
 });
 group("L8 SETUP_DIR resolves PLYROBJS torso targets and traced Hulk anchors",[&]{
  struct Sample{i32 x,y;u16 direction;};constexpr Sample samples[]={{565,391,4},{610,408,4},{807,398,6},{579,539,2}};
  EditorImages hulk;CHECK(hulk.open(pack->find("FINGRNT.IMG")->bytes));
  for(const auto& sample:samples){
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
   ObjectHandle hulk_object;SourceOhulkCoordinates coordinates{u32(sample.x)<<16,u32(sample.y)<<16};
   CHECK(source_ohulk_beginobj2(processes,objects,actor,hulk,0,coordinates,hulk_object));
   ObjectHandle p1;CHECK(objects.allocate(p1));objects.cells[p1].sprite.position.x=662u<<16;objects.cells[p1].sprite.position.y=463u<<16;
   const u16 player_torsos[2]={p1,no_object};
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::seek_player,0));
   SourceHulkDirectionResult result{};CHECK(source_hulk_setup_dir(processes,actor,objects,player_torsos,result));
   u32 value=0;CHECK(result.player_slot==0&&result.direction==sample.direction);
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::seek_player,value)&&value==0);
   CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::seek_direction,value)&&value==sample.direction);
  }
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
  ObjectHandle hulk_object;CHECK(source_ohulk_beginobj2(processes,objects,actor,hulk,0,{565u<<16,391u<<16},hulk_object));
  ObjectHandle dead,live;CHECK(objects.allocate(dead));CHECK(objects.allocate(live));
  objects.cells[live].sprite.position.x=700u<<16;objects.cells[live].sprite.position.y=400u<<16;
  const u16 fallback_torsos[2]={dead,live};
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::seek_player,0));
  SourceHulkDirectionResult result{};CHECK(source_hulk_setup_dir(processes,actor,objects,fallback_torsos,result));
  CHECK(result.player_slot==1&&result.direction==3);
  u32 value=0;CHECK(source_hulk_pdata_read(processes.cells[actor],SourceHulkField::seek_player,value)&&value==1);
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::seek_player,0));
  objects.cells[live].sprite.position.x=0;const auto before=processes.cells[actor];
  CHECK(!source_hulk_setup_dir(processes,actor,objects,fallback_torsos,result));
  CHECK(equal(processes.cells[actor].data,before.data,sizeof before.data));
 });
 group("L8 first-wake INTO applies PDATA speeds to its linked Hulk object",[&]{
  struct Sample{i32 x,y;u16 direction;u32 vx,vy;};constexpr Sample samples[]={
   {565,391,4,0x6660,0x5998},{610,408,4,0x6660,0x5998},
   {807,398,6,0xffff99a0,0x5998},{579,539,2,0x6660,0xffffa668}};
  EditorImages hulk;CHECK(hulk.open(pack->find("FINGRNT.IMG")->bytes));
  for(const auto& sample:samples){
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
   ObjectHandle hulk_object;CHECK(source_ohulk_beginobj2(processes,objects,actor,hulk,0,
       {u32(sample.x)<<16,u32(sample.y)<<16},hulk_object));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::direction,sample.direction));
   CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::level,0));
   CHECK(source_hulk_do_diff(processes.cells[actor]));
   auto& position=objects.cells[hulk_object].sprite.position;
   position.vx=0x12345678;position.vy=0x87654321;
   const u32 x=position.x,y=position.y;
   CHECK(source_hulk_into(processes,actor,objects,sample.direction));
   CHECK(position.x==x&&position.y==y&&position.vx==sample.vx&&position.vy==sample.vy);
   CHECK(processes.validate()&&objects.validate());
  }
  NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
  ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,0x4f48554cu,nullptr,actor));
  ObjectHandle hulk_object;CHECK(source_ohulk_beginobj2(processes,objects,actor,hulk,0,{565u<<16,391u<<16},hulk_object));
  auto& position=objects.cells[hulk_object].sprite.position;position.vx=0x12345678;position.vy=0x87654321;
  CHECK(!source_hulk_into(processes,actor,objects,0));
  CHECK(position.vx==0x12345678&&position.vy==0x87654321);
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::direction,9));
  CHECK(!source_hulk_into(processes,actor,objects,9));
  CHECK(position.vx==0x12345678&&position.vy==0x87654321);
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::direction,4));
  CHECK(source_hulk_pdata_write(processes.cells[actor],SourceHulkField::level,0));
  CHECK(source_hulk_do_diff(processes.cells[actor]));
  CHECK(source_hulk_into(processes,actor,objects,6));
  CHECK(position.vx==0xffff99a0&&position.vy==0x5998);
 });
 group("L8 HULKS run-in DIS expiry uses NEWANG and A0-directed INTO",[&]{
  EditorImages art;CHECK(art.open(pack->find("FINGRNT.IMG")->bytes));
  static constexpr u16 angles[4][4]={
   {7,3,8,2}, {1,5,2,4}, {7,6,4,3}, {8,6,1,5}
  };
  constexpr u32 seed=0x93a17c25;
  u16 count_for_index[3]{};bool found[3]{};
  for(u32 count=0;count<=0xffffu;++count){
   SourceRng probe{seed};const u32 index=probe.range(1,4,u16(count));
   if(index>=2&&index<=4&&!found[index-2]){
    count_for_index[index-2]=u16(count);found[index-2]=true;
   }
  }
  CHECK(found[0]&&found[1]&&found[2]);
  for(u16 row=0;row<4;++row)for(u16 pick=0;pick<3;++pick){
   NativeProcesses processes;processes.initialize();NativeObjects objects;objects.initialize();
   ProcessHandle actor;CHECK(processes.create(source_hulk_process_id,
       source_hulk_resume_lup_top,nullptr,actor));
   ObjectHandle hulk;CHECK(source_ohulk_beginobj2(processes,objects,actor,art,0,
       {200u<<16,160u<<16},hulk));
   NativeProcess& process=processes.cells[actor];
   CHECK(source_hulk_pdata_write(process,SourceHulkField::age,0x012c));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::level,0));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::distance,1));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::direction,u16(row*2+1)));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::seek_direction,5));
   CHECK(source_hulk_pdata_write(process,SourceHulkField::control_flags,0x0002));
   CHECK(source_hulk_do_diff(process));
   HCountFixture hcount{count_for_index[pick]};
   SourceRng rng{seed},expected_rng{seed};
   const u32 selected=expected_rng.range(1,4,hcount.value);
   CHECK(selected==u32(pick+2));
   SourceHulkLupTopWorld world{};world.ehalt=0;world.hulk_type=0;
   world.rng=&rng;world.read_hcount=read_hcount;world.hcount_context=&hcount;
   ProcessAction action{};CHECK(source_hulk_lup_top(processes,objects,actor,world,action));
   u32 value=0;
   CHECK(source_hulk_pdata_read(process,SourceHulkField::age,value)&&value==0x012b);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::distance,value)&&value==0);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::direction,value)&&value==u32(row*2+1));
   CHECK(source_hulk_pdata_read(process,SourceHulkField::seek_direction,value)&&
         value==angles[row][pick+1]);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::control_flags,value)&&value==3);
   CHECK(source_hulk_pdata_read(process,SourceHulkField::oscillation,value)&&value==2);
   CHECK(rng.seed==expected_rng.seed&&hcount.reads==1);
   HulkSpeeds speeds{};CHECK(hulk_speeds(0,speeds));
   SourcePosition velocity{};CHECK(hulk_velocity(angles[row][pick+1],speeds,velocity));
   CHECK(objects.cells[hulk].sprite.position.vx==velocity.vx&&
         objects.cells[hulk].sprite.position.vy==velocity.vy);
   CHECK(action.kind==ProcessAction::sleep&&action.delay==3&&
         action.next==source_hulk_resume_lup_top);
  }
 });
 group("NDSP1 object ties insert before; YZSORT remains one pass",[]{auto o=std::make_unique<NativeObjects>();o->initialize();ObjectHandle a,b,c;CHECK(o->allocate(a));CHECK(o->allocate(b));CHECK(o->allocate(c));for(auto h:{a,b,c}){o->cells[h].z=159;o->cells[h].sprite.position.y=100u<<16;CHECK(o->insert(h));}CHECK(o->head==c&&o->cells[c].next==b&&o->cells[b].next==a);o->cells[c].z=3;o->cells[b].z=2;o->cells[a].z=1;CHECK(o->sort_pass());CHECK(o->head==b&&o->cells[b].next==a&&o->cells[a].next==c);CHECK(o->sort_pass());CHECK(o->head==a);CHECK(!o->sort_pass());CHECK(o->erase(b));CHECK(o->validate());ObjectHandle reuse;CHECK(o->allocate(reuse));CHECK(reuse==b&&o->cells[b].z==2&&o->validate());});
 std::cout<<"RESULT "<<groups<<" groups; "<<checks<<" checks. Source-data tests, not arcade differential proof.\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
