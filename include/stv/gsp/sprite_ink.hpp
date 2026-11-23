#pragma once
#include "stv/gsp/machine.hpp"
#include <limits>

namespace stv::gsp {
inline bool sprite_ink(const Machine& m, u32 node, int wx, int wy) {
    auto word = [&](u32 a) { return m.ram[(a - 0x01000000u) >> 4]; };
    const int w = word(node + 0xf0), h = word(node + 0x100);
    const int ox = s16(word(node + 0x70)), oy = s16(word(node + 0x90));
    const int tx = wx - ox, ty = wy - oy;
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || tx < 0 || ty < 0 || tx >= w || ty >= h) return false;
    const u16 flags = word(node + 0xb0);
    u32 sag = u32(word(node + 0xd0)) | (u32(word(node + 0xe0)) << 16);
    if (flags & 16) sag -= u32(w - 1) * 8;
    const long long base = (long long)((sag >= 0x02000000u ? sag - 0x02000000u : sag) >> 3);
    const int pitch = (s16(word(node + 0xc0)) + ((flags & 16) ? -w : w) + 3) & ~3;
    const int sx = (flags & 16) ? w - 1 - tx : tx, sy = (flags & 32) ? h - 1 - ty : ty;
    const long long at = base + (long long)pitch * sy + sx;
    return at >= 0 && at < (long long)m.gfx.size() && (m.gfx[std::size_t(at)] & 63) != 0;
}

// Return the occupied sprite pixel nearest the requested world coordinate.
inline bool nearest_sprite_ink(const Machine& m,u32 node,int& x,int& y) {
    if(sprite_ink(m,node,x,y))return true;
    auto word=[&](u32 a){return m.ram[(a-0x01000000u)>>4];};
    const int width=word(node+0xf0),height=word(node+0x100);
    if(width<1 || width>64 || height<1 || height>64)return false;
    const int left=s16(word(node+0x70)),top=s16(word(node+0x90));
    const int requestedX=x,requestedY=y;
    int best=std::numeric_limits<int>::max();
    for(int row=0;row<height;++row)for(int col=0;col<width;++col){
        const int dx=left+col-requestedX,dy=top+row-requestedY;
        const int distance=dx*dx+dy*dy;
        if(distance<best && sprite_ink(m,node,left+col,top+row)){
            best=distance;x=left+col;y=top+row;
        }
    }
    return best!=std::numeric_limits<int>::max();
}
} // namespace stv::gsp
