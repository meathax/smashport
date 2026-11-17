#!/usr/bin/env python3
"""Motorola 6809 decoder with cycle counts for the Smash TV sound ROM (PLAN D7, P6).

decode(mem, pc) -> Insn(pc, length, mnem, mode, operand info, cycles, targets)
Cycles follow the MC6809 datasheet (base + indexed extras); they are checked
against the MAME 0.289 m6809 core by the sound lockstep harness.

  python tools/m6809_aot/decode6809.py --selftest   linear decode of U4 vs unidasm
"""
import argparse, os, re, subprocess, sys, tempfile
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))

# mode: inh imm8 imm16 dir ext idx rel8 rel16 ; entries: (mnem, mode, cycles)
P1 = {}
def put(tbl, op, mn, mode, cyc):
    tbl[op] = (mn, mode, cyc)

for base, mode, cyc in ((0x00, "dir", 6), (0x60, "idx", 6), (0x70, "ext", 7)):
    for off, mn in ((0x0, "NEG"), (0x3, "COM"), (0x4, "LSR"), (0x6, "ROR"), (0x7, "ASR"), (0x8, "ASL"), (0x9, "ROL"),
                    (0xA, "DEC"), (0xC, "INC"), (0xD, "TST"), (0xF, "CLR")):
        put(P1, base + off, mn, mode, cyc)
    put(P1, base + 0xE, "JMP", mode, {"dir": 3, "idx": 3, "ext": 4}[mode])
for op, mn, mode, cyc in ((0x12, "NOP", "inh", 2), (0x13, "SYNC", "inh", 4), (0x16, "LBRA", "rel16", 5),
                          (0x17, "LBSR", "rel16", 9), (0x19, "DAA", "inh", 2), (0x1A, "ORCC", "imm8", 3),
                          (0x1C, "ANDCC", "imm8", 3), (0x1D, "SEX", "inh", 2), (0x1E, "EXG", "imm8", 8),
                          (0x1F, "TFR", "imm8", 6), (0x30, "LEAX", "idx", 4), (0x31, "LEAY", "idx", 4),
                          (0x32, "LEAS", "idx", 4), (0x33, "LEAU", "idx", 4), (0x34, "PSHS", "imm8", 5),
                          (0x35, "PULS", "imm8", 5), (0x36, "PSHU", "imm8", 5), (0x37, "PULU", "imm8", 5),
                          (0x39, "RTS", "inh", 5), (0x3A, "ABX", "inh", 3), (0x3B, "RTI", "inh", 6),
                          (0x3C, "CWAI", "imm8", 20), (0x3D, "MUL", "inh", 11), (0x3F, "SWI", "inh", 19)):
    put(P1, op, mn, mode, cyc)
for i, mn in enumerate(("BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ", "BVC", "BVS", "BPL", "BMI", "BGE", "BLT", "BGT", "BLE")):
    put(P1, 0x20 + i, mn, "rel8", 3)
for base, r in ((0x40, "A"), (0x50, "B")):
    for off, mn in ((0x0, "NEG"), (0x3, "COM"), (0x4, "LSR"), (0x6, "ROR"), (0x7, "ASR"), (0x8, "ASL"), (0x9, "ROL"),
                    (0xA, "DEC"), (0xC, "INC"), (0xD, "TST"), (0xF, "CLR")):
        put(P1, base + off, mn + r, "inh", 2)
ALU = ((0x0, "SUB"), (0x1, "CMP"), (0x2, "SBC"), (0x4, "AND"), (0x5, "BIT"), (0x6, "LD"), (0x8, "EOR"), (0x9, "ADC"),
       (0xA, "OR"), (0xB, "ADD"))
for hi, r in ((0x80, "A"), (0xC0, "B")):
    for mi, mode, c8 in ((0x00, "imm8", 2), (0x10, "dir", 4), (0x20, "idx", 4), (0x30, "ext", 5)):
        for off, mn in ALU:
            put(P1, hi + mi + off, mn + r, mode, c8)
        if mode != "imm8":
            put(P1, hi + mi + 0x7, "ST" + r, mode, c8)
