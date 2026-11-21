// Exact L8 geometry regressions. Fixtures are source/retail constants, not a
// second copy of the remap implementation. Execute the generated instruction in
// both profiles so a missing/partial emitter patch cannot pass a data-only test.
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"

#include <array>
#include <cstdio>
#include <string>

using namespace stv::gsp;
namespace {
struct Case {
    const char* source;
    u32 pc, reference, enhanced;
    int reg;
    bool compare;
};
constexpr Case cases[] = {
    {"retail CKONSCRN lower bound",0xffe7bec0u,0x001e001au,0x001e001eu,2,false},
    {"retail CKONSCRN upper bound",0xffe7bef0u,0x00fc0176u,0x00fc0203u,3,false},
    {"COBRA.ASM:179", 0xfff39610u, 0x00300000u, 0x00340000u, 3, true},
    {"COBRA.ASM:181", 0xfff39650u, 0x01600000u, 0x01ed0000u, 3, true},
    {"COBRA.ASM:221", 0xfff39a60u, 0x00300000u, 0x00340000u, 3, true},
    {"COBRA.ASM:226", 0xfff39ab0u, 0x01600000u, 0x01ed0000u, 3, true},
    {"COBRA.ASM:1615",0xfff42840u, 0x00300000u, 0x00340000u, 3, true},
    {"COBRA.ASM:1620",0xfff428a0u, 0x01600000u, 0x01ed0000u, 3, true},
    {"RBOSS.ASM:967",0xfff22420u, 0x002e002au, 0x002e002eu, 2, false},
    {"RBOSS.ASM:968",0xfff22450u, 0x00980116u, 0x009801a3u, 3, false},
    {"TANK.ASM:1902",0xffee7fb0u, 0x00a50000u, 0x00ed0000u, 0, true},
    {"TANK.ASM:1904",0xffee7ff0u, 0x00ed0000u, 0x01350000u, 0, true},
    {"SHOTS.ASM:1729",0xffe37880u,0x00780000u, 0x00c00000u, 0, true},
    {"SHOTS.ASM:1731",0xffe378c0u,0x011a0000u, 0x01620000u, 0, true},
    {"SHOTS.ASM:1738",0xffe37960u,0x00a50000u, 0x00ed0000u, 0, true},
    {"SHOTS.ASM:1740",0xffe379a0u,0x00ed0000u, 0x01350000u, 0, true},
};
constexpr u32 red_x = 0xffe4cfc0u, red_y = 0xffe4d060u;
constexpr std::array<u16,10> reference_x{248,0,512,152,248,0,248,136,248,136};
constexpr std::array<u16,10> enhanced_x{389,4,653,156,389,4,389,140,389,140};
int failures = 0;
void check(bool ok, const char* what, const char* source) {
    if (!ok) { std::fprintf(stderr, "%s: %s\n", source, what); ++failures; }
}
struct Result { u32 flags, value; u64 cycles; };
Result one(const Case& row, bool wide, s32 delta) {
    Machine m;
    m.enhanced = wide;
    m.ww(L8_GAME_STATE, 0x10);
    m.external_timers = true;
    const u32 expected = wide ? row.enhanced : row.reference;
    m.c.a[row.reg] = row.compare ? expected + u32(delta) : 0xdeadbeefu;
    const u32 next = l8_run(m, row.pc, 1);
    check(m.faults.empty() && !m.stop, "single translated instruction faulted", row.source);
    check(next == row.pc + 48, "instruction length/continuation changed", row.source);
    if (row.compare) {
        check(Z(m.c) == (delta == 0), "comparison did not use the profile's doorway/wall bound", row.source);
        check(m.c.a[row.reg] == expected + u32(delta), "comparison changed its operand", row.source);
    } else {
        check(m.c.a[row.reg] == expected, "packed X bound or unchanged Y is wrong", row.source);
        check((m.c.a[row.reg] >> 16) == (row.reference >> 16), "Y geometry changed", row.source);
    }
    return {m.c.st & (ST_N|ST_Z|ST_V|ST_C), m.c.a[row.reg], m.c.cyc};
}
std::size_t index(u32 address) { return (address - 0xff800000u) >> 4; }
void cobra_spawns() {
    constexpr u32 entries[]={0xfff38bc0u,0xfff38d50u};
    constexpr u32 original[]={553u,738u}, widened[]={694u,1016u};
    // Full world-origin load plus ADDI, including room-transition scene 18h.
    // Values are bound to the observed adjacent-room pan and floor positions,
    // not a uniform centring offset applied to both heads.
    for (bool wide:{false,true})
        for (u16 state:{u16(0),u16(0x10),u16(0x18),u16(0x30),u16(0x40),u16(0x50)})
            for (unsigned head=0;head<2;++head)
                for (u32 camera:{0u,0xfff00000u,0x07690000u,0x09001234u}) {
                    Machine m;m.enhanced=wide;m.external_timers=true;m.c.st=0x830;
                    m.ww(L8_GAME_STATE,state);wlong(m,0x01000240u,camera);
                    m.c.a[0]=0xdeadbeefu;m.c.a[11]=0xface1234u;
                    const bool live=wide && (state==0x10 || state==0x18 || state==0x50);
                    const u32 expected=camera+((live?widened[head]:original[head])<<16);
                    u32 pc=entries[head];
                    for (unsigned n=0;n<2;++n)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==entries[head]+96 && !m.stop && m.faults.empty(),
                          "spawn prefix continuation/fault differs","COBRA spawn");
                    check(m.c.a[10]==expected,"spawn X does not follow target room geometry","COBRA spawn");
                    check(m.c.a[0]==0xdeadbeefu && m.c.a[11]==0xface1234u,
                          "spawn prefix changed unrelated register/Y","COBRA spawn");
                    check(m.c.cyc==8,"spawn prefix guest timing changed","COBRA spawn");
                }
}

