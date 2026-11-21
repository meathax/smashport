// Throughput of the two join-replay paths: main-only rollback machine and the
// coupled (main + sound board) machine. Development benchmark, not a CTest.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <chrono>
#define wWinMain unused_bench_entry
#include "../src/host/win_main.cpp"
#undef wWinMain
int main(int argc,char**argv){
    if(argc<3)return 2;
    std::filesystem::create_directories(argv[2]);g_rom_path=std::filesystem::path(argv[2])/"retail-l8.dat";
    std::string error;if(!stv::gsp::import_rom_set(argv[1],g_rom_path,error))return 3;
    g_enhanced=true;if(!reset_coupled_game())return 4;
    {   // Existing design: coupled g_m and a main-only runner both from power-on.
        Machine base;if(!stv::gsp::load_imported_roms(base,g_rom_path,error))return 11;
        base.enhanced=true;base.set_wide(1024,137);if(!stv::gsp::apply_enhanced(base,137))return 12;
        stv::net::FrameRunner fr(base);fr.start();base.cmos=g_m.cmos;
        unsigned first=~0u,diffs=0;
        for(unsigned i=0;i<8000;++i){
            const u32 in=(i>2600&&i<2620)?0x100u:(i>3150&&i<3170)?0x200u:(i/120)%2?0x0110u:0x0028u;
            g_pcm.clear();run_one_frame();stv::net::apply_inputs(g_m,in,0);
            fr.step(in,0);
            if(fr.frame()&&base.ram!=g_m.ram){diffs++;if(first==~0u)first=i;
                if(i==first||i==first+1||i%1000==0){unsigned n=0;std::printf("frame %u differing words:",i);
                    for(size_t w=0;w<base.ram.size();++w)if(base.ram[w]!=g_m.ram[w]){if(n<12)std::printf(" %08x",unsigned(0x01000000+w*16));++n;}
                    std::printf(" (total %u)\n",n);}}
        }
        std::printf("power-on runner vs coupled: first RAM difference at frame %d, differing frames %u/8000\n",first==~0u?-1:int(first),diffs);
        if(!reset_coupled_game())return 4;
    }
    const unsigned N=20000;
    auto t=std::chrono::steady_clock::now();
    for(unsigned i=0;i<N;++i){g_pcm.clear();run_one_frame();if(g_m.stop)return 5;}
    double coupled=std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();
    Machine mr;if(!stv::gsp::load_imported_roms(mr,g_rom_path,error))return 6;
    mr.enhanced=true;mr.set_wide(1024,137);if(!stv::gsp::apply_enhanced(mr,137))return 7;
    stv::net::FrameRunner runner(mr);runner.start();
    t=std::chrono::steady_clock::now();
    for(unsigned i=0;i<N;++i)runner.step(0,0);
    double main_only=std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();
    {   // Exact drop-in: restore the coupled snapshot and replay identical inputs.
        auto record=[&](std::vector<u32>& ram,std::vector<u32>& pcm){
            for(unsigned i=0;i<3000;++i){
                const u32 in=(i/90)%3==0?0x0028u:(i/90)%3==1?0x0164u:0x0012u;
                g_pcm.clear();run_one_frame();stv::net::apply_inputs(g_m,in,in>>1);
                u32 h=2166136261u;for(u16 w:g_m.ram)h=(h^w)*16777619u;ram.push_back(h);
                u32 a=2166136261u;for(float f:g_pcm){u32 b;std::memcpy(&b,&f,4);a=(a^b)*16777619u;}pcm.push_back(a);
            }
        };
        const auto snapshot=coupled_snapshot();
        std::vector<u32> ram1,pcm1,ram2,pcm2;record(ram1,pcm1);
        if(!load_coupled_snapshot(snapshot))return 13;
        record(ram2,pcm2);
        unsigned ram_bad=0,pcm_bad=0;for(size_t i=0;i<ram1.size();++i){ram_bad+=ram1[i]!=ram2[i];pcm_bad+=pcm1[i]!=pcm2[i];}
        std::printf("drop-in snapshot %zu bytes: replay of 3000 frames differs in RAM on %u frames, audio on %u frames\n",snapshot.size(),ram_bad,pcm_bad);
    }
    std::vector<u8> snap;g_m.save_state(snap,g_sched->main_pc(),false);
    // Equivalence: a main-only runner restored from the coupled snapshot must
    // follow the coupled machine frame for frame (same inputs).
    {
        Machine fork;if(!stv::gsp::load_imported_roms(fork,g_rom_path,error))return 8;
        fork.enhanced=true;fork.set_wide(1024,137);if(!stv::gsp::apply_enhanced(fork,137))return 9;
        stv::net::FrameRunner fr(fork);
        if(!fr.load(snap,0))return 10;
        fork.external_timers=false;fork.check_requested=false;fork.recompute_next_event();
        unsigned first_diff=~0u;
        for(unsigned i=0;i<6000;++i){
            const u32 in=(i/120)%2?0x0110u:0x0028u; // move/fire patterns
            g_pcm.clear();run_one_frame();stv::net::apply_inputs(g_m,in,0);
            fr.step(in,0);
            if(first_diff==~0u && fork.ram!=g_m.ram)first_diff=i;
        }
        std::printf("snapshot fork vs coupled: first RAM difference at frame %d\n",first_diff==~0u?-1:int(first_diff));
    }
    std::printf("frames=%u coupled_fps=%.0f main_only_fps=%.0f snapshot_bytes=%zu\n",N,N/coupled,N/main_only,snap.size());
    std::printf("one hour (196,939 frames) replay: guest %.0f s, host %.0f s\n",196939/(N/coupled)+196939/(N/main_only),196939/(N/main_only));
}