# 16-bit column ops
sixteen = {
    0x83: ("SUBD", 4, 6, 6, 7), 0x8C: ("CMPX", 4, 6, 6, 7), 0x8E: ("LDX", 3, 5, 5, 6),
    0xC3: ("ADDD", 4, 6, 6, 7), 0xCC: ("LDD", 3, 5, 5, 6), 0xCE: ("LDU", 3, 5, 5, 6),
}
for op, (mn, ci, cd, cx, ce) in sixteen.items():
    put(P1, op, mn, "imm16", ci)
    put(P1, op + 0x10, mn, "dir", cd)
    put(P1, op + 0x20, mn, "idx", cx)
    put(P1, op + 0x30, mn, "ext", ce)
for op, mn in ((0x9F, "STX"), (0xDD, "STD"), (0xDF, "STU")):
    put(P1, op, mn, "dir", 5)
    put(P1, op + 0x10, mn, "idx", 5)
    put(P1, op + 0x20, mn, "ext", 6)
put(P1, 0x8D, "BSR", "rel8", 7)
put(P1, 0x9D, "JSR", "dir", 7)
put(P1, 0xAD, "JSR", "idx", 7)
put(P1, 0xBD, "JSR", "ext", 8)

P2 = {}
for i, mn in enumerate(("LBRA", "LBRN", "LBHI", "LBLS", "LBCC", "LBCS", "LBNE", "LBEQ", "LBVC", "LBVS", "LBPL", "LBMI", "LBGE", "LBLT", "LBGT", "LBLE")):
    if i:
        put(P2, 0x20 + i, mn, "rel16", 5)
put(P2, 0x3F, "SWI2", "inh", 20)
for op, (mn, ci, cd, cx, ce) in {0x83: ("CMPD", 5, 7, 7, 8), 0x8C: ("CMPY", 5, 7, 7, 8), 0x8E: ("LDY", 4, 6, 6, 7),
                                  0xCE: ("LDS", 4, 6, 6, 7)}.items():
    put(P2, op, mn, "imm16", ci)
    put(P2, op + 0x10, mn, "dir", cd)
    put(P2, op + 0x20, mn, "idx", cx)
    put(P2, op + 0x30, mn, "ext", ce)
for op, mn in ((0x9F, "STY"), (0xDF, "STS")):
    put(P2, op, mn, "dir", 6)
    put(P2, op + 0x10, mn, "idx", 6)
    put(P2, op + 0x20, mn, "ext", 7)
P3 = {}
put(P3, 0x3F, "SWI3", "inh", 20)
for op, (mn, ci, cd, cx, ce) in {0x83: ("CMPU", 5, 7, 7, 8), 0x8C: ("CMPS", 5, 7, 7, 8)}.items():
    put(P3, op, mn, "imm16", ci)
    put(P3, op + 0x10, mn, "dir", cd)
    put(P3, op + 0x20, mn, "idx", cx)
    put(P3, op + 0x30, mn, "ext", ce)

IDXREG = ("X", "Y", "U", "S")


@dataclass
class Idx:
    reg: str = None        # X Y U S PC or None (extended indirect)
    kind: str = ""         # off5 inc1 inc2 dec1 dec2 zero accB accA off8 off16 accD pc8 pc16 ext
    offset: int = 0
    indirect: bool = False
    extra: int = 0         # extra cycles
    nbytes: int = 0        # bytes after postbyte


@dataclass
class Insn:
    pc: int
    length: int
    page: int
    op: int
    mnem: str
    mode: str
    cycles: int
    value: int = None      # immediate / direct low byte / extended address / postbyte (EXG TFR PSH PUL)
    idx: Idx = None
    target: int = None
    text: str = ""


def decode_idx(mem, a, pc_after_fn):
    pb = mem(a)
    r = IDXREG[(pb >> 5) & 3]
    if not pb & 0x80:
        off = pb & 0x1f
        off = off - 32 if off & 0x10 else off
        return Idx(r, "off5", off, False, 1, 0), pb
    ind = bool(pb & 0x10)
    t = pb & 0x0f
    table = {0x0: ("inc1", 2, 0), 0x1: ("inc2", 3, 0), 0x2: ("dec1", 2, 0), 0x3: ("dec2", 3, 0), 0x4: ("zero", 0, 0),
             0x5: ("accB", 1, 0), 0x6: ("accA", 1, 0), 0x8: ("off8", 1, 1), 0x9: ("off16", 4, 2), 0xB: ("accD", 4, 0),
             0xC: ("pc8", 1, 1), 0xD: ("pc16", 5, 2), 0xF: ("ext", 5, 2)}
    if t not in table:
        return None, pb
    kind, extra, nb = table[t]
    if ind and kind in ("inc1", "dec1"):
        return None, pb
    if kind == "ext" and not ind:
        return None, pb
    off = 0
    if nb == 1:
        off = mem(a + 1)
        off = off - 256 if off & 0x80 else off
    elif nb == 2:
        off = (mem(a + 1) << 8) | mem(a + 2)
        if kind != "ext":
            off = off - 65536 if off & 0x8000 else off
    ix = Idx(None if kind == "ext" else ("PC" if kind.startswith("pc") else r), kind, off, ind, extra + (3 if ind else 0), nb)
    return ix, pb