void widescreen_pickups() {
    for (bool wide : {false,true}) for (u16 state : {u16(0),u16(0x10)}) {
        Machine m;
        m.enhanced=wide;
        m.external_timers=true;
        m.ww(L8_GAME_STATE,state);
        const bool live=wide && state==0x10;
        auto run=[&](u32 pc) { l8_run(m,pc,m.c.cyc+1); };
        run(0xffe35f90u);
        run(0xffe35fb0u);
        check(m.c.b[0]==(live?7:5) && m.c.b[1]==(live?24:18),"bubble flight bounds","pickup");
        run(0xffed6a50u);
        check(m.c.a[0]==(live?405:348),"boss collection window","pickup");
        run(0xffed6b30u);
        check(m.c.a[0]==(live?222:186),"arena collection window","pickup");
        m.c.a[13]=0x01086790u;
        for (unsigned hits : {0u,1u,5u,7u}) {
            m.ww(m.c.a[13]+0x1a0,hits);
            for (u32 pc : {0xffed2b90u,0xffed2c20u}) {
                run(pc);
                check(m.c.a[0]==(live && hits?7:5),"blade contact sweep radius","pickup");
            }
            m.c.a[1]=hits;
            run(0xffed3920u);
            check(Z(m.c)==(hits==(live?7u:5u)),"blade retirement threshold","pickup");
        }
        check(!m.stop && m.faults.empty(),"unexpected fault","pickup");
    }
}

