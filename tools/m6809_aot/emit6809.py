#!/usr/bin/env python3
"""Static translator for the Smash TV sound CPU (MC6809E), PLAN P6.

Each instruction in the code map becomes one C++ function that performs the
MAME 0.289 m6809 microcode for that exact opcode and operand (m6809.lst,
base6x09.lst): the same memory accesses in the same order, one cycle per
access, and a sync point (SY) wherever the microcode has an '@' step. At a sync
point the runtime applies every scheduled event whose cycle threshold has been
reached, which is where MAME can end a timeslice mid-instruction.

Output (private, derived from the user's ROM): generated/private/snd/snd_code.cpp

  python tools/m6809_aot/emit6809.py <smashtv.zip>
"""
import argparse, hashlib, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from codemap6809 import load, descend, RAM_IMAGES
from decode6809 import decode

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PAGE_KEYS = ["u4.0", "u4.1", "u19.0", "u19.1", "u20.0", "u20.1", "ram"]
REG16 = {"X": "c.x", "Y": "c.y", "U": "c.u", "S": "c.s"}
IREG = ["c.x", "c.y", "c.u", "c.s"]


class Gen:
    def __init__(self, ins, page, data):
        self.i = ins
        self.page = page
        self.data = data
        self.L = []
        self.pc = ins.pc
        self.next = (ins.pc + ins.length) & 0xFFFF
        self.dyn_pc = False
        self.at = False

    # -- primitive steps ---------------------------------------------------
    def e(self, s):
        self.L.append(s)
        if self.at:                  # the '@' statement just emitted: suspend check follows it
            self.at = False
            self.L.append("SY;")

    def sy(self):
        """Mark the next emitted statement as an '@' step."""
        self.at = True

    def cyc(self, n):
        self.e(f"c.cyc += {n};")

    def at_cyc(self, n):          # '@' statement costing n cycles with no side effect
        self.sy()
        self.cyc(n)

    def arg(self):                 # read_opcode_arg(): one cycle, static byte
        b = self.data[self.pc]
        self.pc = (self.pc + 1) & 0xFFFF
        self.cyc(1)
        return b

    # -- operand access ----------------------------------------------------
    def opr8(self, mode):
        """C++ expression reading the 8-bit operand (read_operand)."""
        if mode == "imm8":
            return f"(uint8_t)0x{self.arg():02x}"
        if mode == "A":
            return "c.a"
        if mode == "B":
            return "c.b"
        return "c.rd(ea)"

    def opr16(self, mode, ordinal):
        if mode == "imm16":
            return f"(uint8_t)0x{self.arg():02x}"
        return f"c.rd((uint16_t)(ea + {ordinal}))" if ordinal else "c.rd(ea)"

    def wr8(self, mode, val):
        if mode == "A":
            self.e(f"c.a = {val};")
        elif mode == "B":
            self.e(f"c.b = {val};")
        elif mode == "imm8":
            pass
        else:
            self.e(f"c.wr(ea, {val});")

    # -- addressing modes (after the opcode fetch) --------------------------
    def direct(self):
        self.sy()
        b = self.arg()
        self.e(f"ea = (uint16_t)((c.dp << 8) | 0x{b:02x});")
        self.at_cyc(1)

    def extended(self):
        self.sy()
        h = self.arg()
        self.sy()
        l = self.arg()
        self.e(f"ea = 0x{(h << 8) | l:04x};")
        self.at_cyc(1)

    def indexed(self):
        self.sy()
        pb = self.arg()                  # @m_opcode = read_opcode_arg()
        r = IREG[(pb >> 5) & 3]
        if pb & 0x80:
            k = pb & 0x0F
            if k == 0x0:
                self.e(f"t = {r}; {r}++;"); self.at_cyc(1); self.at_cyc(2)
            elif k == 0x1:
                self.e(f"t = {r}; {r} += 2;"); self.at_cyc(1); self.at_cyc(3)
            elif k == 0x2:
                self.e(f"{r}--; t = {r};"); self.at_cyc(1); self.at_cyc(2)
            elif k == 0x3:
                self.e(f"{r} -= 2; t = {r};"); self.at_cyc(1); self.at_cyc(3)
            elif k == 0x4:
                self.e(f"t = {r};"); self.at_cyc(1)
            elif k == 0x5:
                self.e(f"t = (uint16_t)({r} + (int8_t)c.b);"); self.at_cyc(1); self.at_cyc(1)
            elif k == 0x6:
                self.e(f"t = (uint16_t)({r} + (int8_t)c.a);"); self.at_cyc(1); self.at_cyc(1)
            elif k == 0x8:
                self.sy(); o = self.arg()
                self.e(f"t = (uint16_t)({r} + {o - 256 if o & 0x80 else o});"); self.at_cyc(1)
            elif k == 0x9:
                self.sy(); h = self.arg(); self.sy(); l = self.arg()
                self.e(f"t = (uint16_t)({r} + 0x{(h << 8) | l:04x});"); self.at_cyc(3)
            elif k == 0xB:
                self.e(f"t = (uint16_t)({r} + c.d());"); self.at_cyc(1); self.at_cyc(1); self.at_cyc(3)
            elif k == 0xC:
                self.sy(); o = self.arg()
                self.e(f"t = 0x{(self.pc + (o - 256 if o & 0x80 else o)) & 0xFFFF:04x};"); self.at_cyc(1)
            elif k == 0xD:
                self.sy(); h = self.arg(); self.sy(); l = self.arg()
                v = (h << 8) | l
                self.e(f"t = 0x{(self.pc + v) & 0xFFFF:04x};"); self.at_cyc(4)
            elif k == 0xF:
                self.sy(); h = self.arg(); self.sy(); l = self.arg()
                self.e(f"t = 0x{(h << 8) | l:04x};"); self.at_cyc(1)
            else:
                self.e("t = 0;")
            if pb & 0x10:
                self.e("ea = t;")
                self.sy(); self.e("t8 = c.rd(ea);")
                self.sy(); self.e("t = (uint16_t)((t8 << 8) | c.rd((uint16_t)(ea + 1)));")
                self.at_cyc(1)
        else:
            o = pb & 0x0F
            o = o - 16 if pb & 0x10 else o
            self.e(f"t = (uint16_t)({r} + {o});"); self.at_cyc(1); self.at_cyc(1)
        self.sy()                       # @set_ea(m_temp.w)
        self.e("ea = t;")

    def address(self):
        m = self.i.mode
        if m == "dir":
            self.direct()
        elif m == "ext":
            self.extended()
        elif m == "idx":
            self.indexed()

    # -- stack helpers -----------------------------------------------------
    def push(self, mask, reg):
        other = "c.u" if reg == "c.s" else "c.s"
        seq = [(0x80, [f"(uint8_t){self.pcexpr()}", f"(uint8_t)({self.pcexpr()} >> 8)"]),
               (0x40, [f"(uint8_t){other}", f"(uint8_t)({other} >> 8)"]),
               (0x20, ["(uint8_t)c.y", "(uint8_t)(c.y >> 8)"]),
               (0x10, ["(uint8_t)c.x", "(uint8_t)(c.x >> 8)"]),
               (0x08, ["c.dp"]), (0x04, ["c.b"]), (0x02, ["c.a"]), (0x01, ["c.cc"])]
        for bit, vals in seq:
            if mask & bit:
                for v in vals:
                    self.sy(); self.e(f"--{reg}; c.wr({reg}, {v});")

    def pcexpr(self):
        return f"0x{self.next:04x}"

    def pull(self, mask, reg):
        other = "c.u" if reg == "c.s" else "c.s"
        if mask & 0x01:
            self.sy(); self.e(f"c.cc = c.rd({reg}++);")
        if mask & 0x02:
            self.sy(); self.e(f"c.a = c.rd({reg}++);")
        if mask & 0x04:
            self.sy(); self.e(f"c.b = c.rd({reg}++);")
        if mask & 0x08:
            self.sy(); self.e(f"c.dp = c.rd({reg}++);")
        for bit, r in ((0x10, "c.x"), (0x20, "c.y"), (0x40, other)):
            if mask & bit:
                self.sy(); self.e(f"t8 = c.rd({reg}++);")
                self.sy(); self.e(f"{r} = (uint16_t)((t8 << 8) | c.rd({reg}++));")
        if mask & 0x80:
            self.sy(); self.e(f"t8 = c.rd({reg}++);")
            self.sy(); self.e(f"c.pc = (uint16_t)((t8 << 8) | c.rd({reg}++));")
            self.dyn_pc = True
        self.sy(); self.e(f"(void)c.rd({reg});")

    def gosub(self, target_expr):
        self.sy(); self.e(f"--c.s; c.wr(c.s, 0x{self.next & 0xFF:02x});")
        self.sy(); self.e(f"--c.s; c.wr(c.s, 0x{self.next >> 8:02x});")
        self.jump(target_expr)

    def jump(self, expr):
        if isinstance(expr, int):
            self.e(f"c.pc = 0x{expr:04x};")
        else:
            self.e(f"c.pc = {expr};")
        self.dyn_pc = True

    def vector(self, addr):
        self.at_cyc(1)
        self.sy(); self.e(f"t8 = c.rd(0x{addr:04x});")
        self.sy(); self.e(f"t = (uint16_t)((t8 << 8) | c.rd(0x{addr + 1:04x}));")
        self.at_cyc(1)
        self.jump("t")

    # -- instruction body ----------------------------------------------------
    def body(self):
        i = self.i
        mn, mode = i.mnem, i.mode
        # opcode fetch (MAIN) and page prefix (DISPATCH10/11)
        self.sy(); self.arg()
        if i.page != 1:
            self.sy(); self.arg()
        if mn.endswith("A") and mode == "inh" and mn[:-1] in RMW or mn.endswith("B") and mode == "inh" and mn[:-1] in RMW:
            return self.rmw(mn[:-1], mn[-1])
        if mn in RMW:
            self.address()
            return self.rmw(mn, "M")
        if mn in ("TSTA", "TSTB"):
            return self.tst(mn[-1])
        if mn == "TST":
            self.address()
            return self.tst("M")
        if mn in ("CLRA", "CLRB"):
            return self.clr(mn[-1])
        if mn == "CLR":
            self.address()
            return self.clr("M")
        if mn == "JMP":
            self.address()
            if i.mode == "ext":
                return self.jump(i.value)
            return self.jump("ea")
        if mn[:-1] in ALU8 and mn[-1] in "AB" and mode in ("imm8", "dir", "ext", "idx"):
            reg = "c." + mn[-1].lower()
            self.address()
            src = self.opr8("imm8" if mode == "imm8" else "M")
            self.e(ALU8[mn[:-1]].format(r=reg, v=src))
            return
        if mn in ("STA", "STB"):
            reg = "c." + mn[-1].lower()
            self.address()
            self.e(f"c.wr(ea, flags_nzv8(c, {reg}));")
            return
        if mn in W16:
            reg, op = W16[mn]
            self.address()
            m = "imm16" if mode == "imm16" else "M"
            self.sy(); self.e(f"t8 = {self.opr16(m, 0)};")
            self.sy(); self.e(f"t = (uint16_t)((t8 << 8) | {self.opr16(m, 1)});")
            get = "c.d()" if reg == "D" else reg
            put = (lambda v: f"c.setd({v});") if reg == "D" else (lambda v: f"{reg} = {v};")
            if op == "LD":
                self.e(put("t") + " flags_nzv16(c, t);")
                if mn == "LDS":
                    self.e("c.lds = true;")
                return
            if op == "ADD":
                self.e(put(f"add16(c, {get}, t)"))
            elif op == "SUB":
                self.e(put(f"sub16(c, {get}, t)"))
            elif op == "CMP":
                self.e(f"(void)sub16(c, {get}, t);")
            self.at_cyc(1)
            return
        if mn in ST16:
            reg = ST16[mn]
            self.address()
            self.sy(); self.e(f"c.wr(ea, (uint8_t)({reg} >> 8));")
            self.sy(); self.e(f"c.wr((uint16_t)(ea + 1), (uint8_t){reg});")
            self.e(f"flags_nzv16(c, {reg});")
            return
        if mn == "NOP":
            return self.at_cyc(1)
        if mn == "SYNC":
            self.at_cyc(1)
            self.e("if (c.sync_wait()) return;")
            self.at_cyc(1)
            return
        if mn == "DAA":
            self.e("daa(c);")
            return self.at_cyc(1)
        if mn in ("ORCC", "ANDCC"):
            v = self.arg()
            self.e(f"c.cc |= 0x{v:02x};" if mn == "ORCC" else f"c.cc &= 0x{v:02x};")
            return self.at_cyc(1)
        if mn == "SEX":
            self.e("c.setd(flags_nz16(c, (uint16_t)(int16_t)(int8_t)c.b));")
            return self.at_cyc(1)
        if mode == "rel8" and mn not in ("BSR",):
            self.sy(); self.arg()
            self.at_cyc(1)
            return self.branch(mn[1:], i.target, long=False)
        if mode == "rel16" and mn != "LBSR":
            self.sy(); self.arg(); self.sy(); self.arg()
            self.at_cyc(1)
            return self.branch(mn[2:] if mn != "LBRA" else "RA", i.target, long=True)
        if mn == "BSR":
            self.sy(); self.arg()
            self.at_cyc(3)
            return self.gosub(i.target)
        if mn == "LBSR":
            self.sy(); self.arg(); self.sy(); self.arg()
            self.at_cyc(4)
            return self.gosub(i.target)
        if mn == "JSR":
            self.address()
            self.at_cyc(1)
            self.at_cyc(1)
            return self.gosub(i.value if mode == "ext" else "ea")
        if mn == "RTS":
            self.at_cyc(1)
            return self.pull(0x80, "c.s")
        if mn == "ABX":
            self.e("c.x = (uint16_t)(c.x + c.b);")
            self.at_cyc(1); self.at_cyc(1)
            return
        if mn == "MUL":
            self.e("mul(c);")
            self.at_cyc(1); self.at_cyc(9)
            return
        if mn == "RTI":
            self.at_cyc(1)
            self.sy(); self.e("c.cc = c.rd(c.s++);")
            self.e("if (c.cc & 0x80) {")
            self.pull(0xFE, "c.s")
            self.e("} else {")
            self.pull(0x80, "c.s")
            self.e("}")
            return
        if mn == "CWAI":
            self.sy(); v = self.arg(); self.e(f"c.cc &= 0x{v:02x};")
            self.at_cyc(1); self.at_cyc(1)
            self.e("c.cc |= 0x80;")
            self.push(0xFF, "c.s")
            self.e("if (c.cwai_wait()) return;")
            return self.jump("c.pc")
        if mn in ("LEAX", "LEAY"):
            self.address()
            r = REG16[mn[-1]]
            self.e(f"{r} = ea; c.cc = (uint8_t)((c.cc & ~0x04) | (ea == 0 ? 0x04 : 0));")
            return self.at_cyc(1)
        if mn in ("LEAU", "LEAS"):
            self.address()
            r = REG16[mn[-1]]
            self.e(f"{r} = ea;" + (" c.lds = true;" if mn == "LEAS" else ""))
            return self.at_cyc(1)
        if mn in ("PSHS", "PSHU"):
            r = "c.s" if mn == "PSHS" else "c.u"
            self.sy(); m = self.arg()
            self.at_cyc(2)
            self.sy(); self.e(f"(void)c.rd({r});")
            return self.push(m, r)
        if mn in ("PULS", "PULU"):
            r = "c.s" if mn == "PULS" else "c.u"
            self.sy(); m = self.arg()
            self.at_cyc(2)
            return self.pull(m, r)
        if mn == "SWI":
            self.e("c.cc |= 0x80;")
            self.at_cyc(1); self.at_cyc(1)
            self.push(0xFF, "c.s")
            self.e("c.cc |= 0x50;")
            return self.vector(0xFFFA)
        if mn in ("SWI2", "SWI3"):
            self.e("c.cc |= 0x80;")
            self.at_cyc(1); self.at_cyc(1)
            self.push(0xFF, "c.s")
            return self.vector(0xFFF4 if mn == "SWI2" else 0xFFF2)
        if mn in ("EXG", "TFR"):
            p = self.arg()
            self.e(f"c.pc = 0x{self.next:04x};")    # PC as seen by the microcode; a PC write overrides it
            self.dyn_pc = True
            if mn == "EXG":
                rd = "rd816" if p & 0x80 else "rd168"
                self.e(f"{{ uint16_t r1 = {rd}(c, {p >> 4}), r2 = {rd}(c, {p & 15});")
                self.e(f"wr_reg(c, {p & 15}, r1); wr_reg(c, {p >> 4}, r2); }}")
                self.at_cyc(6)
            else:
                self.e(f"wr_reg(c, {p & 15}, rd816(c, {p >> 4}));")
                if (p & 15) == 4:
                    self.e("c.lds = true;")
                self.at_cyc(4)
            return
        raise ValueError(f"no emitter for {mn} {mode} at {self.i.pc:04x}")

    def rmw(self, op, where):
        mode = {"A": "A", "B": "B", "M": "M"}[where]
        self.sy(); self.e(f"t8 = {self.opr8(mode)};")
        self.e(RMW[op])
        self.at_cyc(1)                  # @dummy_read_opcode_arg(0)
        self.sy(); self.wr8(mode, "t8")

    def tst(self, where):
        self.sy(); self.e(f"t8 = {self.opr8(where)};")
        self.e("flags_nzv8(c, t8);")
        self.at_cyc(1)
        if where == "M":
            self.at_cyc(1)

    def clr(self, where):
        self.sy(); self.e(f"(void){self.opr8(where)};")
        self.e("c.cc = (uint8_t)((c.cc & ~0x0F) | 0x04);")
        self.at_cyc(1)
        self.sy(); self.wr8(where, "0")

    def branch(self, cond, target, long):
        c = COND[cond]
        if long:
            if cond == "RA":
                self.at_cyc(1)
                return self.jump(target)
            if cond == "RN":
                return self.jump(self.next)
            self.e(f"if ({c}) {{")
            self.at_cyc(1)
            self.e(f"c.pc = 0x{target:04x}; }} else c.pc = 0x{self.next:04x};")
            self.dyn_pc = True
            return
        if cond == "RA":
            return self.jump(target)
        if cond == "RN":
            return self.jump(self.next)
        self.e(f"c.pc = ({c}) ? 0x{target:04x} : 0x{self.next:04x};")
        self.dyn_pc = True

    def generate(self):
        self.body()
        if self.at:
            self.at = False
            self.L.append("SY;")
        if not self.dyn_pc:
            self.e(f"c.pc = 0x{self.next:04x};")
        return self.L


