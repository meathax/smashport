// Exercise the real translated threshold load, including flags and scene isolation.
#include "stv/gsp/ops.hpp"
#include <cstdio>
#include <string>
using namespace stv::gsp;
int main(int argc,char**argv){
 if(argc!=2)return 2;Machine loaded;std::string error;if(!loaded.load_roms(argv[1],error)){std::fprintf(stderr,"%s\n",error.c_str());return 2;}
 unsigned checks=0,failures=0;
 auto check=[&](bool value,const char*why){++checks;if(!value){++failures;if(failures<=5)std::fprintf(stderr,"%s\n",why);}};
 for(bool wide:{false,true}){
  Machine m=loaded;m.enhanced=wide;m.external_timers=true;
  if(wide){m.set_wide(1024,137);if(!apply_enhanced(m,137))return 2;}
  for(u16 state:{u16(0),u16(0x10),u16(0x18),u16(0x30),u16(0x50)})
   for(u32 background:{0u,0xffea54f0u,0xffea5550u,0xffea55b0u,0xffea5610u,0xffea5670u})
    for(unsigned circuit=0;circuit<3;++circuit)for(unsigned wave=1;wave<=23;++wave)
     for(unsigned status=0;status<4;++status)for(u16 raw:{u16(0),u16(1),u16(250),u16(2900),u16(10000)}){
      m.ww(0x010865a0u,state);m.ww(0x010eb610u,u16(circuit));m.ww(0x010eb630u,u16(wave));m.ww(0x010ee600u,u16(status));
      wlong(m,0x010dd4e0u,background);m.ww(0x010de340u,raw);
      m.c=Cpu{};m.c.st=0x830u|ST_N|ST_Z|ST_V|ST_C;m.c.a[0]=0x12345678u;m.c.a[2]=0xabcdef01u;
      m.stop=false;m.run_until_cycle=~0ull;m.next_event_cycle=~0ull;m.check_requested=false;
      const bool active=wide&&(state==0x10||state==0x18||state==0x50)&&!background&&circuit==2&&wave==7&&status==3;
      const u32 expected=active?u32(raw)*7/8:u32(raw);
      const u32 next=l8_run(m,0xfff3cfd0u,1);
      check(next==0xfff3d000u&&!m.stop,"Cobra load did not stop at the next instruction");
      check(m.c.a[1]==expected,"Cobra threshold scale or profile/player/scene isolation differs");
      check(m.c.cyc==5,"Cobra load guest cycles changed");
      check((m.c.st&(ST_N|ST_Z|ST_V|ST_C))==(ST_C|(expected?0:ST_Z)),"Cobra MOV flags changed incorrectly");
      check(m.c.a[0]==0x12345678u&&m.c.a[2]==0xabcdef01u,"Cobra load changed unrelated registers");
      check(m.rw(0x010de340u)==raw,"Cobra load changed the retail threshold RAM");
     }
  for(u16 state:{u16(0x10),u16(0x18),u16(0x30)})
   for(u16 oid:{u16(0x8300),u16(0x8400),u16(0x8600),u16(0x0800),u16(0x8904),
                u16(0x0b00),u16(0x0e00),u16(0x9000),u16(0x9100),u16(0x9200),
                u16(0x4101),u16(0x4102),u16(0x4201),u16(0x9801),u16(0x8a30),u16(0)})
    for(s32 velocity:{-196608,-65537,0,65537,196608}){
     constexpr u32 node=object_pool_base;
     m.ww(0x010865a0,state);wlong(m,0x010dd4e0,0);m.ww(node+0x150,oid);
     wlong(m,node+0x20,u32(velocity));wlong(m,node+0x40,u32(-velocity));
     m.c=Cpu{};m.c.a[0]=node;m.c.a[12]=u32(velocity);m.c.a[11]=u32(-velocity);
     m.c.a[10]=0x500000;m.c.a[9]=0x600000;m.stop=false;m.run_until_cycle=~0ull;m.next_event_cycle=~0ull;
     const s32 expected=velocity;
     const u32 next=l8_run(m,0xffe017c0,1);
     check(next==0xffe017d0 && m.c.a[10]==u32(0x500000+expected),"Enemy motion differs from retail");
     check(m.c.a[11]==u32(-expected),"Enemy Y motion differs from retail");
     check(rlong(m,node+0x20)==u32(velocity) && rlong(m,node+0x40)==u32(-velocity),"Enemy motion changed velocity RAM");
     check(m.c.a[9]==0x600000,"X integration changed the Y position early");
    }
  check(m.prog==loaded.prog,"Cobra enhancement changed the imported program bytes");
  // Mutoid map awareness: GET_ST must not send the tank into the wall it is
  // already against, and a jerk that clears the tank velocity must expire the
  // MDN/MUP timer instead of leaving it frozen. Classic retains the retail path.
  {
   struct Case {int rel;u8 action,expected;};
   constexpr Case cases[]={{170,1,2},{165,13,2},{162,1,2},{161,1,1},{100,1,1},{100,13,13},{100,2,2},{170,2,2},
                           {45,2,1},{48,12,1},{49,2,2},{40,2,1},{100,12,12},{170,12,12},{45,1,1},{0,10,10},{170,0,0}};
   for(bool tank_oid:{true,false})for(u16 state:{u16(0x10),u16(0x18)})for(const Case&k:cases){
    constexpr u32 tank=object_pool_base;constexpr u32 action_addr=0x010f2e38u;
    m.ww(0x010865a0,state);wlong(m,0x010dd4e0,0);m.ww(tank+0x150,tank_oid?0x9801:0x9100);
    wlong(m,0x01000260u,338u<<16);wlong(m,tank+0x80,u32(338+k.rel)<<16);
    m.c=Cpu{};m.c.a[8]=tank;m.c.a[0]=k.action;m.stop=false;m.run_until_cycle=~0ull;m.next_event_cycle=~0ull;
    const u32 next=l8_run(m,0xfff02aa0,1);
    const u8 expected=wide&&tank_oid&&state==0x10?k.expected:k.action;
    check(next==0xfff02ad0&&!m.stop,"Mutoid MACTION store did not stop at the next instruction");
    check(rb(m,action_addr)==expected,"Mutoid wall-aware action differs");
   }
   for(bool tank_oid:{true,false}){
    constexpr u32 tank=object_pool_base;
    m.ww(0x010865a0,0x10);wlong(m,0x010dd4e0,0);m.ww(tank+0x150,tank_oid?0x9801:0x9100);
    wlong(m,tank+0x20,0x12345u);wlong(m,tank+0x40,0x23456u);m.ww(0x010f2e40u,77);
    m.c=Cpu{};m.c.a[8]=tank;m.c.a[11]=0;m.stop=false;m.run_until_cycle=~0ull;m.next_event_cycle=~0ull;
    const u32 next=l8_run(m,0xffef2ec0,1);
    check(next==0xffef2ee0&&!m.stop,"Mutoid jerk release did not stop at the next instruction");
    check(rlong(m,tank+0x40)==0,"Mutoid jerk release no longer clears the tank Y velocity");
    check(rlong(m,tank+0x20)==0x12345u,"Mutoid jerk release changed the X velocity write");
    check(m.rw(0x010f2e40u)==(wide?1:77),"Mutoid jerk release timer differs from the profile");
   }
  }
  for(u32 pc:{0xfff1df50u,0xfff1df70u,0xfff1edc0u,0xfff1ede0u,0xfff39850u,0xfff39870u})
   for(s16 acceleration:{s16(-256),s16(-31),s16(0),s16(31),s16(256)}){
    constexpr u32 proc=0x01086790;
    m.ww(0x010865a0,0x10);wlong(m,0x010dd4e0,0);
    if(pc>=0xfff39850)wlong(m,proc+0x140,u32(s32(acceleration)));
    else {m.ww(proc+0x140,u16(acceleration));m.ww(proc+0x150,u16(acceleration));}
    wlong(m,proc+0x160,u32(s32(acceleration)));
    const auto fields=m.ram;
    m.c=Cpu{};m.c.st=0x830;m.c.a[13]=proc;m.stop=false;m.run_until_cycle=~0ull;m.next_event_cycle=~0ull;
    (void)l8_run(m,pc,1);
    const unsigned reg=(pc==0xfff1df70 || pc==0xfff1ede0 || pc==0xfff39870)?3:2;
    const s32 expected=acceleration;
    check(m.c.a[reg]==u32(expected),"Face/skull/Cobra acceleration scale differs");
    check(m.ram==fields,
          "Boss acceleration compounded process fields");
   }
 }
 std::printf("COMBAT_CHECKS checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