void cobra_finishing_hit() {
    // Start after the wound increment in COBBULHD. Difficulty supplies
    // COBRAKIL; the retail branch subtracts 250 for either solo player.
    struct Room { unsigned circuit, wave, setting; bool tuned; };
    constexpr Room rooms[] = {{2,7,2900,true}, {2,7,900,true}, {2,7,4900,true},
                              {2,6,2900,false}, {1,7,2900,false}};
    for (const auto& room : rooms) {
        for (bool enhanced : {false, true}) {
            for (unsigned status : {1u, 2u, 3u}) {
                const unsigned solo = room.setting - 250;
                const unsigned threshold = status == 3 ? (enhanced && room.tuned ? room.setting*7/8 : room.setting) :
                    (enhanced && room.tuned ? solo * 4 / 5 : solo);
                for (unsigned wounds : {threshold - 1, threshold, threshold + 1}) {
                    Machine m;
                    m.enhanced = enhanced;
                    m.external_timers = true;
                    m.c.st = 0x830;
                    m.ww(L8_GAME_STATE, 0x10);
                    m.ww(0x010eb610u, room.circuit);
                    m.ww(0x010eb630u, room.wave);
                    m.ww(0x010de340u, room.setting);
                    m.ww(0x010ee600u, status);
                    m.c.a[2] = wounds;
                    u32 pc = 0xfff3cfd0u;
                    for (unsigned step = 0; step < 16 && pc != 0xfff3d1b0u &&
                         pc != 0xfff3d130u && !m.stop; ++step)
                        pc = l8_run(m, pc, m.c.cyc + 1);
                    const bool killed = wounds >= threshold;
                    check(pc == (killed ? 0xfff3d130u : 0xfff3d1b0u) &&
                          m.rw(0x010f49f0u) == unsigned(killed) &&
                          rb(m, 0x010dd7d0u) == unsigned(killed) && m.faults.empty(),
                          "threshold branch or end flags differ", "COBBULHD");
                }
            }
        }
    }
}
void rboss_spawn() {
    // Adjacent room pitch adds 137; right floor placement adds 141.
    // Explicit retail entry: source alignment does not label this prefix.
    for (bool wide:{false,true})
        for (u16 state:{u16(0),u16(0x10),u16(0x18),u16(0x30),u16(0x40),u16(0x50)})
            for (u32 camera:{0u,0xfff00000u,0x07690000u,0x09031234u}) {
                Machine m;m.enhanced=wide;m.external_timers=true;m.c.st=0x830;
                m.ww(L8_GAME_STATE,state);wlong(m,0x01000240u,camera);
                wlong(m,0x01000260u,0x029e5678u);m.c.a[11]=0xface1234u;
                const bool live=wide && (state==0x10 || state==0x18 || state==0x50);
                u32 pc=0xfff1d3a0u;
                for(unsigned n=0;n<4;++n)pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==0xfff1d460u && !m.stop && m.faults.empty(),
                      "spawn prefix continuation/fault differs","RBOSS spawn");
                check(m.c.a[0]==camera+((live?1014u:736u)<<16),
                      "spawn X does not follow right floor in next room","RBOSS spawn");
                check(m.c.a[1]==0x031e5678u && m.c.a[11]==0xface1234u,
                      "spawn Y/fraction or unrelated register changed","RBOSS spawn");
                check(m.c.cyc==16,"spawn prefix guest timing changed","RBOSS spawn");
            }
}
void rboss_movement() {
    constexpr u32 entry[]={0xfff1e240u,0xfff1efa0u};
    constexpr u32 end[]={0xfff1e660u,0xfff1f3c0u};
    u64 baseline[2][7][5]{};
    for(bool wide:{false,true})
        for(u16 state:{u16(0),u16(0x10),u16(0x18),u16(0x30),u16(0x40),u16(0x50)})
            for(unsigned path=0;path<2;++path) {
                const bool live=wide&&(state==0x10||state==0x18||state==0x50);
                const u32 lo=(live?92u:88u)<<16,hi=(live?461u:320u)<<16;
                const u32 xs[]={lo-0x8000,lo,lo+0x8000,(lo+hi)/2,hi-0x8000,hi,hi+0x8000};
                const s32 vs[]={-0x18000,-1,0,1,0x18000};
                for(unsigned xi=0;xi<7;++xi)for(unsigned vi=0;vi<5;++vi) {
                    Machine m;m.enhanced=wide;m.external_timers=true;m.c.st=0x830;
                    m.ww(L8_GAME_STATE,state);m.c.sp=0x010fffe0u;
                    m.c.a[0]=u32(vs[vi]);m.c.a[1]=0x1234u;
                    m.c.a[2]=90u<<16;m.c.a[3]=xs[xi];m.c.a[8]=0xdeadbeefu;
                    s32 expected=vs[vi];
                    if(xs[xi]<lo)expected=s32(lo-xs[xi]);
                    else if(xs[xi]>hi)expected=s32(hi-xs[xi]);
                    if(s64(xs[xi])+expected<lo || s64(xs[xi])+expected>hi)expected=0;
                    u32 pc=entry[path];unsigned steps=0;
                    while(pc!=end[path]&&!m.stop&&steps++<80)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==end[path]&&!m.stop&&m.faults.empty(),"movement boundary/fault differs","RBOSS movement");
                    check(rf(m,0x010f3200u,1)==u32(expected),"X correction/edge velocity differs","RBOSS movement");
                    check(rf(m,0x010f3220u,1)==0x1234u,"Y velocity changed","RBOSS movement");
                    check(m.c.sp==0x010fffe0u&&m.c.a[8]==0xdeadbeefu,"stack/unrelated register changed","RBOSS movement");
                    if(!wide&&state==0)baseline[path][xi][vi]=m.c.cyc;
                    check(m.c.cyc==baseline[path][xi][vi],"equivalent edge path timing differs","RBOSS movement");
                }
            }
}
void opening_caps(const Machine& reference, const Machine& wide) {
    constexpr std::array<int,12> caps{2,12,11,25,32,20,20,25,29,24,32,32};
    for (const u16 scene : {u16(0x10),u16(0x18),u16(0x30)}) {
        for (unsigned wave=0; wave<caps.size(); ++wave) {
            for (const int modifier : {-4,-1,6}) {
                std::array<u64,2> cycles{};
                for (unsigned mode=0; mode<2; ++mode) {
                    Machine m=mode ? wide : reference;
                    m.external_timers=true;
                    m.ww(L8_GAME_STATE,scene);
                    m.ww(0x010eb630u,u16(wave));
                    m.ww(0x010de310u,u16(modifier));
                    m.c.sp=0x010fffe0u;
                    u32 pc=0xffe129c0u;
                    unsigned steps=0;
                    while (pc!=0xffe12a90u && !m.stop && ++steps<20)
                        pc=l8_run(m,pc,m.c.cyc+1);
                    if (pc!=0xffe12a90u && wave==0 && modifier==-4 && scene==0x10) {
                        std::fprintf(stderr,"cap mode=%u pc=%08x sp=%08x steps=%u\n",mode,pc,m.c.sp,steps);
                        for (const auto& fault:m.faults)
                            std::fprintf(stderr,"%s at %08x\n",fault.what.c_str(),fault.addr);
                    }
                    check(pc==0xffe12a90u && m.faults.empty(),
                          "translated cap consumer failed", "HMAX");
                    check((m.ram[(0x010de560u-0x01000000u)>>4]&255u)==
                          unsigned(((mode && scene!=0x30 && caps[wave]>2 ? (3*caps[wave]+1)/2 : caps[wave])+modifier)&255),
                          "wrong scene/wave cap or signed difficulty", "HMAX");
                    cycles[mode]=m.c.cyc;
                }
                check(cycles[0]==cycles[1], "cap adjustment changed guest timing", "HMAX");
            }
        }
    }
}
void instructions() {
    for (const Case& row : cases) {
        for (const s32 delta : {-65536,0,65536}) {
            if (!row.compare && delta != 0) continue;
            const Result ref = one(row,false,delta), enh = one(row,true,delta);
            check(ref.cycles == enh.cycles, "profile change altered instruction cycle cost", row.source);
            if (row.compare)
                check(ref.flags == enh.flags, "same relative position changed comparison semantics", row.source);
        }
    }
    // The Mutoid opening is a wider version of the same centre doorway. Its
    // original 45-pixel margins are local widths, not separate arena anchors.
    check(cases[12].enhanced + (45u << 16) == cases[14].enhanced &&
          cases[13].enhanced - (45u << 16) == cases[15].enhanced,
          "Mutoid opening lost its original +/-45-pixel margins", "SHOTS doorway");
}