RMW = {
    "NEG": "t8 = flags8(c, 0x0F, 0, t8, (uint32_t)(0 - t8));",
    "COM": "c.cc = (uint8_t)((c.cc & ~0x02) | 0x01); t8 = flags8(c, 0x0C, 0, (uint8_t)~t8, (uint8_t)~t8);",
    "LSR": "c.cc = (uint8_t)((c.cc & ~0x01) | (t8 & 1)); t8 = flags8(c, 0x0C, 0, (uint8_t)(t8 >> 1), (uint8_t)(t8 >> 1));",
    "ROR": "t8 = ror8(c, t8); t8 = flags8(c, 0x0C, 0, t8, t8);",
    "ASR": "c.cc = (uint8_t)((c.cc & ~0x01) | (t8 & 1)); t8 = (uint8_t)((int8_t)t8 >> 1); t8 = flags8(c, 0x0C, 0, t8, t8);",
    "ASL": "t8 = flags8(c, 0x0F, t8, t8, (uint32_t)t8 << 1);",
    "ROL": "t8 = flags8(c, 0x0E, t8, t8, rol8(c, t8));",
    "DEC": "t8 = flags8(c, 0x0E, t8, 1, (uint32_t)(t8 - 1));",
    "INC": "t8 = flags8(c, 0x0E, t8, 1, (uint32_t)(t8 + 1));",
}

