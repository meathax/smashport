// The real retail L8 ROMCHECK must test original program bytes, including
// when called during an enhanced live scene. No forced pass or checksum hook.
#include "stv/gsp/ops.hpp"
#include "stv/gsp/scene.hpp"
#include "../tools/lockstep/bot.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

using namespace stv::gsp;
namespace {
constexpr u32 kRomCheck = 0xfff4f170u, kProgramChips = 0xfff543d0u;
constexpr u32 kReturn = 0xfff48a00u;
int failures = 0, checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr,"%s\n",what); }
}
std::size_t index(u32 a) { return (a - 0xff800000u) >> 4; }
bool native_pointer(u32 a) { return a >= ENHANCED_DATA_BASE && a < ENHANCED_DATA_END; }
u16 lane_sum(Machine& m, bool high) {
    u32 sum = 0;
    for (u32 a = 0xffe00000u; a != 0; a += 16)
        sum += high ? m.rw(a) >> 8 : m.rw(a) & 0xff;
    return u16(sum);
}
struct Result { u32 mask, pc; u64 cycles; };
Result romcheck(Machine m, u16 scene, u32 expected, const char* name) {
    m.reset();
    m.ww(L8_GAME_STATE, scene);
    m.c.a[14] = kProgramChips;
    m.c.b[5] = kReturn;
    m.c.b[0] = 0;
    m.c.sp = 0x010fffe0u;
    u32 pc = kRomCheck;
    unsigned long long steps = 0;
    while (pc != kReturn && !m.stop && m.c.cyc < 20000000 && steps < 8000000) {
        pc = l8_run(m, pc, m.c.cyc + 1);
        ++steps;
    }
    std::printf("%s: pc=%08x failure_mask=%u cycles=%llu instructions=%llu faults=%zu\n",
                name, pc, m.c.b[0], (unsigned long long)m.c.cyc, steps, m.faults.size());
    check(pc == kReturn, "retail ROMCHECK did not return normally");
    check(!m.stop && m.faults.empty(), "retail ROMCHECK encountered a runtime fault");
    check(m.c.b[0] == expected, "retail ROMCHECK chip failure mask differs");
    return {m.c.b[0], pc, m.c.cyc};
}
void alias_profile_ownership() {
    // The enhancement-only range was unmapped on the reference machine.
    // Retaining a prepared vector must not change that bus contract.
    for (bool prepared : {false,true}) {
        Machine reference;
        if (prepared) reference.enhanced_data = {0xbeefu};
        check(reference.rw(ENHANCED_DATA_BASE) == 0,
              "reference profile exposed native enhancement data");
        check(!reference.stop, "reference unmapped read became fatal");
        check(reference.faults.size() == 1 &&
              reference.faults[0].what == "read from unmapped address" &&
              reference.faults[0].addr == ENHANCED_DATA_BASE,
              "reference alias-range read changed its unmapped fault");
    }
    Machine unprepared;
    unprepared.enhanced = true;
    check(unprepared.rw(ENHANCED_DATA_BASE) == 0,
          "unprepared native-data read returned nonzero data");
    check(unprepared.stop, "unprepared native-data read did not stop");
    check(unprepared.faults.size() == 1 &&
          unprepared.faults[0].what == "read from unprepared native enhancement data" &&
          unprepared.faults[0].addr == ENHANCED_DATA_BASE,
          "unprepared native-data read lost its distinct failure");
}
void data_ownership(const Machine& original, Machine& wide) {
    check(wide.prog == original.prog, "enhancement changed original program bytes");
    check(wide.enhanced_data.size() == original.prog.size(), "native data region extent differs");
    check(wide.enhanced_data != original.prog, "native enhancement was not prepared");
    check(lane_sum(wide,false) == 0xfa76u && lane_sum(wide,true) == 0x1550u,
          "loaded U105/U89 Data I/O checksums differ from L8");
    check(wide.rw(kProgramChips) == 0x1550u && wide.rw(kProgramChips+0xa0u) == 0xfa76u,
          "retail checksum descriptors were changed");
    // Every physical ROM word remains identical in every known scene policy.
    // The native alias is a separate domain, even with an identical low offset.
    for (u16 scene : {u16(0),u16(0x10),u16(0x18),u16(0x30),u16(0x40),u16(0x50)}) {
        wide.ww(L8_GAME_STATE,scene);
        bool physical = true, native = true;
        for (std::size_t i=0; i<original.prog.size(); ++i) {
            const u32 a=0xff800000u+u32(i)*16;
            physical = physical && wide.rw(a)==original.prog[i];
            native = native && wide.rw(enhanced_data_address(a))==wide.enhanced_data[i];
        }
        check(physical, "a scene redirected physical program-ROM reads");
        check(native, "native data alias did not address its own image");
    }
    // Follow actual pointer graphs, not just top-level table words. This catches
    // a root redirected into native data while a nested list still uses old art.
    for (u32 c=0;c<3;++c) {
        const u32 list=rlong(wide,enhanced_data_address(0xffe72650u+c*32));
        check(native_pointer(list), "GAMEMODS circuit pointer escaped native data");
        u32 e=list;
        unsigned count=0;
        for (;count<200 && rlong(wide,e)!=0xffffffffu;++count,e+=64) {
            const u32 mod=rlong(wide,e);
            check(native_pointer(mod), "module record pointer escaped native data");
            const u32 blocks=rlong(wide,mod+48), headers=rlong(wide,mod+80);
            check(blocks>=ENHANCED_DATA_BASE && headers>=ENHANCED_DATA_BASE,
                  "module block/header pointer escaped valid data regions");
        }
        check(count>0 && count<200, "native circuit module list did not terminate");
        for (u32 root : {0xffe8c7f0u,0xffe8b8d0u,0xffec30b0u,0xffecce80u})
            check(native_pointer(rlong(wide,enhanced_data_address(root+c*32))),
                  "native geometry root retained an original nested table pointer");
    }
    constexpr u32 scripts[] = {0xffea44f0u,0xffea5450u,0xffea4d60u,0xffea54a0u,0xffea4db0u};
    for (u32 i=0;i<5;++i) {
        const u32 slot=0xffea3df0u+i*32;
        const bool horizontal=i==1 || i==2;
        const u32 script=rlong(wide,enhanced_data_address(slot));
        check(rlong(wide,slot)==scripts[i], "original movement-script pointer changed");
        check(script==(horizontal?enhanced_data_address(scripts[i]):scripts[i]),
              "native movement table redirected an incorrect script pointer");
        if(horizontal)
            check(wide.rw(script+16)==274 && wide.rw(scripts[i]+16)==223,
                  "horizontal script did not reach its separate native duration");
    }
    check(wide.rw(0xffe4cfc0u)==248 && wide.rw(enhanced_data_address(0xffe4cfc0u))==389,
          "reference and native RED_CARPETX data are not independently addressable");
}
void bot_exit_geometry(Machine& original, Machine& wide) {
    const u32 base=rlong(original,0xffecce80u);
    u32 ul=0,lr=0;
    for(unsigned k=0;k<3 && !ul;++k) {
        ul=rlong(original,base+k*32); lr=rlong(original,base+96+k*32);
        if(!lr) ul=0;
    }
    check(ul && lr,"first room has no real exit box for bot regression");
    const int x=(int(int16_t(ul))+int(int16_t(lr)))/2;
    const int y=(int(int16_t(ul>>16))+int(int16_t(lr>>16)))/2;
    for(int player : {1,2}) for(bool enhanced : {false,true}) {
        Machine m=enhanced?wide:original;
        std::fill(m.ram.begin(),m.ram.end(),0);
        auto wl=[&](u32 a,u32 v){m.ww(a,u16(v));m.ww(a+16,u16(v>>16));};
        constexpr u32 object=0x01001000u;
        wl(0x01000040u,object);
        m.ww(object+0x70,u16(x));m.ww(object+0x90,u16(y));m.ww(object+0x150,u16(0x4100+player));
        m.ww(L8_GAME_STATE,0x10);m.ww(0x010eb630u,1);
        m.ww(player==1?0x01086360u:0x01086520u,3);
        stv::bot::Bot bot(0,player);bot.log_every_=0;
        unsigned held=0;
        const unsigned shift=player==1?0:8;
        for(unsigned f=0;f<=60;++f) for(const auto& event:bot.step(f,m)) {
            if(event.port!=0) continue;
            const unsigned mask=(event.mask>>shift)&15;
            if(event.value) held|=mask; else held&=~mask;
        }
        check(held==(enhanced?8u:0u), enhanced?
              "enhanced bot stopped at the obsolete reference exit instead of moving right":
              "reference bot changed its exit target");
        if (!enhanced) {
            // Two pixels from the target, a distant mine activates the grid
            // pathfinder while start and goal occupy the same six-pixel cell.
            constexpr u32 mine = object + 0x400;
            wl(object, mine);
            m.ww(mine+0x70,u16(x-50));m.ww(mine+0x90,u16(y-50));
            m.ww(mine+0x150,0x1700);
            m.ww(object+0x70,u16(x-2));
            for (const auto& event : bot.step(61,m)) {
                if (event.port != 0) continue;
                const unsigned mask=(event.mask>>shift)&15;
                if (event.value) held|=mask; else held&=~mask;
            }
            check(held==8,"bot must finish the last two pixels inside its target grid cell");
        }
    }
}
void snapshots(Machine& wide) {
    wide.ww(L8_GAME_STATE,0x18);
    wide.frame_vis_extra=137;
    wide.c.a[1]=enhanced_data_address(0xffe72650u);
    std::vector<u8> full, short_state;
    wide.save_state(full,0xffe75750u,true);
    wide.save_state(short_state,0xffe75750u,false);
    check(short_state.size() < (64u<<20), "rollback snapshot exceeds the existing network extent limit");
    check(full.size() > short_state.size()+wide.prog.size()*2+wide.enhanced_data.size()*2,
          "full snapshot omitted native constants/derived graphics");
    Machine fresh;
    u32 pc=0;
    check(fresh.load_state(full,pc) && pc==0xffe75750u, "full native-data snapshot did not restore");
    check(fresh.prog==wide.prog && fresh.enhanced_data==wide.enhanced_data && fresh.gfx==wide.gfx,
          "full snapshot lost immutable ROM/native tables/derived graphics");
    check(fresh.frame_vis_extra==137 && fresh.c.a[1]==wide.c.a[1],
          "full snapshot lost latched scene width or a native data pointer");
    check(rlong(fresh,fresh.c.a[1])==rlong(wide,wide.c.a[1]), "restored native pointer graph differs");
    Machine rollback=wide;
    const auto* rom=rollback.prog.data();
    const auto* native=rollback.enhanced_data.data();
    const auto* gfx=rollback.gfx.data();
    rollback.frame_vis_extra=0;
    rollback.ww(L8_GAME_STATE,0x30);
    check(rollback.load_state(short_state,pc), "rollback snapshot did not restore");
    check(rollback.prog.data()==rom && rollback.enhanced_data.data()==native && rollback.gfx.data()==gfx,
          "rollback restored/reallocated immutable data");
    check(rollback.frame_vis_extra==137 && game_scene_state(rollback)==0x18,
          "rollback lost scene/frame ownership");
    auto legacy=short_state;
    legacy[7]='2';
    check(!rollback.load_state(legacy,pc), "old mutable-ROM snapshot format was silently accepted");
    auto bad_flags=short_state;
    bad_flags[12]|=0x80;
    check(!rollback.load_state(bad_flags,pc), "unknown snapshot flags were silently accepted");
    std::printf("snapshot_bytes: full=%zu rollback=%zu gfx=%zu\n",full.size(),short_state.size(),wide.gfx.size());
}
} // namespace
int main(int argc,char** argv) {
    if(argc!=2) { std::fprintf(stderr,"usage: stv_rom_integrity_tests smashtv.zip\n"); return 2; }
    alias_profile_ownership();
    Machine original;
    std::string error;
    if(!original.load_roms(argv[1],error)) { std::fprintf(stderr,"%s\n",error.c_str()); return 2; }
    Machine wide=original;
    wide.enhanced=true;
    wide.set_wide(1024,137);
    if(!apply_enhanced(wide,137)) {
        for(const auto& f:wide.faults) std::fprintf(stderr,"%s@%08x\n",f.what.c_str(),f.addr);
        return 1;
    }
    data_ownership(original,wide);
    const auto ref=romcheck(original,0,0,"reference valid ROMs");
    const auto boot=romcheck(wide,0,0,"enhanced boot valid ROMs");
    const auto live=romcheck(wide,0x10,0,"enhanced live valid ROMs");
    const auto diag=romcheck(wide,0x40,0,"enhanced operator valid ROMs");
    check(ref.cycles==boot.cycles && ref.cycles==live.cycles && ref.cycles==diag.cycles,
          "unchanged ROMCHECK timing differs by profile/scene");
    for(bool enhanced : {false,true}) {
        for(bool high : {false,true}) {
            Machine bad=enhanced?wide:original;
            bad.prog[index(0xffe00100u)]^=u16(high?0x100:1);
            romcheck(bad,0x10,high?1u:2u,
                     high ? (enhanced?"enhanced bad U89":"reference bad U89")
                          : (enhanced?"enhanced bad U105":"reference bad U105"));
        }
    }
    bot_exit_geometry(original,wide);
    snapshots(wide);
    check(wide.prog==original.prog, "diagnostic/snapshot tests modified original fixture");
    std::printf("ROM integrity: %d checks, %d failures; real ROMCHECK and corruption controls executed\n",checks,failures);
    return failures?1:0;
}