void player_rescue() {
    constexpr u32 process=0x01086790u,torso=0x010003a0u,legs=torso+0x1f0u;
    for (const u16 scene : {u16(0x10),u16(0x18),u16(0x30)}) {
        for (const int x : {100,450,560}) {
            for (const bool wide : {false,true}) {
                Machine m;
                m.enhanced=wide;
                m.external_timers=true;
                m.ww(L8_GAME_STATE,scene);
                m.c.sp=0x010fffe0u;
                m.c.a[1]=process;
                wfield(m,process+0x100u,torso,32);
                wfield(m,process+0x300u,legs,32);
                for (const u32 object : {torso,legs}) {
                    wfield(m,object+0x60u,u32(x)<<16,32);
                    wfield(m,object+0x80u,100u<<16,32);
                    wfield(m,object+0xf0u,0x00100010u,32);
                }
                u32 pc=0xffe7be80u;
                unsigned steps=0;
                while (pc!=0xffe7c080u && !m.stop && ++steps<100)
                    pc=l8_run(m,pc,m.c.cyc+1);
                const bool live=wide && scene!=0x30;
                const bool rescued=x==560 || (x==450 && !live);
                const int expected_x=rescued ? (live ? 272 : 200) : x;
                const int expected_y=rescued ? 128 : 100;
                check(pc==0xffe7c080u && m.faults.empty(),
                      "translated rescue path did not finish", "CKONSCRN");
                for (const u32 object : {torso,legs}) {
                    check(m.rw(object+0x70u)==expected_x,
                          "legal widened position rescued or rescue X not centred", "CKONSCRN");
                    check(m.rw(object+0x90u)==expected_y,
                          "torso/legs rescue Y changed unexpectedly", "CKONSCRN");
                }
            }
        }
    }
}

void gang_doors(const Machine& reference, const Machine& wide) {
    // Accepted HULKS.ASM:163 CREATE selects this retail entry. Its 29-instruction
    // source-matched prefix consumes GXL/GXR/GYT/GYB before calling RANGRAND.
    constexpr u32 entry=0xffe3beb0u, first_random_call=0xffe3c0e0u;
    constexpr std::array<s32,4> left_ref{-64,400,173,173}, right_ref{-1,464,221,221};
    constexpr std::array<s32,4> left_wide{-60,541,245,245}, right_wide{3,605,293,293};
    constexpr std::array<u16,4> directions{3,7,5,1};
    Machine producer;
    producer.external_timers=true;
    check(l8_run(producer,0xffe3b520u,1)==0xffe3b550u && producer.c.a[7]==entry &&
          producer.faults.empty(), "accepted CREATE no longer selects the bound GHULK entry", "HULKS.ASM:163");
    for (std::size_t door=0;door<4;++door) {
        std::array<u64,2> cycles{};
        for (std::size_t mode=0;mode<2;++mode) {
            Machine m=mode ? wide : reference;
            m.external_timers=true;
            m.c.a[8]=u32(door);
            m.c.a[13]=0x01010000u;
            u32 pc=entry;
            unsigned steps=0;
            while (pc!=first_random_call && !m.stop && ++steps<64)
                pc=l8_run(m,pc,m.c.cyc+1);
            check(pc==first_random_call && m.faults.empty() && !m.stop,
                  "real gang prefix did not reach the X random-range call", "GHULK");
            const u32 left=u32(mode ? left_wide[door] : left_ref[door])<<16;
            const u32 right=u32(mode ? right_wide[door] : right_ref[door])<<16;
            check(m.c.a[2]==left && m.c.a[3]==right && m.c.b[0]==left && m.c.b[1]==right,
                  "door index, signed fixed-point bounds or X-table stride changed", "GHULK");
            const u32 yt=u32(reference.prog[index(0xffe3bcd0u)+door*2]) |
                         (u32(reference.prog[index(0xffe3bcd0u)+door*2+1])<<16);
            const u32 yb=u32(reference.prog[index(0xffe3bd50u)+door*2]) |
                         (u32(reference.prog[index(0xffe3bd50u)+door*2+1])<<16);
            check(m.c.a[4]==yt && m.c.a[5]==yb, "gang Y ranges changed", "GHULK");
            check(m.c.a[8]==door*32 && m.rw(0x01010110u)==directions[door] &&
                  m.rw(0x01010120u)==30, "door/direction/process fields changed", "GHULK");
            check(right-left==(u32(right_ref[door])<<16)-(u32(left_ref[door])<<16),
                  "gang random interval width changed", "GHULK");
            cycles[mode]=m.c.cyc;
        }
        check(cycles[0]==cycles[1], "gang prefix timing changed", "GHULK");
    }
    // The remap changes only each fixed-point integer half, even when the
    // generic table mechanism is exercised with a nonzero fraction.
    Machine fractional=reference;
    fractional.prog[index(0xffe3bbd0u)]=0x1234u;
    check(apply_enhanced(fractional,137) && fractional.enhanced_data[index(0xffe3bbd0u)]==0x1234u &&
          fractional.enhanced_data[index(0xffe3bbd0u)+1]==u16(-60) &&
          fractional.prog[index(0xffe3bbd0u)+1]==u16(-64),
          "signed fixed-point remap changed fractional bits", "GHULK GXL");
}

