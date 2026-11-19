// TMS34010 graphics instructions used by L8 (DIAG power-on screens, MAIN error
// display): PIXT, DRAV, CVXYL, FILL XY/L and PIXBLT B,XY/L. Semantics and cycle
// accounting follow MAME 0.289 tms34010.cpp/34010ops.hxx/34010gfx.hxx, including
// the interruptible "eat cycles" behaviour of FILL/PIXBLT (P flag, PC backed up).
#include "stv/gsp/ops.hpp"

namespace stv::gsp {

namespace {
constexpr u32 ST_P = 1u << 25;
int psize_bits(u16 psize) {
    switch (psize) { case 2: return 2; case 4: return 4; case 8: return 8; case 0x10: return 16; case 0x20: return 32; default: return 1; }
}
u32 raster_op(int rop, u32 n, u32 o, u16 psize) {
    switch (rop) {
    case 1: return n & o;           case 2: return n & ~o;        case 3: return 0;
    case 4: return n | ~o;          case 5: return ~(n ^ o);      case 6: return ~o;
    case 7: return ~(n | o);        case 8: return n | o;         case 9: return o;
    case 10: return n ^ o;          case 11: return ~n & o;       case 12: return 0xffff;
    case 13: return ~n | o;         case 14: return ~(n & o);     case 15: return ~n;
    case 16: return n + o;
    case 17: { const u32 mx = psize ? 0xffffffffu >> (32 - (psize & 31 ? psize & 31 : 32)) : 0xffffffffu; const u32 t = n + o; return t > mx ? mx : t; }
    case 18: return o - n;
    case 19: return o > n ? o - n : 0;
    case 20: return o > n ? o : n;
    case 21: return o > n ? n : o;
    default: return n;
    }
}
u32 pixel_op(int rop, u32 dst, u32 mask, u32 src) {
    switch (rop) {
    case 0: return src;                       case 1: return src & dst;
    case 2: return src & ~dst;                case 3: return 0;
    case 4: return (src | ~dst) & mask;       case 5: return ~(src ^ dst) & mask;
    case 6: return ~dst & mask;               case 7: return ~(src | dst) & mask;
    case 8: return (src | dst) & mask;        case 9: return dst & mask;
    case 10: return (src ^ dst) & mask;       case 11: return (~src & dst) & mask;
    case 12: return mask;                     case 13: return (~src & dst) & mask;
    case 14: return ~(src & dst) & mask;      case 15: return src ^ mask;
    case 16: return (src + dst) & mask;
    case 17: { s32 t = s32(src + (dst & mask)); return t > s32(mask) ? mask : u32(t); }
    case 18: return (dst - src) & mask;
    case 19: { s32 t = s32(src - (dst & mask)); return t < 0 ? 0 : u32(t); }
    case 20: dst &= mask; return src > dst ? src : dst;
    case 21: dst &= mask; return src < dst ? src : dst;
    default: return src;
    }
}
const u8 op_timing[33] = {2,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,5,5,6,5,5,2,2,2,2,2,2,2,2,2,2,2};
inline s16 X(u32 v) { return s16(v & 0xffff); }
inline s16 Y(u32 v) { return s16(v >> 16); }
inline u32 XY(s16 x, s16 y) { return (u32(u16(y)) << 16) | u16(x); }
} // namespace

// MAME set_pixel_function()/WP*/RP*: honour PSIZE, transparency and raster op.
static void wpixel(Machine& m, u32 addr, u32 data) {
    if (m.io[DPYCTL] & 0x0800) { m.fault("shift-register pixel transfer (not modelled)", addr); return; }
    const int bpp = psize_bits(m.io[PSIZE]);
    const bool trans = m.io[CONTROL] & 0x20;
    const int rop = (m.io[CONTROL] >> 10) & 0x1f;
    const bool has_rop = rop >= 1 && rop <= 21;
    if (bpp == 32) {
        const u32 a = addr & 0xffffffe0u;
        if (has_rop) data = raster_op(rop, data, rd_dword(m, a), m.io[PSIZE]);
        if (!trans || data) wr_dword(m, a, data);
        return;
    }
    if (bpp == 16) {
        const u32 a = addr & 0xfffffff0u;
        if (has_rop) data = raster_op(rop, data, m.rw(a), m.io[PSIZE]);
        if (!trans || data) m.ww(a, u16(data));
        return;
    }
    const u32 m2 = (1u << bpp) - 1;
    const u32 a = addr & 0xfffffff0u;
    const u32 shift = addr & (0x0f & ~(u32(bpp) - 1));
    if (!has_rop && trans) {
        data &= m2;
        if (!data) return;
        u32 pix = m.rw(a);
        pix = (pix & ~(m2 << shift)) | (data << shift);
        m.ww(a, u16(pix));
        return;
    }
    u32 pix = m.rw(a);
    if (has_rop) data = raster_op(rop, data & m2, (pix >> shift) & m2, m.io[PSIZE]) & m2;
    else data &= m2;
    if (trans && has_rop && !data) return;
    pix = (pix & ~(m2 << shift)) | (data << shift);
    m.ww(a, u16(pix));
}
static u32 rpixel(Machine& m, u32 addr) {
    const int bpp = psize_bits(m.io[PSIZE]);
    if (bpp == 32) return rd_dword(m, addr & 0xffffffe0u);
    if (bpp == 16) return m.rw(addr & 0xfffffff0u);
    const u32 m2 = (1u << bpp) - 1;
    return (u32(m.rw(addr & 0xfffffff0u)) >> (addr & (0x0f & ~(u32(bpp) - 1)))) & m2;
}
static u32 dxytol(Machine& m, u32 xy) { return u32(s32(Y(xy)) * s32(m.convdp)) + (u32(s32(X(xy))) << m.pixelshift) + m.c.b[4]; }
static u32 sxytol(Machine& m, u32 xy) { return u32(s32(Y(xy)) * s32(m.convsp)) + (u32(s32(X(xy))) << m.pixelshift) + m.c.b[4]; }
static int window_mode(Machine& m) { return (m.io[CONTROL] >> 6) & 3; }
static bool outside_window(Machine& m, u32 xy) {
    const u32 ws = m.c.b[5], we = m.c.b[6];
    return X(xy) < X(ws) || X(xy) > X(we) || Y(xy) < Y(ws) || Y(xy) > Y(we);
}

void gsp_cvxyl(Machine& m, u32& rd, u32 rs) { rd = dxytol(m, rs); m.c.cyc += 3; }

void gsp_pixt_ri(Machine& m, u32 rs, u32 rd) { wpixel(m, rd, rs); m.c.cyc += 2; }
void gsp_pixt_rixy(Machine& m, u32 rs, u32 rdxy) {
    if (window_mode(m) != 0) {
        m.c.st &= ~ST_V;
        if (outside_window(m, rdxy)) { m.c.st |= ST_V; m.c.cyc += 4; return; }
        if (window_mode(m) == 1) { m.c.cyc += 4; return; }
    }
    wpixel(m, dxytol(m, rdxy), rs);
    m.c.cyc += 4;
}
void gsp_pixt_ir(Machine& m, u32 rs, u32& rd) {
    const u32 t = rpixel(m, rs);
    m.c.st &= ~ST_V;
    rd = t;
    if (t) m.c.st |= ST_V;
    m.c.cyc += 4;
}
void gsp_pixt_ii(Machine& m, u32 rs, u32 rd) { wpixel(m, rd, rpixel(m, rs)); m.c.cyc += 4; }
void gsp_pixt_ixyr(Machine& m, u32 rsxy, u32& rd) {
    const u32 t = rpixel(m, sxytol(m, rsxy));
    m.c.st &= ~ST_V;
    rd = t;
    if (t) m.c.st |= ST_V;
    m.c.cyc += 6;
}
void gsp_pixt_ixyixy(Machine& m, u32 rsxy, u32 rdxy) {
    if (window_mode(m) != 0) {
        m.c.st &= ~ST_V;
        if (outside_window(m, rdxy)) { m.c.st |= ST_V; m.c.cyc += 7; return; }
        if (window_mode(m) == 1) { m.c.cyc += 7; return; }
    }
    wpixel(m, dxytol(m, rdxy), rpixel(m, sxytol(m, rsxy)));
    m.c.cyc += 7;
}
void gsp_drav(Machine& m, u32 rs, u32& rd) {
    bool skip = false;
    if (window_mode(m) != 0) {
        m.c.st &= ~ST_V;
        if (outside_window(m, rd)) { m.c.st |= ST_V; skip = true; }
        else if (window_mode(m) == 1) skip = true;
    }
    if (!skip) wpixel(m, dxytol(m, rd), m.c.b[9]);
    rd = XY(s16(X(rd) + X(rs)), s16(Y(rd) + Y(rs)));
    m.c.cyc += 4;
}

// MAME apply_window(); returns extra cycles.
static int apply_window(Machine& m, int srcbpp, u32* srcaddr, u32& dst, int& dx, int& dy) {
    if (window_mode(m) == 0) return 0;
    int sx = X(dst), sy = Y(dst), ex = sx + dx - 1, ey = sy + dy - 1, cycles = 3;
    m.c.st &= ~ST_V;
    if (window_mode(m) == 1) m.c.st |= ST_V;
    int diff = X(m.c.b[5]) - sx;
    if (diff > 0) { if (srcaddr) *srcaddr += u32(diff * srcbpp); sx += diff; m.c.st |= ST_V; }
    diff = ex - X(m.c.b[6]);
    if (diff > 0) { ex -= diff; m.c.st |= ST_V; }
    diff = Y(m.c.b[5]) - sy;
    if (diff > 0) { if (srcaddr) *srcaddr += u32(diff) * m.convsp; sy += diff; m.c.st |= ST_V; }
    diff = ey - Y(m.c.b[6]);
    if (diff > 0) { ey -= diff; m.c.st |= ST_V; }
    if (dx != ex - sx + 1 || dy != ey - sy + 1) cycles += (X(dst) != sx || Y(dst) != sy) ? 11 : 3;
    else if (X(dst) != sx || Y(dst) != sy) cycles += 7;
    dst = XY(s16(sx), s16(sy));
    dx = ex - sx + 1;
    dy = ey - sy + 1;
    return cycles;
}

// Shared FILL / PIXBLT B implementation (MAME 34010gfx.hxx fill()/pixblt_b()).
// Returns the PC to continue with ('self' while cycles remain, i.e. PC backed up).
static u32 gfx_op(Machine& m, bool is_blt, bool dst_linear, u32 self, u32 next) {
    Cpu& c = m.c;
    if (!(c.st & ST_P)) {
        if (m.io[DPYCTL] & 0x0800) { m.fault("shift-register FILL/PIXBLT (not modelled)", self); return next; }
        const int bpp = psize_bits(m.io[PSIZE]);
        const bool trans = m.io[CONTROL] & 0x20;
        const int rop = (m.io[CONTROL] >> 10) & 0x1f;
        const int ppw = 16 / (bpp > 16 ? 16 : bpp);
        const u32 pmask = bpp >= 16 ? 0xffffu : (1u << bpp) - 1;
        int timing = rop == 0 ? (trans ? 4 : 2) : (trans ? 2 + op_timing[rop] : op_timing[rop]);
        const bool need_dst = rop != 0 || trans;
        if (bpp > 16) { m.fault("32-bit FILL/PIXBLT (not modelled)", self); return next; }
        u32 saddr = c.b[0];
        int dx = X(c.b[7]), dy = Y(c.b[7]);
        m.gfxcycles = 4;
        u32 daddr;
        u32 dstxy = 0;
        if (!dst_linear) {
            dstxy = c.b[2];
            m.gfxcycles += 2 + apply_window(m, is_blt ? 1 : 0, is_blt ? &saddr : nullptr, dstxy, dx, dy);
            daddr = dxytol(m, dstxy);
        } else daddr = c.b[2];
        daddr &= ~u32(bpp - 1);
        if (dx <= 0 || dy <= 0) return next;
        if (window_mode(m) == 1 && !dst_linear) {
            c.st &= ~ST_V;
            c.b[2] = dstxy;
            c.b[7] = XY(s16(dx), s16(dy));
            m.io[INTPEND] |= INT_WV;
            return m.check_interrupt(next);
        }
        int left = (ppw - int((daddr & 15) / u32(bpp))) & (ppw - 1);
        int right = int(((daddr + u32(dx * bpp)) & 15) / u32(bpp));
        int full = dx - left - right;
        if (full < 0) { left = dx; right = full = 0; }
        else full /= ppw;
        if (is_blt) {
            int words = full + (left ? 1 : 0) + (right ? 1 : 0);
            m.gfxcycles += (words * timing + (words * bpp / 16) * 2) * dy + 2;
        } else m.gfxcycles += 2;
        c.st |= ST_P;
        for (int y = 0; y < dy; ++y) {
            u32 dw = daddr >> 4;
            u32 sw = saddr >> 4;
            u16 srcword = 0, srcmask = 0;
            if (is_blt) { srcword = m.rw(sw++ << 4); srcmask = u16(1u << (saddr & 15)); }
            else m.gfxcycles += (full + (left ? 1 : 0) + (right ? 1 : 0)) * timing;
            auto pixel_for = [&]() -> u32 {
                if (!is_blt) return c.b[9];
                const u32 p = (srcword & srcmask) ? c.b[9] : c.b[8];
                srcmask = u16(srcmask << 1);
                if (!srcmask) { srcword = m.rw(sw++ << 4); srcmask = 1; }
                return p;
            };
            auto run = [&](int count, u32 dstmask, bool read_dst) {
                u32 dstword = read_dst ? m.rw(dw << 4) : 0;
                for (int x = 0; x < count; ++x) {
                    u32 pixel = pixel_for() & dstmask;
                    if (rop) pixel = pixel_op(rop, dstword, dstmask, pixel);
                    if (!trans || pixel != 0) dstword = (dstword & ~dstmask) | pixel;
                    dstmask <<= bpp;
                }
                m.ww(dw++ << 4, u16(dstword));
            };
            if (left) run(left, pmask << (daddr & 15), true);
            for (int w = 0; w < full; ++w) run(ppw, pmask, need_dst);
            if (right) run(right, pmask, true);
            saddr += c.b[1];
            daddr += c.b[3];
        }
    }
    // eat cycles against the current slice (MAME m_icount)
    const u64 avail = m.next_event_cycle > c.cyc ? m.next_event_cycle - c.cyc : 0;
    if (u64(m.gfxcycles) > avail) {
        m.gfxcycles -= int(avail);
        c.cyc += avail;
        return m.boundary(self);          // PC backed up: resume the op after the slice
    }
    c.cyc += u64(m.gfxcycles);
    c.st &= ~ST_P;
    if (is_blt) c.b[0] += u32(Y(c.b[7])) * c.b[1];
    if (dst_linear) c.b[2] += u32(Y(c.b[7])) * c.b[3];
    else c.b[2] = XY(X(c.b[2]), s16(Y(c.b[2]) + Y(c.b[7])));
    if (c.cyc >= m.next_event_cycle) return m.boundary(next);
    return next;
}

u32 gsp_fill(Machine& m, bool linear, u32 self, u32 next) { return gfx_op(m, false, linear, self, next); }
u32 gsp_pixblt_b(Machine& m, bool linear, u32 self, u32 next) { return gfx_op(m, true, linear, self, next); }

} // namespace stv::gsp
