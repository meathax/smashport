#pragma once

#include "stv/gsp/machine.hpp"
#include <algorithm>

namespace stv::gsp::sprite_shadows {

// Presentation metadata for the selected retail L8 set. Headers use GSP bit
// addresses; image extents use decoded graphics bytes. No sprite pixels are
// stored here. Aliased headers can point to the same physical cel.
enum class InkUse { Dedicated, Shared, Side };
struct Family {
    u32 first_header, last_header, header_step, first_byte, end_byte;
    u64 inks;
    InkUse use;
};
constexpr u64 ink(unsigned pen) { return u64{1} << pen; }
constexpr auto dedicated=InkUse::Dedicated, shared=InkUse::Shared, side=InkUse::Side;
constexpr Family families[] = {
    {0xfffaa540,0xfffaa6c0,0x60,0x1e354,0x1e714,ink(1),dedicated}, // standing / spin legs
    {0xfffab4e0,0xfffac740,0x60,0x204a0,0x231a4,ink(1),dedicated}, // walking legs
    {0xfffad7a0,0xfffae320,0x80,0x24a4c,0x26af8,ink(1),shared}, // PDTH
    {0xfffcfad0,0xfffd0150,0x80,0x27a18,0x29dd8,ink(1),shared}, // bat and swing
    {0xfffd01d0,0xfffd1350,0x80,0x29dd8,0x2e21c,ink(8),shared}, // clubman walking
    {0xfffe5220,0xfffe5420,0x80,0x49168,0x4999c,ink(1)|ink(12),shared}, // Galaga GB
    {0xfffe5520,0xfffe5a20,0x80,0x4999c,0x4a7d4,ink(30),shared}, // Galaga rolling shell
    {0xfffe57a0,0xfffe5b20,0x80,0x4a7d4,0x4b460,ink(1),shared}, // Galaga GA
    {0xfffb4120,0xfffb4420,0x80,0x4b460,0x4bce0,ink(12),shared}, // BRGN
    {0xfffb53a0,0xfffb5c20,0x80,0x4d08c,0x4d990,ink(1),shared}, // orb
    {0xfffb6920,0xfffb6a20,0x80,0x509e0,0x5126c,ink(11),shared}, // MC
    {0xfffb6aa0,0xfffb6c20,0x80,0x5126c,0x51998,ink(3)|ink(4),shared}, // GAL
    {0xfffb6d20,0xfffb7520,0x80,0x51998,0x545d4,ink(25)|ink(29),shared}, // Fat Man
    {0xfffb8ca0,0xfffb9220,0x80,0x6187c,0x64a14,ink(1)|ink(8),side}, // mobile tank
    {0xfffbdaa0,0xfffbdc20,0x80,0x889dc,0x891f0,ink(1),shared}, // Cobra spitball
    {0xfffbfca0,0xfffbfca0,0x80,0x8e710,0x8e738,ink(2),dedicated}, // grenade shadow
    {0xfffc1e20,0xfffc2f20,0x80,0x9c104,0xa1658,ink(21)|ink(23)|ink(24),shared}, // SNG
    {0xfffc34a0,0xfffc3820,0x80,0xa2474,0xa382c,ink(21)|ink(23)|ink(24),shared}, // SNG attacks
    {0xfffc4320,0xfffc4520,0x80,0xac1e8,0xad2d8,ink(12),shared}, // RB robots
    {0xfffdf920,0xfffdf920,0x80,0xee0,0x1740,ink(1),side}, // Mutoid arm overlays
    {0xfffdfda0,0xfffdfda0,0x80,0x4bfc,0x5764,ink(1)|ink(9)|ink(16),side},
    {0xfffc4d20,0xfffc4d20,0x80,0xb2a14,0xb3434,ink(1)|ink(20),side},
    {0xfffdffa0,0xfffe00a0,0x80,0x5a7c,0x601c,ink(1),side}, // Mutoid tread edge
    {0xfffe0120,0xfffe01a0,0x80,0x601c,0x65f4,ink(1),dedicated}, // SHAD / SHAD2
};

enum Pixel : u8 { Unknown, Character, Shadow };

inline void classify(const Family& family, const u8* art, u8* mask, int width, int height) {
    const int size=width*height;
    std::fill_n(mask,size,Character);
    const auto eligible=[&](int i){return (family.inks & ink(art[i]))!=0;};
    if(family.use==dedicated){
        for(int i=0;i<size;++i)if(eligible(i))mask[i]=Shadow;
        return;
    }

    // Detached dots seed the footprint. Follow its alternating ink around
    // boots and across gaps. A filled 2x2 corner belongs to the solid actor.
    const auto opaque=[&](int x,int y){
        return x>=0 && y>=0 && x<width && y<height && art[y*width+x]!=0;
    };
    const auto neighbors=[&](int x,int y){
        return opaque(x-1,y)+opaque(x+1,y)+opaque(x,y-1)+opaque(x,y+1);
    };
    std::vector<int> frontier;
    const auto mark=[&](int i){mask[i]=Shadow;frontier.push_back(i);};
    for(int y=family.use==side?0:height/2;y<height;++y)
        for(int x=0;x<width;++x){
            const int i=y*width+x;
            if(eligible(i) && !neighbors(x,y))mark(i);
        }
    constexpr int steps[][2]={{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},
                               {0,1},{1,1},{-2,0},{2,0},{0,-2},{0,2}};
    for(size_t next=0;next<frontier.size();++next){
        const int pos=frontier[next];
        for(const auto& step:steps){
            const int x=pos%width+step[0], y=pos/width+step[1];
            if(!opaque(x,y))continue;
            const int i=y*width+x;
            if(mask[i]==Shadow || !eligible(i) || neighbors(x,y)>2)continue;
            bool solid=false;
            for(int dx:{-1,1})for(int dy:{-1,1})
                solid|=opaque(x+dx,y) && opaque(x,y+dy) && opaque(x+dx,y+dy);
            if(!solid)mark(i);
        }
    }
}

inline std::vector<u8> build(const std::vector<u16>& program,const std::vector<u8>& graphics) {
    std::vector<u8> mask;
    for(const auto& family:families){
        for(u32 header=family.first_header;header<=family.last_header;header+=family.header_step){
            const auto i=(header-0xff800000u)/16;
            if(i+5>=program.size())continue;
            const int width=(program[i]+3)&~3, height=program[i+1];
            const u32 sag=program[i+4]|(u32(program[i+5])<<16);
            const u32 off=(sag-0x02000000u)/8;
            const size_t end=size_t(off)+size_t(width)*height;
            if(!width || !height || off<family.first_byte || end>family.end_byte || end>graphics.size())continue;
            if(mask.size()<end)mask.resize(end);
            classify(family,graphics.data()+off,mask.data()+off,width,height);
        }
    }
    return mask;
}
}
