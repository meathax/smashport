#!/usr/bin/env python3
"""gsp2cpp emitter: statically translate the L8 program ROM to C++ (PLAN D1/D2, P2).

Input: the user's L8 program ROM (via compare.retail) and the code map
(generated/private/l8_codemap.json). Every instruction becomes C++ that calls the
semantics in include/stv/gsp/{core,ops}.hpp with registers, immediates and
targets fixed at build time; nothing decodes instructions at runtime.

Code is grouped into resumable chunks: each chunk function switches on the entry
PC to a label per instruction, so control can re-enter anywhere (interrupt
returns, process wakes). Static branches inside a chunk use goto; everything
else returns the next PC to the dispatcher (l8_run). After every instruction the
cycle count is compared with the next timer event (MAME slice semantics).

Output (derived from the user's ROM, private, never redistributed):
  generated/private/l8/l8_chunk_NNN.cpp, l8_dispatch.cpp, l8_translation_index.csv

  python tools/gsp2cpp/emit.py [--chunk 192]
"""
import argparse, hashlib, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compare
from decode import Decoder

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "generated", "private", "l8")
ROM_LO = 0xFFE00000


class Unsupported(Exception):
    pass


def R(name):
    if name == "SP":
        return "c.sp"
    return f"c.{name[0].lower()}[{int(name[1:])}]"


def reg_file(name):
    return None if name == "SP" else name[0]


def regnum(name):
    return 15 if name == "SP" else int(name[1:])


def REGI(file, i):
    return "c.sp" if i == 15 else f"c.{file.lower()}[{i}]"


def h(v):
    return f"0x{v & 0xFFFFFFFF:08x}u"


