// Execute accepted retail L8 instructions, not a second implementation of the
// object lookup. Source bindings: clearance_contract.py, NDSP1/ENDWAVE.
#include "stv/gsp/ops.hpp"
#include <cstdio>
using namespace stv::gsp;
namespace {
constexpr u32 first=0x010003a0, second=first+0x1f0, third=second+0x1f0;
constexpr u32 stack=0x010fffe0, returned=0xffec7270;
int failures=0, checks=0;
void check(bool ok,const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr,"%s\n",name); }
}
void prepare(Machine& m,bool wide,unsigned count,u16 oid1,u16 oid2,u16 oid3) {
    m.enhanced=wide;
    m.external_timers=true;
    m.c.sp=stack;
    m.c.a[2]=0x12345678; m.c.a[3]=0x87654321;
    wlong(m,0x01000040,count?first:0);
    wlong(m,first,count>1?second:0); m.ww(first+0x150,oid1);
    wlong(m,second,count>2?third:0); m.ww(second+0x150,oid2);
    wlong(m,third,0); m.ww(third+0x150,oid3);
}
u32 run(Machine& m,u32 pc,u32 end1,u32 end2) {
    for(unsigned n=0;n<128 && pc!=end1 && pc!=end2 && !m.stop;++n)
        pc=l8_run(m,pc,m.c.cyc+1);
    check(!m.stop && m.faults.empty(),"lookup/guard faulted");
    check(pc==end1 || pc==end2,"lookup/guard failed bounded return");
    check(m.c.a[2]==0x12345678 && m.c.a[3]==0x87654321,"lookup clobbered A2/A3");
    check(m.c.sp==stack,"lookup did not restore stack");
    return pc;
}
void lookup(bool wide) {
    struct Row { unsigned count; u16 x,y,z; u32 query,mask,expected; bool signed_field=false; };
    const Row rows[]={
        {0,0,0,0,0x8000,0xc000,0},
        {1,0x8001,0,0,0x8000,0xc000,first},
        {3,0x4001,0x0001,0x8fff,0x8000,0xc000,third},
        {3,0x4001,0x8001,0x8002,0x8000,0xc000,second},
        {3,0x4001,0xc001,0x0001,0x8000,0xc000,0},
        {1,0x8001,0,0,0x8001,0xffffffff,0},
        {1,0x8001,0,0,0x8001,0xffffffff,first,true},
        {1,0x4001,0,0,0x8000,0,first},
    };
    for(const auto& r:rows) {
        Machine m; prepare(m,wide,r.count,r.x,r.y,r.z);
        if(r.signed_field) m.c.st|=0x20u; // FE0, with FW0=16.
        m.c.a[0]=r.query; m.c.a[1]=r.mask;
        push(m,m.c,returned);
        run(m,0xffe03660,returned,returned);
        check(m.c.a[0]==r.expected,"wrong first matching object");
        check(Z(m.c)==(r.expected==0),"wrong returned Z flag");
        check(m.c.a[1]==r.mask,"lookup changed mask");
    }
}
void guard(bool wide) {
    for(unsigned circuit=0;circuit<3;++circuit)
        for(unsigned wave=6;wave<=8;++wave)
            for(bool enemy:{false,true}) {
                Machine m; prepare(m,wide,2,0x4000,enemy?0x8001:0xc001,0);
                m.ww(0x010eb610,u16(circuit));m.ww(0x010eb630,u16(wave));
                const bool proceed=!enemy || (circuit==2 && wave==7);
                const u32 pc=run(m,0xffec71e0,0xffec73a0,0xffec7340);
                check(pc==(proceed?0xffec73a0u:0xffec7340u),"wrong retail exit decision");
            }
}
void cooperative_ready(bool wide) {
    constexpr u32 p1_in=0x010ee818, p2_in=0x010ee828, accept=0xffec9050;
    for(bool second_player:{false,true})for(unsigned status:{1u,2u,3u})
        for(unsigned countdown=0;countdown<4;++countdown)
            for(unsigned door=1;door<=3;++door)for(unsigned other=0;other<=3;++other) {
                Machine m;prepare(m,wide,0,0,0,0);m.c.st=0x830;
                m.ww(0x010ee600,status);
                wb(m,0x010de388,(countdown&1)?0x80:0);
                wb(m,0x010de390,(countdown&2)?0x80:0);
                wfield(m,p1_in,0x5a5a,16);wfield(m,p2_in,other,16);
                m.c.a[5]=door;m.c.a[6]=door;
                push(m,m.c,returned);
                u32 pc=second_player?0xffec8c90:0xffec8dc0;
                for(unsigned step=0;step<24 && pc!=accept && pc!=returned && !m.stop;++step)
                    pc=l8_run(m,pc,m.c.cyc+1);
                const bool solo=status!=3 && countdown==0;
                const bool ready=solo || (!second_player && door==other);
                check(pc==(ready?accept:returned) && !m.stop && m.faults.empty(),
                      "door agreement or buy-in countdown readiness differs");
                check(m.c.sp==(ready?stack-32:stack),"readiness path changed hardware stack");
                if(!ready) {
                    check(!C(m.c),"waiting player incorrectly authorizes departure");
                    check(rfield(m,p1_in,16)==(second_player?0x5a5au:0u) &&
                          rfield(m,p2_in,16)==(second_player?door:other),
                          "waiting path changed the wrong player door");
                }
            }
}
void departure_barriers(bool wide) {
    constexpr u32 p1=0x01086790,p2=0x01086d30,flight=0x010872d0,script=0x01087870;
    for(unsigned status:{1u,2u,3u})for(unsigned countdown=0;countdown<4;++countdown)
        for(unsigned dead=0;dead<4;++dead)for(unsigned blockers:{0u,1u,2u,4u,8u,15u}) {
            Machine m;prepare(m,wide,(blockers&1)?1:0,4,0,0);m.c.st=0x830;
            m.ww(0x010ee600,status);
            wb(m,0x010de388,(countdown&1)?1:0);wb(m,0x010de390,(countdown&2)?1:0);
            wb(m,0x010eea48,(blockers&2)?1:0);
            wlong(m,0x01000000,(blockers&4)?flight:(blockers&8)?script:0);
            wlong(m,flight,(blockers&8)?script:0);m.ww(flight+0x20,0x107);
            wlong(m,script,0);m.ww(script+0x20,0xfd);
            wlong(m,0x010de2b0,p1);wlong(m,0x010de2d0,p2);
            wfield(m,p1+0x208,(dead&1)?0x8300:0,16);
            wfield(m,p2+0x208,(dead&2)?0x8300:0,16);
            wfield(m,0x010ee818,2,16);wfield(m,0x010ee828,2,16);
            const unsigned present=countdown||status==3?3:status;
            const bool allowed=blockers==0 && !(dead&present);
            push(m,m.c,returned);
            u32 pc=0xffec9050;
            for(unsigned n=0;n<160 && pc!=returned && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
            check(pc==returned && !m.stop && m.faults.empty() && m.c.sp==stack,
                  "departure barrier did not return cleanly");
            check(C(m.c)==allowed,"departure ignored a hazard, script, prize flight or dying player");
            check(rb(m,0x010ee810)==unsigned(!(blockers&3) && (blockers&4)),
                  "flying-prize wait flag or barrier priority differs");
            check(rfield(m,0x010ee818,16)==(allowed?2u:0u) &&
                  rfield(m,0x010ee828,16)==(allowed?2u:0u),"departure did not reset both door selections");
        }
}
}
int main() {
    for(bool wide:{false,true}) { lookup(wide); guard(wide); cooperative_ready(wide); departure_barriers(wide); }
    std::printf("%d checks, %d failures; 16 lookup, 36 guard, 576 door-readiness and 576 barrier cases\n",checks,failures);
    return failures?1:0;
}