void native_dma_clipping() {
    struct Edge { int x, width, first, count; u16 command; };
    constexpr Edge edges[] = {
        {510,8,510,2,0x800c}, {509,8,509,3,0x801c},
        {-3,8,0,5,0x800c}, {520,8,0,0,0x800c},
    };
    auto draw = [](Machine& m, const Edge& e) {
        constexpr u32 dma_base=0x01a00000u;
        m.ww(dma_base+DMA_XSTART*16,u16(e.x));
        m.ww(dma_base+DMA_YSTART*16,5);
        m.ww(dma_base+DMA_WIDTH*16,u16(e.width));
        m.ww(dma_base+DMA_HEIGHT*16,1);
        m.ww(dma_base+DMA_PALETTE*16,2);
        m.ww(dma_base+DMA_COLOR*16,7);
        m.ww(dma_base+DMA_COMMAND*16,e.command);
    };
    for (const Edge& edge:edges) {
        Machine reference;
        draw(reference,edge);
        check(reference.dma_done_ps==u64(edge.count)*41000,
              "reference edge fixture has wrong original completion time", "DMA native clip");
        for (u16 scene:{u16(0),u16(0x30),u16(0x40)}) {
            Machine wide;
            wide.enhanced=true;
            wide.set_wide(1024,137);
            wide.ww(L8_GAME_STATE,scene);
            draw(wide,edge);
            check(wide.dma_done_ps==reference.dma_done_ps,
                  "native scene changed DMA completion time", "DMA native clip");
            for (int x=0;x<1024;++x) {
                const u16 expected=x>=edge.first && x<edge.first+edge.count ? 0x0207 : 0;
                check(wide.vram[5*wide.vram_w+u32(x)]==expected,
                      "native scene drew outside the original DMA clip", "DMA native clip");
            }
        }
    }
    Machine live;
    live.enhanced=true;
    live.set_wide(1024,137);
    live.ww(L8_GAME_STATE,0x10);
    draw(live,edges[0]);
    check(live.dma_done_ps==8*41000 && live.vram[5*live.vram_w+517]==0x0207,
          "live gameplay lost the widened DMA domain", "DMA live clip");
}

