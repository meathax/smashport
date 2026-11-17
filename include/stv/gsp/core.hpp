#pragma once
// Native TMS34010 instruction semantics for statically translated L8 code (PLAN D2/D3).
// This is not an interpreter: the translator emits one call per original
// instruction with registers, immediates and targets resolved at build time.
// Semantics and cycle costs match the MAME 0.289 tms34010 core (the lockstep
// reference); every class is checked against it by tools/gsp_ops_oracle and the
// L8 lockstep harness.
#include <cstdint>

namespace stv::gsp {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

enum : u32 {
    ST_N = 1u << 31, ST_C = 1u << 30, ST_Z = 1u << 29, ST_V = 1u << 28,
    ST_PBX = 1u << 25, ST_IE = 1u << 21,
};

struct Cpu {
    u32 a[15]{};   // A0..A14
    u32 b[15]{};   // B0..B14
    u32 sp{};      // A15 == B15
    u32 st{0x00000010};
    u64 cyc{};     // total executed cycles (MAME COUNT_CYCLES accounting)
};

// ---- flags (MAME 34010ops.hxx macro equivalents) ----------------------------
inline void clr(Cpu& c, u32 bits) { c.st &= ~bits; }
inline void set_nz(Cpu& c, u32 v) { c.st |= (v == 0 ? ST_Z : 0) | (v & 0x80000000u); }
inline void set_z(Cpu& c, u32 v) { c.st |= (v == 0 ? ST_Z : 0); }
inline void set_nzcv_add(Cpu& c, u32 a, u32 b, u32 r) {
    set_nz(c, r);
    c.st |= ((~(a ^ b) & (a ^ r)) >> 3) & ST_V;
    c.st |= ((~a) < b) ? ST_C : 0;
}
inline void set_nzcv_sub(Cpu& c, u32 a, u32 b, u32 r) {
    set_nz(c, r);
    c.st |= (((a ^ b) & (a ^ r)) >> 3) & ST_V;
    c.st |= (b > a) ? ST_C : 0;
}
inline bool N(const Cpu& c) { return c.st & ST_N; }
inline bool C(const Cpu& c) { return c.st & ST_C; }
inline bool Z(const Cpu& c) { return c.st & ST_Z; }
inline bool V(const Cpu& c) { return c.st & ST_V; }

// Condition codes in JRcc/JAcc encoding order.
inline bool cond(const Cpu& c, int cc) {
    switch (cc) {
    case 0x0: return true;
    case 0x1: return !N(c) && !Z(c);
    case 0x2: return C(c) || Z(c);
    case 0x3: return !C(c) && !Z(c);
    case 0x4: return N(c) != V(c);
    case 0x5: return N(c) == V(c);
    case 0x6: return (N(c) != V(c)) || Z(c);
    case 0x7: return (N(c) == V(c)) && !Z(c);
    case 0x8: return C(c);
    case 0x9: return !C(c);
    case 0xA: return Z(c);
    case 0xB: return !Z(c);
    case 0xC: return V(c);
    case 0xD: return !V(c);
    case 0xE: return N(c);
    default: return !N(c);
    }
}

// Field size/extension from ST (FS0 bits 4..0, FE0 bit 5; FS1 bits 10..6, FE1 bit 11).
inline int fw(const Cpu& c, int f) { int s = (c.st >> (f ? 6 : 0)) & 0x1f; return s ? s : 32; }
inline bool fe(const Cpu& c, int f) { return (c.st >> (f ? 11 : 5)) & 1; }
inline u32 fw_inc(const Cpu& c, int f) { return u32(fw(c, f)); }

inline u32 sext(u32 v, int width) { return width >= 32 ? v : u32(s32(v << (32 - width)) >> (32 - width)); }
inline u32 zext(u32 v, int width) { return width >= 32 ? v : v & (0xffffffffu >> (32 - width)); }

// ---- memory: field access with MAME's exact word/dword access pattern -------
// M must provide u16 rw(u32 bitaddr) and void ww(u32 bitaddr, u16 v) on 16-bit words.
template <class M> inline u32 rd_dword(M& m, u32 a) { return m.rw(a) | (u32(m.rw(a + 16)) << 16); }
template <class M> inline void wr_dword(M& m, u32 a, u32 v) { m.ww(a, u16(v)); m.ww(a + 16, u16(v >> 16)); }

template <class M> inline u32 rfield(M& m, u32 addr, int size) {
    const u32 shift = addr & 0x0f;
    const u32 base = addr & 0xfffffff0u;
    if (size == 32) {
        if (!shift) return rd_dword(m, base);
        return (rd_dword(m, base) >> shift) | (rd_dword(m, base + 0x20) << (0x20 - shift));
    }
    const u32 mask = 0xffffffffu >> (32 - size);
    if (size <= 16) {
        const u32 max = 17 - u32(size);
        if (size == 16 && !shift) return m.rw(base);
        if (shift >= max) return (rd_dword(m, base) >> shift) & mask;
        return (u32(m.rw(base)) >> shift) & mask;
    }
    // 17..31 bits (RFIELDMAC_BIG for >= 18; 17 uses the word/dword form)
    if (size == 17) {
        if (shift >= 0) return (rd_dword(m, base) >> shift) & mask;
    }
    u32 r = rd_dword(m, base) >> shift;
    if (shift >= u32(33 - size)) r |= u32(m.rw(base + 0x20)) << (32 - shift);
    return r & mask;
}

template <class M> inline void wfield(M& m, u32 addr, u32 data, int size) {
    const u32 shift = addr & 0x0f;
    const u32 base = addr & 0xfffffff0u;
    if (size == 32) {
        if (!shift) { wr_dword(m, base, data); return; }
        const u32 old = rd_dword(m, base) & (0xffffffffu >> (0x20 - shift));
        const u32 hi = rd_dword(m, base + 0x20) & (0xffffffffu << shift);
        wr_dword(m, base, (data << shift) | old);
        wr_dword(m, base + 0x20, (data >> (0x20 - shift)) | hi);
        return;
    }
    const u32 mask = 0xffffffffu >> (32 - size);
    const u32 md = data & mask;
    if (size <= 17) {
        if (size == 16 && !shift) { m.ww(base, u16(data)); return; }
        const u32 max = 17 - u32(size);
        if (shift >= max) {
            const u32 old = rd_dword(m, base) & ~(mask << shift);
            wr_dword(m, base, (md << shift) | old);
        } else {
            const u32 old = u32(m.rw(base)) & ~(mask << shift);
            m.ww(base, u16((md << shift) | old));
        }
        return;
    }
    // 18..31 bits: MAME WFIELDMAC_BIG. When the field crosses into the third
    // word, MAME writes the high part back to 'base' (not base+0x20); this is
    // reproduced for lockstep and flagged by the machine as a fidelity note.
    const u32 old = rd_dword(m, base) & ~u32(mask << shift);
    wr_dword(m, base, u32(md << shift) | old);
    const u32 maxb = 33 - u32(size);
    if (shift >= maxb) {
        const u32 s2 = 32 - shift;
        const u32 old2 = u32(m.rw(base + 0x20)) & ~(mask >> s2);
        m.note_big_field_quirk(addr, size);
        m.ww(base, u16((md >> s2) | old2));
    }
}

template <class M> inline u32 rfield_ext(M& m, const Cpu& c, u32 addr, int f) {
    const int size = fw(c, f);
    u32 v = rfield(m, addr, size);
    return fe(c, f) ? sext(v, size) : v;
}
template <class M> inline u32 rbyte_s(M& m, u32 addr) { return sext(rfield(m, addr, 8), 8); }
template <class M> inline u32 rlong(M& m, u32 addr) { return rfield(m, addr, 32); }
template <class M> inline void wlong(M& m, u32 addr, u32 v) { wfield(m, addr, v, 32); }
template <class M> inline void push(M& m, Cpu& c, u32 v) { c.sp -= 0x20; wlong(m, c.sp, v); }
template <class M> inline u32 pop(M& m, Cpu& c) { u32 v = rlong(m, c.sp); c.sp += 0x20; return v; }

} // namespace stv::gsp