ALU8 = {
    "SUB": "{{ uint8_t v = {v}; {r} = flags8(c, 0x0F, {r}, v, (uint32_t)({r} - v)); }}",
    "CMP": "{{ uint8_t v = {v}; (void)flags8(c, 0x0F, {r}, v, (uint32_t)({r} - v)); }}",
    "SBC": "{{ uint8_t v = {v}; {r} = flags8(c, 0x0F, {r}, v, (uint32_t)({r} - v - (c.cc & 1))); }}",
    "AND": "{{ c.cc &= ~0x02; uint8_t v = {v}; {r} = flags8(c, 0x0C, 0, {r}, (uint32_t)({r} & v)); }}",
    "BIT": "{{ c.cc &= ~0x02; uint8_t v = {v}; (void)flags8(c, 0x0C, 0, {r}, (uint32_t)({r} & v)); }}",
    "EOR": "{{ c.cc &= ~0x02; uint8_t v = {v}; {r} = flags8(c, 0x0C, 0, {r}, (uint32_t)({r} ^ v)); }}",
    "ADC": "{{ uint8_t v = {v}; {r} = flags8(c, 0x2F, {r}, v, (uint32_t)({r} + v + (c.cc & 1))); }}",
    "OR": "{{ c.cc &= ~0x02; uint8_t v = {v}; {r} = flags8(c, 0x0C, 0, {r}, (uint32_t)({r} | v)); }}",
    "ADD": "{{ uint8_t v = {v}; {r} = flags8(c, 0x2F, {r}, v, (uint32_t)({r} + v)); }}",
    "LD": "{{ {r} = {v}; flags_nzv8(c, {r}); }}",
}

