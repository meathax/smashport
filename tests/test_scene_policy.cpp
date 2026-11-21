// Exercise the actual translated retail writers before testing scene policy.
// This guards the RAM binding; source-label alignment alone can be imperfect.
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"
#include <algorithm>
#include <cstdio>

using namespace stv::gsp;
namespace {
int failures = 0;
void check(bool good, const char* message) {
    if (!good) { std::fprintf(stderr, "%s\n", message); ++failures; }
}
// MYOINIT's original DMA fill is 511 columns by two rows at y=510.
// Live widescreen must extend that erase color before the new right wall
// scrolls into view. Test through the real scanline timer, including native
// scenes, both source-row parities, disabled erasure and source preservation.
void check_autoerase() {
    for (bool enhanced : {false,true}) {
        for (u16 state : {u16(0x10),u16(0x18),u16(0x30)}) {
            for (int row : {12,13}) {
                for (bool enabled : {false,true}) {
                    Machine m;
                    m.enhanced = enhanced;
                    if (enhanced) m.set_wide(1024,137);
                    m.reset();
                    m.ww(L8_GAME_STATE,state);
                    std::fill(m.vram.begin(),m.vram.end(),u16(0x6a6a));
                    for (int parity=0;parity<2;++parity) {
                        auto* source = m.vram.data()+std::size_t(510+parity)*m.vram_w;
                        // A distinct pen on each row detects accidental parity loss.
                        std::fill_n(source,511,u16(0x3d3d-parity*0x1111));
                        std::fill(source+511,source+m.vram_w,u16(0x0101+parity));
                    }
                    const auto before = m.vram;
                    m.autoerase_enable = enabled;
                    m.io[DPYCTL] = 0xa400; // enabled, master, direct display address
                    m.io[DPYADR] = u16((row+1)<<4);
                    m.io[HEBLNK] = VIS_MINX/2;
                    m.io[HSBLNK] = (VIS_MAXX+1)/2;
                    m.io[VEBLNK] = VIS_MINY;
                    m.io[VSBLNK] = VIS_MAXY+1;
                    m.io[VTOTAL] = SCREEN_H-1;
                    m.scan_param = VIS_MINY;
                    m.scan_next_ps = u64(VIS_MINY)*PS_PER_LINE;
                    const u64 when = m.scan_next_ps;
                    m.fire_timers_through(when);
                    const bool wide = enhanced && state != 0x30;
                    const std::size_t source = std::size_t(510+(row&1))*m.vram_w;
                    const std::size_t target = std::size_t(row)*m.vram_w;
                    bool contents_ok = true, other_rows_ok = true;
                    for (std::size_t i=0;i<m.vram.size();++i) {
                        if (i>=target && i<target+m.vram_w) {
                            const std::size_t x = i-target;
                            const u16 expected = !enabled ? before[i] :
                                before[source+(wide && x>=511 ? 510 : x)];
                            contents_ok &= m.vram[i] == expected;
                        } else {
                            other_rows_ok &= m.vram[i] == before[i];
                        }
                    }
                    check(contents_ok,"scanline erase left a stale widescreen floor edge or changed native pixels");
                    check(other_rows_ok,"scanline erase changed another row or its original source");
                    check(m.c.cyc == 0 && m.scan_next_ps > when,
                          "widescreen erase changed original CPU timing");
                    check(m.faults.empty(),"scanline erase fixture faulted");
                }
            }
        }
    }
}

struct WriterCase { u32 literal_pc, store_pc; u16 state; bool live; };
constexpr WriterCase writers[] = {
    {0xffe0d910u,0xffe0d930u,0x30,false}, // INIT_SYS / attract
    {0xffe95eb0u,0xffe95ed0u,0x30,false}, // ATSTRT
    {0xffea31d0u,0xffea31f0u,0x10,true},  // GAMESTR / studio
    {0xffe74cc0u,0xffe74ce0u,0x18,true},  // WAVES intro
    {0xffe76c60u,0xffe76c80u,0x18,true},  // WAVES intro, A2 producer
    {0xffe78c70u,0xffe78c90u,0x18,true},  // WAVES room transition
    {0xffe7aea0u,0xffe7aec0u,0x10,true},  // WAVES live gameplay
    {0xffe1c360u,0xffe1c380u,0x50,true},  // ROBO death / continue
    {0xffe91850u,0xffe91870u,0x50,true},  // game over in live world
    {0xffe93e90u,0xffe93eb0u,0x10,true},  // continue accepted
    {0xffe94a50u,0xffe94a70u,0x10,true},  // restart live world
    {0xffebad30u,0xffebad50u,0x40,false}, // operator diagnostics
};
}
int main() {
    check_autoerase();
    for (const bool enhanced : {false,true}) {
        for (const auto& row : writers) {
            Machine m;
            m.enhanced = enhanced;
            if (enhanced) m.set_wide(1024,137);
            m.ww(0x010002a0u,5);m.ww(0x010002b0u,1);
            m.ww(0x010002c0u,0x018b);
            m.ww(0x010002d0u,0x00fc);
            m.external_timers = true;
            m.c.st = 0x10; // original field zero is 16 bits
            m.ww(L8_GAME_STATE,0xffff);
            const u32 pc = l8_run(m,row.literal_pc,1);
            check(pc == row.store_pc,"retail scene literal no longer reaches its bound store");
            const u32 next = l8_run(m,pc,m.c.cyc+1);
            check(next == row.store_pc+48,"retail scene store length/continuation changed");
            check(!m.stop && m.faults.empty(),"retail scene writer faulted");
            check(game_scene_state(m) == row.state,"GAMSTATE binding differs from actual L8 writer");
            check(enhanced_scene(m) == (enhanced && row.live),"native/live scene geometry policy is wrong");
            check(m.rw(0x010002c0u) == (enhanced && row.live ? 0x0218 : 0x018b) &&
                  m.rw(0x010002d0u) == 0x00fc,"scene writer retained the wrong full-screen background bound");
            const bool settled = enhanced && row.live && row.state != 0x18;
            check(m.rw(0x010002a0u) == (settled ? 9 : 5) && m.rw(0x010002b0u) == 1,
                  "settled arena clip starts before its wall or crops the studio");
            check(m.c.cyc == 5,"scene-bound update changed original writer cycle count");
        }
    }
    Machine m;
    // A new world is published after GAMSTATE while the title still owns the
    // previous frame. The actual32-bit BAKMODS writer must refresh the aperture.
    for (bool enhanced:{false,true}) {
        Machine world;world.enhanced=enhanced;world.external_timers=true;world.c.st=0x10;
        if(enhanced)world.set_wide(1024,137);
        world.ww(L8_GAME_STATE,0x18);
        world.ww(0x010002a0u,5);world.ww(0x010002b0u,1);
        world.ww(0x010002c0u,395);world.ww(0x010002d0u,252);
        world.c.a[0]=enhanced?0xff6726b0u:0xffe726b0u;
        check(l8_run(world,0xffe75760u,1)==0xffe75790u,"world publication continuation");
        check(world.rw(0x010002c0u)==(enhanced?536:395),"new studio inherited native-page clip");
        check(world.c.cyc==3&&!world.stop&&world.faults.empty(),"world publication timing/fault");
        world.ww(L8_GAME_STATE,0x18);world.c.a[0]=0xffea54f0u;
        check(l8_run(world,0xffe98530u,world.c.cyc+1)==0xffe98560u,"fixed page publication continuation");
        check(world.rw(0x010002c0u)==395,"fixed page retained world clip");
    }
    m.enhanced = true;
    m.external_timers = true;
    m.c.st = 0x10; // field one remains 32-bit for BAKMODS
    m.ww(L8_GAME_STATE,0x50);
    check(enhanced_scene(m),"game-over arena lost its expanded geometry");
    u32 page_pc = l8_run(m,0xffe97e70u,1);
    check(page_pc == 0xffe97ea0u,"INITIALS literal did not reach BAKMODS store");
    page_pc = l8_run(m,page_pc,m.c.cyc+1);
    check(page_pc == 0xffe97ed0u && scene_background(m) == 0xffea5670u,
          "INITIALS actual retail background binding changed");
    check(game_scene_state(m) == 0x50 && !enhanced_scene(m),
          "initials page inherited widescreen from GAMEOVER");
    for (u32 page : {0xffea54f0u,0xffea5550u,0xffea55b0u,0xffea5610u,0xffea5670u}) {
        m.ww(L8_BACKGROUND_MODULES,u16(page));
        m.ww(L8_BACKGROUND_MODULES+16,u16(page>>16));
        m.ww(L8_GAME_STATE,0x50);
        check(!enhanced_scene(m),"original page widened during GAMEOVER");
        for (u16 state : {u16(0x10),u16(0x18)}) {
            m.ww(L8_GAME_STATE,state);
            check(!enhanced_scene(m),"fixed page inherited live-world geometry");
            for (u32 root_pc : {0xffe6e7d0u,0xffe75720u}) {
                m.c.a[0]=0;
                const u32 next=l8_run(m,root_pc,m.c.cyc+1);
                check(next==root_pc+48 && m.c.a[0]==enhanced_data_address(0xffe72650u),
                      "fixed-page exit blocked the translated enhanced world root");
            }
        }
    }
    // Production ending writer: unlike initials, GRATS inherits INGAME (10h).
    for (bool enhanced : {false,true}) {
        Machine end;end.enhanced=enhanced;end.external_timers=true;end.c.st=0x10;
        end.ww(L8_GAME_STATE,0x10);
        check(l8_run(end,0xffe98500u,1)==0xffe98530u,"GRATS literal continuation");
        check(l8_run(end,0xffe98530u,end.c.cyc+1)==0xffe98560u,"GRATS store continuation");
        check(scene_background(end)==0xffea54f0u&&!enhanced_scene(end),"GRATS retained expanded scene");
        check(l8_run(end,0xffe98590u,end.c.cyc+1)==0xffe985c0u && end.c.a[0]==0x00fc018bu,
              "GRATS full-page clip widened beyond original artwork");
        check(!end.stop&&end.faults.empty()&&end.c.cyc==9,"GRATS producer timing/fault");
    }
    for (u16 state : {u16(0),u16(0x20),u16(0x60),u16(0xffff)}) {
        m.ww(L8_GAME_STATE,state);
        check(!enhanced_scene(m),"unknown/unbound state enabled widened geometry");
    }
    m.set_wide(1024,137);
    m.ww(L8_BACKGROUND_MODULES,0);m.ww(L8_BACKGROUND_MODULES+16,0);
    m.ww(L8_GAME_STATE,0x18);
    m.ww(0x010002a0u,5);m.ww(0x010002b0u,1);
    m.ww(0x010002c0u,0x018b);m.ww(0x010002d0u,0x00fc);
    sync_scene_bounds(m);
    check(m.rw(0x010002c0u) == 0x0218,"live full-screen bound did not expand");
    check(m.rw(0x010002a0u) == 5,"studio/scrolling clip removed original border art");
    for (u16 state : {u16(0x10),u16(0x50)}) {
        m.ww(L8_GAME_STATE,state);sync_scene_bounds(m);
        check(m.rw(0x010002a0u) == 9 && m.rw(0x010002c0u) == 0x0218,
              "settled arena clip starts before its wall or crops the studio");
    }
    m.ww(L8_GAME_STATE,0x18);sync_scene_bounds(m);
    check(m.rw(0x010002a0u) == 5 && m.rw(0x010002c0u) == 0x0218,
          "room transition did not restore the full studio/scrolling aperture");
    m.ww(L8_GAME_STATE,0x30);sync_scene_bounds(m);
    check(m.rw(0x010002a0u) == 5 && m.rw(0x010002c0u) == 0x018b,
          "attract full-screen bounds did not restore");
    m.ww(0x010002c0u,0x0123);m.ww(L8_GAME_STATE,0x18);sync_scene_bounds(m);
    check(m.rw(0x010002c0u) == 0x0123,"scene transition changed a custom background window");
    m.ww(0x010002a0u,20);m.ww(0x010002c0u,0x018b);sync_scene_bounds(m);
    check(m.rw(0x010002c0u) == 0x018b,"custom top-left with a full lower-right was widened");
    m.frame_vis_extra = 0;
    m.ww(L8_GAME_STATE,0x10);
    check(scene_view_width(m) == 410,"current live state changed completed native-frame width");
    m.frame_vis_extra = 137;
    m.ww(L8_GAME_STATE,0x30);
    check(scene_view_width(m) == 547,"attract transition changed completed wide-frame width");
    if (failures) return 1;
    std::puts("Retail scene writers, reference/native geometry policy and completed-frame width pass");
    return 0;
}
