// Smash TV sound board (Williams CVSD, MAME williams_cvsd_sound_device), PLAN P6.
//
// The MC6809E program is statically translated (tools/m6809_aot/emit6809.py):
// one function per instruction, following MAME 0.289's m6809 microcode access
// by access. `cyc` counts CPU cycles since power-on (2 MHz, 500 ns each); a
// memory access advances it by one before the device sees the access, exactly
// like MAME's eat(1)-then-access. Sync points (SY) are where MAME may end a
// timeslice; scheduled events are applied there once `cyc` reaches their
// threshold (see Cpu::service).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace stv::snd {

// MAME attotime: whole seconds plus attoseconds.
struct Atto {
    int64_t sec{};
    int64_t as{};
    static constexpr int64_t kPerSec = 1000000000000000000LL;
    friend bool operator<(const Atto& x, const Atto& y) { return x.sec != y.sec ? x.sec < y.sec : x.as < y.as; }
    friend bool operator<=(const Atto& x, const Atto& y) { return !(y < x); }
    friend bool operator==(const Atto& x, const Atto& y) { return x.sec == y.sec && x.as == y.as; }
    friend Atto operator+(const Atto& x, const Atto& y) {
        Atto r{x.sec + y.sec, x.as + y.as};
        if (r.as >= kPerSec) { r.as -= kPerSec; ++r.sec; }
        return r;
    }
};
Atto from_ticks(uint64_t ticks, uint32_t hz);     // attotime::from_ticks
constexpr int64_t kCpuAs = 500000000000LL;          // attoseconds per sound CPU cycle
inline Atto cyc_time(uint64_t cyc) {
    return Atto{int64_t(cyc / 2000000), int64_t(cyc % 2000000) * kCpuAs};
}
inline uint64_t cyc_floor(const Atto& t) { return uint64_t(t.sec) * 2000000 + uint64_t(t.as / kCpuAs); }
inline uint64_t cyc_ceil(const Atto& t) { return cyc_floor(t) + (t.as % kCpuAs ? 1 : 0); }

struct Board;
struct Cpu;
using Fn = void (*)(Cpu&);
struct Entry {
    uint16_t pc;
    Fn fn;
    uint8_t nchk;          // RAM-resident code: bytes that must match before running
    uint8_t chk[5];
};
struct PageTable {
    const char* name;
    const Entry* entries;
    size_t count;
};
extern const PageTable k_pages[7];   // u4.0 u4.1 u19.0 u19.1 u20.0 u20.1 ram (generated)

struct Cpu {
    uint8_t a{}, b{}, dp{}, cc{0x50};
    uint16_t x{}, y{}, u{}, s{}, pc{};
    uint64_t cyc{};
    uint64_t due{~0ull};            // earliest pending event threshold
    bool nmi_asserted{}, nmi_line{}, firq_line{}, irq_line{}, lds{};
    Board* bd{};

    uint16_t d() const { return uint16_t((a << 8) | b); }
    void setd(uint16_t v) { a = uint8_t(v >> 8); b = uint8_t(v); }

    uint8_t rd(uint16_t addr);
    void wr(uint16_t addr, uint8_t v);
    bool service();                 // apply due events; true = CPU was reset, abandon the instruction
    bool sync_wait();               // SYNC: wait for an interrupt line
    bool cwai_wait();               // CWAI: wait, then take the interrupt (sets pc)
};