W16 = {"ADDD": ("D", "ADD"), "SUBD": ("D", "SUB"), "CMPD": ("D", "CMP"),
       "CMPX": ("c.x", "CMP"), "CMPY": ("c.y", "CMP"), "CMPU": ("c.u", "CMP"), "CMPS": ("c.s", "CMP"),
       "LDX": ("c.x", "LD"), "LDY": ("c.y", "LD"), "LDU": ("c.u", "LD"), "LDS": ("c.s", "LD"),
       "LDD": ("D", "LD")}
ST16 = {"STX": "c.x", "STY": "c.y", "STU": "c.u", "STS": "c.s", "STD": "c.d()"}

COND = {"RA": "true", "RN": "false", "HI": "!(c.cc & 0x05)", "LS": "(c.cc & 0x05)", "CC": "!(c.cc & 0x01)",
        "CS": "(c.cc & 0x01)", "NE": "!(c.cc & 0x04)", "EQ": "(c.cc & 0x04)", "VC": "!(c.cc & 0x02)",
        "VS": "(c.cc & 0x02)", "PL": "!(c.cc & 0x08)", "MI": "(c.cc & 0x08)",
        "GE": "(!(c.cc & 0x08) == !(c.cc & 0x02))", "LT": "(!(c.cc & 0x08) != !(c.cc & 0x02))",
        "GT": "((!(c.cc & 0x08) == !(c.cc & 0x02)) && !(c.cc & 0x04))",
        "LE": "(!((!(c.cc & 0x08) == !(c.cc & 0x02)) && !(c.cc & 0x04)))"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip")
    ap.add_argument("--seeds")
    ap.add_argument("--out", default=os.path.join(ROOT, "generated", "private", "snd"))
    a = ap.parse_args()
    pages = load(a.zip)
    seeds = []
    for p, d in pages.items():
        if p == "ram":
            continue
        for off in range(0x7FF2, 0x8000, 2):
            v = (d.data[off] << 8) | d.data[off + 1]
            if v >= 0x8000:
                seeds.append((p, v))
    if a.seeds and os.path.exists(a.seeds):
        for line in open(a.seeds):
            line = line.split("#")[0].split()
            if line:
                seeds.append((line[0], int(line[1], 16)))
    m = descend(pages, seeds)
    bodies = {}          # body text -> function name
    table = {k: {} for k in PAGE_KEYS}
    ram_checks = {}
    for (page, pc), ins in sorted(m.code.items()):
        g = Gen(ins, page, pages[page])
        text = "\n".join("    " + l for l in g.generate())
        name = bodies.get(text)
        if name is None:
            name = f"i{len(bodies):05d}_{pc:04x}"
            bodies[text] = name
        table[page][pc] = name
        if page == "ram":
            ram_checks[pc] = bytes(pages["ram"][pc + k] for k in range(ins.length))
    os.makedirs(a.out, exist_ok=True)
    out = [
        "// GENERATED by tools/m6809_aot/emit6809.py from the user's Smash TV sound ROMs.",
        "// PRIVATE: derived from copyrighted ROM data. Never commit or redistribute.",
        "#include \"stv/snd/ops.hpp\"",
        "namespace stv::snd {",
        "#define SY if (c.cyc >= c.due && c.service()) return",
    ]
    for text, name in bodies.items():
        out.append(f"static void {name}(Cpu& c) {{")
        out.append("    uint16_t ea = 0, t = 0; uint8_t t8 = 0; (void)ea; (void)t; (void)t8;")
        out.append(text)
        out.append("}")
    out.append("#undef SY")
    for k in PAGE_KEYS:
        entries = table[k]
        out.append(f"static const Entry k_page_{k.replace('.', '_')}[] = {{")
        for pc, name in sorted(entries.items()):
            chk = ram_checks.get(pc, b"") if k == "ram" else b""
            chk_s = ", ".join(f"0x{b:02x}" for b in chk)
            out.append(f"    {{0x{pc:04x}, {name}, {len(chk)}, {{{chk_s}}}}},")
        out.append("};")
    out.append("const PageTable k_pages[7] = {")
    for k in PAGE_KEYS:
        n = f"k_page_{k.replace('.', '_')}"
        out.append(f"    {{\"{k}\", {n}, sizeof({n}) / sizeof({n}[0])}},")
    out.append("};")
    out.append("}  // namespace stv::snd")
    path = os.path.join(a.out, "snd_code.cpp")
    open(path, "w", newline="\n").write("\n".join(out) + "\n")
    print(f"{len(m.code)} instructions, {len(bodies)} unique functions -> {path}")


if __name__ == "__main__":
    main()
