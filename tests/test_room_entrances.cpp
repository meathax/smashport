// Headless native transition fixtures. Boot both real players, then place them
// at the previous room's exit and dispatch retail ENDWAVE's SET_WAVE call.
// The fixture supplies the cleared-room precondition, never simulated movement.
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"
#include "../tools/lockstep/bot.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdlib>
using namespace stv::gsp;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void combat_escape() {
    constexpr int directions[][4]={{804,457,8,4},{424,457,4,8},
                                   {614,637,2,1},{614,277,1,2}};
    for(bool wide:{false,true})for(int player=1;player<=2;++player)
        for(const auto& d:directions) {
            Machine m;m.enhanced=wide;
            constexpr u32 actor=0x010003a0u,enemy=0x01000590u;
            wlong(m,0x01000040u,actor);wlong(m,actor,enemy);wlong(m,enemy,0);
            m.ww(actor+0x150,u16(0x4100|player));m.ww(enemy+0x150,0x8600);
            m.ww(actor+0x70,u16(d[0]));m.ww(actor+0x90,u16(d[1]));
            m.ww(enemy+0x70,u16(d[0]+(d[2]==4?30:d[2]==8?-30:0)));
            m.ww(enemy+0x90,u16(d[1]+(d[2]==1?30:d[2]==2?-30:0)));
            m.ww(L8_GAME_STATE,0x10);m.ww(0x010eb630u,7);
            stv::bot::Bot bot(1,player);bot.log_every_=0;
            unsigned held=0,shift=unsigned(player-1)*8;
            for(unsigned frame=1;frame<=14;++frame) {
                for(const auto& e:bot.step(frame,m))if(e.port==0) {
                    if(e.value)held|=e.mask;else held&=~e.mask;
                }
                const unsigned movement=(held>>shift)&15;
                require(movement==unsigned(frame<=12?d[2]:d[3]),
                        "combat retreat failed to escape after twelve blocked frames");
                require(((held>>(shift+4))&15)==unsigned(d[3]),"combat escape stopped aiming at the enemy");
            }
        }
}
constexpr u32 players=0x010de2b0, world=0x01000280, waveyx=0x010ee080;
constexpr u32 stop_pc=0xffe03660;
void call(Machine& m,u32 pc){
    m.c.sp=0x010fffe0;push(m,m.c,stop_pc);
    for(unsigned n=0;pc!=stop_pc&&!m.stop&&n<20000;++n)pc=l8_run(m,pc,m.c.cyc+1);
    if(pc!=stop_pc||m.stop||!m.faults.empty()){
        std::fprintf(stderr,"fixture pc=%08x stop=%d cyc=%llu next=%llu\n",pc,int(m.stop),(unsigned long long)m.c.cyc,(unsigned long long)m.next_event_cycle);
        for(const auto&f:m.faults)std::fprintf(stderr,"%s %08x\n",f.what.c_str(),f.addr);
    }
    require(pc==stop_pc&&!m.stop&&m.faults.empty(),"native fixture call failed");
}
std::array<double,2> anchor(Machine& m,u32 obj){
    // Retail GANIOF/GETANIXY offsets, including reflected animation origins.
    const u32 im=rlong(m,obj+0x130),size=rlong(m,obj+0xf0);
    u32 off=rlong(m,im+0x20);if(s32(off)>=0x02000000)off=0;
    int x=s16(off),y=s16(off>>16);const u16 flags=m.rw(obj+0xb0);
    if(flags&0x10)x=int(size&65535)-1-x;
    if(flags&0x20)y=int(size>>16)-1-y;
    return {s32(rlong(m,obj+0x60))/65536.0+x,s32(rlong(m,obj+0x80))/65536.0+y};
}
u32 legs(Machine& m,int p){const auto proc=rlong(m,players+p*32);return rlong(m,proc+0x300);}
void protect(Machine& m){
    // Fixture-only retail DELYDET immunity (FFE194A0, low nine bits): the
    // walking assertion must not become an unrelated enemy-survival test.
    for(int p=0;p<2;++p){const auto proc=rlong(m,players+p*32);if(proc)m.ww(proc+0x160,511);}
}
void step_to(Machine& m,u32& pc,unsigned end,const std::function<void(Machine&,u32)>& trace_instruction={},bool protected_entry=true){
    m.run_until_cycle=~0ull;m.recompute_next_event();
    m.on_vblank=[&]{if(protected_entry)protect(m);if(m.frame_number>=end){m.run_until_cycle=m.c.cyc;m.recompute_next_event();}};
    while(m.frame_number<end&&!m.stop){
        if(trace_instruction)trace_instruction(m,pc);
        pc=l8_run(m,pc,m.c.cyc+(trace_instruction?1:1000000));
    }
    if(m.stop){for(const auto&f:m.faults)std::fprintf(stderr,"%s %08x\n",f.what.c_str(),f.addr);}
    require(!m.stop,"native transition fault");
    m.on_vblank={};
}
struct Result{std::array<std::array<double,2>,2> last;};
void verify_anchor(Machine& source,u32 obj){
    auto expected=anchor(source,obj);Machine m=source;m.faults.clear();m.on_vblank={};
    m.external_timers=true;m.run_until_cycle=~0ull;m.next_event_cycle=~0ull;m.c=Cpu{};m.c.a[8]=obj;
    call(m,0xffe04c20);
    require(std::abs(expected[0]-s32(m.c.a[3])/65536.0)<0.0001&&
            std::abs(expected[1]-s32(m.c.a[2])/65536.0)<0.0001,"observer differs from retail GETANIXY");
}
// Drive the actual ENDWAVE transition from circuit 1 room 2 to room 3.
// Absolute entry coordinates depend on where each player crosses the door.
std::array<double,2> north_walk(const char* rom,bool wide,bool timed_start=false){
    Machine m;std::string error;
    require(m.load_roms(rom,error),"north route ROM load");
    if(wide){m.enhanced=true;m.set_wide(1024,137);require(apply_enhanced(m,137),"north route enhancement");}
    m.reset();
    std::array<stv::bot::Bot,2> bots={stv::bot::Bot(0,1),stv::bot::Bot(0,2)};
    for(auto& bot:bots)bot.log_every_=0;
    std::array<u32,2> objects{};
    std::array<std::array<double,2>,2> start{},previous{};
    std::array<double,2> travel{};
    int last_state=-1,last_wave=-1;
    unsigned entry_frame=0;
    bool paused=false,walking=false,done=false,from_second_room=false;
    m.on_vblank=[&]{
        const unsigned frame=unsigned(m.frame_number);
        const int state=m.rw(L8_GAME_STATE),wave=m.rw(0x010eb630);
        if(state==0x10 && (state!=last_state || wave!=last_wave)){
            entry_frame=frame;
            if(wave==3 && last_wave==2)from_second_room=true;
            for(auto& bot:bots)bot.route_slot_=wave==2?1:0;
        }
        protect(m);
        const bool neutral=state==0x10 && frame-entry_frame<120;
        if(paused && !neutral)for(auto& bot:bots)bot.resync();
        paused=neutral;
        for(int p=0;p<2;++p){
            for(const auto& event:bots[p].step(frame,m)){
                bool pressed=event.value!=0;
                if(event.port==1 && rlong(m,players+p*32))pressed=false;
                if(event.port==0 && neutral)pressed=false;
                if(pressed)m.ports[event.port]&=u16(~event.mask);
                else m.ports[event.port]|=u16(event.mask);
            }
        }
        if(neutral)m.ports[0]=0xffff;
        if(timed_start && frame<=3200){
            // Alternate legal boot from the room-entry fixture. The timing of
            // the start changes the phase of the later north walk in Classic.
            m.ports[1]=0xffff;
            if(frame>=2600 && frame<2760 && (frame-2600)%40<12)m.ports[1]&=u16(~1);
            if(frame>=3150 && frame<3170)m.ports[1]&=u16(~0x24);
        }
        if(!walking && from_second_room && state==0x18 && wave==3){
            require(rfield(m,0x010ee988,16)==3,"wrong north exit");
            for(int p=0;p<2;++p){
                objects[p]=legs(m,p);require(objects[p]!=0,"missing north entrant");
                start[p]=previous[p]=anchor(m,objects[p]);
            }
            walking=true;
        }
        if(walking){
            require(wave==3 && m.rw(0x010eb610)==0,"north destination changed");
            for(int p=0;p<2;++p){
                require(legs(m,p)==objects[p],"north entrant replaced");
                auto a=anchor(m,objects[p]);
                const double dy=a[1]-previous[p][1];
                require(a[0]==start[p][0] && (dy==0 || dy==-1.375),"north player left lane or changed speed");
                if(state==0x10 && frame-entry_frame>=60){
                    require(dy==0 && rlong(m,objects[p]+0x20)==0 && rlong(m,objects[p]+0x40)==0,
                            "north player did not stay stopped");
                    require(rfield(m,rlong(m,rlong(m,players+p*32)+0x140),16)==0,"north control retained");
                }
                previous[p]=a;
                travel[p]=start[p][1]-a[1];
            }
            if(state==0x10 && frame-entry_frame==120){
                require(s16(m.rw(world))==(wide?1077:944) && s16(m.rw(world+16))==5,"north destination camera");
                done=true;
            }
        }
        last_state=state;last_wave=wave;
        if(done || frame>=12000){m.run_until_cycle=m.c.cyc;m.recompute_next_event();}
    };
    u32 pc=m.reset_vector();
    while(!done && !m.stop && m.frame_number<12000)pc=l8_run(m,pc,m.c.cyc+1000000);
    require(done && !m.stop,"north route did not settle");
    for(unsigned p=0;p<2;++p){
        std::printf("NORTH_TRAVEL %s p%u start_y=%.3f end_y=%.3f distance=%.3f\n",
                    wide?"wide":"classic",p+1,start[p][1],previous[p][1],travel[p]);
    }
    return travel;
}
Result entrance(const Machine& boot,u32 bootpc,int door,bool negative){
    Machine m=boot;u32 pc=bootpc;m.on_vblank={};m.faults.clear();
    const bool wide=m.enhanced;const int pitch=wide?609:472;
    if(wide&&negative){
        // Test-only mutation: reproduce the old boosted script velocities in
        // this disposable machine. No ROM file or production code is changed.
        for(u32 root:{0xffe1f8a0u,0xffe1f9e0u,0xffe1fb20u,0xffe1fc60u})
            for(unsigned i=0;i<20;++i)m.prog[((root-0xff800000)>>4)+i]=m.enhanced_data[((root-0xff800000)>>4)+i];
    }
    // Destination circuit 1, room 2; both modes obtain its camera from their
    // actual CRCTTAB, not a duplicated coordinate transform.
    const u32 root=wide?enhanced_data_address(0xffe8c7f0):0xffe8c7f0;
    const u32 target=rlong(m,rlong(m,root)+64);
    int tx=s16(target),ty=s16(target>>16),cx=tx,cy=ty;
    // FROMDR names the outgoing direction, not the destination's entrance.
    if(door==1)cx+=pitch;if(door==2)cx-=pitch;
    if(door==3)cy+=332;if(door==4)cy-=332;
    wlong(m,world,u16(cx)|(u32(u16(cy))<<16));wlong(m,waveyx,rlong(m,world));
    wlong(m,0x01000240,u32(cx)<<16);wlong(m,0x01000260,u32(cy)<<16);
    m.ww(0x010eb630,2);wfield(m,0x010ee988,door,16);
    std::array<std::array<double,2>,2> previous{};
    std::array<u32,2> original_legs{};
    for(int p=0;p<2;++p){
        const u32 proc=rlong(m,players+p*32),leg=legs(m,p),torso=rlong(m,proc+0x100);
        require(proc&&leg&&torso,"both live players required");
        double x=(door==1?40:door==2?360:190)+(wide?(door==1?4:door==2?141:72):0);
        double y=door==3?35:door==4?237:130;
        if(door<=2)y+=p*8;else x+=p*8;
        auto a=anchor(m,leg);
        for(u32 obj:{leg,torso}){
            wlong(m,obj+0x60,rlong(m,obj+0x60)+s32((cx+x-a[0])*65536));
            wlong(m,obj+0x80,rlong(m,obj+0x80)+s32((cy+y-a[1])*65536));
            wlong(m,obj+0x20,0);wlong(m,obj+0x40,0);
        }
        verify_anchor(m,leg);previous[p]=anchor(m,leg);original_legs[p]=leg;
    }
    // Native GETPRC allocates and links the transition; restore the interrupted
    // CPU context afterwards. Only the allocator's RAM changes are retained.
    {Machine setup=m;setup.external_timers=true;setup.run_until_cycle=~0ull;setup.next_event_cycle=~0ull;setup.c=Cpu{};
     setup.c.a[13]=0x01000000;
     // REDREG disables the old room's collision process before SET_WAVE.
     setup.c.a[0]=234;setup.c.a[1]=0xffffffff;call(setup,0xffe0af30);
     setup.c.a[1]=0;setup.c.a[7]=0xffecafa0;
     call(setup,0xffe0aba0);
     // The allocator's temporary host-call stack overlaps the interrupted
     // game's hardware stack. It is scratch, not part of the retained fixture.
     std::copy(m.ram.end()-256,m.ram.end(),setup.ram.end()-256);
     m.ram=setup.ram;}
    const unsigned start=unsigned(m.frame_number);Result result{};unsigned stable=0;bool saw_intro=false,saw_play=false;
    const bool trace=std::getenv("STV_TRACE_ENTRANCE")!=nullptr;
    std::array<double,2> distance{};
    std::array<unsigned,2> moving_ticks{};
    std::array<int,2> prior_control{-1,-1};
    std::array<bool,2> prior_moving{},pending_origin{};
    std::array<u32,2> prior_image{};
    std::array<s32,2> prior_raw_x{};
    for(int p=0;p<2;++p){
        prior_image[p]=rlong(m,original_legs[p]+0x130);
        prior_raw_x[p]=s32(rlong(m,original_legs[p]+0x60));
    }
    for(unsigned f=1;f<=550;++f){
        std::function<void(Machine&,u32)> instruction_trace;
        if(door==2&&f<=300&&std::getenv("STV_TRACE_SCRIPT"))
            instruction_trace=[&](Machine& machine,u32 address){
                const u32 control=rlong(machine,rlong(machine,players)+0x140);
                if(machine.c.a[8]==control&&(address==0xffe9f2d0u||address==0xffe9f340u))
                    std::printf("SCRIPT %s tick=%llu pc=%08x proc=%08x a9=%08x a10=%u control=%x cycles=%llu\n",
                      wide?"wide":"classic",(unsigned long long)(machine.frame_number-start),address,
                      machine.c.a[13],machine.c.a[9],machine.c.a[10],rfield(machine,control,16),
                      (unsigned long long)machine.c.cyc);
            };
        step_to(m,pc,start+f,instruction_trace);
        saw_intro|=m.rw(L8_GAME_STATE)==0x18;saw_play|=saw_intro&&m.rw(L8_GAME_STATE)==0x10;
        bool moved=false;
        for(int p=0;p<2;++p){
            require(legs(m,p)==original_legs[p],"entry replaced a player instead of walking to the landing");
            const u32 leg=original_legs[p],image=rlong(m,leg+0x130);
            const s32 raw_x=s32(rlong(m,leg+0x60));
            const double vx=s32(rlong(m,leg+0x20))/65536.;
            auto a=anchor(m,leg);
            // Vblank can fall between an animation image write and its origin
            // adjustment. At that sample raw X advances by exactly the walking
            // velocity. Require exact repayment on the next real anchor.
            if(pending_origin[p]){
                require(image==prior_image[p] && a[0]==previous[p][0]+vx,
                        "animation origin correction missing on next frame");
                pending_origin[p]=false;
            }else if(door<=2 && image!=prior_image[p] &&
                     raw_x-s64(prior_raw_x[p])==s32(rlong(m,leg+0x20)) &&
                     a[0]!=previous[p][0]+vx){
                a[0]=previous[p][0]+vx;
                pending_origin[p]=true;
            }
            prior_image[p]=image;
            prior_raw_x[p]=raw_x;
            const double dx=a[0]-previous[p][0],dy=a[1]-previous[p][1];
            const double along=door==1?-dx:door==2?dx:door==3?-dy:dy;
            const double across=door<=2?dy:dx;
            require(std::abs(across)<0.0001,"entrance left its walking lane");
            require(along>=-0.0001&&along<=1.3751,"script used wrong direction or non-arcade walking speed");
            const bool walking=along>0.0001;
            const int control=rfield(m,rlong(m,rlong(m,players+p*32)+0x140),16);
            distance[p]+=along;moving_ticks[p]+=walking;
            if(trace&&(control!=prior_control[p]||walking!=prior_moving[p]))
                std::printf("WALK_EDGE %s door=%d p%d tick=%u control=%x moving=%d distance=%.6f moving_ticks=%u state=%x\n",wide?"wide":"classic",door,p+1,f,control,int(walking),distance[p],moving_ticks[p],m.rw(L8_GAME_STATE));
            prior_control[p]=control;prior_moving[p]=walking;
            moved|=walking;previous[p]=a;
            result.last[p]={a[0]-s16(m.rw(world)),a[1]-s16(m.rw(world+16))};
        }
        stable=moved?0:stable+1;
        if(f%25==0||f==1){
            std::printf("TRACE %s door=%d tick=%u state=%x camera=%d,%d",wide?"wide":"classic",door,f,m.rw(L8_GAME_STATE),s16(m.rw(world)),s16(m.rw(world+16)));
            for(int p=0;p<2;++p){auto a=anchor(m,legs(m,p));result.last[p]={a[0]-s16(m.rw(world)),a[1]-s16(m.rw(world+16))};std::printf(" p%d=%.3f,%.3f",p+1,result.last[p][0],result.last[p][1]);}
            std::puts("");
        }
    }
    require(!pending_origin[0] && !pending_origin[1],"unfinished animation origin adjustment");
    require(saw_intro&&saw_play&&stable>=60,"transition did not settle and stay stopped");
    require(s16(m.rw(world))==tx&&s16(m.rw(world+16))==ty,"camera did not reach destination");
    for(int p=0;p<2;++p){
        verify_anchor(m,legs(m,p));
        require(rlong(m,legs(m,p)+0x20)==0&&rlong(m,legs(m,p)+0x40)==0,"player retains velocity after script");
        require(rfield(m,rlong(m,rlong(m,players+p*32)+0x140),16)==0,"script failed to release player control");
        const auto a=result.last[p];
        require(a[0]>(wide?250:180)&&a[0]<(wide?290:220)&&a[1]>120&&a[1]<145,"player did not stop in the normal landing zone");
    }
    return result;
}
}
int main(int argc,char**argv){
 const bool negative=argc==3&&std::strcmp(argv[2],"--negative-control")==0;
 bool checking_injected_walk=false;
 try{
    if(argc<2||argc>3||(argc==3&&!negative))return 2;
    combat_escape();
    const char* phase_env=std::getenv("STV_ENTRANCE_BOOT_FRAME");
    const unsigned ready_frame=phase_env?unsigned(std::stoul(phase_env)):4100;
    require(ready_frame>=4100&&ready_frame<=4300,"diagnostic boot phase out of range");
    std::array<Result,4> reference{};
    std::array<double,2> north_reference{},north_reference_timed{};
    std::array<std::array<double,2>,2> studio_reference{};
    for(bool wide:{false,true}){
        Machine m;std::string error;require(m.load_roms(argv[1],error),"ROM load");
        if(wide){m.enhanced=true;m.set_wide(1024,137);require(apply_enhanced(m,137),"enhanced load");}m.reset();
        // Execute each real velocity-root producer, including shoe tables.
        for(u16 state:{u16(0x10),u16(0x18),u16(0x30)}){
            constexpr u32 sites[]={0xffe1feb0,0xffe1ff00,0xffe1fe00,0xffe1fe50};
            constexpr u32 roots[]={0xffe1f8a0,0xffe1f9e0,0xffe1fb20,0xffe1fc60};
            for(unsigned i=0;i<4;++i){
                Machine probe=m;probe.external_timers=true;probe.ww(L8_GAME_STATE,state);
                const auto next=l8_run(probe,sites[i],1);
                const u32 expected=wide&&state==0x10?enhanced_data_address(roots[i]):roots[i];
                require(next==sites[i]+48&&probe.c.a[2]==expected&&probe.c.cyc==3,
                        "velocity root policy or retail instruction timing changed");
            }
        }
        u32 pc=m.reset_vector();
        bool ready=false;
        m.on_vblank=[&]{const auto f=m.frame_number;
            if(f==2600||f==2640||f==2680||f==2720)m.ports[1]&=u16(~1);
            if(f==2612||f==2652||f==2692||f==2732)m.ports[1]|=1;
            if(f==3150)m.ports[1]&=u16(~0x24);if(f==3170)m.ports[1]|=0x24;
            protect(m);
            ready=f>=ready_frame&&m.rw(L8_GAME_STATE)==0x10;
            if(ready||f>=4500){m.run_until_cycle=m.c.cyc;m.recompute_next_event();}};
        while(!ready&&m.frame_number<4500&&!m.stop)pc=l8_run(m,pc,m.c.cyc+1000000);
        require(!m.stop&&ready,"boot did not reach gameplay");m.on_vblank={};
        std::printf("BOOT %s frame=%llu\n",wide?"wide":"classic",(unsigned long long)m.frame_number);
        for(int p=0;p<2;++p){
            auto a=anchor(m,legs(m,p));a[0]-=s16(m.rw(world));a[1]-=s16(m.rw(world+16));
            std::printf("STUDIO %s p%d=%.3f,%.3f\n",wide?"wide":"classic",p+1,a[0],a[1]);
            if(!wide)studio_reference[p]=a;
            else require(std::abs(a[0]-studio_reference[p][0]-72)<=4.125&&
                         std::abs(a[1]-studio_reference[p][1])<0.0001,"studio entry landing changed");
        }
        if(!negative){
            const auto travel=north_walk(argv[1],wide);
            if(!wide){
                north_reference=travel;
                north_reference_timed=north_walk(argv[1],false,true);
                for(int p=0;p<2;++p){
                    require(north_reference[p]==235.125,"Classic default north travel changed");
                    require(north_reference_timed[p]==233.75,"Classic timed-start north travel changed");
                }
            }else for(int p=0;p<2;++p){
                require(travel[p]==north_reference[p] || travel[p]==north_reference_timed[p],
                        "north walk differs from both measured Classic endpoints");
            }
        }
        for(int d=1;d<=4;++d){
            checking_injected_walk=negative&&wide;
            auto result=entrance(m,pc,d,negative);
            checking_injected_walk=false;
            if(!wide)reference[d-1]=result;
            else for(int p=0;p<2;++p){
                // Timed walks can differ by up to three animation ticks after
                // the longer scroll; preserve the same landing within 4.125px.
                require(std::abs(result.last[p][0]-reference[d-1].last[p][0]-72)<=4.125,
                        "widescreen landing differs from the translated Classic landing");
                // Exact north travel is checked on the real route above. This injected
                // SET_WAVE skips the original REDREG process and its sleep phase.
                if(d!=3)require(std::abs(result.last[p][1]-reference[d-1].last[p][1])<0.0001,"vertical landing changed");
            }
        }
    }
    require(!negative,"negative control failed to detect boosted script velocity");
    std::puts("PASS: all four native room transitions, both players, arcade walking velocity, lane, landing, neutral control and stationary endpoint");
    return 0;
}catch(const std::exception&e){
 if(checking_injected_walk&&std::strcmp(e.what(),"script used wrong direction or non-arcade walking speed")==0){
  std::puts("PASS: negative control detected boosted script velocity");return 0;
 }
 std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
