// Online drop-in/drop-out on the real program: a departing player's character
// dies through the retail death path (last life, DEAD raised on the torso), and
// a joining player enters mid-game with a coin and START, as in the arcade.
#define main drop_fixture_unused_main
#include "test_room_entrances.cpp"
#undef main
#include "stv/host/drop_player.hpp"

namespace {
Machine boot2(const char* rom,bool wide,u32& pc){
    Machine m;std::string err;require(m.load_roms(rom,err),"drop fixture ROM load");
    if(wide){m.enhanced=true;m.set_wide(1024,137);require(apply_enhanced(m,137),"drop fixture wide data");}
    m.reset();pc=m.reset_vector();
    m.on_vblank=[&]{const auto f=m.frame_number;
        if(f==2600||f==2640||f==2680||f==2720)m.ports[1]&=u16(~1);
        if(f==2612||f==2652||f==2692||f==2732)m.ports[1]|=1;
        if(f==3150)m.ports[1]&=u16(~0x24);if(f==3170)m.ports[1]|=0x24;
        if(f>=4300){m.run_until_cycle=m.c.cyc;m.recompute_next_event();}};
    while(m.frame_number<4300&&!m.stop)pc=l8_run(m,pc,m.c.cyc+1000000);
    require(!m.stop&&m.rw(L8_GAME_STATE)==0x10,"drop fixture boot failed");m.on_vblank={};return m;
}
void run_frames(Machine& m,u32& pc,unsigned n){
    const unsigned end=unsigned(m.frame_number)+n;
    m.run_until_cycle=~0ull;m.recompute_next_event();
    m.on_vblank=[&]{const auto t=rlong(m,0x010de2b0);if(t)m.ww(t+0x160,511); // P1 immunity (fixture)
        if(m.frame_number>=end){m.run_until_cycle=m.c.cyc;m.recompute_next_event();}};
    while(m.frame_number<end&&!m.stop)pc=l8_run(m,pc,m.c.cyc+1000000);
    m.on_vblank={};require(!m.stop,"drop fixture stopped");
}
}
int main(int argc,char**argv){try{
    if(argc!=2)return 2;
    for(bool wide:{false,true}){
        u32 pc=0;Machine m=boot2(argv[1],wide,pc);
        require(stv::host::player_active(m,1) && stv::host::player_active(m,0),"both players should be in play");
        const unsigned p1_lives=stv::host::player_lives(m,0);
        // Drop-out: player 2 leaves.
        require(stv::host::drop_player(m,1),"drop_player refused an active player");
        unsigned frames=0;
        for(;frames<900 && stv::host::player_active(m,1);frames+=10)run_frames(m,pc,10);
        require(!stv::host::player_active(m,1),"departed player 2 did not leave play");
        require(stv::host::player_active(m,0) && stv::host::player_lives(m,0)==p1_lives,"player 1 was affected");
        require(m.rw(L8_GAME_STATE)==0x10,"game did not continue after the departure");
        std::printf("%s: player 2 death sequence finished in %u frames; player 1 unaffected\n",wide?"wide":"classic",frames);
        // Drop-in: a new player 2 inserts a coin and presses START.
        for(int i=0;i<4;++i)run_frames(m,pc,30);    // continue countdown may still be running
        unsigned joined=0;
        for(unsigned f=0;f<1800 && !stv::host::player_active(m,1);f+=1){
            const auto pulse=stv::host::join_pulse(f%120);
            m.ports[1]=u16(0xffff & ~(pulse.coin?0x02:0) & ~(pulse.start?0x20:0));
            run_frames(m,pc,1);joined=f;
            if(f%60==0)std::printf("  f=%u state=%x p2lives=%u p2torso=%08x p1lives=%u coin=%d start=%d\n",f,m.rw(L8_GAME_STATE),stv::host::player_lives(m,1),stv::host::player_torso(m,1),stv::host::player_lives(m,0),pulse.coin,pulse.start);
        }
        m.ports[1]=0xffff;
        require(stv::host::player_active(m,1),"coin and START did not bring player 2 back");
        run_frames(m,pc,120);
        require(stv::host::player_active(m,0) && stv::host::player_active(m,1),"players not both in play after drop-in");
        std::printf("%s: new player 2 joined %u frames after the coin\n",wide?"wide":"classic",joined);
    }
    std::puts("PASS: drop-out kills the departed character through the retail death path; drop-in joins with coin+START");
    return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL drop in/out: %s\n",e.what());return 1;}}
