// Headless, full-route stage/pressure runner. Probe runs use retail immunity and
// a 120-frame neutral landing window; pressure runs use legal bot inputs only.
// This is development evidence, not a MAME lockstep or human difficulty score.
#define main room_fixture_unused_main
#include "../../tests/test_room_entrances.cpp"
#undef main
#include "bot.hpp"
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#ifdef _WIN32
#include "visual_stage_audit.hpp"
#endif

namespace {
// Select a legal exit by graph search over the retail destination table.
// Only the bot's input policy changes; no room, progression or game RAM is written.
std::vector<int> destinations(Machine& m,int circuit,int wave){
    const u32 root=m.enhanced?enhanced_data_address(0xffecce80u):0xffecce80u;
    const u32 base=rlong(m,root+u32(circuit)*32)+u32(wave-1)*384;
    std::vector<int> result;
    for(unsigned slot=0;slot<3;++slot){
        if(!rlong(m,base+slot*32)||!rlong(m,base+96+slot*32))continue;
        const int next=int(rlong(m,base+192+slot*32));
        require(next>=1&&next<=(circuit==0?11:circuit==1?19:23),"route destination outside circuit");
        result.push_back(next);
    }
    return result;
}
int next_route_slot(Machine& m,int circuit,int wave,int target){
    const int last=circuit==0?10:circuit==1?18:22;
    if(wave==target)return -1;
    std::array<bool,24> visited{};
    std::vector<std::pair<int,int>> queue{{wave,-1}};
    visited[wave]=true;
    for(std::size_t head=0;head<queue.size();++head){
        const auto [node,first]=queue[head];
        if(node>last)continue; // Bonus tally has no outgoing normal-room row.
        const auto exits=destinations(m,circuit,node);
        for(std::size_t slot=0;slot<exits.size();++slot){
            const int next=exits[slot],choice=first<0?int(slot):first;
            if(next==target)return choice;
            if(!visited[next]){visited[next]=true;queue.push_back({next,choice});}
        }
    }
    return -1;
}
void bmp(const std::filesystem::path& path,Machine&m){
    const unsigned w=scene_view_width(m),h=256,stride=(w*3+3)&~3u,size=54+stride*h;
    std::vector<u8> bytes(size);auto put=[&](unsigned at,u32 v,int n){for(int i=0;i<n;++i)bytes[at+i]=u8(v>>(8*i));};
    bytes[0]='B';bytes[1]='M';put(2,size,4);put(10,54,4);put(14,40,4);put(18,w,4);put(22,h,4);put(26,1,2);put(28,24,2);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){u32 rgb=m.pens[m.frame[(y+VIS_MINY)*m.vram_w+x+VIS_MINX]&4095];unsigned at=54+(h-1-y)*stride+x*3;put(at,rgb,3);}
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(bool(f),"stage image write");
}
}
int main(int argc,char**argv){try{
    if(argc<8||argc>10){std::fprintf(stderr,"ROM OUT wide players seed frames probe [branch [recorded-inputs]]\n");return 2;}
    const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
    const bool wide=std::atoi(argv[3])!=0,probe=std::atoi(argv[7])!=0;
    const char* probe_env=std::getenv("STV_STAGE_PROBE_FROM");
    const unsigned probe_from=probe_env?unsigned(std::stoul(probe_env)):0;
    const int branch=argc>=9?std::atoi(argv[8]):0;
    const int count=std::atoi(argv[4]),seed=std::atoi(argv[5]);const unsigned frames=unsigned(std::stoul(argv[6]));
    require((count==1||count==2)&&frames>0,"stage audit arguments");
    Machine m;std::string error;require(m.load_roms(argv[1],error),"stage audit ROM load");
    if(wide){m.enhanced=true;m.set_wide(1024,137);require(apply_enhanced(m,137),"stage audit enhanced data");}m.reset();
    std::array<int,3> route_targets{};
    std::array<bool,3> targets_seen{};
    if(const char* text=std::getenv("STV_STAGE_ROUTE_TARGETS")){
        std::istringstream stream(text);char comma=0;
        for(int c=0;c<3;++c){
            require(bool(stream>>route_targets[c])&&route_targets[c]>=1&&route_targets[c]<=(c==0?10:c==1?18:22),"invalid route target");
            if(c<2)require(bool(stream>>comma)&&comma==',',"invalid route target separator");
        }
        stream>>std::ws;require(stream.eof(),"extra route target data");
        require(argc!=10,"route targeting is not supported with recorded inputs");
    }
    if(std::getenv("STV_STAGE_PLAN_ONLY")){
        for(int c=0;c<3;++c){
            require(route_targets[c]!=0,"plan-only requires three targets");
            int w=1;unsigned steps=0;
            while(w!=route_targets[c]){
                require(++steps<=23,"route plan contains a cycle");
                const int slot=next_route_slot(m,c,w,route_targets[c]);
                require(slot>=0,"route target unreachable in exit table");
                const auto exits=destinations(m,c,w);
                std::printf("PLAN circuit=%d source=%d slot=%d destination=%d\n",c,w,slot,exits[slot]);
                w=exits[slot];
            }
        }
        std::puts("PLAN_VALIDATED: exit-table reachability only, no gameplay executed");return 0;
    }
    stv::bot::Bot bots[]={stv::bot::Bot(seed,1),stv::bot::Bot(seed,2)};
    if(branch)for(auto& bot:bots)bot.route_slot_=seed;
    struct Input {unsigned frame,mask;int port,value;};
    std::vector<Input> replay;std::size_t cursor=0;
    if(argc==10) {
        std::ifstream file(argv[9]);unsigned f,mask;int value;std::string port;
        while(file>>f>>port>>std::hex>>mask>>std::dec>>value) {
            require((port==":IN0"||port==":IN1")&&f<=frames,"invalid replay input");
            replay.push_back({f,mask,port==":IN0"?0:1,value});
        }
        require(file.eof()&&!replay.empty(),"missing or malformed replay input");
    }
    std::ofstream samples(out/"entrances.csv"),inputs(out/"inputs.txt");
    samples<<"frame,state,circuit,wave,world_x,world_y,p1_x,p1_y,p2_x,p2_y,event,from_door,p1_control,p1_vx,p1_vy,p2_control,p2_vx,p2_vy\n";
    std::set<std::pair<int,int>> rooms,images;int oldstate=-1,oldc=-1,oldw=-1;unsigned landed=0,state_age=0;bool paused=false,done=false;
    // These per-frame coverage taps are diagnostic observations, not clearance
    // ownership or instruction order within a single sampled frame.
    require(l8_cov_count==33295,"stage observer requires the reviewed L8 coverage map");
    constexpr unsigned taps[]={18081,18094,18107,18114,18206,18389};
    std::ofstream visits(out/"clear-events.csv");
    visits<<"frame,oldstate,oldc,oldw,state,c,w,guard,accepted,tank_cleanup,shake_wait,door_check,exit_write\n";
    std::ofstream integrity(out/"list-integrity.csv");unsigned health_samples=0,first_invalid_frame=0;
#ifdef _WIN32
    VisualStageAudit visuals(m,out);
#endif
    m.on_vblank=[&]{const unsigned f=unsigned(m.frame_number);int gs=m.rw(L8_GAME_STATE),c=m.rw(0x010eb610),w=m.rw(0x010eb630);
#ifdef _WIN32
        visuals.sample();
#endif
        bool tapped=false;
        for(unsigned i:taps)tapped|=l8_cov[i]!=0;
        if(tapped){
            visits<<f<<','<<oldstate<<','<<oldc<<','<<oldw<<','<<gs<<','<<c<<','<<w;
            for(unsigned i:taps){visits<<','<<int(l8_cov[i]);l8_cov[i]=0;}
            visits<<'\n';
        }
        if(f>=2500 && !first_invalid_frame){
            ++health_samples;
            for(u32 root:{0x01000000u,0x01000020u,0x01000040u,0x01000060u,0x010000a0u}){
                if(first_invalid_frame)break;
                const bool process=root<0x01000040u;
                const u32 first=process?0x01086790u:0x010003a0u,stride=process?0x5a0u:0x1f0u,count=process?240u:420u;
                std::array<bool,420> seen{};
                for(u32 node=rlong(m,root);node;node=rlong(m,node)){
                    if(node<first || node>=first+stride*count || (node-first)%stride || seen[(node-first)/stride]){
                        integrity<<f<<','<<std::hex<<root<<','<<node<<std::dec<<'\n';integrity.flush();
                        std::fprintf(stderr,"LIST_FAILURE frame=%u root=%08x node=%08x\n",f,root,node);
                        first_invalid_frame=f;
                        // Classic preserves the retail defect; stop using its
                        // subsequent visits for balance. Wide must be repaired.
                        if(wide){m.stop=true;return;}
                        break;
                    }
                    seen[(node-first)/stride]=true;
                }
            }
        }
        const bool changed=gs!=oldstate||c!=oldc||w!=oldw;
        if(changed)state_age=0;else ++state_age;
        if(gs==0x10&&changed) {
            landed=f;
            // Legal input strategy changes at actual room entry; never change
            // the game's room, enemy state, score or route selection tables.
            if(branch&&((c==0&&w==4)||(c==1&&w==1)||(c==2&&w==1))) {
                int next=(c==2?(branch==1?5:branch==2?1:0):0);
                for(int p=0;p<count;++p) {
                    bots[p].finish_pressure(f);
                    bots[p]=stv::bot::Bot(next,p+1);
                    bots[p].route_slot_=next;
                }
            }
        }
        if(gs==0x10&&changed&&c>=0&&c<3&&w>=1&&w<=(c==0?10:c==1?18:22)&&route_targets[c]){
            targets_seen[c]|=w==route_targets[c];
            // Once the requested room is reached, restore the normal timed retry
            // policy; permanently selecting slot0 can strand the bot at a gated exit.
            const int slot=targets_seen[c]?-1:next_route_slot(m,c,w,route_targets[c]);
            std::printf("ROUTE_TARGET frame=%u circuit=%d wave=%d target=%d seen=%d slot=%d\n",f,c,w,route_targets[c],int(targets_seen[c]),slot);
            // Fighting players can cross another legal exit before the room
            // clears. Preserve that outcome and let the normal input policy
            // continue until the target becomes reachable again.
            if(!targets_seen[c]&&slot<0)
                std::printf("ROUTE_MISSED frame=%u circuit=%d wave=%d target=%d\n",f,c,w,route_targets[c]);
            for(int p=0;p<count;++p)bots[p].route_slot_=slot;
        }
        static u32 mutptr_addr=0;static std::vector<u32> hits;
        if(std::getenv("STV_MUTOID_TRACE") && !mutptr_addr){
            u32 tank=0;
            for(u32 i=0;i<object_slots;++i){const u32 o=object_pool_base+i*object_stride;if(m.rw(o+0x150)==0x9801){tank=o;break;}}
            if(tank){
                const u32 proc=rlong(m,tank+0x160);
                std::printf("TANK %08x proc %08x type %u parent %08x\n",tank,proc,m.rw(proc+0x20),rlong(m,proc+0x100));
                for(u32 cand:{proc,rlong(m,proc+0x100)})
                    for(u32 a=0x01000000;a<0x010f0000;a+=32)if(rlong(m,a)==cand&&a!=tank+0x160){std::printf("HIT cand=%08x at %08x\n",cand,a);if(cand==proc)hits.push_back(a);}
                for(int k=0;k<3;++k){
                    const u32 t=rlong(m,enhanced_data_address(0xffe8b8d0u)+u32(k)*32);
                    std::printf("BND set %d:",k);for(int e=0;e<6;++e)std::printf(" %d",s16(rlong(m,t+u32(e)*32)>>16));std::printf("\n");
                }
                mutptr_addr=1;
            }
        }
        if(std::getenv("STV_MUTOID_TRACE") && mutptr_addr)
            std::printf("MUTST f=%u act=%d mtime=%d dmgpnt=%d lzr=%d snkout=%d upd=%d\n",f,rb(m,17772088u),s16(m.rw(17772096u)),rb(m,17771520u),rb(m,17771608u),rb(m,17772144u),s16(m.rw(17771640u)));
        if(std::getenv("STV_MUTOID_TRACE")){
            // Diagnostic only: Mutoid tank half position relative to the world origin.
            for(u32 i=0;i<object_slots;++i){
                const u32 o=object_pool_base+i*object_stride;
                if(m.rw(o+0x150)!=0x9801)continue;
                std::printf("MUT f=%u gs=%x c=%d w=%d x=%.2f y=%.2f vx=%.3f vy=%.3f wx=%.2f wy=%.2f act=%d\n",f,gs,c,w,
                    s32(rlong(m,o+0x60))/65536.0,s32(rlong(m,o+0x80))/65536.0,
                    s32(rlong(m,o+0x20))/65536.0,s32(rlong(m,o+0x40))/65536.0,
                    s32(rlong(m,0x01000240))/65536.0,s32(rlong(m,0x01000260))/65536.0,0);
            }
        }
        if(const char* shots=std::getenv("STV_MUTOID_SHOTS")){
            std::istringstream list(shots);unsigned want;char comma;
            while(list>>want){if(want==f)bmp(out/("mut-"+std::to_string(f)+".bmp"),m);list>>comma;}
        }
        if(probe&&f>=probe_from)protect(m);
        const bool neutral=probe&&f>=probe_from&&gs==0x10&&f-landed<120;
        if(paused&&!neutral)for(int p=0;p<count;++p)bots[p].resync();paused=neutral;
        while(cursor<replay.size()&&replay[cursor].frame<=f) {
            const auto&e=replay[cursor++];
            require(e.frame==f,"skipped replay input frame");
            m.ports[e.port]=e.value?u16(m.ports[e.port]&~e.mask):u16(m.ports[e.port]|e.mask);
            inputs<<f<<" :IN"<<e.port<<" "<<std::hex<<e.mask<<std::dec<<" "<<e.value<<"\n";
        }
        for(int p=0;p<count&&argc!=10;++p)for(const auto&e:bots[p].step(f,m)){
            bool value=e.value!=0;
            if(probe&&e.port==1&&rlong(m,players+p*32))value=false;
            if(neutral&&e.port==0)value=false;
            m.ports[e.port]=value?u16(m.ports[e.port]&~e.mask):u16(m.ports[e.port]|e.mask);
            inputs<<f<<" :IN"<<e.port<<" "<<std::hex<<e.mask<<std::dec<<" "<<int(value)<<"\n";
        }
        if(neutral)m.ports[0]=0xffff;
        if(gs==0x10&&c>=0&&c<3&&w>0&&w<(c==0?11:c==1?19:23)&&(c==0||w!=10))rooms.insert({c,w});
        const char*event=changed?"boundary":gs==0x10&&f-landed==120?"settled":"tick";
        if(changed||gs==0x18||(gs==0x10&&f-landed<=120)||f%60==0||f>=frames){
            samples<<f<<','<<gs<<','<<c<<','<<w<<','<<s16(m.rw(world))<<','<<s16(m.rw(world+16));
            for(int p=0;p<2;++p){u32 pr=rlong(m,players+p*32),obj=pr?legs(m,p):0;
                if(obj>=0x010003a0&&obj<0x01080000){auto a=anchor(m,obj);samples<<','<<a[0]-s16(m.rw(world))<<','<<a[1]-s16(m.rw(world+16));}
                else samples<<",nan,nan";
            }
            samples<<','<<event<<','<<rfield(m,0x010ee988,16);
            for(int p=0;p<2;++p){
                const u32 pr=rlong(m,players+p*32),obj=pr?legs(m,p):0;
                if(pr>=0x01000000&&pr<0x01100000&&obj>=0x010003a0&&obj<0x01080000)
                    samples<<','<<rfield(m,rlong(m,pr+0x140),16)<<','<<s32(rlong(m,obj+0x20))/65536.0<<','<<s32(rlong(m,obj+0x40))/65536.0;
                else samples<<",-1,nan,nan";
            }
            samples<<'\n';
        }
        if(probe&&((gs==0x10&&f-landed==120)||(gs==0x18&&state_age==600))&&images.insert({c,w}).second)
            bmp(out/("stage-"+std::to_string(c)+"-"+std::to_string(w)+".bmp"),m);
        oldstate=gs;oldc=c;oldw=w;
        if(f>=frames){done=true;m.run_until_cycle=m.c.cyc;m.recompute_next_event();}
    };
    u32 pc=m.reset_vector();while(!done&&!m.stop)pc=l8_run(m,pc,m.c.cyc+1000000);
    std::printf("LIST_INTEGRITY samples=%u first_invalid_frame=%u\n",health_samples,first_invalid_frame);
    for(int p=0;p<count;++p)bots[p].finish_pressure(unsigned(m.frame_number));
    require(!m.stop&&done,"stage audit stopped before endpoint");require(bool(samples)&&bool(inputs),"stage audit output failure");
    require(cursor==replay.size(),"unconsumed replay input");
    if(std::getenv("STV_STAGE_DIAGNOSTICS")){
        bmp(out/"terminal.bmp",m);
        std::ofstream coverage(out/"native.cov",std::ios::binary);
        coverage.write(reinterpret_cast<const char*>(l8_cov),std::streamsize(l8_cov_count));
        coverage.close();require(bool(coverage),"stage coverage write failure");
    }
    std::printf("STAGE_TERMINAL frames=%llu mode=%s players=%d seed=%d probe=%d rooms=%zu pc=%08x\n",(unsigned long long)m.frame_number,wide?"wide":"classic",count,seed,int(probe),rooms.size(),pc);
    std::printf("native: frames=%llu cycles=%llu pc=%08x faults=%zu hcount_left=0 input_left=0 input_mismatch=0 big_field=%llu\n",(unsigned long long)m.frame_number,(unsigned long long)m.c.cyc,pc,m.faults.size(),(unsigned long long)m.big_field_quirks);
    for(auto [c,w]:rooms)std::printf("ROOM %d %d\n",c,w);
    return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL stage playthrough: %s\n",e.what());return 1;}}
