// Reuse the validated native boot/anchor/process helpers. These are fixtures,
// not a second implementation of gameplay or an unassisted clearance proof.
#define main four_entrance_test_main
#include "test_room_entrances.cpp"
#undef main
#include <vector>
#include <set>
#include <cstdlib>

namespace {
struct Route {int circuit,source,destination,slot,door;};
struct Landing {
    std::array<std::array<double,2>,2> player{};
    int camera_x=0,camera_y=0,state=0,circuit=0,wave=0;
    unsigned frames=0,replacements=0,jumps=0;
    bool settled=false,bonus=false;
};
u32 data_root(bool wide,u32 a){return wide?enhanced_data_address(a):a;}
u32 camera(Machine& m,int c,int w){return rlong(m,rlong(m,data_root(m.enhanced,0xffe8c7f0)+c*32)+w*32);}
void world_at(Machine& m,u32 xy){
    wlong(m,world,xy);wlong(m,waveyx,xy);
    wlong(m,0x01000240,u32(s16(xy))<<16);wlong(m,0x01000260,u32(s16(xy>>16))<<16);
}
bool bonus_stage(const Route&r){return r.destination==(r.circuit==0?11:r.circuit==1?19:23);}
Machine boot_game(const char* rom,bool wide,u32& pc,int player_count=2){
    Machine m;std::string err;require(m.load_roms(rom,err),"stage fixture ROM load");
    if(wide){m.enhanced=true;m.set_wide(1024,137);require(apply_enhanced(m,137),"stage fixture wide data");}m.reset();
    pc=m.reset_vector();m.on_vblank=[&]{const auto f=m.frame_number;
        if(f==2600||f==2640||f==2680||f==2720)m.ports[1]&=u16(~1);
        if(f==2612||f==2652||f==2692||f==2732)m.ports[1]|=1;
        if(f==3150)m.ports[1]&=u16(~(player_count==1?0x04:0x24));if(f==3170)m.ports[1]|=0x24;
        protect(m);if(f>=4100){m.run_until_cycle=m.c.cyc;m.recompute_next_event();}
        if(std::getenv("STV_STAGE_BOOT_TRACE") && f>=3100 && f%50==0){const u32 pr=rlong(m,players);const u32 lg=pr?rlong(m,pr+0x300):0;
            if(lg)std::printf("BOOT %s f=%u state=%x camera=%d legX=%.2f\n",wide?"wide":"classic",unsigned(f),m.rw(L8_GAME_STATE),s16(m.rw(world)),s32(rlong(m,lg+0x60))/65536.0-s16(m.rw(world)));}};
    while(m.frame_number<4100&&!m.stop)pc=l8_run(m,pc,m.c.cyc+1000000);
    require(!m.stop&&m.rw(L8_GAME_STATE)==0x10,"stage fixture boot failed");m.on_vblank={};return m;
}
std::vector<Route> routes(Machine& m){
    std::vector<Route> result;
    for(int c=0;c<3;++c){
        const int last=c==0?10:c==1?18:22;
        const u32 table=rlong(m,0xffecce80+c*32);
        for(int w=1;w<=last;++w){
            if(!camera(m,c,w))continue; // Explicit unused selector10 in circuits2/3.
            for(int slot=0;slot<3;++slot){
                const u32 a=table+(w-1)*384+slot*32;
                if(!rlong(m,a))continue;
                int dest=int(rlong(m,a+192)),door=int(rlong(m,a+288));
                require(dest>=1&&dest<=(c==0?11:c==1?19:23)&&door>=1&&door<=4,"retail route extent changed");
                result.push_back({c,w,dest,slot,door});
            }
        }
    }
    return result;
}
Landing run_route(const Machine& boot,u32 pc,const Route&r,Machine* landed=nullptr,u32* resume=nullptr,int player_count=2){
    Machine m=boot;m.faults.clear();const bool wide=m.enhanced;
    m.ww(0x010eb610,r.circuit);m.ww(0x010eb630,r.source);
    wlong(m,L8_BACKGROUND_MODULES,rlong(m,data_root(wide,0xffe72650)+r.circuit*32));sync_scene_bounds(m);
    const u32 start_camera=camera(m,r.circuit,r.source);world_at(m,start_camera);
    const u32 table=rlong(m,data_root(wide,0xffecce80)+r.circuit*32);
    const u32 box=table+(r.source-1)*384+r.slot*32;
    std::array<u32,2> objects{};std::array<std::array<double,2>,2> previous{};
    for(int p=0;p<player_count;++p){
        u32 proc=rlong(m,players+p*32),leg=legs(m,p),torso=rlong(m,proc+0x100);
        require(proc&&leg&&torso,"stage fixture missing a live player");
        double x=(r.door==1?40:r.door==2?360:190)+(wide?(r.door==1?4:r.door==2?141:72):0);
        double y=r.door==3?35:r.door==4?237:130;
        if(r.door<=2)y+=p*8;else x+=p*8;
        auto a=anchor(m,leg);
        for(u32 obj:{leg,torso}){
            wlong(m,obj+0x60,rlong(m,obj+0x60)+s32((s16(start_camera)+x-a[0])*65536));
            wlong(m,obj+0x80,rlong(m,obj+0x80)+s32((s16(start_camera>>16)+y-a[1])*65536));
            wlong(m,obj+0x20,0);wlong(m,obj+0x40,0);
        }
        verify_anchor(m,leg);objects[p]=leg;previous[p]=anchor(m,leg);
    }
    // Run the actual REDREG prelude through its first PRCSLP. This includes
    // special route handling preceding SET_WAVE; A1 names the selected box's
    // lower-right row (the caller contract at ENDWAVE TX/READY1/READY2).
    {Machine setup=m;setup.external_timers=true;setup.run_until_cycle=~0ull;setup.next_event_cycle=~0ull;setup.c=Cpu{};
     // The boot is an active first room, not a cleared source room. Suspend its
     // obsolete processes (including spawners) while retaining both real torso
     // and leg processes. Newly created destination processes run normally.
     std::set<u32> keep;
     for(int p=0;p<player_count;++p){const u32 torso=rlong(setup,players+p*32);keep.insert(torso);keep.insert(rlong(setup,torso+0x2c0));}
     for(u32 n=rlong(setup,0x01000000);n;n=rlong(setup,n)){
         const u32 wake=rlong(setup,n+0xe0);
         if(!keep.count(n)&&!(wake>=0xffe6e540&&wake<=0xffe6e720))setup.ww(n+0x30,30000);
     }
     setup.c.a[13]=0x01000000;setup.c.a[1]=0;setup.c.a[7]=0xffec9540;call(setup,0xffe0aba0);
     const u32 node=setup.c.a[0];setup.c.a[13]=node;setup.c.a[12]=node+0x5a0;setup.c.a[1]=box+96;
     setup.c.sp=0x010fffe0;u32 entry=0xffec9540;
     for(unsigned n=0;entry!=0xffe0a500&&!setup.stop&&n<20000;++n)entry=l8_run(setup,entry,setup.c.cyc+1);
     require(entry==0xffe0a500&&!setup.stop&&setup.faults.empty(),"REDREG did not reach its native sleep boundary");
     std::copy(m.ram.end()-256,m.ram.end(),setup.ram.end()-256);m.ram=setup.ram;}
    Landing out;out.bonus=bonus_stage(r);bool intro=false;unsigned stable=0;
    const unsigned start=unsigned(m.frame_number),limit=out.bonus?6500:2200;
    for(unsigned f=1;f<=limit;++f){
        // Diagnostic: report the instruction that moves P1's legs to a new X.
        if(const char* watch=std::getenv("STV_STAGE_WATCH");watch && f>=unsigned(std::atoi(watch)) && f<unsigned(std::atoi(watch))+400){
            u32 last=0,prev_pc=pc;
            step_to(m,pc,start+f,[&](Machine& mm,u32 at){
                const u32 proc=rlong(mm,players);const u32 leg=proc?rlong(mm,proc+0x300):0;
                const u32 x=leg?rlong(mm,leg+0x60):0;
                if(at==0xffe9ef50u||at==0xffe9ef90u||at==0xffe9efe0u||at==0xffe9ef30u||at==0xffe9ef00u)
                    std::printf("REG f=%u pc=%08x state=%x fromdr=%x a0=%08x a9=%08x\n",f,at,mm.rw(L8_GAME_STATE),mm.rw(0x010eb5d0u),mm.c.a[0],mm.c.a[9]);
                if(last && x!=last && (std::abs(s32(x-last))>0x40000))
                    std::printf("WATCH f=%u pc=%08x prev=%08x x %08x -> %08x leg=%08x\n",f,at,prev_pc,last,x,leg);
                last=x;prev_pc=at;
            });
        } else
        step_to(m,pc,start+f);out.frames=f;
        if(const char* dump=std::getenv("STV_STAGE_DUMP");dump && f%(std::getenv("STV_STAGE_DUMP_EVERY")?std::atoi(std::getenv("STV_STAGE_DUMP_EVERY")):4)==0 && f<=unsigned(std::getenv("STV_STAGE_DUMP_LAST")?std::atoi(std::getenv("STV_STAGE_DUMP_LAST")):400)){
            const unsigned w=scene_view_width(m);char name[512];
            std::snprintf(name,sizeof name,"%s/%s-%d-%d-%d-f%04u.ppm",dump,wide?"wide":"classic",r.circuit,r.source,r.destination,f);
            if(FILE* o=std::fopen(name,"wb")){
                std::fprintf(o,"P6\n%u 256\n255\n",w);
                for(unsigned y=0;y<256;++y)for(unsigned x=0;x<w;++x){
                    const u32 c=m.pens[m.frame[(y+VIS_MINY)*m.vram_w+x+VIS_MINX]&4095];
                    const unsigned char rgb[3]={u8(c>>16),u8(c>>8),u8(c)};std::fwrite(rgb,1,3,o);
                }
                std::fclose(o);
            }
        }out.state=m.rw(L8_GAME_STATE);out.circuit=m.rw(0x010eb610);out.wave=m.rw(0x010eb630);
        out.camera_x=s16(m.rw(world));out.camera_y=s16(m.rw(world+16));intro|=out.state==0x18;
        bool moved=false,neutral=true;
        for(int p=0;p<player_count;++p){
            u32 proc=rlong(m,players+p*32),leg=proc?legs(m,p):0;
            if(!leg){neutral=false;continue;}
            if(leg!=objects[p]){++out.replacements;objects[p]=leg;}
            auto a=anchor(m,leg);
            double dx=a[0]-previous[p][0],dy=a[1]-previous[p][1];
            moved|=std::abs(dx)>0.0001||std::abs(dy)>0.0001;
            if(std::abs(dx)>8||std::abs(dy)>8)++out.jumps;
            previous[p]=a;out.player[p]={a[0]-out.camera_x,a[1]-out.camera_y};
            neutral&=rfield(m,rlong(m,proc+0x140),16)==0;
        }
        stable=(!moved&&neutral)?stable+1:0;
        if(f%100==0)std::printf("STAGE %s c=%d %d>%d slot=%d f=%u state=%x camera=%d,%d p1=%.3f,%.3f p2=%.3f,%.3f bg=%08x dead=%x,%x\n",wide?"wide":"classic",r.circuit,r.source,r.destination,r.slot,f,out.state,out.camera_x,out.camera_y,out.player[0][0],out.player[0][1],out.player[1][0],out.player[1][1],scene_background(m),rfield(m,rlong(m,players)+0x208,16),player_count==2?rfield(m,rlong(m,players+32)+0x208,16):0);
        if(!out.bonus&&intro&&out.state==0x10&&stable>=60){out.settled=true;break;}
        if(out.bonus&&out.circuit==r.circuit+1&&out.wave==1&&out.state==0x10&&stable>=60){out.settled=true;break;}
        // Final circuit's bonus exits to the ending page, not a fourth circuit.
        if(out.bonus&&r.circuit==2&&f>1800&&scene_view_width(m)==410&&scene_background(m)==0xffea54f0){out.settled=true;break;}
    }
    for(int p=0;p<player_count;++p)if(rlong(m,players+p*32)&&legs(m,p))verify_anchor(m,legs(m,p));
    std::printf("RESULT %s c=%d %d>%d slot=%d door=%d frames=%u state=%x next=%d/%d camera=%d,%d p1=%.3f,%.3f p2=%.3f,%.3f replacements=%u jumps=%u settled=%d bonus=%d\n",wide?"wide":"classic",r.circuit,r.source,r.destination,r.slot,r.door,out.frames,out.state,out.circuit,out.wave,out.camera_x,out.camera_y,out.player[0][0],out.player[0][1],out.player[1][0],out.player[1][1],out.replacements,out.jumps,int(out.settled),int(out.bonus));
    if(resume)*resume=pc;
    if(landed)*landed=std::move(m);
    return out;
}
}
#ifndef STV_STAGE_FIXTURE_LIBRARY
int main(int argc,char**argv){try{
    if(argc!=2)return 2;unsigned failures=0;std::vector<Landing> refs;std::vector<Route> matrix;
    const char* filter=std::getenv("STV_STAGE_CASE");
    for(bool wide:{false,true}){
        u32 pc=0;Machine boot=boot_game(argv[1],wide,pc);
        if(!wide)matrix=routes(boot);
        for(std::size_t i=0;i<matrix.size();++i){const auto&r=matrix[i];
            if(filter){char key[64];std::snprintf(key,sizeof(key),"%d/%d/%d",r.circuit,r.source,r.slot);if(std::strcmp(filter,key))continue;}
            auto result=run_route(boot,pc,r);
            if(!wide){if(refs.size()<matrix.size())refs.resize(matrix.size());refs[i]=result;}
            else{
                const auto& ref=refs[i];bool good=result.settled&&ref.settled;
                if(!r.circuit||!result.bonus){
                    for(int p=0;p<2;++p){
                        const double x=ref.player[p][0];const int shift=x<154?4:x<247?72:141;
                        good&=std::abs(result.player[p][0]-x-shift)<=4.125&&std::abs(result.player[p][1]-ref.player[p][1])<=2.75;
                    }
                }
                if(!good){++failures;std::printf("MISMATCH c=%d %d>%d slot=%d\n",r.circuit,r.source,r.destination,r.slot);}
            }
        }
    }
    std::printf("STAGE AUDIT routes=%zu failures=%u\n",matrix.size(),failures);return failures?1:0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL stage audit: %s\n",e.what());return 1;}}
#endif
