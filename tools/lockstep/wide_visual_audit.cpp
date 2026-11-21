// Selected-ROM visual fixtures, not natural-route or clearance evidence.
// Reuse the native REDREG/SET_WAVE entry contract and real two-player processes.
#define STV_STAGE_FIXTURE_LIBRARY
#include "../../tests/test_all_stage_entrances.cpp"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include "visual_stage_audit.hpp"

int main(int argc,char**argv){try{
    const std::string_view option=argc==4?argv[3]:"";
    const bool pressure=option=="--pressure-1p" || option=="--pressure-2p";
    const int player_count=option=="--pressure-1p"?1:2;
    if(argc!=3 && !pressure)return 2;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
    u32 classicPc=0;auto classic=boot_game(argv[1],false,classicPc);const auto matrix=routes(classic);
    u32 bootPc=0;auto boot=boot_game(argv[1],std::getenv("STV_VISUAL_CLASSIC")==nullptr,bootPc,player_count);
    // Carry each circuit's native initialization (including palette/background
    // ownership) through its bonus transition, rather than changing only CIRCUIT.
    std::vector<Machine> circuitBoots;std::vector<u32> circuitPcs;
    circuitBoots.push_back(boot);circuitPcs.push_back(bootPc);
    for(int circuit=1;circuit<3;++circuit){
        const Route* entry=nullptr;
        for(const auto& route:matrix)if(route.circuit==circuit-1 && bonus_stage(route)){entry=&route;break;}
        require(entry!=nullptr,"missing native circuit transition");
        Machine next=circuitBoots.back();u32 nextPc=circuitPcs.back();
        run_route(circuitBoots.back(),nextPc,*entry,&next,&nextPc,player_count);
        require(next.rw(0x010eb610)==circuit && next.rw(0x010eb630)==1,"native circuit initialization failed");
        circuitBoots.push_back(std::move(next));circuitPcs.push_back(nextPc);
    }
    _putenv_s("STV_STAGE_DXR","1");_putenv_s("STV_STAGE_DXR_EFFECTS","1");
    _putenv_s("STV_STAGE_ALL_EFFECTS","1");_putenv_s("STV_STAGE_BOSS_RAW","1");
    std::ofstream manifest(out/"rooms.csv");manifest<<"room,settled,frames,width,max_boss_parts\n";
    unsigned visited=0,bosses=0,failures=0;
    const int last[]={10,18,22};
    for(int c=0;c<3;++c)for(int w=1;w<=last[c];++w){
        if(c && w==10)continue;
        const unsigned key=(unsigned(c)<<16)|unsigned(w);
        if(const char* only=std::getenv("STV_WIDE_VISUAL_ROOM");only && key!=std::stoul(only))continue;
        const auto& circuitBoot=circuitBoots[c];const u32 circuitPc=circuitPcs[c];
        Machine m=circuitBoot;u32 pc=circuitPc;bool settled=true;
        if(w!=1){
            const Route* chosen=nullptr;
            for(const auto& route:matrix){
                const bool match=w==1 ? route.circuit==c-1 && bonus_stage(route) : route.circuit==c && route.destination==w;
                if(match && (!chosen || (route.door<=2 && chosen->door>2)))chosen=&route;
            }
            require(chosen!=nullptr,"no native entry for requested visual room");
            // Scrolling redraws only newly exposed background strips. First
            // render the real source room; relocating a first-room framebuffer
            // alone can leave stale rectangular patches in the destination.
            if(chosen->source!=1){
                const Route* prior=nullptr;
                for(const auto& route:matrix){
                    const bool match=chosen->source==1 ? route.circuit==chosen->circuit-1 && bonus_stage(route) :
                        route.circuit==chosen->circuit && route.destination==chosen->source;
                    if(match && (!prior || (route.door<=2 && prior->door>2)))prior=&route;
                }
                require(prior!=nullptr,"no source-room entry for visual fixture");
                run_route(circuitBoot,circuitPc,*prior,&m,&pc,player_count);
                require(m.rw(0x010eb610)==chosen->circuit && m.rw(0x010eb630)==chosen->source,"source-room priming failed");
            }
            settled=run_route(m,pc,*chosen,&m,&pc,player_count).settled;
        }
        if(m.rw(0x010eb610)!=c || m.rw(0x010eb630)!=w){
            std::printf("VISUAL_MISSING room=%u actual=%u/%u\n",key,m.rw(0x010eb610),m.rw(0x010eb630));++failures;continue;
        }
        const bool boss=key==10 || key==0x10007 || key==0x20007 || key==0x20015;
        _putenv_s("STV_STAGE_BOSS_REVIEW",boss?"1":"");
        const auto folder=out/("room-"+std::to_string(key));std::filesystem::create_directories(folder);
        std::optional<VisualStageAudit> visual;
        if(pressure){
            // Remove the entry fixture's immunity before measuring combat.
            for(unsigned p=0;p<player_count;++p){
                const u32 proc=rlong(m,players+p*32);
                if(proc)m.ww(proc+0x160,0);
            }
        }else visual.emplace(m,folder);
        stv::bot::Bot bots[]={stv::bot::Bot(0,1),stv::bot::Bot(0,2)};
        for(auto& bot:bots)bot.log_every_=0;
        const unsigned start=unsigned(m.frame_number),limit=pressure?(boss?24000:9000):(boss?36000:1200);
        unsigned elapsed=0,maxParts=0;
        for(unsigned f=1;f<=limit;++f){
            for(int player=0;player<player_count;++player)for(const auto& e:bots[player].step(unsigned(m.frame_number),m)){
                const bool value=e.value && (pressure || !(e.port==1 && rlong(m,players+player*32)));
                m.ports[e.port]=value?u16(m.ports[e.port]&~e.mask):u16(m.ports[e.port]|e.mask);
            }
            step_to(m,pc,start+f,{},!pressure);elapsed=f;
            if(m.rw(0x010eb610)!=c || m.rw(0x010eb630)!=w ||
               (pressure && game_scene_state(m)==0x30))break;
            if(visual)visual->sample();
            if(f%4==0){const auto scene=stv::host::observe_effect_scene(m.ram,m.frame_number,scene_view_width(m),256);
                maxParts=std::max(maxParts,scene.studio.bossParts);}
        }
        if(pressure){
            for(int p=0;p<player_count;++p)bots[p].finish_pressure(unsigned(m.frame_number));
            std::printf("PRESSURE_ROOM room=%u changed_room=%d state=%x frames=%u limit=%u\n",
                key,int(m.rw(0x010eb610)!=c || m.rw(0x010eb630)!=w),m.rw(L8_GAME_STATE),elapsed,limit);
        }
        manifest<<key<<','<<settled<<','<<elapsed<<','<<scene_view_width(m)<<','<<maxParts<<'\n';manifest.flush();
        std::printf("VISUAL_ROOM room=%u settled=%d frames=%u boss_parts=%u\n",key,int(settled),elapsed,maxParts);
        ++visited;if(boss){++bosses;if(!maxParts)++failures;}
        m.on_object={};
    }
    std::printf("VISUAL_TERMINAL rooms=%u bosses=%u failures=%u fixture_only=1\n",visited,bosses,failures);
    return failures?1:0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL widescreen visual fixture: %s\n",e.what());return 1;}}
