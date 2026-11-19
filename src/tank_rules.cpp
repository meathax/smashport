#include "stv/tank_rules.hpp"
namespace stv {
namespace {
constexpr u32 sx[]={u32(-0x20000),0x1880000,0xca0000,0xca0000};
constexpr u32 sy[]={0x540000,0x540000,u32(-0x280000),0xe80000};
constexpr u16 sd[]={5,13,9,1};
// Original packed constants are [Y,X]. ANYIN uses strict inequalities.
constexpr i32 left[]={0,250,95,95},top[]={50,50,0,150};
constexpr i32 right[]={120,440,310,310},bottom[]={190,190,80,250};
constexpr const char* body_images[]={"TNK12A","TNK115A","TNK11A","TNK10A","TNK9A","TNK85A","TNK8A","TNK7A","TNK6A","TNK7A","TNK8A","TNK85A","TNK9A","TNK10A","TNK11A","TNK115A"};
constexpr const char* turret_images[]={"MTK12","MTK115","MTK11","MTK10","MTK9","MTK85","MTK8","MTK7","MTK6","MTK7","MTK8","MTK85","MTK9","MTK10","MTK11","MTK115"};
constexpr u16 spin_flags[]={2,18,18,18,18,18,18,18,2,2,2,2,2,2,2,2};
constexpr u32 xoff[]={0x70000,0xf0000,0x140000,0x160000,0x1a0000,0x180000,0x140000,0xe0000,0x90000,0x40000,0x20000,0x20000,0,0,0x10000,0x40000};
constexpr u32 yoff[]={0x20000,0x20000,0x20000,0x20000,0xb0000,0x110000,0x120000,0x170000,0x180000,0x170000,0x120000,0x110000,0xb0000,0x20000,0x10000,0x20000};
constexpr i32 xv[]={0,0xb333,0x19999,0x19999,0x19999,0x19999,0x19999,0xb333,0,-0xb333,-0x19999,-0x19999,-0x19999,-0x19999,-0x19999,-0xb333};
constexpr i32 yv[]={-0x19999,-0x19999,-0x19999,-0xcccc,0,0xcccc,0x19999,0x19999,0x19999,0x19999,0x19999,0xcccc,0,-0xcccc,-0x19999,-0x19999};
constexpr u8 cvrt[]={0,5,13,9,1,7,11,3,15,6,12,2,16,8,10,4,0};
u32 magnitude(u32 v){return v&0x80000000u?0-v:v;}
}
TankSpawnRequest tank_spawn_request(u16 halt,u16 count,u8 maximum,u16 circuit) {
    if(halt||signed16(count)>=signed8(maximum))return {};
    return {true,circuit==0||circuit==2,maximum};
}
unsigned tank_gate_from_random(u32 r){return unsigned((u64(r)*3)>>32)+2;}
Status tank_entry(unsigned gate,TankGateRead timer,u8 maximum,bool occupied,TankEntry& out) {
    out={};if(gate<1||gate>4)return Status::fail("TANK INIT_TNK gate outside original table",gate);
    if(!timer.resolved)return Status::fail("TANK DRINF gate-byte read unresolved; do not substitute a closed door",gate);
    if(timer.value!=0&&signed8(timer.value)<6){out.rejected=true;return {};}
    out.set_econ=maximum!=1;out.object_id=maximum==1?0x0e00:0x8e00;
    if(occupied){out.rejected=true;return {};}
    out.direction=sd[gate-1];out.position.x=sx[gate-1];out.position.y=sy[gate-1];
    out.distance=20;out.gate_duration=0x1f;return {};
}
bool tank_door_contains(unsigned gate,u16 id,u8 maximum,u32 x,u32 y,u32 waveyx) {
    if(gate<1||gate>4||id!=(maximum==1?0x0e00:0x8e00))return false;
    x-=u32(u16(waveyx))<<16;y-=waveyx&0xffff0000u;
    const i32 px=signed16(u16(x>>16)),py=signed16(u16(y>>16));const unsigned i=gate-1;
    return px>left[i]&&px<right[i]&&py>top[i]&&py<bottom[i];
}
Status tank_turn(u16 direction,u16 seek,TankTurn& out) {
    if(direction<1||direction>16||seek<1||seek>16)return Status::fail("TANK WAY direction outside 1..16");
    i32 delta=i32(seek)-i32(direction);const i32 absolute=delta<0?-delta:delta;
    if(absolute>=9)delta=-delta;
    if(delta<0)out={u16(direction==1?16:direction-1),false};
    else out={u16(direction==16?1:direction+1),true};
    return {};
}
const char* tank_stand_image(u16 d,bool turret,u16& flags) {
    const unsigned i=(unsigned(d)-1)&15;flags=spin_flags[i];return turret?turret_images[i]:body_images[i];
}
const char* tank_run_script(u16 d) {switch(d){case 1:return "TNK01";case 5:return "TNK05";case 9:return "TNK09";case 13:return "TNK13";default:return nullptr;}}
Status tank_speeds(u16 level,bool reverse,HulkSpeeds& out) {
    if(signed16(level)<0)return Status::fail("Negative TANK DO_DIFF2 level is outside validated source state");
    const u32 h=(reverse?0x4000u:0x12000u)+u32(level)*0x2000u,v=h-0x1000u;
    out={h,v,(h/10)*8,(v/10)*8};return {};
}
Status tank_velocity16(u16 d,const HulkSpeeds& speeds,SourcePosition& p) {
    if(d>16)return Status::fail("INTO16 table address outside original 0..16",d);
    // JTBL16 LK entries fall through to IN1. They do not mean stand still.
    const u16 eight=(d==0||!(d&1))?1:u16((d+1)/2);
    return hulk_velocity(eight,speeds,p);
}
u16 tank_seek_from_random(u32 r){return u16(1+4*unsigned((u64(r)*8)>>32)%16);}
u16 tank_distance_from_random(u32 r){return u16(26+unsigned((u64(r)*25)>>32));}
u16 tank_heading_code(u32 x,u32 y) {
    const u32 ax=magnitude(x),ay=magnitude(y);
    if(u32(ax+ay)==0)return 0;
    if(ax>=u32(ay*3))return x&0x80000000u?2:1;
    if(ay>=u32(ax*3))return y&0x80000000u?4:3;
    const unsigned quadrant=(x>>31)+2*(y>>31);
    if(signed32(ay)>signed32(ax))return u16(quadrant+(signed32(ay-ax)<0x2800?5:13));
    return u16(quadrant+(signed32(ax-ay)<0x2800?5:9));
}
Status tank_convert_heading(u16 code,u16& direction) {
    if(code>16)return Status::fail("TANK CVRT address outside GETHDR result",code);
    direction=cvrt[code];return {};
}
Status tank_cannonball(u16 d,const SourcePosition& turret,const SourcePosition& body,SourcePosition& out) {
    if(d<1||d>16)return Status::fail("TANK CBALL direction outside source table",d);
    const unsigned i=d-1;out={turret.x+xoff[i],turret.y+yoff[i],body.vx+u32(xv[i]),body.vy+u32(yv[i])};return {};
}
Status tank_account_spawn(WaveRelease& w,unsigned index,TankAccounting& a) {
    if(index>=w.count||w.records[index].type!=3)return Status::fail("SETUP_TNK requires actual TANK wave record",index);
    auto& r=w.records[index];a.count=u16(a.count+1);a.level=r.level;r.remaining=u16(r.remaining-1);
    if(!r.remaining)a.done=1;
    return {};
}
Status tank_hit(bool turret,u8 has_driver,u8 absorbed,unsigned weapon,u16 hits,bool mutoid,TankHit& out) {
    out={};out.absorbed=absorbed;
    if(!turret&&has_driver){out.ignored=true;return {};}
    if(weapon<1||weapon>5)return Status::fail("TANK PUNISH subtype outside original five-entry table",weapon);
    constexpr u8 body_damage[]={3,3,5,20,20},turret_damage[]={2,2,5,20,20};
    const i32 sum=signed8(absorbed)+(turret?turret_damage[weapon-1]:body_damage[weapon-1]);
    out.absorbed=u8(sum);const i32 threshold=turret?(mutoid?51:signed16(hits)):3;
    out.destroy=sum>=threshold;out.flash=!out.destroy;return {};
}
}