class Emitter:
    def __init__(self, W, codemap, sources):
        self.W = W
        self.cm = codemap
        self.src = sources
        self.dec = Decoder()
        self.unsupported = {}

    # ---- one instruction -> list of C++ lines; returns (lines, kind)
    # kind: "fall" (continue to next), "end" (control transfer handled)
    def emit(self, ins, chunk_pcs):
        pc, nxt, t = ins.pc, ins.pc + ins.words * 16, ins.text
        m = ins.mnem
        ops = ins.ops
        L = []

        def cy(n):
            L.append(f"c.cyc += {n};")

        def goto_or_ret(target, dyn=False):
            if not dyn and target in chunk_pcs:
                return f"goto L_{target:08x};"
            return f"return {target if dyn else h(target)};"

        def chk_then(target_expr, static_target=None):
            # boundary check, then continue to target
            if static_target is not None:
                L.append(f"if (c.cyc >= m.next_event_cycle) return m.boundary({h(static_target)});")
                L.append(goto_or_ret(static_target))
            else:
                L.append(f"{{ u32 npc = {target_expr}; if (c.cyc >= m.next_event_cycle) return m.boundary(npc); return npc; }}")

        def fall():
            L.append(f"if (c.cyc >= m.next_event_cycle) return m.boundary({h(nxt)});")
            return "fall"

        regs = [o.reg for o in ops if o.kind in ("R",)]
        F = None
        if ops and ops[-1].kind == "K" and m in ("MOVE", "SEXT", "ZEXT", "EXGF"):
            F = ops[-1].value

        # ---------------- register-register ALU
        if m in ("ADD", "ADDC", "SUB", "SUBB", "CMP", "AND", "ANDN", "OR", "XOR") and len(regs) == 2:
            s, d = R(regs[0]), R(regs[1])
            if m == "ADD":
                L.append(f"{{ u32 a={s}, b={d}, r=a+b; clr(c,ST_N|ST_C|ST_Z|ST_V); {d}=r; set_nzcv_add(c,a,b,r); }}")
            elif m == "ADDC":
                L.append(f"{{ u32 a={s}, b={d}, r=a+b+(C(c)?1u:0u); clr(c,ST_N|ST_C|ST_Z|ST_V); {d}=r; set_nzcv_add(c,a,b,r); }}")
            elif m == "SUB":
                L.append(f"{{ u32 a={d}, b={s}, r=a-b; clr(c,ST_N|ST_C|ST_Z|ST_V); set_nzcv_sub(c,a,b,r); {d}=r; }}")
            elif m == "SUBB":
                L.append(f"{{ u32 a={d}, b={s}, r=a-b-(C(c)?1u:0u); clr(c,ST_N|ST_C|ST_Z|ST_V); set_nzcv_sub(c,a,b,r); {d}=r; }}")
            elif m == "CMP":
                L.append(f"{{ u32 a={d}, b={s}, r=a-b; clr(c,ST_N|ST_C|ST_Z|ST_V); set_nzcv_sub(c,a,b,r); }}")
            elif m == "AND":
                L.append(f"clr(c,ST_Z); {d}&={s}; set_z(c,{d});")
            elif m == "ANDN":
                L.append(f"clr(c,ST_Z); {d}&=~{s}; set_z(c,{d});")
            elif m == "OR":
                L.append(f"clr(c,ST_Z); {d}|={s}; set_z(c,{d});")
            else:
                L.append(f"clr(c,ST_Z); {d}^={s}; set_z(c,{d});")
            cy(1)
            return L, fall()
        if m == "CLR":
            d = R(regs[0])
            L.append(f"clr(c,ST_Z); {d}=0; set_z(c,{d});")
            cy(1)
            return L, fall()
        if m in ("ADDI", "SUBI", "CMPI", "ANDI", "ORI", "XORI"):
            v, d = ops[0].value, R(regs[0])
            longf = ins.words == 3
            if m == "ADDI":
                a = h(v) if longf else f"u32(s32(s16({v & 0xffff})))"
                L.append(f"{{ u32 a={a}, b={d}, r=a+b; clr(c,ST_N|ST_C|ST_Z|ST_V); {d}=r; set_nzcv_add(c,a,b,r); }}")
            elif m in ("SUBI", "CMPI"):
                tt = h(v) if longf else f"u32(s32(s16({v & 0xffff})))"
                store = f"{d}=r;" if m == "SUBI" else ""
                L.append(f"{{ u32 a={d}, b={tt}, r=a-b; clr(c,ST_N|ST_C|ST_Z|ST_V); set_nzcv_sub(c,a,b,r); {store} }}")
            elif m == "ANDI":
                L.append(f"clr(c,ST_Z); {d}&={h(v)}; set_z(c,{d});")
            elif m == "ORI":
                L.append(f"clr(c,ST_Z); {d}|={h(v)}; set_z(c,{d});")
            else:
                L.append(f"clr(c,ST_Z); {d}^={h(v)}; set_z(c,{d});")
            cy(3 if longf else 2)
            return L, fall()
        if m in ("ADDK", "SUBK", "INC", "DEC"):
            d = R(regs[0])
            k = ops[0].value if m in ("ADDK", "SUBK") else 1
            if m in ("ADDK", "INC"):
                L.append(f"{{ u32 a={k}u, b={d}, r=a+b; clr(c,ST_N|ST_C|ST_Z|ST_V); {d}=r; set_nzcv_add(c,a,b,r); }}")
            else:
                L.append(f"{{ u32 a={d}, b={k}u, r=a-b; clr(c,ST_N|ST_C|ST_Z|ST_V); set_nzcv_sub(c,a,b,r); {d}=r; }}")
            cy(1)
            return L, fall()
        if m == "MOVK":
            L.append(f"{R(regs[0])}={ops[0].value}u;")
            cy(1)
            return L, fall()
        if m == "MOVI":
            d = R(regs[0])
            v = ops[0].value if ins.words == 3 else ((ops[0].value ^ 0x8000) - 0x8000) & 0xFFFFFFFF
            L.append(f"clr(c,ST_N|ST_Z|ST_V); {d}={h(v)}; set_nz(c,{d});")
            cy(3 if ins.words == 3 else 2)
            return L, fall()
        if m == "NOT":
            d = R(regs[0])
            L.append(f"clr(c,ST_Z); {d}=~{d}; set_z(c,{d});")
            cy(1)
            return L, fall()
        if m == "NEG":
            d = R(regs[0])
            L.append(f"{{ u32 b={d}, r=0u-b; clr(c,ST_N|ST_C|ST_Z|ST_V); set_nzcv_sub(c,0u,b,r); {d}=r; }}")
            cy(1)
            return L, fall()
        if m == "NEGB":
            d = R(regs[0])
            L.append(f"{{ u32 b={d}+(C(c)?1u:0u), r=0u-b; clr(c,ST_N|ST_C|ST_Z|ST_V); set_nzcv_sub(c,0u,b,r); {d}=r; }}")
            cy(1)
            return L, fall()
        if m == "ABS":
            d = R(regs[0])
            L.append(f"{{ s32 r=s32(0u-{d}); clr(c,ST_N|ST_Z|ST_V); if (r>0) {d}=u32(r); set_nz(c,u32(r)); "
                     f"if (u32(r)==0x80000000u) c.st|=ST_V; }}")
            cy(1)
            return L, fall()
        if m == "MOVE" and len(ops) == 2 and ops[0].kind == "R" and ops[1].kind == "R":
            s, d = R(ops[0].reg), R(ops[1].reg)
            L.append(f"clr(c,ST_N|ST_Z|ST_V); {d}={s}; set_nz(c,{d});")
            cy(1)
            return L, fall()
        if m in ("MOVX", "MOVY"):
            s, d = R(regs[0]), R(regs[1])
            if m == "MOVX":
                L.append(f"{d}=({d}&0xffff0000u)|({s}&0xffffu);")
            else:
                L.append(f"{d}=({s}&0xffff0000u)|({d}&0xffffu);")
            cy(1)
            return L, fall()
        if m in ("ADDXY", "SUBXY", "CMPXY"):
            s, d = R(regs[0]), R(regs[1])
            if m == "ADDXY":
                L.append(f"{{ s16 x=s16(s16({d})+s16({s})), y=s16(s16({d}>>16)+s16({s}>>16)); clr(c,ST_N|ST_C|ST_Z|ST_V); "
                         f"{d}=(u32(u16(y))<<16)|u16(x); if(x==0)c.st|=ST_N; if(y&0x8000)c.st|=ST_C; if(y==0)c.st|=ST_Z; if(x&0x8000)c.st|=ST_V; }}")
            elif m == "SUBXY":
                L.append(f"{{ s16 ax=s16({s}), ay=s16({s}>>16), bx=s16({d}), by=s16({d}>>16); clr(c,ST_N|ST_C|ST_Z|ST_V); "
                         f"if(ax==bx)c.st|=ST_N; if(ay>by)c.st|=ST_C; if(ay==by)c.st|=ST_Z; if(ax>bx)c.st|=ST_V; "
                         f"{d}=(u32(u16(by-ay))<<16)|u16(bx-ax); }}")
            else:
                L.append(f"{{ s16 rx=s16(s16({d})-s16({s})), ry=s16(s16({d}>>16)-s16({s}>>16)); clr(c,ST_N|ST_C|ST_Z|ST_V); "
                         f"if(rx==0)c.st|=ST_N; if(rx&0x8000)c.st|=ST_V; if(ry==0)c.st|=ST_Z; if(ry&0x8000)c.st|=ST_C; }}")
            cy(1)
            return L, fall()
        if m == "BTST":
            d = R(regs[-1])
            if ops[0].kind == "K":
                L.append(f"clr(c,ST_Z); if(!(({d}>>{ops[0].value})&1u)) c.st|=ST_Z;")
                cy(1)
            else:
                L.append(f"clr(c,ST_Z); if(!(({d}>>({R(regs[0])}&31u))&1u)) c.st|=ST_Z;")
                cy(2)
            return L, fall()
        if m in ("SLL", "SLA", "SRL", "SRA", "RL"):
            d = R(regs[-1])
            if ops[0].kind == "K":
                disp = ops[0].value
                k = str(disp & 31)
            else:
                rs = R(regs[0])
                k = f"({rs}&31u)" if m in ("SLL", "SLA", "RL") else f"((0u-{rs})&31u)"
            if m == "SLL":
                L.append(f"{{ u32 k={k}, r={d}; clr(c,ST_C|ST_Z); if(k){{ r<<=(k-1); if(r&0x80000000u)c.st|=ST_C; r<<=1; {d}=r; }} set_z(c,r); }}")
                cy(1)
            elif m == "SLA":
                L.append(f"{{ u32 k={k}, r={d}; clr(c,ST_N|ST_C|ST_Z|ST_V); if(k){{ u32 mask=(0xffffffffu<<(31-k))&0x7fffffffu; "
                         f"u32 r2=(r&0x80000000u)?r^mask:r; if(r2&mask)c.st|=ST_V; r<<=(k-1); if(r&0x80000000u)c.st|=ST_C; r<<=1; {d}=r; }} set_nz(c,r); }}")
                cy(3)
            elif m == "SRL":
                L.append(f"{{ u32 k={k}, r={d}; clr(c,ST_C|ST_Z); if(k){{ r>>=(k-1); if(r&1u)c.st|=ST_C; r>>=1; {d}=r; }} set_z(c,r); }}")
                cy(1)
            elif m == "SRA":
                L.append(f"{{ u32 k={k}; s32 r=s32({d}); clr(c,ST_N|ST_C|ST_Z); if(k){{ r>>=(k-1); if(r&1)c.st|=ST_C; r>>=1; {d}=u32(r); }} set_nz(c,u32(r)); }}")
                cy(1)
            else:
                L.append(f"{{ u32 k={k}, v={d}, r=v; clr(c,ST_C|ST_Z); if(k){{ r<<=(k-1); if(r&0x80000000u)c.st|=ST_C; r<<=1; r|=v>>((0u-k)&31u); {d}=r; }} set_z(c,r); }}")
                cy(1)
            return L, fall()
        if m in ("SEXT", "ZEXT"):
            d = R(regs[0])
            if m == "SEXT":
                L.append(f"clr(c,ST_N|ST_Z); {d}=sext({d},fw(c,{F})); set_nz(c,{d});")
                cy(3)
            else:
                L.append(f"clr(c,ST_Z); {d}=zext({d},fw(c,{F})); set_z(c,{d});")
                cy(1)
            return L, fall()
        if m == "LMO":
            s, d = R(regs[0]), R(regs[1])
            L.append(f"{{ u32 rs={s}, res=0; clr(c,ST_Z); set_z(c,rs); if(rs) while(!(rs&0x80000000u)){{ ++res; rs<<=1; }} {d}=res; }}")
            cy(1)
            return L, fall()
        if m in ("MPYS", "MPYU", "DIVS", "DIVU", "MODS", "MODU"):
            s, d = regs[0], regs[1]
            fl, dn = d[0] if d != "SP" else "A", regnum(d)
            rd, rs, rd2 = R(d), R(s), REGI(fl, dn | 1)
            if m == "MPYS":
                L.append(f"{{ s64 p=s64(s32(sext({rs},fw(c,1))))*s64(s32({rd})); clr(c,ST_N|ST_Z); if(p==0)c.st|=ST_Z; "
                         f"if(u64(p)>>63)c.st|=ST_N; u32 hi=u32(u64(p)>>32), lo=u32(p); {rd}=hi; {rd2}=lo; }}")
                cy(20)
            elif m == "MPYU":
                L.append(f"{{ u64 p=u64(zext({rs},fw(c,1)))*u64({rd}); clr(c,ST_Z); if(p==0)c.st|=ST_Z; "
                         f"u32 hi=u32(p>>32), lo=u32(p); {rd}=hi; {rd2}=lo; }}")
                cy(21)
            elif m == "DIVS":
                if dn & 1:
                    L.append(f"clr(c,ST_N|ST_Z|ST_V); if(!{rs}) c.st|=ST_V; else {{ {rd}=u32(s32({rd})/s32({rs})); set_nz(c,{rd}); }}")
                    cy(39)
                else:
                    rn = REGI(fl, dn + 1)
                    L.append(f"clr(c,ST_N|ST_Z|ST_V); if(!{rs}) c.st|=ST_V; else {{ s64 dv=s64((u64({rd})<<32)|{rn}); "
                             f"s64 q=dv/s32({rs}); s32 rm=s32(dv%s32({rs})); if(s64(s32(q))!=q) c.st|=ST_V; else {{ {rd}=u32(q); {rn}=u32(rm); set_nz(c,{rd}); }} }}")
                    cy(40)
            elif m == "DIVU":
                if dn & 1:
                    L.append(f"clr(c,ST_Z|ST_V); if(!{rs}) c.st|=ST_V; else {{ {rd}={rd}/{rs}; set_z(c,{rd}); }}")
                else:
                    rn = REGI(fl, dn + 1)
                    L.append(f"clr(c,ST_Z|ST_V); if(!{rs}) c.st|=ST_V; else {{ u64 dv=(u64({rd})<<32)|{rn}; u64 q=dv/{rs}; "
                             f"u32 rm=u32(dv%{rs}); if(q>0xffffffffu) c.st|=ST_V; else {{ {rd}=u32(q); {rn}=rm; set_z(c,{rd}); }} }}")
                cy(37)
            elif m == "MODS":
                L.append(f"clr(c,ST_N|ST_Z|ST_V); if({rs}!=0){{ {rd}=u32(s32({rd})%s32({rs})); set_nz(c,{rd}); }} else c.st|=ST_V;")
                cy(40)
            else:
                L.append(f"clr(c,ST_Z|ST_V); if({rs}!=0){{ {rd}={rd}%{rs}; set_z(c,{rd}); }} else c.st|=ST_V;")
                cy(35)
            return L, fall()

        # ---------------- status / misc
        if m == "SETF":
            fs, fe_, f = ops[0].value & 31, ops[1].value, ops[2].value
            val = fs | (fe_ << 5)
            sh = 6 if f else 0
            L.append(f"c.st=(c.st&~(0x3fu<<{sh}))|({val}u<<{sh});")
            cy(1 + f)
            return L, fall()
        if m == "EXGF":
            d, f = R(regs[0]), F
            sh = 6 if f else 0
            L.append(f"{{ u32 t=(c.st>>{sh})&0x3fu; c.st&=~(0x3fu<<{sh}); c.st|=({d}&0x3fu)<<{sh}; {d}=t; }}")
            cy(1)
            return L, fall()
        if m in ("SETC", "CLRC"):
            L.append("c.st|=ST_C;" if m == "SETC" else "clr(c,ST_C);")
            cy(1)
            return L, fall()
        if m == "NOP":
            cy(1)
            return L, fall()
        if m == "EMU":
            cy(6)
            return L, fall()
        if m == "REV":
            L.append(f"{R(regs[0])}=0x0008u;")
            cy(1)
            return L, fall()
        if m == "DINT":
            L.append("c.st&=~ST_IE;")
            cy(3)
            return L, fall()
        if m == "GETST":
            L.append(f"{R(regs[0])}=c.st;")
            cy(1)
            return L, fall()
        if m == "GETPC":
            L.append(f"{R(regs[0])}={h(nxt)};")
            cy(1)
            return L, fall()
        if m == "PUSHST":
            L.append("push(m,c,c.st);")
            cy(2)
            return L, fall()
        if m in ("EINT", "PUTST", "POPST"):
            if m == "EINT":
                L.append("c.st|=ST_IE;")
                n = 3
            elif m == "PUTST":
                L.append(f"c.st={R(regs[0])};")
                n = 3
            else:
                L.append("c.st=pop(m,c);")
                n = 8
            L.append(f"{{ u32 npc=m.check_interrupt({h(nxt)}); c.cyc+={n}; if(npc!={h(nxt)} || c.cyc>=m.next_event_cycle) "
                     f"return c.cyc>=m.next_event_cycle ? m.boundary(npc) : npc; }}")
            return L, "fall_nochk"
        if m == "MMTM" or m == "MMFM":
            rp = ops[0].reg
            fl = reg_file(rp) or ("B" if ins.op & 0x10 else "A")
            mask = ops[1].value
            P = R(rp)
            if m == "MMTM":
                n = 2
                for i in range(16):
                    if mask & (0x8000 >> i):
                        L.append(f"{P}-=0x20u; wlong(m,{P},{REGI(fl, i)});")
                        n += 4
            else:
                n = 3
                for i in range(15, -1, -1):
                    if mask & (1 << i):
                        L.append(f"{{ u32 v=rlong(m,{P}); {REGI(fl, i)}=v; {P}+=0x20u; }}")
                        n += 4
            cy(n)
            return L, fall()

        # ---------------- moves with memory
        if m in ("MOVE", "MOVB"):
            return self.emit_move(ins, L, cy, fall, nxt)

        # ---------------- control flow
        if m == "CALLA":
            L.append(f"push(m,c,{h(nxt)});")
            cy(4)
            chk_then(None, ins.target & 0xFFFFFFF0)
            return L, "end"
        if m == "CALLR":
            L.append(f"push(m,c,{h(nxt)});")
            cy(3)
            chk_then(None, ins.target)
            return L, "end"
        if m in ("CALL", "JUMP", "EXGPC"):
            d = R(regs[0])
            if m == "CALL":
                L.append(f"push(m,c,{h(nxt)}); c.cyc+=3;")
                chk_then(f"{d}&0xfffffff0u")
            elif m == "JUMP":
                L.append("c.cyc+=2;")
                chk_then(f"{d}&0xfffffff0u")
            else:
                L.append(f"{{ u32 t={d}; {d}={h(nxt)}; c.cyc+=2; u32 npc=t&0xfffffff0u; "
                         f"if (c.cyc >= m.next_event_cycle) return m.boundary(npc); return npc; }}")
            return L, "end"
        if m == "RETS":
            n = ops[0].value if ops else 0
            L.append(f"{{ u32 npc=pop(m,c)&0xfffffff0u; " + (f"c.sp+=({n}u<<4); " if n else "") +
                     "c.cyc+=7; if (c.cyc >= m.next_event_cycle) return m.boundary(npc); return npc; }")
            return L, "end"
        if m == "RETI":
            L.append("{ u32 st=pop(m,c); u32 rpc=pop(m,c)&0xfffffff0u; c.st=st; u32 npc=m.check_interrupt(rpc); c.cyc+=11; "
                     "if (c.cyc >= m.next_event_cycle) return m.boundary(npc); return npc; }")
            return L, "end"
        if m == "TRAP":
            n = ins.op & 0x1f
            if n:
                L.append(f"push(m,c,{h(nxt)}); push(m,c,c.st);")
            L.append(f"c.st=0x10u; c.cyc+=16; {{ u32 npc=rlong(m,{h(0xFFFFFFE0 - (n << 5))})&0xfffffff0u; "
                     "if (c.cyc >= m.next_event_cycle) return m.boundary(npc); return npc; }")
            return L, "end"
        if (ins.op & 0xF000) == 0xC000:
            cc = (ins.op >> 8) & 0xF
            low = ins.op & 0xFF
            tgt = ins.target
            if low == 0x80:      # JAcc
                tgt &= 0xFFFFFFF0
                taken, not_taken = 3, 4
            elif low == 0:
                taken, not_taken = 3, 2
            else:
                taken, not_taken = 2, 1
            if cc == 0:
                cy(taken)
                chk_then(None, tgt)
                return L, "end"
            L.append(f"if (cond(c,{cc})) {{ c.cyc+={taken};")
            chk_then(None, tgt)
            L.append(f"}} c.cyc+={not_taken};")
            return L, fall()
        if m in ("DSJ", "DSJEQ", "DSJNE", "DSJS"):
            d = R(regs[0])
            tgt = ins.target
            pre = {"DSJ": "true", "DSJEQ": "Z(c)", "DSJNE": "!Z(c)", "DSJS": "true"}[m]
            if m == "DSJS":
                L.append(f"if (--{d}) {{ c.cyc+=2;")
                chk_then(None, tgt)
                L.append("} c.cyc+=3;")
            else:
                L.append(f"if ({pre} && --{d}) {{ c.cyc+=3;")
                chk_then(None, tgt)
                L.append("} c.cyc+=2;")
            return L, fall()

        # ---------------- graphics (src/gsp/gfx.cpp)
        if m == "CVXYL":
            L.append(f"gsp_cvxyl(m,{R(regs[1])},{R(regs[0])});")
            return L, fall()
        if m == "DRAV":
            L.append(f"gsp_drav(m,{R(regs[0])},{R(regs[1])});")
            return L, fall()
        if m == "PIXT":
            k = tuple(o.kind for o in ops)
            if k == ("R", "IND"):
                L.append(f"gsp_pixt_ri(m,{R(ops[0].reg)},{R(ops[1].reg)});")
            elif k == ("R", "IND", "LIT"):
                L.append(f"gsp_pixt_rixy(m,{R(ops[0].reg)},{R(ops[1].reg)});")
            elif k == ("IND", "R"):
                L.append(f"gsp_pixt_ir(m,{R(ops[0].reg)},{R(ops[1].reg)});")
            elif k == ("IND", "IND"):
                L.append(f"gsp_pixt_ii(m,{R(ops[0].reg)},{R(ops[1].reg)});")
            elif k == ("IND", "LIT", "R"):
                L.append(f"gsp_pixt_ixyr(m,{R(ops[0].reg)},{R(ops[2].reg)});")
            elif k == ("IND", "LIT", "IND", "LIT"):
                L.append(f"gsp_pixt_ixyixy(m,{R(ops[0].reg)},{R(ops[2].reg)});")
            else:
                raise Unsupported(t)
            return L, fall()
        if m in ("FILL", "PIXBLT"):
            lits = [o.reg for o in ops if o.kind == "LIT"]
            if m == "FILL" and lits in (["XY"], ["L"]):
                L.append(f"return gsp_fill(m,{'true' if lits == ['L'] else 'false'},{h(pc)},{h(nxt)});")
                return L, "end"
            if m == "PIXBLT" and lits in (["B", "XY"], ["B", "L"]):
                L.append(f"return gsp_pixblt_b(m,{'true' if lits[1] == 'L' else 'false'},{h(pc)},{h(nxt)});")
                return L, "end"
            raise Unsupported(t)

        raise Unsupported(t)

    def emit_move(self, ins, L, cy, fall, nxt):
        m, ops = ins.mnem, ins.ops
        byte = m == "MOVB"
        F = None
        if not byte and ops and ops[-1].kind == "K":
            F = ops[-1].value
            ops = ops[:-1]

        def rd(addr):
            return f"rb(m,{addr})" if byte else f"rf(m,{addr},{F})"

        def wr(addr, val):
            return f"wb(m,{addr},{val});" if byte else f"wf(m,{addr},{val},{F});"

        inc = "8u" if byte else f"fw_inc(c,{F})"
        s, d = ops[0], ops[1]
        kinds = (s.kind, d.kind)

        def to_reg(dst, expr, n):
            v = f"sext({expr},8)" if byte else expr
            L.append(f"{{ u32 v={v}; clr(c,ST_N|ST_Z|ST_V); {R(dst.reg)}=v; set_nz(c,v); }}")
            cy(n)

        if kinds == ("R", "IND"):
            L.append(wr(R(d.reg), R(s.reg))); cy(1)
        elif kinds == ("R", "DEC"):
            L.append(f"{R(d.reg)}-={inc}; " + wr(R(d.reg), R(s.reg))); cy(2)
        elif kinds == ("R", "INC"):
            L.append(wr(R(d.reg), R(s.reg)) + f" {R(d.reg)}+={inc};"); cy(1)
        elif kinds == ("IND", "R"):
            to_reg(d, rd(R(s.reg)), 3)
        elif kinds == ("DEC", "R"):
            L.append(f"{R(s.reg)}-={inc};")
            to_reg(d, rd(R(s.reg)), 4)
        elif kinds == ("INC", "R"):
            v = f"sext({rd(R(s.reg))},8)" if byte else rd(R(s.reg))
            L.append(f"{{ u32 v={v}; clr(c,ST_N|ST_Z|ST_V); {R(s.reg)}+={inc}; {R(d.reg)}=v; set_nz(c,v); }}")
            cy(3)
        elif kinds == ("IND", "IND"):
            L.append(wr(R(d.reg), rd(R(s.reg)))); cy(3)
        elif kinds == ("DEC", "DEC"):
            L.append(f"{{ {R(s.reg)}-={inc}; u32 v={rd(R(s.reg))}; {R(d.reg)}-={inc}; " + wr(R(d.reg), "v") + " }"); cy(4)
        elif kinds == ("INC", "INC"):
            L.append(f"{{ u32 v={rd(R(s.reg))}; {R(s.reg)}+={inc}; " + wr(R(d.reg), "v") + f" {R(d.reg)}+={inc}; }}"); cy(4)
        elif kinds == ("R", "DISP"):
            L.append(wr(f"{R(d.reg)}+{h(self.sx16(d.value))}", R(s.reg))); cy(3)
        elif kinds == ("DISP", "R"):
            to_reg(d, rd(f"{R(s.reg)}+{h(self.sx16(s.value))}"), 5)
        elif kinds == ("DISP", "INC"):
            L.append(f"{{ u32 v={rd(f'{R(s.reg)}+{h(self.sx16(s.value))}')}; " + wr(R(d.reg), "v") + f" {R(d.reg)}+={inc}; }}"); cy(5)
        elif kinds == ("DISP", "DISP"):
            L.append(f"{{ u32 v={rd(f'{R(s.reg)}+{h(self.sx16(s.value))}')}; " +
                     wr(f"{R(d.reg)}+{h(self.sx16(d.value))}", "v") + " }"); cy(5)
        elif kinds == ("R", "ABS"):
            L.append(wr(h(d.value), R(s.reg))); cy(1 if byte else 3)
        elif kinds == ("ABS", "R"):
            to_reg(d, rd(h(s.value)), 5)
        elif kinds == ("ABS", "INC"):
            L.append(f"{{ u32 v={rd(h(s.value))}; " + wr(R(d.reg), "v") + f" {R(d.reg)}+={inc}; }}"); cy(5)
        elif kinds == ("ABS", "ABS"):
            L.append(f"{{ u32 v={rd(h(s.value))}; " + wr(h(d.value), "v") + " }"); cy(6 if byte else 7)
        else:
            raise Unsupported(ins.text)
        if byte and kinds in (("IND", "IND"), ("DISP", "DISP"), ("ABS", "ABS")):
            pass
        return L, fall()

    @staticmethod
    def sx16(v):
        return ((v & 0xFFFF) ^ 0x8000) - 0x8000 & 0xFFFFFFFF