def decode(mem, pc):
    """mem(addr) -> byte for the current bank mapping."""
    op = mem(pc)
    page, tbl, n = 1, P1, 1
    if op == 0x10:
        page, tbl, op, n = 2, P2, mem(pc + 1), 2
    elif op == 0x11:
        page, tbl, op, n = 3, P3, mem(pc + 1), 2
    if op not in tbl:
        return None
    mn, mode, cyc = tbl[op]
    ins = Insn(pc, 0, page, op, mn, mode, cyc)
    a = pc + n
    if mode == "inh":
        pass
    elif mode == "imm8":
        ins.value = mem(a); n += 1
        if mn in ("PSHS", "PULS", "PSHU", "PULU"):
            v = ins.value
            ins.cycles += sum(1 for b in range(8) if v & (1 << b)) + sum(1 for b in (4, 5, 6, 7) if v & (1 << b))
    elif mode == "imm16":
        ins.value = (mem(a) << 8) | mem(a + 1); n += 2
    elif mode == "dir":
        ins.value = mem(a); n += 1
    elif mode == "ext":
        ins.value = (mem(a) << 8) | mem(a + 1); n += 2
    elif mode == "idx":
        ix, pb = decode_idx(mem, a, None)
        if ix is None:
            return None
        n += 1 + ix.nbytes
        ins.idx = ix
        ins.cycles += ix.extra
        ins.value = pb
    elif mode == "rel8":
        d = mem(a); d = d - 256 if d & 0x80 else d
        n += 1
        ins.target = (pc + n + d) & 0xFFFF
    elif mode == "rel16":
        d = (mem(a) << 8) | mem(a + 1); d = d - 65536 if d & 0x8000 else d
        n += 2
        ins.target = (pc + n + d) & 0xFFFF
    ins.length = n
    if ins.idx and ins.idx.kind in ("pc8", "pc16"):
        ins.idx.offset = ins.idx.offset  # relative to PC after the instruction
    if mode == "ext" and mn in ("JMP", "JSR"):
        ins.target = ins.value
    if mode == "dir" and mn in ("JMP", "JSR"):
        ins.target = None     # depends on DP; resolved by the emitter when DP is known
    return ins


def selftest(path):
    rom = open(path, "rb").read()
    base = 0x8000
    # U4 is 64K; the CPU sees one 32K half at a time; test both halves at 0x8000
    bad = checked = 0
    for half in (0, 1):
        data = rom[half * 0x8000:(half + 1) * 0x8000]
        with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
            f.write(data); name = f.name
        out = subprocess.run([r"D:\Arcade\AI\mameexe\unidasm.exe", name, "-arch", "m6809", "-basepc", "8000"],
                             capture_output=True, text=True, check=True).stdout
        os.unlink(name)
        mem = lambda a: data[(a - base) & 0x7FFF] if base <= a < base + 0x8000 else 0
        for line in out.splitlines():
            m = re.match(r"([0-9a-f]+): ((?:[0-9a-f]{2} )+)\s*(\S+)", line)
            if not m:
                continue
            pc = int(m.group(1), 16)
            nbytes = len(m.group(2).split())
            mn = m.group(3).upper()
            if mn.startswith("FCB") or mn.startswith("???") or mn == "ILLEGAL":
                continue
            ins = decode(mem, pc)
            checked += 1
            if ins is None or ins.length != nbytes:
                bad += 1
                if bad <= 10:
                    print("  mismatch", hex(pc), line.strip()[:60], ins and (ins.mnem, ins.length))
    print(f"checked {checked} instructions, {bad} length/decode mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", metavar="U4_ROM")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest(a.selftest))
