#include "stv/wave_flow.hpp"
#include "stv/tank_rules.hpp"
#include "stv/source_assets.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace stv;
static unsigned groups=0;static u64 checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x);}while(false)
static void ok(Status s){if(!s)throw std::runtime_error(s.message?s.message:"Failed status");++checks;}
template<class F>void group(const char* name,F f){f();++groups;std::cout<<"PASS "<<name<<'\n';}
struct Event {u64 tick;unsigned kind,index,number;};
struct Harness {
    NativeProcesses pool{};WaveOverlord flow{};WaveFlowHooks hooks{};
    std::vector<Event> events;u64 tick{};bool fail_tanks{};
    Harness(){pool.initialize();ok(set_difficulty(4,flow.world.difficulty));hooks={initial,release,cash,palette};ProcessHandle h;ok(pool.create(200,WaveOverlord::continuation,nullptr,h));}
    static Status initial(WaveOverlord& w,unsigned i,u16 n,void* ctx) {
        auto& h=*static_cast<Harness*>(ctx);h.events.push_back({h.tick,0,i,n});
        CHECK(w.release.records[i].remaining>=n);
        // Fixture children are explicitly NOT enemies. They test MPROC order and
        // the moment SETUP_* consumes initial population, never room playability.
        for(unsigned j=0;j<n;++j){u32 saved[4]={i,j,0,0};ProcessHandle out;auto s=h.pool.create(250,2,saved,out);if(!s)return s;}
        return {};
    }
    static Status release(WaveRelease& w,unsigned i,void* ctx) {
        auto& h=*static_cast<Harness*>(ctx);h.events.push_back({h.tick,1,i,w.records[i].type});
        if(h.fail_tanks&&w.records[i].type==3)return Status::fail("Fixture boundary: TANK actor is not connected");
        // Deliberate rejected allocation: request != spawned, no consumption.
        return {};
    }
    static Status cash(WaveOverlord&,void* ctx){auto& h=*static_cast<Harness*>(ctx);h.events.push_back({h.tick,3,0,0});return {};}
    static Status palette(WaveOverlord&,const PaletteCycleRequest& p,void* ctx){auto& h=*static_cast<Harness*>(ctx);CHECK(p.first==32&&p.last==45&&p.period_ticks==3);h.events.push_back({h.tick,4,0,0});return {};}
    static ProcessAction step(NativeProcesses& p,ProcessHandle id,void* ctx) {
        auto& h=*static_cast<Harness*>(ctx);
        if(p.cells[id].continuation==WaveOverlord::continuation)return h.flow.step(h.hooks,&h);
        CHECK(p.cells[id].continuation==2);unsigned i=p.cells[id].saved[0];
        h.events.push_back({h.tick,2,i,p.cells[id].saved[1]});
        auto s=h.flow.release.consume(i);if(!s)return ProcessAction::fail(s.message);
        // Only original first-wave Hulk records are exercised as real accounting.
        if(h.flow.release.records[i].type==6)++h.flow.world.enemy_count[0];
        return ProcessAction::suicide();
    }
    Status one(){auto s=pool.dispatch(step,this);++tick;return s;}
    void until(u64 end){while(tick<end)ok(one());}
};
int main(int argc,char**argv){try {
 group("OVERLORD sleeps three ticks, initializes once, and queues children in source order",[]{Harness h;h.flow.world.econ=9;h.flow.world.enon=8;h.flow.world.pzon=7;for(auto& n:h.flow.world.enemy_count)n=123;h.until(3);CHECK(h.flow.world.ehalt==0&&h.events.empty());h.until(4);CHECK(h.flow.world.ehalt==1&&h.flow.world.econ==0&&h.flow.world.enon==0&&h.flow.world.pzon==0);CHECK(h.flow.world.maximums.max_hulk==12&&h.flow.world.maximums.max_tank==1);CHECK(h.flow.world.ramxy[0]==0x003c003c&&h.flow.world.keepxy==1);CHECK(h.flow.world.enemy_count[0]==4&&h.flow.world.enemy_count[1]==0);CHECK(h.flow.release.records[0].remaining==96&&h.flow.release.records[1].remaining==5);CHECK(h.events.size()==5);for(unsigned i=1;i<5;++i)CHECK(h.events[i].tick==3&&h.events[i].kind==2&&h.events[i].number==4-i);});
 group("DNE waits for EHALT two; SAD uses 120 ticks; first scan falls through without extra delay",[]{Harness h;h.until(10);CHECK(h.flow.phase==WaveFlowPhase::wait_text&&h.flow.release.scans==0);h.flow.world.ehalt=2;h.until(130);CHECK(h.flow.release.scans==0);h.until(131);CHECK(h.flow.release.scans==1&&h.flow.world.ehalt==0&&h.flow.release.records[0].counter==29);h.until(134);CHECK(h.flow.release.scans==2&&h.flow.release.records[0].counter==28);});
 group("First-wave literal release counters request Hulk at 217 and tank at 4087",[]{Harness h;h.until(10);h.flow.world.ehalt=2;h.until(4200);u64 first_h=0,first_t=0;for(auto e:h.events)if(e.kind==1){if(e.number==6&&!first_h)first_h=e.tick;if(e.number==3&&!first_t)first_t=e.tick;}CHECK(first_h==217&&first_t==4087);CHECK(h.flow.release.records[0].remaining==96&&h.flow.release.records[1].remaining==5);CHECK(h.flow.initializations==1);});
 group("HALT pauses RLSE counters, not the OVERLORD sleeping process",[]{Harness h;h.until(10);h.flow.world.ehalt=2;h.flow.world.halt=1;h.until(200);CHECK(h.flow.phase==WaveFlowPhase::release&&h.flow.release.scans==0);CHECK(h.flow.release.records[0].counter==30);h.flow.world.halt=0;h.until(203);CHECK(h.flow.release.scans==1&&h.flow.release.records[0].counter==29);});
 group("TV presence is sampled at both source boundaries without clearing pre-existing HALT",[]{Harness h;h.until(10);h.flow.world.ehalt=2;h.flow.world.tv_process_present=true;h.until(11);CHECK(h.flow.world.halt==1);h.flow.world.tv_process_present=false;h.until(131);CHECK(h.flow.world.ehalt==0&&h.flow.world.halt==1);Harness b;b.until(10);b.flow.world.ehalt=2;b.until(11);b.flow.world.tv_process_present=true;b.until(131);CHECK(b.flow.world.ehalt==1&&b.flow.world.halt==0&&b.flow.release.scans==1);});
 group("INIT_RAM clears only its named globals, not ENON/ECON/HALT or populations",[]{WaveOverlord w;w.world.econ=2;w.world.enon=3;w.world.halt=4;w.world.enemy_count[0]=7;w.world.ehalt=2;for(auto& v:w.world.done)v=5;for(auto& v:w.world.gate_timer)v=255;w.world.pzon=w.world.shk_on=w.world.face_on=w.world.boss_on=w.world.toid_on=w.world.wntout=w.world.pctot=w.world.endon=9;w.init_ram();for(auto v:w.world.done)CHECK(v==0);for(auto v:w.world.gate_timer)CHECK(v==0);CHECK(w.world.econ==2&&w.world.enon==3&&w.world.halt==4&&w.world.enemy_count[0]==7&&w.world.ehalt==2);CHECK(w.world.pzon==0&&w.world.shk_on==0&&w.world.face_on==0&&w.world.boss_on==0&&w.world.toid_on==0&&w.world.wntout==0&&w.world.pctot==0&&w.world.endon==0);});
 group("Attract wave one selects source wave nine without changing global WAVE",[]{Harness h;h.flow.world.attract_mode=true;h.until(4);CHECK(h.flow.release.wave==9&&h.flow.world.wave==1&&h.flow.world.maximums.max_hulk==30&&h.flow.world.maximums.max_tank==1);});
 group("Untranslated O_ITEMS and COLCYC effects fail explicitly only when required",[]{Harness h;h.flow.world.wave=5;h.hooks.cash=nullptr;h.until(3);CHECK(!h.one()&&h.flow.phase==WaveFlowPhase::failed);CHECK(std::string(h.flow.error).find("O_ITEMS")!=std::string::npos);Harness b;b.flow.world.circuit=2;b.hooks.palette=nullptr;b.until(3);CHECK(!b.one()&&std::string(b.flow.error).find("COLCYC")!=std::string::npos);Harness c;c.hooks.cash=nullptr;c.hooks.palette=nullptr;c.until(4);CHECK(c.flow.phase==WaveFlowPhase::wait_text);});
 group("Missing initial handler and the first unsupported tank release are hard boundaries",[]{Harness h;h.hooks.initial=nullptr;h.until(3);CHECK(!h.one()&&h.flow.phase==WaveFlowPhase::failed);Harness b;b.fail_tanks=true;b.until(10);b.flow.world.ehalt=2;b.until(4087);CHECK(!b.one());CHECK(b.tick==4088&&b.flow.phase==WaveFlowPhase::failed&&b.flow.release.records[1].remaining==5);});
 group("SPAWN_TNK respects signed count/cap and circuit-specific ENON write",[]{for(unsigned m=0;m<256;++m)for(u16 n:{u16(0),u16(1),u16(12),u16(0x7fff),u16(0x8000),u16(0xffff)}){auto r=tank_spawn_request(0,n,u8(m),0);CHECK(r.create==(signed16(n)<signed8(u8(m))));if(r.create)CHECK(r.write_enon&&r.enon==m);CHECK(!tank_spawn_request(1,n,u8(m),0).create);}CHECK(tank_spawn_request(0,0,1,1).create&&!tank_spawn_request(0,0,1,1).write_enon);CHECK(tank_spawn_request(0,0,1,2).write_enon);});
 group("INIT_TNK gate 4 uses executed L8 reference read; unknown reads fail closed",[]{CHECK(tank_gate_from_random(0)==2&&tank_gate_from_random(0xffffffff)==4);TankEntry e;CHECK(!tank_entry(4,{},1,false,e));for(unsigned t=0;t<256;++t){ok(tank_entry(3,{true,u8(t)},1,false,e));CHECK(e.rejected==(t!=0&&signed8(u8(t))<6));}CHECK(l8_reference_drinf_index4.resolved&&l8_reference_drinf_index4.value==0);ok(tank_entry(4,l8_reference_drinf_index4,1,false,e));CHECK(!e.rejected&&e.position.x==0xca0000&&e.position.y==0xe80000&&e.direction==1&&e.distance==20&&e.gate_duration==31&&e.object_id==0x0e00);ok(tank_entry(2,{true,0},2,true,e));CHECK(e.rejected&&e.set_econ&&e.object_id==0x8e00);});
 group("ANYIN uses packed Y/X, strict edges, object identity and WAVEYX subtraction",[]{CHECK(tank_door_contains(1,0x0e00,1,60u<<16,100u<<16,0));CHECK(!tank_door_contains(1,0x8e00,1,60u<<16,100u<<16,0));CHECK(!tank_door_contains(1,0x0e00,1,0,100u<<16,0));CHECK(!tank_door_contains(1,0x0e00,1,120u<<16,100u<<16,0));CHECK(!tank_door_contains(1,0x0e00,1,60u<<16,50u<<16,0));CHECK(tank_door_contains(2,0x8e00,2,300u<<16,100u<<16,0));CHECK(tank_door_contains(3,0x0e00,1,200u<<16,40u<<16,0));CHECK(tank_door_contains(4,0x0e00,1,200u<<16,200u<<16,0));CHECK(tank_door_contains(4,0x0e00,1,210u<<16,220u<<16,0x0014000a));});
 group("WAY preserves wrap, half-turn tie and equal-direction increment",[]{for(u16 d=1;d<=16;++d)for(u16 target=1;target<=16;++target){TankTurn r;ok(tank_turn(d,target,r));int delta=int(target)-d;if(std::abs(delta)>=9)delta=-delta;CHECK(r.increment==(delta>=0));CHECK(r.direction==u16(delta>=0?(d==16?1:d+1):(d==1?16:d-1)));}TankTurn r;ok(tank_turn(1,1,r));CHECK(r.direction==2);ok(tank_turn(1,9,r));CHECK(r.direction==2);CHECK(!tank_turn(0,1,r));});
 group("TANK DO_DIFF2 rounding and INTO16 even LK entries fall through to upward IN1",[]{HulkSpeeds s;ok(tank_speeds(0,false,s));CHECK(s.horizontal==0x12000&&s.vertical==0x11000&&s.diagonal_x==(0x12000/10)*8);for(u16 d=0;d<=16;++d){SourcePosition p;ok(tank_velocity16(d,s,p));if(!(d&1))CHECK(p.vx==0&&p.vy==0-s.vertical);}SourcePosition p;ok(tank_velocity16(5,s,p));CHECK(p.vx==s.horizontal&&p.vy==0);ok(tank_speeds(3,true,s));CHECK(s.horizontal==0xa000&&s.vertical==0x9000);CHECK(!tank_speeds(0xffff,false,s));});
 group("GET_SK and GET_DIS ranges preserve exclusive lower bounds",[]{CHECK(tank_seek_from_random(0)==1&&tank_seek_from_random(0xffffffff)==13);CHECK(tank_distance_from_random(0)==26&&tank_distance_from_random(0xffffffff)==50);for(unsigned i=0;i<8;++i)CHECK(tank_seek_from_random(u32(u64(i)<<29))==1+(i%4)*4);});
 group("GETHDR uses original discrete thresholds; CVRT code sixteen falls into CVRT2 zero",[]{CHECK(tank_heading_code(0,0)==0);CHECK(tank_heading_code(1,0)==1);CHECK(tank_heading_code(u32(-1),0)==2);CHECK(tank_heading_code(0,1)==3);CHECK(tank_heading_code(0,u32(-1))==4);CHECK(tank_heading_code(0x10000,0x10000)==5);CHECK(tank_heading_code(0x10000,0x127ff)==5);CHECK(tank_heading_code(0x10000,0x12800)==13);CHECK(tank_heading_code(u32(-0x10000),u32(-0x12800))==16);u16 d;ok(tank_convert_heading(16,d));CHECK(d==0);CHECK(!tank_convert_heading(17,d));});
 group("CBALL uses raw turret coordinates plus direction tables and inherited body velocity",[]{SourcePosition turret{0xfffff000,0xffffff00,3,4},body{1,2,0xfffffffe,0x80000000},b;ok(tank_cannonball(1,turret,body,b));CHECK(b.x==turret.x+0x70000&&b.y==turret.y+0x20000&&b.vx==body.vx&&b.vy==body.vy-0x19999);ok(tank_cannonball(5,turret,body,b));CHECK(b.vx==body.vx+0x19999&&b.vy==body.vy&&b.x==turret.x+0x1a0000);CHECK(!tank_cannonball(0,turret,body,b));});
 group("SETUP_TNK accounting wraps fields, copies level and does not create DEXIT",[]{WaveRelease w;ok(w.load(0,1));w.records[1].remaining=1;w.records[1].level=7;TankAccounting a{0xffff,0,0};ok(tank_account_spawn(w,1,a));CHECK(a.count==0&&a.level==7&&a.done==1&&w.records[1].remaining==0);ok(tank_account_spawn(w,1,a));CHECK(w.records[1].remaining==0xffff);CHECK(!tank_account_spawn(w,0,a));});
 group("Body and turret PUNISH differ; signed MOVB absorption compares before byte writeback",[]{TankHit r;ok(tank_hit(false,1,0,99,20,false,r));CHECK(r.ignored&&!r.destroy);ok(tank_hit(false,0,0,1,20,false,r));CHECK(r.destroy&&r.absorbed==3);ok(tank_hit(true,1,0,2,3,false,r));CHECK(r.flash&&r.absorbed==2);ok(tank_hit(true,1,127,4,130,false,r));CHECK(r.destroy&&r.absorbed==147);ok(tank_hit(true,1,255,1,20,false,r));CHECK(r.flash&&r.absorbed==1);ok(tank_hit(true,1,50,1,20,true,r));CHECK(r.destroy);CHECK(!tank_hit(true,1,0,0,20,false,r));});
 if(argc>=2){std::filesystem::path out=argv[1];std::filesystem::create_directories(out);std::ofstream trace(out/"wave-flow-events.csv");trace<<"tick,kind,record,value\n";Harness h;h.until(10);h.flow.world.ehalt=2;h.until(4200);for(auto e:h.events)trace<<e.tick<<','<<e.kind<<','<<e.index<<','<<e.number<<'\n';if(!trace)throw std::runtime_error("Wave trace write failed");std::ofstream v(out/"tank-kernel-vectors.csv");v<<"r,gate,seek,distance,x,y,heading,converted,direction,turn,increment,bullet_x,bullet_y,bullet_vx,bullet_vy\n";u32 seed=0xdeadbeef;for(unsigned i=0;i<4096;++i){seed=seed*1664525u+1013904223u;u32 x=seed;seed=seed*1664525u+1013904223u;u32 y=seed;u16 d=u16(i%16+1),code=tank_heading_code(x,y),converted;ok(tank_convert_heading(code,converted));TankTurn turn;ok(tank_turn(d,u16((i/16)%16+1),turn));SourcePosition b;ok(tank_cannonball(d,{x,y,0,0},{0,0,y,x},b));v<<seed<<','<<tank_gate_from_random(seed)<<','<<tank_seek_from_random(seed)<<','<<tank_distance_from_random(seed)<<','<<x<<','<<y<<','<<code<<','<<converted<<','<<d<<','<<turn.direction<<','<<turn.increment<<','<<b.x<<','<<b.y<<','<<b.vx<<','<<b.vy<<'\n';}if(!v)throw std::runtime_error("Kernel vector write failed");}
 std::cout<<"PASS "<<groups<<" wave/tank-kernel groups, "<<checks<<" assertions\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