// Flag helpers: MAME m6809_base_device::set_flags and friends, bit for bit.
inline uint8_t flags8(Cpu& c, uint8_t mask, uint8_t a, uint8_t b, uint32_t r) {
    c.cc &= uint8_t(~mask);
    if (mask & 0x20) c.cc |= ((a ^ b ^ r) & 0x10) ? 0x20 : 0;
    if (mask & 0x08) c.cc |= (r & 0x80) ? 0x08 : 0;
    if (mask & 0x04) c.cc |= (uint8_t(r) == 0) ? 0x04 : 0;
    if (mask & 0x02) c.cc |= ((a ^ b ^ r ^ (r >> 1)) & 0x80) ? 0x02 : 0;
    if (mask & 0x01) c.cc |= (r & 0x100) ? 0x01 : 0;
    return uint8_t(r);
}
inline uint16_t flags16(Cpu& c, uint8_t mask, uint16_t a, uint16_t b, uint32_t r) {
    c.cc &= uint8_t(~mask);
    if (mask & 0x08) c.cc |= (r & 0x8000) ? 0x08 : 0;
    if (mask & 0x04) c.cc |= (uint16_t(r) == 0) ? 0x04 : 0;
    if (mask & 0x02) c.cc |= ((a ^ b ^ r ^ (r >> 1)) & 0x8000) ? 0x02 : 0;
    if (mask & 0x01) c.cc |= (r & 0x10000) ? 0x01 : 0;
    return uint16_t(r);
}
inline uint8_t flags_nzv8(Cpu& c, uint8_t v) { return flags8(c, 0x0E, 0, v, v); }
inline uint16_t flags_nzv16(Cpu& c, uint16_t v) { return flags16(c, 0x0E, 0, v, v); }
inline uint16_t flags_nz16(Cpu& c, uint16_t v) { return flags16(c, 0x0C, 0, v, v); }
inline uint16_t add16(Cpu& c, uint16_t r, uint16_t v) { return flags16(c, 0x0F, r, v, uint32_t(r) + v); }
inline uint16_t sub16(Cpu& c, uint16_t r, uint16_t v) { return flags16(c, 0x0F, r, v, uint32_t(r) - v); }
inline uint8_t ror8(Cpu& c, uint8_t v) {
    bool carry = v & 1;
    v = uint8_t((v >> 1) | ((c.cc & 1) ? 0x80 : 0));
    c.cc = uint8_t(carry ? (c.cc | 1) : (c.cc & ~1));
    return v;
}
inline uint32_t rol8(Cpu& c, uint8_t v) {
    bool carry = v & 0x80;
    uint32_t r = (uint32_t(v) << 1) | ((c.cc & 1) ? 1 : 0);
    c.cc = uint8_t(carry ? (c.cc | 1) : (c.cc & ~1));
    return r;
}
inline void daa(Cpu& c) {
    uint16_t cf = 0;
    uint8_t msn = c.a & 0xF0, lsn = c.a & 0x0F;
    if (lsn > 0x09 || (c.cc & 0x20)) cf |= 0x06;
    if (msn > 0x80 && lsn > 0x09) cf |= 0x60;
    if (msn > 0x90 || (c.cc & 0x01)) cf |= 0x60;
    uint16_t t = uint16_t(c.a + cf);
    c.cc &= ~0x02;
    if (t & 0x0100) c.cc |= 0x01;
    c.a = flags8(c, 0x0C, 0, uint8_t(t), uint8_t(t));
}
inline void mul(Cpu& c) {
    uint16_t r = uint16_t(uint16_t(c.a) * uint16_t(c.b));
    c.setd(flags16(c, 0x04, 0, r, r));
    c.cc = uint8_t((c.d() & 0x80) ? (c.cc | 1) : (c.cc & ~1));
}
inline uint16_t rd816(Cpu& c, int r) {
    switch (r & 15) {
    case 0: return c.d(); case 1: return c.x; case 2: return c.y; case 3: return c.u;
    case 4: return c.s; case 5: return c.pc;
    case 8: return uint16_t(0xff00 | c.a); case 9: return uint16_t(0xff00 | c.b);
    case 10: return uint16_t((c.cc << 8) | c.cc); case 11: return uint16_t((c.dp << 8) | c.dp);
    default: return 0xffff;
    }
}
inline uint16_t rd168(Cpu& c, int r) {
    switch (r & 15) {
    case 10: return uint16_t(0xff00 | c.cc); case 11: return uint16_t(0xff00 | c.dp);
    default: return rd816(c, r);
    }
}
inline void wr_reg(Cpu& c, int r, uint16_t v) {
    switch (r & 15) {
    case 0: c.setd(v); break; case 1: c.x = v; break; case 2: c.y = v; break; case 3: c.u = v; break;
    case 4: c.s = v; break; case 5: c.pc = v; break; case 8: c.a = uint8_t(v); break;
    case 9: c.b = uint8_t(v); break; case 10: c.cc = uint8_t(v); break; case 11: c.dp = uint8_t(v); break;
    default: break;
    }
}

}  // namespace stv::snd