// Enter at a retail CALLA BEGINOBJ2 with its descriptor in registers. Read
// back the allocated sprite through the same anchor getter collision uses.
void check_spawned_sprite(Machine& m,u32 call,u32 resume,u32 wx,u32 wy,u16 oid) {
    constexpr u32 obj=0x010003a0u, owner=0x01086790u;
    const u32 image=m.c.a[2], stack=m.c.sp;
    wlong(m,0x010853f0u,rlong(m,image+0x60)); // resident image palette
    wlong(m,0x01000040u,0);wlong(m,0x01000060u,obj);wlong(m,obj,0);
    m.c.a[13]=owner;
    auto run=[&](u32 pc) {
        for(unsigned n=0;n<500 && pc!=resume && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
        return pc==resume && !m.stop && m.faults.empty();
    };
    check(run(call) && m.c.a[8]==obj && rlong(m,0x01000040u)==obj &&
          rlong(m,obj+0x160)==owner && rlong(m,obj+0x130)==image && m.rw(obj+0x150)==oid,
          "spawned image, owner or list insertion differs","BEGINOBJ2");
    push(m,m.c,resume);
    check(run(0xffe04c20u) && m.c.a[3]==wx && m.c.a[2]==wy && m.c.sp==stack,
          "stored sprite anchor differs","GETANIXY");
}

void room_mine(Machine& m,u32 origin,unsigned door) {
    constexpr u32 positions=0x010dd8b0u;
    const unsigned lo=m.enhanced?46:42, hi=m.enhanced?484:343;
    wfield(m,0x010ee988u,door,16);
    wlong(m,positions,(33u<<16)|lo); // Existing shape at the second draw.
    wlong(m,positions+32,0);
    wlong(m,0x010dd7f0u,positions+32);
    constexpr u32 samples[]={0x80000000u,0x80000000u,0,0,0xffffffffu,0xffffffffu};
    unsigned draws=0;
    m.c.sp=0x010fffe0u;m.c.st=0x830;
    u32 pc=0xfff14080u;
    for(unsigned n=0;n<1000 && pc!=0xfff141f0u && !m.stop;++n) {
        if(pc==0xffe06cf0u) {
            if(draws==6)break;
            m.c.a[0]=samples[draws++];
        }
        pc=l8_run(m,pc,m.c.cyc+1);
    }
    const u32 wx=(u16(origin)+hi)<<16, wy=((origin>>16)+193)<<16;
    check(draws==6 && pc==0xfff141f0u && !m.stop && m.faults.empty() &&
          rlong(m,positions+32)==((193u<<16)|hi) && rlong(m,0x010dd7f0u)==positions+64 &&
          m.c.a[0]==wx && m.c.a[1]==wy,"carpet or occupied-position retry differs","AMINE");
    if(pc==0xfff141f0u)check_spawned_sprite(m,pc,0xfff14220u,wx,wy,0x1700u);
}

void room_spawns(Machine& m) {
    // Drive the normalized random variates at RANDU's multiply, after RANDOM
    // has returned. Keep the range calculation, stack and caller code live.
    constexpr u32 draws[][4] = {
        {0,0,0,0}, {0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu},
        {0x40000000u,0xc0000000u,0x80000000u,0x20000000u}
    };
    const auto scaled=[](u32 draw, unsigned lo, unsigned hi) {
        return lo + 1 + unsigned((u64(draw) * (hi-lo)) >> 32);
    };
    m.external_timers=true;
    m.ww(L8_GAME_STATE,0x10);
    const u32 root=m.enhanced?enhanced_data_address(0xffe8c7f0u):0xffe8c7f0u;
    for(unsigned circuit=0;circuit<3;++circuit) {
        m.ww(0x010eb610u,circuit);
        const u32 table=rlong(m,root+circuit*32);
        for(unsigned wave=1;wave<=(circuit==0?10u:circuit==1?18u:22u);++wave) {
            if(circuit && wave==10)continue; // unused selector
            const u32 origin=rlong(m,table+wave*32);
            wlong(m,0x010ee080u,origin);
            m.ww(0x010eb630u,wave);
            for(unsigned door=0;door<=4;++door)room_mine(m,origin,door);
            for(const auto& sample:draws) {
                m.c.st=0x830;m.c.sp=0x010fffe0u;
                unsigned consumed=0;
                auto run=[&](u32 pc,u32 end) {
                    for(unsigned n=0;n<500 && pc!=end && !m.stop;++n) {
                        if(pc==0xffe06cf0u && consumed<4)m.c.a[0]=sample[consumed++];
                        pc=l8_run(m,pc,m.c.cyc+1);
                    }
                    return pc==end && !m.stop && m.faults.empty();
                };
                check(run(0xfff13c40u,0xfff13d60u),"cluster origin did not return","SENDR");
                const unsigned x=scaled(sample[0],m.enhanced?39:35,m.enhanced?357:216);
                const unsigned y=scaled(sample[1],45,164);
                check(m.c.a[10]==x && m.c.a[11]==y && m.c.a[9]==40 && consumed==2,
                      "cash origin or item count differs","SENDR");
                // ANITEM inherits A9..A11 through CREATE. Continue through
                // the item descriptor setup to the actual BEGINOBJ2 call.
                check(run(0xfff143b0u,0xfff14600u),"item setup did not return","ANITEM");
                const u32 wx=(u16(origin)+x+scaled(sample[3],0,116))<<16;
                const u32 wy=((origin>>16)+y+scaled(sample[2],0,76))<<16;
                check(consumed==4 && m.c.a[0]==wx && m.c.a[1]==wy &&
                      m.c.a[5]==0x0c0fu && m.c.sp==0x010fffe0u,
                      "world position, reward type or stack differs","ANITEM");
                check_spawned_sprite(m,0xfff14600u,0xfff14630u,wx,wy,0x0c0f);
            }
        }
    }
}

void carpet(const char* zip) {
    Machine reference;
    std::string error;
    if (!reference.load_roms(zip,error)) {
        std::fprintf(stderr,"ROM fixture: %s\n",error.c_str()); ++failures; return;
    }
    for (std::size_t i=0; i<reference_x.size(); ++i)
        check(reference.prog[index(red_x)+i] == reference_x[i], "ROM/source bound mismatch", "RED_CARPETX fixture");
    std::array<u16,10> original_y{};
    for (std::size_t i=0; i<original_y.size(); ++i) original_y[i]=reference.prog[index(red_y)+i];
    Machine wide = reference;
    wide.enhanced = true;
    wide.ww(L8_GAME_STATE, 0x10);
    const bool prepared = apply_enhanced(wide,137);
    check(prepared, "valid table rejected", "RED_CARPETX");
    check(wide.faults.empty() && !wide.stop, "valid retail table failed enhanced patch validation", "RED_CARPETX");
    if (!prepared) {
        for (const auto& fault : wide.faults)
            std::fprintf(stderr, "%s at %08x\n", fault.what.c_str(), fault.addr);
        return; // no native data image exists after a rejected transaction
    }
    check(wide.prog == reference.prog, "enhancement modified imported program ROM", "immutable ROM");
    // Run SETMAX's circuit-three robot lookup through both retail adjustments
    // and the MAXRBTA store. STATUS 1 and 2 cover either single-player slot.
    for (bool enhanced : {false, true}) {
        Machine m = enhanced ? wide : reference;
        m.external_timers = true;
        m.ww(L8_GAME_STATE, 0x10);
        for (unsigned wave : {15u, 16u, 17u}) {
            for (unsigned status : {1u, 2u, 3u}) {
                for (int difficulty : {-2, 0, 2}) {
                    m.c.st = 0x830;
                    m.c.sp = 0x010fffe0u;
                    m.c.a[0] = wave * 8;
                    m.ww(0x010ee600u, status);
                    m.ww(0x010de310u, u16(difficulty));
                    const int retail = rb(m, 0xffe13018u + wave * 8);
                    const int base = enhanced ? 2*retail : retail;
                    u32 pc = 0xffe11e10u;
                    for (unsigned step = 0; step < 25 && pc != 0xffe11ed0u && !m.stop; ++step)
                        pc = l8_run(m, pc, m.c.cyc + 1);
                    check(pc == 0xffe11ed0u && m.c.sp == 0x010fffe0u && m.faults.empty() &&
                          rb(m, 0x010f4570u) == u8(base - (status == 3 ? 0 : 5) + difficulty),
                          "robot limit, difficulty offset or return changed", "SETMAX");
                }
            }
        }
    }
    // Exercise the actual initial-placement producers in every scene policy.
    for (bool enabled : {false,true})
        for (u16 state : {u16(0),u16(0x10),u16(0x18),u16(0x30),u16(0x40),u16(0x50)}) {
            Machine m=wide; m.enhanced=enabled; m.external_timers=true;
            m.ww(L8_GAME_STATE,state);
            const bool live=enabled&&(state==0x10||state==0x18||state==0x50);
            for (unsigned i=0;i<2;++i) {
                const u32 pc=i?0xffe41d30u:0xffe41d00u;
                const u64 start=m.c.cyc;
                const u32 next=l8_run(m,pc,start+1);
                check(next==pc+48 && m.c.cyc-start==3,"random bound timing/continuation","initial placement");
                check(m.c.b[i]==((i?(live?484u:343u):(live?46u:42u))<<16),
                      "random bound scene isolation","initial placement");
            }
            for (unsigned i=0;i<2;++i) {
                const u32 pc=i?0xfff1ad10u:0xffe41b10u;
                const u32 root=i?0xfff1ab80u:0xffe417c0u;
                const u64 start=m.c.cyc;
                check(l8_run(m,pc,start+1)==pc+48,"root continuation","initial placement");
                check(m.c.a[i?0:1]==(live?enhanced_data_address(root):root),
                      "typed root scene isolation","initial placement");
            }
            for (unsigned i=0;i<2;++i) {
                const u32 pc=i?0xfff13c80u:0xfff13c60u;
                const u64 start=m.c.cyc;
                check(l8_run(m,pc,start+1)==pc+32 && m.c.cyc-start==2,
                      "cash origin timing/continuation","initial placement");
                check(m.c.b[i]==(i?(live?357u:216u):(live?39u:35u)),
                      "cash origin scene isolation","initial placement");
            }
            check(!m.stop&&m.faults.empty(),"translated producer faulted","initial placement");
        }
    constexpr u32 formation_roots[]={0xffe4d100u,0xffe4d100u,0xffe4d500u,0xffe4d100u,
        0xffe4d100u,0xffe4d300u,0xffe4d100u,0xffe4d500u,0xffe4d100u,0xffe4d100u,0xffe4d500u};
    for (unsigned i=0;i<11;++i)
        check(rlong(wide,enhanced_data_address(0xffe417c0u+i*32))==enhanced_data_address(formation_roots[i]),
              "nested formation pointer not relocated","initial placement");
    constexpr u16 expected_x[3][16]={
        {44,76,108,140,240,272,304,393,44,76,108,140,240,272,304,393},
        {236,236,236,228,150,150,232,255,310,310,310,318,397,397,389,297},
        {236,236,236,228,150,150,232,255,310,310,310,318,397,397,389,297}};
    // Exercise GET_VALID_XY from its type check to OEXIT, including the
    // wave mask, descending TMP cursor and indirect reads of packed points.
    for (bool enhanced : {false, true}) {
        Machine m = enhanced ? wide : reference;
        m.external_timers = true;
        m.ww(L8_GAME_STATE, 0x10);
        constexpr unsigned camera_x[][2] = {
            {944,1077}, {1416,1686}, {1888,2295}, {944,1077}, {1416,1686}, {1416,1686}
        };
        constexpr unsigned camera_y[] = {5,5,5,670,670,338};
        const u32 root = 0xffe8c7f0u;
        const u32 origins = rlong(m, enhanced ? enhanced_data_address(root) : root);
        for (unsigned wave = 3; wave <= 8; ++wave) {
            const u32 origin = rlong(m, origins + wave*32);
            const u32 world_x = camera_x[wave-3][enhanced];
            const u32 world_y = camera_y[wave-3];
            check(origin == ((world_y << 16) | world_x), "unexpected room origin", "WAVEYX");
            wlong(m, 0x010ee080u, origin);
            m.ww(0x010eb630u, wave);
            wfield(m, 0x010de708u, 0, 16); // HULKTYP for circuit one
            wfield(m, 0x010de5e8u, 16, 16);
            const unsigned table = (formation_roots[wave-1] - 0xffe4d100u) / 0x200;
            for (unsigned remaining = 16; remaining; --remaining) {
                const unsigned i = remaining - 1;
                const u32 retail = rlong(reference, formation_roots[wave-1] + i*32);
                const u32 x = enhanced ? expected_x[table][i] : u16(retail);
                m.c.st = 0x830;
                m.c.a[11] = 6; // HULK
                m.c.sp = 0x010fffe0u;
                wlong(m, 0x010dd7f0u, 0x010ff000u); // KEEPXY output buffer
                u32 pc = 0xffe3b970u; // actual OHULK call and return address
                for (unsigned step = 0; step < 48 && pc != 0xffe421c0u && !m.stop; ++step)
                    pc = l8_run(m, pc, m.c.cyc + 1);
                check(pc == 0xffe421c0u && m.faults.empty() && !m.stop,
                      "formation did not reach OEXIT", "GET_VALID_XY");
                check(rfield(m, 0x010de5e8u, 16) == i && m.c.a[0] == (x << 16) &&
                      m.c.a[3] == ((retail & 0xffff0000u) | x),
                      "formation cursor or returned anchor differs", "GET_VALID_XY");
                for (unsigned step = 0; step < 20 && pc != 0xffe3baa0u && !m.stop; ++step)
                    pc = l8_run(m, pc, m.c.cyc + 1);
                const u32 packed = (retail & 0xffff0000u) | x;
                check(pc == 0xffe3baa0u && m.c.sp == 0x010fffe0u &&
                      rlong(m, 0x010ff000u) == packed &&
                      m.c.a[0] == ((world_x + x) << 16) && m.c.a[1] == (world_y << 16) + packed,
                      "world spawn anchor or KEEPXY output differs", "OHULK");
                for(unsigned step=0;step<24 && pc!=0xffe3bac0u && !m.stop;++step)
                    pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==0xffe3bac0u,"Hulk descriptor setup differs","OHULK");
                check_spawned_sprite(m,pc,0xffe3baf0u,(world_x+x)<<16,(world_y<<16)+packed,0x8600);
            }
        }
    }
    for (unsigned t=0;t<3;++t) for(unsigned i=0;i<16;++i) {
        const u32 address=0xffe4d100u+t*0x200+i*32;
        const u32 value=rlong(wide,enhanced_data_address(address));
        check(u16(value)==expected_x[t][i],"formation X","initial placement");
        check((value>>16)==(rlong(reference,address)>>16),"formation Y changed","initial placement");
    }
    check(rlong(wide,enhanced_data_address(0xfff1ab80u))==(121u<<16)&&
          rlong(wide,enhanced_data_address(0xfff1aba0u))==(426u<<16),"wall placement","initial placement");
    for (u32 address : {0xffe417c0u,0xffe4d100u,0xffe4d310u,0xffe4d6f0u,0xfff1ab80u}) {
        Machine bad=reference; bad.enhanced=true; bad.prog[index(address)]^=1;
        check(!apply_enhanced(bad,137)&&bad.stop&&bad.enhanced_data.empty(),
              "corrupt placement table accepted","initial placement");
    }
    gang_doors(reference,wide);
    room_spawns(reference);
    room_spawns(wide);
    opening_caps(reference,wide);
    for (std::size_t i=0; i<reference_x.size(); ++i) {
        check(wide.enhanced_data[index(red_x)+i] == enhanced_x[i], "both packed X bounds must follow widened arena", "RED_CARPETX");
        check(reference.prog[index(red_x)+i] == reference_x[i], "reference ROM data changed", "RED_CARPETX");
        check(wide.enhanced_data[index(red_y)+i] == original_y[i], "separate Y table changed", "RED_CARPETY");
    }
    // The data patch must fail at the named L8-table boundary if either packed
    // half differs, rather than silently remap a wrong table/revision.
    for (const std::size_t corrupt : {std::size_t{0},std::size_t{1},std::size_t{9}}) {
        Machine bad = reference;
        bad.enhanced = true;
        bad.prog[index(red_x)+corrupt] ^= 1u;
        const auto before = bad.prog;
        const bool accepted = apply_enhanced(bad,137);
        bool named = false;
        for (const auto& fault : bad.faults)
            named = named || (fault.addr == red_x && fault.what == "enhanced table does not match L8 reference");
        check(!accepted && named && bad.stop, "mismatched bound did not fail closed by return value/name/address", "RED_CARPETX");
        check(bad.prog == before && bad.enhanced_data.empty(),
              "rejected preparation modified ROM or published partial native data", "RED_CARPETX");
    }
}
void balance() {
    // Execute production translated consumers, across gameplay and non-gameplay.
    const u32 pcs[]={0xffe3ecd0u,0xfff1df50u,0xfff1df70u,0xfff1edc0u,0xfff1ede0u,0xfff39850u,0xfff39870u};
    for (bool wide:{false,true})
        for (u16 state:{u16(0),u16(0x10),u16(0x18),u16(0x30),u16(0x50)})
            for (s32 velocity:{-32768,-80,0,80,32760})
                for (unsigned n=0;n<7;++n) {
                    Machine m;m.enhanced=wide;m.external_timers=true;m.c.st=0x830;
                    m.ww(L8_GAME_STATE,state);m.c.a[13]=0x01010000u;
                    const unsigned reg=n==0?1:(n%2?2:3);
                    const u32 offset=n==0?352:(n%2?320:(n<5?336:352));
                    m.c.a[reg]=u32(velocity);
                    wlong(m,m.c.a[13]+offset,u32(velocity));
                    const u32 expected=u32(velocity);
                    const u32 next=l8_run(m,pcs[n],1);
                    check(next==pcs[n]+32&&!m.stop&&m.faults.empty(),"balance continuation/fault","balance");
                    check(m.c.a[reg]==expected,"balance value or scene isolation","balance");
                    if(n==0)check(rlong(m,m.c.a[13]+offset)==expected,"enemy shared speed store","balance");
                }
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr,"usage: stv_enhanced_geometry_tests smashtv.zip\n"); return 2; }
    instructions();
    balance();
    cobra_spawns();
    widescreen_pickups();
    cobra_finishing_hit();
    rboss_spawn();
    rboss_movement();
    player_rescue();
    native_dma_clipping();
    carpet(argv[1]);
    if (failures) { std::fprintf(stderr,"%d geometry regression failure(s)\n",failures); return 1; }
    std::puts("Compiled geometry, two Cobra spawn prefixes and four gang doors preserve reference values/cycles; player rescue, widened gates, signed fractions, carpet bounds and native-scene DMA pass");
    return 0;
}