def matches_reference(row, field, value):
    """Public manifests retain hashes of the instruction witnesses."""
    if field in row:
        return value == row[field]
    encoded = json.dumps(value, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest() == row[field + "_sha256"]


def load_scene_write_sites(path, instructions):
    """Bind enhancement cache refreshes to the complete retail GAMSTATE writers."""
    manifest = json.load(open(path))
    target = int(manifest["destination"], 16)
    if manifest.get("schema") != 1 or target != 0x010865A0:
        raise SystemExit("unsupported enhancement scene-write manifest")
    sites = {}
    for site in manifest["sites"]:
        pc = int(site["pc"], 16)
        if pc in sites:
            raise SystemExit(f"duplicate enhancement scene-write site {pc:08x}")
        sites[pc] = site
    observed = {pc: i for pc, i in instructions.items()
                if i.mnem in ("MOVE", "MOVB") and len(i.ops) >= 2
                and i.ops[1].kind == "ABS" and i.ops[1].value == target}
    if set(observed) != set(sites):
        missing = sorted(set(sites) - set(observed))
        extra = sorted(set(observed) - set(sites))
        raise SystemExit(f"enhancement scene-write coverage changed: missing={missing}, extra={extra}")
    for pc, site in sites.items():
        i = observed[pc]
        if (not matches_reference(site, "instruction", i.text) or len(i.ops) != 3
                or i.ops[0].kind != "R" or i.ops[2].kind != "K" or i.ops[2].value != 0):
            raise SystemExit(f"scene-write at {pc:08x}: instruction changed")
        producer = instructions.get(int(site["value_pc"], 16))
        if (producer is None or producer.mnem != "MOVI" or len(producer.ops) != 2
                or producer.ops[0].kind != "IMM"
                or producer.ops[0].value != int(site["value"], 16)
                or producer.ops[1].kind != "R" or producer.ops[1].reg != i.ops[0].reg
                or producer.pc + producer.words * 16 != pc):
            raise SystemExit(f"scene-write at {pc:08x}: state producer changed")
        site["store_cpp"] = f"wf(m,{h(target)},{R(i.ops[0].reg)},0);"
    return sites


def main():
    global OUT
    ap = argparse.ArgumentParser()
    ap.add_argument("--chunk", type=int, default=192)
    ap.add_argument("--codemap", default=os.path.join(ROOT, "generated", "private", "l8_codemap.json"))
    ap.add_argument("--out", default=OUT, help="isolated private output directory")
    ap.add_argument("--rom", help="verified, normalized user-selected L8 ZIP")
    a = ap.parse_args()
    if a.rom:
        compare.ZIP = a.rom
    OUT = os.path.abspath(a.out)
    rom = compare.retail("l8")
    W = [rom[k] | (rom[k + 1] << 8) for k in range(0, len(rom), 2)]
    cm = json.load(open(a.codemap))
    starts = sorted(int(k, 16) for k in cm["instructions"])
    src = {int(k, 16): (v[2] if len(v) > 2 else "") for k, v in cm["instructions"].items()}
    em = Emitter(W, cm, src)
    # chunks: contiguous instruction runs, split every N instructions
    chunks, cur, prev_end = [], [], None
    for pc in starts:
        ins = em.dec.decode(W, (pc - ROM_LO) // 16, pc)
        if cur and (pc != prev_end or len(cur) >= a.chunk):
            chunks.append(cur)
            cur = []
        cur.append(ins)
        prev_end = pc + ins.words * 16
    if cur:
        chunks.append(cur)
    scene_sites = load_scene_write_sites(
        os.path.join(ROOT, "data", "enhanced_scene_writes.json"),
        {i.pc: i for ch in chunks for i in ch})
    scene_patched = set()
    os.makedirs(OUT, exist_ok=True)
    # Keep derived retail population constants private, like translated code.
    stages = json.load(open(os.path.join(ROOT, "data", "enhanced_stage_balance.json")))["stages"]
    required = {(c,w) for c,last in ((0,10),(1,18),(2,22)) for w in range(1,last+1) if c==0 or w!=10}
    if len(stages)!=48 or {(s["circuit"],s["wave"]) for s in stages}!=required:
        raise SystemExit("stage balance requires all 48 retail rooms")
    population = ["// Generated from data/enhanced_stage_balance.json; private retail data.",
                  "struct StagePopulation { u32 address; std::array<u16,6> original; u16 budget; };",
                  "constexpr StagePopulation stage_population[] = {"]
    for stage in stages:
        for row in stage["rows"]:
            if a.rom:
                start = (int(row["address"], 16) - ROM_LO) // 16
                row["original"] = W[start:start + 6]
            if row.get("budget") is None:
                row["budget"] = row["original"][1]
            if len(row["original"])!=6 or any(not 0<=v<=65535 for v in row["original"]+[row["budget"]]):
                raise SystemExit("invalid stage balance row")
            population.append("    {%su, {%s}, %d}," % (row["address"], ",".join(map(str,row["original"])),row["budget"]))
    population += ["};", ""]
    # Only locations, widths and strides ship. Read comparison tables from the selected image.
    from retail_tables import table_initializers
    population.extend(table_initializers(W, ROM_LO))
    with open(os.path.join(OUT,"enhanced_stage_balance.inc"),"w",newline="\n") as f:
        f.write("\n".join(population))
    for f in os.listdir(OUT):
        if f.startswith("l8_") and f.endswith(".cpp"):
            os.remove(os.path.join(OUT, f))
    index_rows, unsupported, per_file = [], {}, 24
    # Geometry is selected by semantic scene state, not by the global profile.
    # Boot/operator/title/attract retain original constants and original data.
    pp = json.load(open(os.path.join(ROOT, "data", "enhanced_patchpoints.json")))["points"]
    patches = {p["src"]: p for p in pp if "enhanced_imm" in p and "pc" not in p}
    patches_pc = {int(p["pc"], 16): p for p in pp if "enhanced_imm" in p and "pc" in p}
    explicit_patched = set()
    patched = []
    data_manifest = json.load(open(os.path.join(ROOT, "data", "enhanced_data_roots.json")))
    data_sites = {}
    for root in data_manifest["roots"]:
        for site in root["sites"]:
            pc = int(site["pc"], 16)
            if pc in data_sites:
                raise SystemExit(f"duplicate enhancement data-root site {pc:08x}")
            data_sites[pc] = dict(site, root_name=root["name"], reference=root["reference"], root_policy=root.get("policy"))
    balance_hooks = {int(p["pc"],16):p for p in json.load(open(os.path.join(ROOT,"data","enhanced_balance.json")))["hooks"]}
    # SHOTS.ASM launch and grenade displacement owners in retail L8.
    aim_sites = {
        0xffe35f40: (1389, "m.bind_shot_compass()"),
        0xffe346f0: (1102, "m.aim_launch(true)"),
        0xffe36510: (1480, "m.aim_launch(false)"),
        0xffe373d0: (1674, "m.aim_launch(c.a[5] == 0x4201)"),
        0xffe35160: (1250, "m.aim_launch(false)"),
        0xffe354e0: (1284, "m.aim_grenade_step(false)"),
        0xffe356c0: (1305, "m.aim_grenade_step(true)"),
    }
    balance_patched = set()
    data_patched = set()
    files = [chunks[i:i + per_file] for i in range(0, len(chunks), per_file)]
    fn_names = []
    for fi, group in enumerate(files):
        lines = ['// GENERATED by tools/gsp2cpp/emit.py from the user\'s L8 program ROM. Private: do not redistribute.',
                 '#include "stv/gsp/ops.hpp"', '#include "stv/gsp/scene.hpp"', "namespace stv::gsp {", "#if defined(__GNUC__)",
                 '#pragma GCC diagnostic ignored "-Wunused-label"', "#endif"]
        for ch in group:
            name = f"l8c_{ch[0].pc:08x}"
            fn_names.append((ch[0].pc, ch[-1].pc + ch[-1].words * 16, name))
            pcs = {i.pc for i in ch}
            lines.append(f"u32 {name}(Machine& m, u32 pc) {{")
            lines.append("  Cpu& c = m.c;")
            lines.append("  switch (pc) {")
            for i in ch:
                lines.append(f"  case {h(i.pc)}: goto L_{i.pc:08x};")
            lines.append("  default: m.fault(\"entry is not an instruction start\", pc); return pc; }")
            for i in ch:
                note = src.get(i.pc, "")
                lines.append(f"L_{i.pc:08x}: // {i.text}" + (f"  [{note}]" if note else ""))
                lines.append(f"  l8_cov[{len(index_rows)}] = 1;")   # executed-instruction coverage (P5)
                try:
                    body, kind = em.emit(i, pcs)
                    status = "translated"
                except Unsupported as e:
                    unsupported[i.mnem + " " + i.text] = unsupported.get(i.mnem + " " + i.text, 0) + 1
                    body = [f'm.fault("untranslated instruction: {i.text}", {h(i.pc)}); return {h(i.pc)};']
                    kind = "end"
                    status = "blocked"
                if i.pc in data_sites:
                    root = data_sites[i.pc]
                    if not matches_reference(root, "instruction", i.text):
                        raise SystemExit(f"data-root {root['root_name']} at {i.pc:08x}: instruction changed")
                    old = root["reference"].lower() + "u"
                    occurrences = sum(b.count(old) for b in body)
                    if occurrences != 1:
                        raise SystemExit(f"data-root {root['root_name']} at {i.pc:08x}: expected one {old}, got {occurrences}")
                    policy = "enhanced_world_root" if root["root_name"] == "GAMEMODS" else "enhanced_scene"
                    if root["root_name"] in ("player_x widescreen movement", "player_y widescreen movement",
                                             "shoes_x widescreen movement", "shoes_y widescreen movement"):
                        policy = "enhanced_player_motion"
                    if root.get("root_policy") == "studio":
                        policy = "enhanced_studio"
                    new = f"({policy}(m) ? enhanced_data_address({old}) : {old})"
                    body = [b.replace(old, new, 1) if old in b else b for b in body]
                    data_patched.add(i.pc)
                pt = patches_pc.get(i.pc) or patches.get(note)
                if pt is not None:
                    if i.pc in patches_pc:
                        if not matches_reference(pt, "instruction", i.text):
                            raise SystemExit(f"patch point {pt['name']} at {i.pc:08x}: instruction changed")
                        explicit_patched.add(i.pc)
                    old = pt["reference_imm"].lower() + "u"
                    # "studio" points belong to the widened TV studio even when the
                    # writer runs on a native page (contestants for circuits 2 and 3
                    # are placed and scripted during the bonus tally).
                    guards = {"scene": "enhanced_scene(m)", "studio": "enhanced_studio(m)"}
                    if pt.get("condition", "scene") not in guards:
                        raise SystemExit(f"patch point {pt['name']}: unknown condition")
                    guard = guards[pt.get("condition", "scene")]
                    new = f"({guard} ? {pt['enhanced_imm'].lower()}u : {old})"
                    k = next((j for j, b in enumerate(body) if old in b), None)
                    if k is None:
                        # 16-bit immediates are emitted as s16(decimal)
                        old = f"s16({int(pt['reference_imm'], 16)})"
                        new = f"s16({guard} ? {int(pt['enhanced_imm'], 16)} : {int(pt['reference_imm'], 16)})"
                        k = next((j for j, b in enumerate(body) if old in b), None)
                    if k is None:
                        raise SystemExit(f"patch point {pt['name']} at {i.pc:08x}: {old} not in emitted code")
                    body = list(body)
                    body[k] = body[k].replace(old, new, 1)
                    patched.append(pt["name"])
                if i.pc in (0xffe03030, 0xffe03380, 0xffe035e0):
                    # INSOBJ and both successful OFREE returns (DELOBJ / KILOBJ).
                    line, event, reg = {0xffe03030: (1035, "Insert", 0),
                                        0xffe03380: (1107, "Retire", 0),
                                        0xffe035e0: (1153, "Retire", 2)}[i.pc]
                    if note != f"NDSP1.ASM:{line}":
                        raise SystemExit("Changed retail foreground object lifecycle")
                    body.insert(0, f"if (m.on_object) m.on_object(ObjectEvent::{event}, c.a[{reg}], 0);")
                    if event == "Insert":
                        # Presentation-only spawn anchor; no machine state changes.
                        body.insert(0, "m.object_inserted(c.a[0]);")
                if note in ("COLL2.ASM:375", "COLL2.ASM:382"):
                    if i.pc not in (0xffe55800, 0xffe55850):
                        raise SystemExit("Changed accepted collision dispatch")
                    body.insert(0, "if (m.on_object) m.on_object(ObjectEvent::Contact, c.a[8], c.a[0]);")
                if i.pc == 0xffe30980 and note == "SHOTS.ASM:628":
                    body.insert(0, "if (m.on_object) m.on_object(ObjectEvent::SmartBombStart, c.a[9], 0);")
                if note in ("SHOTS.ASM:644", "SHOTS.ASM:654", "SHOTS.ASM:664", "SHOTS.ASM:674",
                            "SHOTS.ASM:683", "SHOTS.ASM:693", "SHOTS.ASM:703", "SHOTS.ASM:712", "SHOTS.ASM:722"):
                    body.insert(0, "if (m.on_object) m.on_object(ObjectEvent::SmartBombVictim, c.a[8], c.a[9]);")
                # One notification at the confirmed arm separation or eye blowout.
                wound_sites = {0xffef53f0: ("MUTOID.ASM:679>CREATE:301", 10),
                               0xffef5830: ("MUTOID.ASM:699>CREATE:301", 10),
                               0xfff252b0: ("RBOSS.ASM:1549", 8)}
                if i.pc in wound_sites:
                    owner, reg = wound_sites[i.pc]
                    if note != owner:
                        raise SystemExit("Changed retail machine wound owner")
                    body.insert(0, f"if (m.on_object) m.on_object(ObjectEvent::MachineWound, c.a[{reg}], 0);")
                if note in ("FATMAN.ASM:261", "FATMAN.ASM:388", "HULKS.ASM:1408"):
                    body.insert(0, "if (m.on_object) m.on_object(ObjectEvent::Blast, c.a[8], 0);")
                if i.pc == 0xffe0ac50:  # GETPRC links the child, before its first wake.
                    if note != "MPROC.ASM:219":
                        raise SystemExit("Changed retail process allocation owner")
                    body.insert(0, "if (m.enhanced && c.a[1]==286) m.latch_shot_aim();")
                if i.pc == 0xffe1f840:  # UPDATE_PLYRXY after both axis-boundary checks
                    if i.text != "MOVE A3,A5": raise SystemExit("Changed retail walk direction")
                    body.insert(0, "if (m.enhanced) m.analog_walk_facing();")
                if i.pc == 0xffe1ff50:  # STUFF_VEL, common normal/shoes write to both objects
                    if i.text != "MOVE A1,*A8({W1}),1":
                        raise SystemExit("Changed retail player velocity write")
                    body.insert(0, "if (m.enhanced) m.analog_velocity();")
                if i.pc == 0xffe017c0:  # NDSP1.DVEL, after MMFM and before X/Y integration
                    if note != "NDSP1.ASM:573" or i.text != "ADD A12,A10":
                        raise SystemExit("Changed retail object motion integration")
                    body.insert(0, "if (m.enhanced) m.boost_enemy_motion();")
                if i.pc == 0xffe00230:  # DCLIPL, before clipping and beam splitting
                    body.insert(0, "if (m.enhanced) m.queue_aimed_sprite();")
                if i.pc in aim_sites:
                    line, hook = aim_sites[i.pc]
                    if note != f"SHOTS.ASM:{line}":
                        raise SystemExit(f"Changed analog weapon owner at {i.pc:08x}: {note}")
                    body.insert(0, f"if (m.enhanced) {hook};")
                if i.pc in balance_hooks:
                    rule = balance_hooks[i.pc]
                    if not matches_reference(rule, "instruction", i.text) or not matches_reference(rule, "operands", [[o.kind,o.reg,o.value] for o in i.ops]):
                        raise SystemExit(f"balance instruction changed at {i.pc:08x}")
                    body = list(body)
                    if rule.get("policy") == "mutoid_lineup_parent":
                        body.insert(0, "if (m.enhanced && !enhanced_mutoid_lineup_parent(m)) { c.cyc += 3; return m.boundary(0xffef8740u); }")
                    elif rule.get("policy") == "tank_reaction_parent":
                        body.insert(0, "if (m.enhanced && !enhanced_tank_reaction_parent(m)) { c.cyc += 3; return m.boundary(0xffe0a5f0u); }")
                    elif rule.get("policy") == "room16_coop_robot_limit":
                        # Replace the byte value at this single SETMAX load, before
                        # MOVB sets flags and ONEPADJ/ADJMAX apply retail modifiers.
                        load = "rb(m,c.a[0])"
                        line = next(j for j,b in enumerate(body) if load in b)
                        body[line] = body[line].replace(load,
                            "(c.a[0] == enhanced_data_address(0xffe13098u) && enhanced_scene(m) && "
                            "m.rw(0x010ee600u) == 3 ? 26u : rb(m,c.a[0]))")
                    elif rule.get("policy") == "coop_cobra_threshold":
                        # Adjust this unique COBRAKIL read before MOVE sets flags.
                        # STATUS3 retains the retail co-op branch and difficulty;
                        # the later head-hit comparison still owns the finish.
                        load = "rf(m,0x010de340u,0)"
                        line = next(j for j,b in enumerate(body) if load in b)
                        body[line] = body[line].replace(load,
                            "(enhanced_scene(m) && m.rw(0x010eb610u) == 2 && "
                            "m.rw(0x010eb630u) == 7 && m.rw(0x010ee600u) == 3 ? "
                            f"u32(u64({load}) * 7 / 8) : {load})")
                    elif rule.get("policy") == "solo_cobra_threshold":
                        # Only the solo path executes SUBI 250. CMP consumes A1
                        # next; keep the original SUBI flags and guest cycles.
                        line = next(j for j,b in enumerate(body) if b.startswith("c.cyc +="))
                        body.insert(line, "if (enhanced_scene(m) && m.rw(0x010eb610u) == 2 && "
                                    "m.rw(0x010eb630u) == 7) c.a[1] = c.a[1] * 4 / 5;")
                    else:
                        reg = f"c.a[{rule['register']}]"
                        numerator, denominator = rule.get("motion_ratio", [5, 4])
                        adjustment = f"if (enhanced_scene(m)) {reg} = u32(s32(s64(s32({reg}))*{numerator}/{denominator}));"
                        body.insert(0 if rule["phase"] == "before" else next(j for j,b in enumerate(body) if b.startswith("c.cyc +=")), adjustment)
                    balance_patched.add(i.pc)
                if i.pc in scene_sites:
                    store = scene_sites[i.pc]["store_cpp"]
                    if body.count(store) != 1:
                        raise SystemExit(f"scene-write at {i.pc:08x}: expected one exact store")
                    body = list(body)
                    body.insert(body.index(store) + 1, "sync_scene_bounds(m);")
                    scene_patched.add(i.pc)
                # BAKMODS changes page ownership without a GAMSTATE write.
                # Refresh only after the complete32-bit retail store, preserving
                # custom windows and all reference-profile state/cycle counts.
                if (i.mnem == "MOVE" and len(i.ops) == 3 and
                        i.ops[1].kind == "ABS" and i.ops[1].value == 0x010dd4e0 and
                        i.ops[2].value == 1):
                    stores = [j for j,b in enumerate(body) if b.startswith("wf(m,0x010dd4e0u,")]
                    if len(stores) != 1:
                        raise SystemExit(f"background-write at {i.pc:08x}: expected one full store")
                    body = list(body)
                    body.insert(stores[0]+1, "sync_scene_bounds(m);")
                lines.extend("  " + b for b in body)
                index_rows.append(f"{i.pc:08x},{i.text},{note},{name},{status}")
            last = ch[-1]
            lines.append(f"  return {h(last.pc + last.words * 16)};")
            lines.append("}")
        lines.append("} // namespace stv::gsp")
        with open(os.path.join(OUT, f"l8_chunk_{fi:03d}.cpp"), "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    if explicit_patched != set(patches_pc):
        missing = sorted(set(patches_pc) - explicit_patched)
        raise SystemExit("unresolved explicit geometry patch sites: " + ", ".join(f"{pc:08x}" for pc in missing))
    if data_patched != set(data_sites):
        missing = sorted(set(data_sites) - data_patched)
        raise SystemExit("unresolved enhancement data-root sites: " + ", ".join(f"{pc:08x}" for pc in missing))
    print(f"native enhancement data roots applied: {len(data_patched)}")
    if scene_patched != set(scene_sites):
        raise SystemExit("unresolved enhancement scene-write sites")
    print(f"native scene-bound refresh sites applied: {len(scene_patched)}")
    # dispatch
    d = ['// GENERATED by tools/gsp2cpp/emit.py. Private: do not redistribute.', '#include "stv/gsp/ops.hpp"',
         "#include <algorithm>", "namespace stv::gsp {"]
    for s, e, n in fn_names:
        d.append(f"u32 {n}(Machine&, u32);")
    d.append(f"u8 l8_cov[{len(index_rows)}];")
    d.append(f"const u32 l8_cov_count = {len(index_rows)};")
    d.append("namespace { struct Range { u32 lo, hi; ChunkFn fn; };")
    d.append("const Range ranges[] = {")
    for s, e, n in fn_names:
        d.append(f"  {{{h(s)}, {h(e)}, &{n}}},")
    d.append("}; }")
    d.append("ChunkFn l8_chunk_for(u32 pc) {")
    d.append("  auto it = std::upper_bound(std::begin(ranges), std::end(ranges), pc, [](u32 p, const Range& r) { return p < r.lo; });")
    d.append("  if (it == std::begin(ranges)) return nullptr; --it; return pc < it->hi ? it->fn : nullptr; }")
    d.append("} // namespace stv::gsp")
    with open(os.path.join(OUT, "l8_dispatch.cpp"), "w", newline="\n") as f:
        f.write("\n".join(d) + "\n")
    with open(os.path.join(OUT, "l8_translation_index.csv"), "w", newline="\n") as f:
        f.write("l8_address,instruction,source,chunk,status\n" + "\n".join(index_rows) + "\n")
    print(f"patch points applied: {len(patched)}")
    print(f"{len(starts)} instructions in {len(chunks)} chunks / {len(files)} files; "
          f"{sum(unsupported.values())} blocked in {len(unsupported)} forms")
    if balance_patched != set(balance_hooks):
        raise SystemExit("unexecuted balance hook binding")
    for k, v in sorted(unsupported.items(), key=lambda x: -x[1])[:30]:
        print("  blocked", v, k)


if __name__ == "__main__":
    main()
