#!/usr/bin/env python3
"""TMS34010 decoder for retail program ROMs (P2), built from isa_table.json.

decode(words, index, pc) returns an Insn with length, mnemonic, typed operands and
control-flow targets. Every opcode shape comes from the table derived from the
pinned MAME disassembler, so decoding cannot drift from the reference.

  python tools/gsp2cpp/decode.py --selftest [--rev l8]   linear decode of the whole ROM vs unidasm text
"""
import argparse, json, os, re, subprocess, sys, tempfile
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compare

REG = re.compile(r"^(?:[AB](?:1[0-4]|[0-9])|SP)$")


@dataclass
class Opnd:
    kind: str            # R IND INC DEC DISP ABS IMM K LIT
    reg: str = None
    value: int = None


@dataclass
class Insn:
    pc: int
    op: int
    words: int
    mnem: str
    ops: list = field(default_factory=list)
    target: int = None   # branch/call target (bit address) when static
    text: str = ""


def _tok(t):
    if t.startswith("-*"):
        return ("DEC", t[2:], None)
    if t.startswith("*"):
        m = re.fullmatch(r"\*([AB]\d+|SP)\((\{[^}]+\})\)", t)
        if m:
            return ("DISP", m.group(1), m.group(2)[1:-1])
        if t.endswith("+"):
            return ("INC", t[1:-1], None)
        return ("IND", t[1:], None)
    if t.startswith("@"):
        return ("ABS", None, t[2:-1])
    if t.startswith("{"):
        return ("IMM", None, t[1:-1])
    if REG.match(t):
        return ("R", t, None)
    if t in ("XY", "L", "B"):
        return ("LIT", t, None)
    if re.fullmatch(r"[0-9A-F]+h", t):
        return ("K", None, int(t[:-1], 16))
    if re.fullmatch(r"[0-9]+", t):
        return ("K", None, int(t))
    raise ValueError(t)


class Decoder:
    def __init__(self):
        with open(os.path.join(HERE, "isa_table.json")) as stream:
            data = json.load(stream)["opcodes"]
        self.table = {}
        for ops, (n, text) in data.items():
            parts = text.split(" ", 1)
            opnds = parts[1].split(",") if len(parts) > 1 else []
            if parts[0] in ("MMTM", "MMFM"):
                toks = [("R", opnds[0], None), ("LIST", None, None)]
            else:
                toks = [_tok(t) for t in opnds]
            self.table[int(ops, 16)] = (n, parts[0], toks, text)

    def decode(self, W, i, pc):
        """W: sequence of 16-bit words; i: index of the opcode word; pc: its bit address."""
        op = W[i]
        if op not in self.table:
            return None
        n, mnem, toks, text = self.table[op]
        if i + n > len(W):
            return None
        ext = W[i + 1:i + n]

        def slot(name):
            neg = name.startswith("~")
            s = name.lstrip("~")
            k = int(s[1:])
            if s[0] == "W":
                v = ext[k - 1]
            else:
                v = ext[k - 1] | (ext[k] << 16)
            if neg:
                v = ~v & (0xFFFF if s[0] == "W" else 0xFFFFFFFF)
            return v

        ins = Insn(pc, op, n, mnem)
        for kind, reg, x in toks:
            if kind == "LIST":
                mask = ext[0]
                ins.ops.append(Opnd("LIST", None, mask))
            elif kind in ("R", "IND", "INC", "DEC", "LIT"):
                ins.ops.append(Opnd(kind, reg))
            elif kind == "DISP":
                ins.ops.append(Opnd("DISP", reg, slot(x)))
            elif kind == "ABS":
                ins.ops.append(Opnd("ABS", None, slot(x)))
            elif kind == "K":
                ins.ops.append(Opnd("K", None, x))
            elif kind == "IMM":
                if x == "REL":
                    ins.target = self.rel_target(op, ext, pc)
                    ins.ops.append(Opnd("REL", None, ins.target))
                else:
                    ins.ops.append(Opnd("IMM", None, slot(x)))
        if mnem in ("CALLA", "JA", "JAC", "JAEQ", "JAGE", "JAGT", "JAHI", "JALE", "JALS", "JALT", "JAN",
                    "JANC", "JANE", "JANN", "JANV", "JAP", "JAV"):
            ins.target = ins.ops[0].value
        ins.text = text
        return ins

    @staticmethod
    def rel_target(op, ext, pc):
        if (op & 0xF000) == 0xC000:                  # JRcc short/long
            if op & 0xFF:
                d = op & 0xFF
                d = d - 256 if d & 0x80 else d
                return (pc + 16 + d * 16) & 0xFFFFFFFF
            d = ext[0] - 65536 if ext[0] & 0x8000 else ext[0]
            return (pc + 32 + d * 16) & 0xFFFFFFFF
        if (op & 0xF800) == 0x3800:                  # DSJS
            k = (op >> 5) & 0x1F
            return (pc + 16 + (-k if op & 0x400 else k) * 16) & 0xFFFFFFFF
        d = ext[0] - 65536 if ext[0] & 0x8000 else ext[0]   # CALLR, DSJ, DSJEQ, DSJNE
        return (pc + 32 + d * 16) & 0xFFFFFFFF

    @staticmethod
    def render(ins):
        """unidasm-compatible text, for the self-test."""
        out, vi = [], 0
        parts = ins.text.split(" ", 1)
        if parts[0] in ("MMTM", "MMFM"):
            return None
        body = parts[1] if len(parts) > 1 else ""

        def fill(m):
            nonlocal vi
            name = m.group(1)
            o = next(o for o in ins.ops[vi:] if o.kind in ("DISP", "ABS", "IMM", "REL"))
            vi = ins.ops.index(o) + 1
            return f"{o.value:X}h"
        body = re.sub(r"\{([^}]+)\}", fill, body)
        return (parts[0] + (" " + body if body else "")).strip()


def selftest(rev):
    rom = compare.retail(rev)
    W = [rom[k] | (rom[k + 1] << 8) for k in range(0, len(rom), 2)]
    with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
        f.write(rom)
        name = f.name
    out = subprocess.run([r"D:\Arcade\AI\mameexe\unidasm.exe", name, "-arch", "tms34010", "-basepc", "ffe00000"],
                         capture_output=True, text=True, check=True).stdout
    os.unlink(name)
    dec = Decoder()
    checked = mismatch = 0
    first = []
    for line in out.splitlines():
        m = re.match(r"([0-9a-f]+): ((?:[0-9a-f]{4} )+)\s*(.*)$", line)
        if not m:
            continue
        pc = int(m.group(1), 16)
        n = len(m.group(2).split())
        text = re.sub(r"\s+", " ", m.group(3).strip())
        if text.startswith("DW "):
            continue
        i = (pc - compare.BASE) // 16
        ins = dec.decode(W, i, pc)
        if ins is None:
            mismatch += 1
            first.append((hex(pc), "undecoded", text))
            continue
        if ins.words != n:
            mismatch += 1
            first.append((hex(pc), f"len {ins.words}!={n}", text))
            continue
        r = Decoder.render(ins)
        if r is None:
            checked += 1
            continue
        ref = text if not (ins.op & 0xFFE0) == 0x07E0 else "MOVB" + text[4:]
        if r != ref:
            mismatch += 1
            if len(first) < 10:
                first.append((hex(pc), r, ref))
        checked += 1
    print(f"{rev}: {checked} instructions checked against unidasm, {mismatch} mismatches")
    for f in first[:10]:
        print("  ", f)
    return 1 if mismatch else 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--rev", default="l8")
    a = ap.parse_args()
    sys.exit(selftest(a.rev) if a.selftest else 0)
