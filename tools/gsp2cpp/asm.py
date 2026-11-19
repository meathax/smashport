#!/usr/bin/env python3
"""gsp2cpp front end: TI GSP (TMS34010) assembler and linker for the Smash TV source.

Development-only (PLAN.md D1/D2, phase P1). It reproduces the original build:
  - module list and link order from ROBO.CMD, memory map from a selectable .CMD
  - .INCLUDE / $MACRO / .IF expansion, sections, data directives
  - instruction encoding from isa_table.json (derived from pinned MAME unidasm)
Unknown constructs are hard errors. Nothing here is used by the shipping game.

  python tools/gsp2cpp/asm.py link --out build/p1/robo.bin --map build/p1/robo.map.json
"""
import argparse, json, os, re, struct, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
ARCHIVE = os.path.join(ROOT, "original", "archive", "smashtv-main")
SYNTH = os.path.join(ROOT, "generated", "gsp2cpp", "synth")
BACKUP_ALLOWED = {"BGNDTBL.ASM", "BGNDPAL.ASM", "IMGTBL.ASM", "IMGPAL.ASM", "ROBO.CMD"}


class AsmError(Exception):
    pass


# ---------------------------------------------------------------- values
class Val:
    """Value in bits/units with optional relocation: section-relative or external."""
    __slots__ = ("off", "sect", "ext", "unknown")

    def __init__(self, off=0, sect=None, ext=None, unknown=False):
        self.off, self.sect, self.ext, self.unknown = off, sect, ext, unknown

    @property
    def absolute(self):
        return self.sect is None and self.ext is None and not self.unknown

    def __repr__(self):
        base = self.ext or self.sect
        return f"Val({base}+{self.off:#x}{' ?' if self.unknown else ''})" if base else f"Val({self.off:#x}{' ?' if self.unknown else ''})"


def s32(x):
    """Value as a signed 32-bit quantity (TI range checks work on 32-bit words)."""
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x


def v_binop(op, a, b):
    unk = a.unknown or b.unknown
    if op == "+":
        if (a.sect or a.ext) and (b.sect or b.ext):
            if unk:
                return Val(0, unknown=True)
            raise AsmError(f"cannot add two relocatable values {a} {b}")
        r = a if (a.sect or a.ext) else b
        return Val(a.off + b.off, r.sect, r.ext, unk)
    if op == "-":
        if b.sect or b.ext:
            if a.sect == b.sect and a.ext == b.ext:
                return Val(a.off - b.off, unknown=unk)
            if unk:
                return Val(0, unknown=True)
            raise AsmError(f"cannot subtract relocatable {b} from {a}")
        return Val(a.off - b.off, a.sect, a.ext, unk)
    if not (a.sect is None and a.ext is None and b.sect is None and b.ext is None):
        if unk:
            return Val(0, unknown=True)
        raise AsmError(f"operator {op} needs absolute operands: {a} {b}")
    x, y = a.off, b.off
    if op == "*":
        r = x * y
    elif op == "/":
        if y == 0:
            if unk:
                return Val(0, unknown=True)
            raise AsmError("division by zero")
        r = int(x / y)
    elif op == "%":
        r = x - int(x / y) * y if y else 0
    elif op == "&":
        r = x & y
    elif op == "|":
        r = x | y
    elif op == "^":
        r = x ^ y
    elif op == "<<":
        r = x << y
    elif op == ">>":
        r = x >> y
    else:
        raise AsmError(f"operator {op}")
    return Val(r, unknown=unk)


TOKEN = re.compile(r"""\s*(?:
    (?P<str>'(?:[^']|'')*'|"(?:[^"]|"")*")|
    (?P<hexg>>[0-9A-Fa-f]+)|
    (?P<num>[0-9][0-9A-Za-z]*)|
    (?P<sym>[A-Za-z_$?.][A-Za-z0-9_$?.]*)|
    (?P<op><<|>>|[-+*/|&^~()%!\[\],])
    )""", re.X)

PRECEDENCE = {"|": 1, "^": 2, "&": 3, "<<": 4, ">>": 4, "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}


def parse_number(text):
    t = text.upper()
    if t.endswith("H") and re.fullmatch(r"[0-9][0-9A-F]*H", t):
        return int(t[:-1], 16)
    if re.fullmatch(r"[01]+B", t):
        return int(t[:-1], 2)
    if re.fullmatch(r"[0-7]+Q", t):
        return int(t[:-1], 8)
    if re.fullmatch(r"0X[0-9A-F]+", t):
        return int(t[2:], 16)
    if re.fullmatch(r"[0-9]+", t):
        return int(t, 10)
    raise AsmError(f"bad number {text}")


def char_value(body):
    body = body.replace("''", "'")
    v = 0
    for ch in body:
        v = (v << 8) | ord(ch)
    return v


class Expr:
    def __init__(self, text, lookup, precedence=True):
        self.toks = []
        pos = 0
        text = text.strip()
        while pos < len(text):
            # '>' is a hex prefix only where a value is expected; after a value
            # it is a comparison (or '>>' shift). Comparisons: < > <= >= = == <> !=
            prev_value = bool(self.toks) and (self.toks[-1][0] in ("num", "hexg", "str", "sym")
                                               or self.toks[-1][1] in (")", "]"))
            rest = text[pos:].lstrip()
            skip = len(text) - pos - len(rest)
            cm = re.match(r"(<=|>=|<>|!=|==|=|<(?!<)|>(?!>))", rest)
            if cm and prev_value:
                self.toks.append(("cmp", cm.group(1)))
                pos += skip + len(cm.group(1))
                continue
            m = TOKEN.match(text, pos)
            if not m or m.end() == pos:
                raise AsmError(f"bad expression {text!r} at {text[pos:]!r}")
            pos = m.end()
            kind = m.lastgroup
            self.toks.append((kind, m.group(kind)))
        self.i = 0
        self.lookup = lookup
        self.precedence = precedence
        self.text = text

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def primary(self):
        kind, t = self.take()
        if kind == "num":
            return Val(parse_number(t))
        if kind == "hexg":
            return Val(int(t[1:], 16))
        if kind == "str":
            return Val(char_value(t[1:-1]))
        if kind == "sym":
            return self.lookup(t)
        if kind == "op" and t == "[":
            y = self.binary(0)
            if self.take() != ("op", ","):
                raise AsmError(f"bad [Y,X] in {self.text}")
            x = self.binary(0)
            if self.take() != ("op", "]"):
                raise AsmError(f"missing ] in {self.text}")
            if not (y.absolute or y.unknown) or not (x.absolute or x.unknown):
                raise AsmError("[Y,X] needs absolute values")
            return Val(((y.off & 0xFFFF) << 16) | (x.off & 0xFFFF), unknown=x.unknown or y.unknown)
        if kind == "op" and t == "(":
            v = self.binary(0)
            if self.take() != ("op", ")"):
                raise AsmError(f"missing ) in {self.text}")
            return v
        if kind == "op" and t in "-+~!":
            v = self.primary()
            if t == "+":
                return v
            if not (v.sect is None and v.ext is None):
                raise AsmError(f"unary {t} on relocatable in {self.text}")
            if t == "-":
                return Val(-v.off, unknown=v.unknown)
            if t == "~":
                return Val(~v.off, unknown=v.unknown)
            return Val(0 if v.off else 1, unknown=v.unknown)
        raise AsmError(f"unexpected {t!r} in {self.text!r}")

    def binary(self, minp):
        left = self.primary()
        while True:
            kind, t = self.peek()
            if kind != "op" or t not in PRECEDENCE:
                return left
            p = PRECEDENCE[t] if self.precedence else 1
            if p < minp or (not self.precedence and minp > 1):
                return left
            self.take()
            right = self.binary(p + 1) if self.precedence else self.primary()
            left = v_binop(t, left, right)

    def compare(self):
        left = self.binary(0)
        while self.peek()[0] == "cmp":
            _, op = self.take()
            right = self.binary(0)
            if (left.sect, left.ext) != (right.sect, right.ext) and not (left.unknown or right.unknown):
                raise AsmError(f"comparison of unrelated values in {self.text}")
            a, b = left.off, right.off
            r = {"<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b, "=": a == b, "==": a == b,
                 "<>": a != b, "!=": a != b}[op]
            left = Val(1 if r else 0, unknown=left.unknown or right.unknown)
        return left

    def value(self):
        v = self.compare()
        if self.i != len(self.toks):
            raise AsmError(f"trailing tokens in {self.text!r}")
        return v


# ---------------------------------------------------------------- source files
class Files:
    def __init__(self, archive=ARCHIVE, synth=SYNTH, variants=None):
        self.index = {}
        for n in os.listdir(archive):
            p = os.path.join(archive, n)
            if os.path.isfile(p):
                self.index[n.upper()] = p
        for n in os.listdir(os.path.join(archive, "BACKUP")):
            if n.upper() in BACKUP_ALLOWED and n.upper() not in self.index:
                self.index[n.upper()] = os.path.join(archive, "BACKUP", n)
        self.synth = synth
        self.used = {}
        # \VIDEO\SYS\ was a shared directory outside the archive; the archive holds
        # several candidate copies (SYS.INC, SYS.RBO, SYS.OLD, BACKUP/SYS.INC).
        for base, alt in (variants or {}).items():
            p = os.path.join(archive, alt)
            if not os.path.isfile(p):
                raise AsmError(f"variant {alt} not found")
            self.index[base.upper()] = p

    def path(self, name):
        base = re.split(r"[\\/:]", name.strip().strip('"').strip("'"))[-1].upper()
        p = self.index.get(base)
        if not p and os.path.isdir(self.synth):
            for n in os.listdir(self.synth):
                if n.upper() == base:
                    p = os.path.join(self.synth, n)
        if not p:
            raise AsmError(f"include not found: {name}")
        self.used[base] = p
        return p

    def lines(self, name):
        p = self.path(name)
        with open(p, "rb") as f:
            data = f.read().decode("latin-1")
        data = data.split("\x1a")[0]
        return p, data.replace("\r\n", "\n").replace("\r", "\n").split("\n")


# ---------------------------------------------------------------- ISA
REG_RE = re.compile(r"^(A(?:1[0-4]|[0-9])|B(?:1[0-4]|[0-9])|SP|A15|B15)$", re.I)
JR_CONDS = {"UC": 0x0, "": 0x0, "P": 0x1, "LS": 0x2, "HI": 0x3, "LT": 0x4, "GE": 0x5, "LE": 0x6, "GT": 0x7,
            "C": 0x8, "LO": 0x8, "B": 0x8, "NC": 0x9, "HS": 0x9, "NB": 0x9, "EQ": 0xA, "Z": 0xA,
            "NE": 0xB, "NZ": 0xB, "V": 0xC, "NV": 0xD, "N": 0xE, "NN": 0xF,
            # CMPXY: N=(X==0), V=X<0, Z=(Y==0), C=Y<0 (MAME CMP_XY). Unverified TI aliases
            # were confirmed by the LA5 byte comparison (XLE = LT = N^V, XGT = GE).
            "YLT": 0x8, "YN": 0x8, "YGE": 0x9, "YNN": 0x9, "YZ": 0xA, "YNZ": 0xB, "YLE": 0x2, "YGT": 0x3,
            "XLT": 0xC, "XN": 0xC, "XGE": 0xD, "XNN": 0xD, "XZ": 0xE, "XNZ": 0xF, "XLE": 0x4, "XGT": 0x5}


def norm_reg(r):
    r = r.upper()
    return "SP" if r in ("A15", "B15") else r


def reg_file(r):
    return r[0] if r != "SP" else "?"


def parse_template_operand(t):
    if t.startswith("-*"):
        return ("DEC", t[2:])
    if t.startswith("*"):
        m = re.fullmatch(r"\*([AB]\d+|SP)\((\{[^}]+\})\)", t)
        if m:
            return ("DISP", m.group(1), m.group(2)[1:-1])
        if t.endswith("+"):
            return ("INC", t[1:-1])
        return ("IND", t[1:])
    if t.startswith("@"):
        return ("ABS", t[2:-1])
    if t.startswith("{"):
        return ("IMM", t[1:-1])
    if REG_RE.match(t):
        return ("R", t)
    if t in ("XY", "L", "B"):
        return ("LIT", t)
    if re.fullmatch(r"[0-9A-F]+h", t):
        return ("K", int(t[:-1], 16))
    if re.fullmatch(r"[0-9]+", t):
        return ("K", int(t))
    raise AsmError(f"template operand {t}")


class ISA:
    def __init__(self, path=os.path.join(HERE, "isa_table.json")):
        data = json.load(open(path))["opcodes"]
        self.index = defaultdict(list)
        self.jr = {}        # cond -> base op
        self.ja = {}
        self.dsjs = {}      # reg -> {disp: op}
        self.text = {}
        self.pref = {}      # op -> preference rank among opcodes sharing its template (0 = TI form)
        canon_path = os.path.join(HERE, "isa_canon.json")
        learned_path = os.path.join(HERE, "isa_learned.json")
        canon = json.load(open(canon_path)) if os.path.exists(canon_path) else {}
        learned = json.load(open(learned_path)) if os.path.exists(learned_path) else {}
        groups = defaultdict(list)
        for ops, (n, text) in data.items():
            groups[text].append(int(ops, 16))
        for text, oplist in groups.items():
            oplist.sort()
            first = oplist[0]
            if text in learned:
                first = int(learned[text], 16)
            elif text in canon and len(canon[text]) > 1:
                (top, c1), (_, c2) = canon[text][0], canon[text][1]
                if c1 >= 50 and c1 >= 10 * c2:
                    first = int(top, 16)
            elif text in canon and canon[text][0][1] >= 50:
                first = int(canon[text][0][0], 16)
            for o in oplist:
                self.pref[o] = 0 if o == first else 1 + oplist.index(o)
        for ops, (n, text) in data.items():
            op = int(ops, 16)
            self.text[op] = text
            parts = text.split(" ", 1)
            mnem = parts[0]
            opnds = [x for x in parts[1].split(",")] if len(parts) > 1 else []
            if mnem in ("MMTM", "MMFM"):
                self.index[(mnem, ("R:" + opnds[0],))].append((op, n, mnem, [("R", opnds[0]), ("LIST",)]))
                continue
            if (op & 0xF000) == 0xC000:
                cond = (op >> 8) & 0xF
                if (op & 0xFF) == 0:
                    self.jr[cond] = op
                elif (op & 0xFF) == 0x80:
                    self.ja[cond] = op
                continue
            if mnem == "DSJS":
                k = (op >> 5) & 0x1F
                d = -k if op & 0x400 else k
                self.dsjs.setdefault(norm_reg(opnds[0]), {})[d] = op
                continue
            toks = [parse_template_operand(t) for t in opnds]
            key = (mnem, tuple(self.key_tok(t) for t in toks))
            self.index[key].append((op, n, mnem, toks))

    @staticmethod
    def key_tok(t):
        k = t[0]
        if k in ("R", "IND", "INC", "DEC", "DISP"):
            return f"{k}:{norm_reg(t[1])}"
        if k == "LIT":
            return f"LIT:{t[1]}"
        if k in ("IMM", "K"):
            return "#"
        return k


# ---------------------------------------------------------------- operands
def split_operands(text):
    out, depth, cur, q = [], 0, "", None
    for ch in text:
        if q:
            cur += ch
            if ch == q:
                q = None
            continue
        if ch in "'\"":
            q = ch
            cur += ch
        elif ch in "([":
            depth += 1
            cur += ch
        elif ch in ")]":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip() or out:
        out.append(cur.strip())
    return out


def strip_comment(line):
    q = None
    for i, ch in enumerate(line):
        if q:
            if ch == q:
                q = None
        elif ch in "'\"":
            q = ch
        elif ch == ";":
            return line[:i]
    return line


# ---------------------------------------------------------------- module assembler
class Section:
    def __init__(self, name, bss=False):
        self.name, self.bss = name, bss
        self.data = bytearray()
        self.loc = 0          # bits
        self.relocs = []      # (bit offset, kind, Val)
        self.align = 16

    def emit_bits(self, value, bits):
        if self.loc % 8 or bits % 8:
            raise AsmError(f"unaligned emission at bit {self.loc} in {self.name}")
        if not self.bss:
            b = self.loc // 8
            need = b + bits // 8
            if len(self.data) < need:
                self.data.extend(b"\0" * (need - len(self.data)))
            self.data[b:need] = (value & ((1 << bits) - 1)).to_bytes(bits // 8, "little")
        self.loc += bits


class Macro:
    def __init__(self, name, params, body, where):
        self.name, self.params, self.body, self.where = name, params, body, where


class Module:
    def __init__(self, name, files, isa, opts):
        self.name, self.files, self.isa, self.opts = name, files, isa, opts
        self.prev_syms = {}
        self.sticky_long = set()
        self.sticky_imm = set()

    # -- one pass
    def run_pass(self, final):
        self.syms = {}
        self.setsyms = {}
        self.regalias = {}
        self.globals = set()
        self.refs = set()
        self.sections = {}
        self.cur = self.section(".text")
        self.macros = {}
        self.cond = []
        self.instr_index = 0
        self.expansions = 0
        self.changed_size = False
        self.final = final
        self.errors = []
        self.listing = []
        self.ended = False
        self.include(self.name, None)

    def section(self, name, bss=False):
        if name not in self.sections:
            self.sections[name] = Section(name, bss)
        return self.sections[name]

    def include(self, name, where):
        path, lines = self.files.lines(name)
        fname = os.path.basename(path)
        self.process_lines([(fname, i + 1, l) for i, l in enumerate(lines)])

    def process_lines(self, lines):
        i = 0
        while i < len(lines) and not self.ended:
            fname, ln, text = lines[i]
            i += 1
            try:
                # macro definition capture
                m = re.match(r"^([A-Za-z_$][\w$?]*):?\s*\$MACRO\b(.*)$", strip_comment(text).rstrip(), re.I)
                if m:
                    params = [p.strip().upper() for p in m.group(2).split(",") if p.strip()]
                    body = []
                    while i < len(lines):
                        t = lines[i]
                        i += 1
                        if re.match(r"^\s+\$END\b", strip_comment(t[2])) or re.match(r"^\$END\b", t[2]):
                            break
                        body.append(t)
                    else:
                        raise AsmError(f"unterminated $MACRO {m.group(1)}")
                    if self.active():
                        self.macros[m.group(1).upper()] = Macro(m.group(1).upper(), params, body, (fname, ln))
                    continue
                self.statement(fname, ln, text)
            except AsmError as e:
                self.fail(fname, ln, text, e)

    def fail(self, fname, ln, text, e):
        msg = f"{fname}:{ln}: {e} :: {text.strip()[:100]}"
        if self.final:
            self.errors.append(msg)
        elif self.opts.get("strict_early"):
            raise AsmError(msg)

    def active(self):
        return all(self.cond)

    # -- symbols
    def lookup(self, name):
        if name == "$":
            return Val(self.cur.loc, self.cur.name)
        if name not in self.setsyms and name not in self.syms:
            self.fwd_flag = True  # not defined yet at this point of the pass
        for table in (self.setsyms, self.syms):
            if name in table:
                return table[name]
        if name in self.prev_syms:
            return self.prev_syms[name]
        if name in self.refs or name in self.globals:
            return Val(0, ext=name)
        if self.final:
            raise AsmError(f"undefined symbol {name}")
        return Val(0, unknown=True)

    def define(self, name, val):
        if name in self.syms and self.final:
            old = self.syms[name]
            if (old.off, old.sect, old.ext) != (val.off, val.sect, val.ext):
                raise AsmError(f"redefined {name}")
        self.syms[name] = val

    def eval(self, text):
        return Expr(text, self.lookup, self.opts.get("precedence", False)).value()

    # -- statements
    def statement(self, fname, ln, text):
        raw = text
        self.srcpos = (fname, ln, text.strip())
        if not text.strip() or text.lstrip().startswith(";"):
            return
        if text[0] == "*":
            return
        text = strip_comment(text).rstrip()
        if not text.strip():
            return
        label = None
        if not text[0].isspace():
            m = re.match(r"^([^\s:]+):?\s*(.*)$", text)
            label, text = m.group(1), m.group(2)
        else:
            text = text.strip()
        parts = text.split(None, 1)
        mnem = parts[0].upper() if parts else ""
        operand = parts[1].strip() if len(parts) > 1 else ""

        # conditional assembly is evaluated even when inactive
        if mnem == "$IF":
            # LA5 evidence: all 348 SLEEP/SLOOP expansions emit MOVI although the
            # macros read "$IF P1 < 33 / MOVK". The macro processor substitutes only
            # :P1: forms; the bare, undefined name P1 makes the condition false.
            val = False
            if self.active():
                def strict(name):
                    if name in self.setsyms or name in self.syms:
                        return self.lookup(name)
                    raise AsmError(f"undefined {name}")
                try:
                    val = bool(Expr(operand, strict, self.opts.get("precedence", False)).value().off)
                except AsmError:
                    val = False
            self.cond.append(val)
            return
        if mnem == ".IF":
            self.cond.append(bool(self.eval(operand).off) if self.active() else False)
            return
        if mnem in (".ELSE", "$ELSE"):
            if not self.cond:
                raise AsmError(".ELSE without .IF")
            outer = all(self.cond[:-1])
            self.cond[-1] = (not self.cond[-1]) and outer
            return
        if mnem in (".ENDIF", "$ENDIF"):
            if not self.cond:
                raise AsmError(".ENDIF without .IF")
            self.cond.pop()
            return
        if not self.active():
            return

        if mnem in (".EQU", ".SET", "EQU", "SET"):
            if not label:
                raise AsmError(f"{mnem} without label")
            reg = self.reg_name(operand.strip())
            if reg:
                self.regalias[label] = reg
                return
            v = self.eval(operand)
            if mnem in (".SET", "SET"):
                self.setsyms[label] = v
            else:
                self.define(label, v)
            return
        if label:
            self.define(label, Val(self.cur.loc, self.cur.name))
        if not mnem:
            return
        if mnem.upper() in self.macros:
            self.expand(self.macros[mnem.upper()], operand, fname, ln)
            return
        if mnem.startswith("."):
            self.directive(mnem, operand, fname, ln)
            return
        self.instruction(mnem, operand, fname, ln, raw)

    def expand(self, macro, operand, fname, ln):
        self.expansions += 1
        uniq = self.expansions
        args = split_operands(operand) if operand else []
        amap = {p: (args[i] if i < len(args) else "") for i, p in enumerate(macro.params)}
        body = []
        for (bf, bl, t) in macro.body:
            def sub(m):
                key = m.group(1).upper()
                if key in amap:
                    return amap[key]
                raise AsmError(f"unknown macro parameter {m.group(0)}")
            t2 = re.sub(r":([A-Za-z_]\w*)(?:\.S)?:", sub, t)
            t2 = re.sub(r"\b([A-Za-z_]\w*)\.S\b", lambda m: amap.get(m.group(1).upper(), m.group(0)), t2)
            # NAME? labels are local to one expansion
            t2 = re.sub(r"\b([A-Za-z_]\w*)\?", lambda m: f"{m.group(1)}_M{uniq}_", t2)
            body.append((f"{fname}:{ln}>{macro.name}", bl, t2))
        self.process_lines(body)

    def directive(self, d, operand, fname, ln):
        ops = split_operands(operand) if operand else []
        if d in (".TITLE", ".OPTION", ".WIDTH", ".MNOLIST", ".FILE", ".LIST", ".NOLIST", ".PAGE",
                 ".MLIST", ".LENGTH", ".TAB", ".NOLIST"):
            return
        if d == ".INCLUDE" or d == ".COPY":
            self.include(ops[0], (fname, ln))
            return
        if d == ".END":
            self.ended = True
            return
        if d in (".DEF", ".GLOBAL", ".GLOBL"):
            for s in ops:
                self.globals.add(s)
            return
        if d == ".REF":
            for s in ops:
                self.refs.add(s)
            return
        if d == ".TEXT":
            self.cur = self.section(".text")
            return
        if d == ".DATA":
            self.cur = self.section(".data")
            return
        if d == ".SECT":
            self.cur = self.section(ops[0].strip('"'))
            return
        if d == ".BSS":
            name, size = ops[0], self.eval(ops[1])
            if not size.absolute and not size.unknown:
                raise AsmError(".BSS size must be absolute")
            bss = self.section(".bss", bss=True)
            if len(ops) > 2 and self.eval(ops[2]).off:
                bss.loc = (bss.loc + 15) & ~15
            self.define(name, Val(bss.loc, ".bss"))
            bss.loc += size.off
            return
        if d == ".EVEN":
            if self.cur.loc % 16:
                self.cur.emit_bits(0, 16 - self.cur.loc % 16)
            return
        if d in (".WORD", ".LONG", ".BYTE", ".INT", ".HWORD"):
            width = {".WORD": 16, ".HWORD": 16, ".LONG": 32, ".INT": 32, ".BYTE": 8}[d]
            for o in ops:
                if o[:1] in "'\"" and o[-1:] == o[:1] and d == ".BYTE" and len(o) > 3:
                    for ch in o[1:-1].replace(o[0] * 2, o[0]):
                        self.cur.emit_bits(ord(ch), 8)
                    continue
                v = self.eval(o)
                self.data_value(v, width)
            return
        if d == ".STRING":
            for o in ops:
                if o[:1] in "'\"" and o[-1:] == o[:1]:
                    for ch in o[1:-1].replace(o[0] * 2, o[0]):
                        self.cur.emit_bits(ord(ch), 8)
                else:
                    self.cur.emit_bits(self.eval(o).off, 8)
            return
        raise AsmError(f"unsupported directive {d}")

    def data_value(self, v, width):
        if v.absolute or v.unknown:
            self.cur.emit_bits(v.off, width)
            return
        if width == 8:
            raise AsmError("relocatable byte")
        self.cur.relocs.append((self.cur.loc, "W" if width == 16 else "L", v))
        self.cur.emit_bits(v.off, width)

    # -- instructions
    def reg_name(self, t):
        t = t.strip()
        if REG_RE.match(t):
            return norm_reg(t)
        return self.regalias.get(t)

    def parse_operand(self, t):
        t = t.strip()
        m = re.fullmatch(r"(-?\*?)([A-Za-z_$][\w$]*)(.*)", t.replace(" ", ""))
        if m and m.group(2) in self.regalias and not REG_RE.match(m.group(2)):
            t = m.group(1) + self.regalias[m.group(2)] + m.group(3)
        u = t.upper().replace(" ", "")
        if u.startswith("*-") and REG_RE.match(u[2:]):
            u = "-*" + u[2:]
        if u.startswith("-*"):
            r = u[2:]
            if REG_RE.match(r):
                return [("DEC", norm_reg(r))]
        if u.startswith("*"):
            m = re.fullmatch(r"\*([AB](?:1[0-5]|[0-9])|SP)(\+|\.XY|\((.*)\))?", u)
            if m:
                r = norm_reg(m.group(1))
                if m.group(2) is None:
                    return [("IND", r)]
                if m.group(2) == "+":
                    return [("INC", r)]
                if m.group(2) == ".XY":
                    return [("IND", r), ("LIT", "XY")]
                inner = t[t.index("(") + 1:t.rindex(")")]
                return [("DISP", r, self.eval(inner))]
        if t.startswith("@"):
            return [("ABS", self.eval(t[1:]))]
        if REG_RE.match(u):
            return [("R", norm_reg(u))]
        if u in ("XY", "L", "B") and self._lit_ok:
            return [("LIT", u)]
        if u in ("L", "W") and t not in self.syms and t not in self.setsyms and t not in self.prev_syms:
            # field/immediate size keyword; modules without SYS.INC still use it
            return [("IMM", Val(1 if u == "L" else 0))]
        return [("IMM", self.eval(t))]

    def instruction(self, mnem, operand, fname, ln, raw):
        if self.cur.loc % 16:
            raise AsmError("instruction not word aligned")
        idx = self.instr_index
        self.instr_index += 1
        pc = self.cur.loc
        m = mnem.upper()
        self._lit_ok = m in ("PIXBLT", "FILL", "PIXT", "LINE")
        ops = []
        self.fwd_flag = False
        for o in split_operands(operand) if operand else []:
            ops.extend(self.parse_operand(o))
        # jumps
        jm = re.fullmatch(r"J([RA])(\w*)", m)
        if jm and jm.group(2).upper() in JR_CONDS and len(ops) == 1:
            return self.jump(jm.group(1), JR_CONDS[jm.group(2).upper()], ops[0], idx, pc)
        if m == "DSJS":
            return self.dsjs(ops, idx, pc)
        if m in ("MMTM", "MMFM"):
            return self.mm(m, operand)
        if m in ("DSJ", "DSJEQ", "DSJNE", "CALLR"):
            return self.rel16(m, ops, pc, idx)
        # unidasm prints these encodings under their alias mnemonics
        if m in ("ADDK", "SUBK") and len(ops) == 2 and ops[0][0] == "IMM" and ops[0][1].absolute and ops[0][1].off == 1:
            return self.generic("INC" if m == "ADDK" else "DEC", ops[1:], idx, pc)
        if m == "XOR" and len(ops) == 2 and ops[0] == ops[1] and ops[0][0] == "R":
            return self.generic("CLR", ops[1:], idx, pc)
        if m == "ANDNI":
            # ANDNI K,Rd is ANDI ~K,Rd (the ANDI encoding stores the complement)
            ops = [("IMM", Val(~ops[0][1].off, ops[0][1].sect, ops[0][1].ext, ops[0][1].unknown))] + ops[1:]
            m = "ANDI"
        return self.generic(m, ops, idx, pc)

    def emit_words(self, words, relocs=()):
        base = self.cur.loc
        for off, kind, v in relocs:
            self.cur.relocs.append((base + off, kind, v))
        for w in words:
            self.cur.emit_bits(w & 0xFFFF, 16)

    def target_disp(self, target, pc_after):
        """Displacement in words if target is in this section and known, else None."""
        if target.ext or target.sect != self.cur.name:
            return None
        if target.unknown:
            return None
        return (target.off - pc_after) // 16

    def jump(self, kind, cond, op, idx, pc):
        if op[0] != "IMM":
            raise AsmError("jump operand")
        target = op[1]
        if kind == "A":
            self.emit_words([self.isa.ja[cond], target.off & 0xFFFF, (target.off >> 16) & 0xFFFF],
                            [] if target.absolute or target.unknown else [(16, "L", target)])
            return
        if target.unknown:
            # first pass forward reference: optimistic short form, grown later if needed
            if idx in self.sticky_long or self.opts.get("forward_jr_long"):
                self.emit_words([self.isa.jr[cond], 0])
            else:
                self.emit_words([self.isa.jr[cond] | 0x01])
            return
        d8 = self.target_disp(target, pc + 16)
        if d8 == 0 and idx not in self.sticky_long:
            # LA5 evidence (NDSP1 ISOBJ "JRUC ISOBP"): the TI assembler emits NOP
            # for a relative jump to the next word, which the short form cannot encode.
            self.note(0x0300)
            self.emit_words([0x0300])
            return
        if d8 is not None and idx not in self.sticky_long and -127 <= d8 <= 127 and d8 != 0:
            self.note(self.isa.jr[cond] | (d8 & 0xFF))
            self.emit_words([self.isa.jr[cond] | (d8 & 0xFF)])
            return
        if self.pass_no >= 4:
            self.sticky_long.add(idx)  # only to force convergence after several passes
        self.note(self.isa.jr[cond])
        d16 = self.target_disp(target, pc + 32)
        if d16 is not None:
            self.emit_words([self.isa.jr[cond], d16 & 0xFFFF])
        else:
            self.emit_words([self.isa.jr[cond], 0], [(16, "REL16", target)])

    def dsjs(self, ops, idx, pc):
        reg, target = ops[0][1], ops[1][1]
        d = self.target_disp(target, pc + 16)
        if d is None:
            if self.final:
                raise AsmError("DSJS target not in section")
            d = -1
        table = self.isa.dsjs.get(reg, {})
        if d not in table:
            raise AsmError(f"DSJS displacement {d} out of range")
        self.note(table[d])
        self.emit_words([table[d]])

    def rel16(self, m, ops, pc, idx=None):
        if m == "CALLR":
            key, target = (m, ("#",)), ops[0][1]
        else:
            key, target = (m, ("R:" + ops[0][1], "#")), ops[1][1]
        entries = self.isa.index.get(key)
        if not entries:
            raise AsmError(f"no encoding for {m}")
        op = min(entries, key=lambda e: self.isa.pref.get(e[0], 99))[0]
        if m == "DSJ" and idx not in self.sticky_long:
            # LA5 evidence (UTIL CRLOAD "DSJ A6,CRLOAD1"): TI emits DSJS when it reaches
            d5 = self.target_disp(target, pc + 16)
            table = self.isa.dsjs.get(ops[0][1], {})
            if target.unknown and not target.ext:
                self.note(0x3800)
                self.emit_words([0x3800])
                return
            if d5 is not None and d5 in table:
                self.note(table[d5])
                self.emit_words([table[d5]])
                return
            if self.pass_no >= 4:
                self.sticky_long.add(idx)
        self.note(op)
        d = self.target_disp(target, pc + 32)
        if d is not None:
            if not -32768 <= d <= 32767:
                raise AsmError(f"{m} out of range")
            self.emit_words([op, d & 0xFFFF])
        elif target.unknown and not target.ext:
            self.emit_words([op, 0])
        else:
            self.emit_words([op, 0], [(16, "REL16", target)])

    def mm(self, m, operand):
        parts = split_operands(operand)
        rp = norm_reg(parts[0].strip())
        regs = []
        for p in parts[1:]:
            p = p.strip().upper()
            if "-" in p:
                a, b = [norm_reg(x) for x in p.split("-")]
                fa = a[0] if a != "SP" else rp[0]
                ia = 15 if a == "SP" else int(a[1:])
                ib = 15 if b == "SP" else int(b[1:])
                regs.extend(range(ia, ib + 1))
            else:
                r = norm_reg(p)
                regs.append(15 if r == "SP" else int(r[1:]))
        entries = self.isa.index.get((m, ("R:" + rp,)))
        if not entries:
            raise AsmError(f"no {m} for {rp}")
        if rp == "SP":
            # SP is A15 or B15: the register list selects the file
            files = {norm_reg(x.strip())[0] for x in re.split(r"[,\-]", ",".join(parts[1:])) if norm_reg(x.strip()) != "SP"}
            want = files.pop() if len(files) == 1 else "A"
            entries = sorted(entries, key=lambda e: (("B" if e[0] & 0x10 else "A") != want, e[0]))
        self.note(entries[0][0])
        mask = 0
        for r in regs:
            mask |= (1 << (15 - r)) if m == "MMTM" else (1 << r)
        self.emit_words([entries[0][0], mask])

    def generic(self, m, ops, idx, pc):
        force = None
        if m in ("MOVI", "ADDI", "SUBI", "CMPI", "ANDI", "ORI", "XORI") and len(ops) == 3 and ops[2][0] == "IMM":
            force = 3 if ops[2][1].off else 2
            ops = ops[:2]
        key = (m, tuple(self.isa.key_tok(o) if o[0] != "IMM" else "#" for o in ops))
        entries = self.isa.index.get(key)
        if not entries:
            # implicit field size 0
            key2 = (key[0], key[1] + ("#",))
            entries = self.isa.index.get(key2)
            if entries:
                ops = ops + [("IMM", Val(0))]
        if not entries:
            raise AsmError(f"no encoding for {m} {key[1]}")
        best = None
        for op, n, mn, toks in entries:
            ok, words, relocs = self.fit(op, n, toks, ops, idx, force)
            if ok and (force is None or n == force):
                rank = (n, self.sp_penalty(op, ops), self.isa.pref.get(op, 99))
                if best is None or rank < best[4]:
                    best = (n, words, relocs, op, rank)
        if best is None:
            raise AsmError(f"operands do not fit {m} {[str(o) for o in ops]}")
        n, words, relocs, op, _ = best
        if any(o[0] == "IMM" for o in ops) and (self.fwd_flag or any(o[0] == "IMM" and o[1].unknown for o in ops)):
            # TI sizes instructions in pass 1: a forward-referenced immediate stays long
            self.sticky_imm.add(idx)
        self.note(op)
        self.emit_words(words, relocs)

    def note(self, op):
        if self.final:
            self.listing.append((self.cur.name, self.cur.loc, op, self.srcpos))

    def sp_penalty(self, op, ops):
        """SP names A15 and B15; use the file of the instruction's other registers."""
        regs = [o[1] for o in ops if o[0] in ("R", "IND", "INC", "DEC", "DISP")]
        if "SP" not in regs:
            return 0
        files = {r[0] for r in regs if r != "SP"}
        if len(files) != 1:
            return 0
        want = files.pop()
        text = self.isa.text.get(op, "")
        # re-decode: the B-file bit is 0x10 for the register-field formats used with SP
        have = "B" if op & 0x10 else "A"
        return 0 if have == want else 1

    def fit(self, op, n, toks, ops, idx, force=None):
        """Try one encoding. Short immediates need a known absolute value in range."""
        words = [op] + [0] * (n - 1)
        relocs = []
        for t, o in zip(toks, ops):
            if t[0] == "K":
                if o[0] != "IMM" or not o[1].absolute or o[1].off != t[1]:
                    return False, None, None
                continue
            if t[0] == "LIT":
                continue
            slot = t[2] if t[0] == "DISP" else t[1] if t[0] in ("IMM", "ABS") else None
            if slot is None:
                continue
            v = o[2] if o[0] == "DISP" else o[1]
            neg = slot.startswith("~")
            s = slot.lstrip("~")
            wi = int(s[1:])
            val = ~v.off if neg else v.off
            if s[0] == "W":
                if t[0] == "IMM" and idx in self.sticky_imm and force != 2:
                    return False, None, None
                if t[0] == "IMM" and force == 2 and (v.absolute or v.unknown):
                    pass
                elif t[0] == "IMM" and not (v.absolute and -32768 <= s32(v.off) <= 32767):
                    return False, None, None
                if t[0] == "DISP" and not ((v.absolute or v.unknown) and -32768 <= v.off <= 65535):
                    return False, None, None
                words[wi] = val & 0xFFFF
            else:
                words[wi] = val & 0xFFFF
                words[wi + 1] = (val >> 16) & 0xFFFF
                if not v.absolute and not v.unknown:
                    relocs.append((wi * 16, "~L" if neg else "L", v))
        return True, words, relocs

    # -- driver
    def assemble(self, max_passes=10):
        prev_layout = None
        for p in range(max_passes):
            self.pass_no = p
            self.run_pass(final=False)
            layout = {k: s.loc for k, s in self.sections.items()}
            self.prev_syms = {**self.setsyms, **self.syms}  # forward refs see last .SET value
            if layout == prev_layout:
                break
            prev_layout = layout
        self.pass_no = max_passes
        self.run_pass(final=True)
        layout = {k: s.loc for k, s in self.sections.items()}
        if layout != prev_layout:
            self.errors.append(f"{self.name}: layout changed in final pass {prev_layout} -> {layout}")
        return self


# ---------------------------------------------------------------- linker
def parse_cmd(files, cmd_name):
    path = files.index[cmd_name.upper()] if not os.path.isabs(cmd_name) else cmd_name
    text = open(path, "rb").read().decode("latin-1")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    objs = [o.upper()[:-4] for o in re.findall(r"^\s*(\w+\.OBJ)", text, re.M | re.I)]
    mem = {m.group(1): (int(m.group(2), 16), int(m.group(3), 16)) for m in
           re.finditer(r"(\w+)\s*:\s*org\s*=\s*0x([0-9A-Fa-f]+),\s*len\s*=\s*0x([0-9A-Fa-f]+)", text)}
    groups = []
    sec_text = text[text.upper().index("SECTIONS"):]
    for m in re.finditer(r"GROUP\s*:\s*\{(.*?)\}\s*>\s*(\w+)|([\w.]+)\s*:\s*\{\s*\}\s*>\s*(\w+)", sec_text, re.S):
        if m.group(1) is not None:
            names = re.findall(r"([\w.]+)\s*:\s*\{\s*\}", m.group(1))
            groups.append((m.group(2), names))
        else:
            groups.append((m.group(4), [m.group(3)]))
    return objs, mem, groups


def link(objs, modules, mem, groups, fill=0xFF):
    place = {}  # (module, section) -> absolute bit address
    overflow = []
    for region, names in groups:
        org, length = mem[region]
        addr = org
        for sname in names:
            for mname in objs:
                mod = modules[mname]
                if sname in mod.sections:
                    s = mod.sections[sname]
                    addr = (addr + 15) & ~15
                    place[(mname, sname)] = addr
                    addr += s.loc
        if addr > org + length:
            overflow.append(f"region {region} overflow: {addr - org:#x} > {length:#x}")
    unplaced = [(m, s) for m in objs for s in modules[m].sections if (m, s) not in place and modules[m].sections[s].loc]
    gsyms, problems = {}, []
    for mname in objs:
        mod = modules[mname]
        for name, v in mod.syms.items():
            if name in mod.globals and v.sect is not None:
                a = place.get((mname, v.sect))
                if a is None:
                    problems.append(f"{mname}: symbol {name} in unplaced section {v.sect}")
                    continue
                if name in gsyms and gsyms[name] != a + v.off:
                    problems.append(f"duplicate global {name}")
                gsyms[name] = a + v.off
            elif name in mod.globals and v.absolute:
                gsyms.setdefault(name, v.off)
    return place, gsyms, unplaced, overflow + problems


def resolve(v, mname, place, gsyms):
    if v.ext:
        if v.ext not in gsyms:
            raise AsmError(f"unresolved external {v.ext}")
        return gsyms[v.ext] + v.off
    if v.sect:
        return place[(mname, v.sect)] + v.off
    return v.off


def build_image(objs, modules, place, gsyms, base, size, fill=0xFF):
    img = bytearray([fill]) * size
    mask = bytearray(size)  # 1 where a byte belongs to a relocated address field
    unresolved = defaultdict(int)
    for mname in objs:
        mod = modules[mname]
        for sname, s in mod.sections.items():
            a = place.get((mname, sname))
            if a is None or s.bss or not (base <= a < base + size * 8):
                continue
            data = bytearray(s.data[: s.loc // 8])
            if len(data) % 2:
                data.append(0)  # LA5 (DIAG.ASM ends on a .byte): sections are zero-padded to 16 bits
            for off, kind, v in s.relocs:
                try:
                    val = resolve(v, mname, place, gsyms)
                except AsmError as e:
                    unresolved[v.ext] += 1
                    continue
                b = off // 8
                width = 2 if kind in ("W", "REL16") else 4
                ms = (a - base) // 8 + b
                mask[ms:ms + width] = b"" * width
                if kind == "W":
                    struct.pack_into("<H", data, b, val & 0xFFFF)
                elif kind == "L":
                    struct.pack_into("<I", data, b, val & 0xFFFFFFFF)
                elif kind == "~L":
                    struct.pack_into("<I", data, b, ~val & 0xFFFFFFFF)
                elif kind == "REL16":
                    pc_after = a + off + 16
                    struct.pack_into("<H", data, b, ((val - pc_after) // 16) & 0xFFFF)
            start = (a - base) // 8
            img[start:start + len(data)] = data
    return img, dict(unresolved), mask


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["module", "link"])
    ap.add_argument("--module")
    ap.add_argument("--cmdfile", default="ROBO.CMD", help="object order")
    ap.add_argument("--memfile", default=os.path.join(ARCHIVE, "BACKUP", "ROBO.CMD"), help="memory map")
    ap.add_argument("--mem", action="append", help="REGION=hexorg override, e.g. ROM2=FFFF7000 (LA5)")
    ap.add_argument("--out")
    ap.add_argument("--map")
    ap.add_argument("--listing", help="write instruction listing and code-pointer relocations (JSON)")
    ap.add_argument("--report")
    ap.add_argument("--precedence", action="store_true", help="C-like precedence (TI GSP evaluates left to right; LA5 SOUNDS.ASM proves it)")
    ap.add_argument("--variant", action="append", default=None, help="BASE=FILE, e.g. SYS.INC=SYS.RBO")
    args = ap.parse_args()
    variants = dict(v.split("=", 1) for v in (args.variant or ["SYS.INC=SYS.RBO"]))
    files, isa = Files(variants=variants), ISA()
    opts = {"precedence": args.precedence}
    if args.cmd == "module":
        mod = Module(args.module, files, isa, opts).assemble()
        for e in mod.errors[:60]:
            print(e)
        print(f"{len(mod.errors)} errors; sections " + ", ".join(f"{k}={s.loc // 8}B" for k, s in mod.sections.items()))
        return 1 if mod.errors else 0
    objs, _, _ = parse_cmd(files, args.cmdfile)
    _, mem, groups = parse_cmd(files, args.memfile)
    for override in args.mem or []:
        name, org = override.split("=", 1)
        mem[name] = (int(org, 16), mem[name][1] - (int(org, 16) - mem[name][0]))
    modules, errors = {}, {}
    for o in objs:
        name = o + ".ASM"
        mod = Module(name, files, isa, opts).assemble()
        modules[o] = mod
        if mod.errors:
            errors[o] = mod.errors
    place, gsyms, unplaced, problems = link(objs, modules, mem, groups)
    base, size = 0xFFE00000, 0x40000
    img, unresolved, mask = build_image(objs, modules, place, gsyms, base, size)
    if args.out:
        open(args.out, "wb").write(img)
        open(args.out + ".relmask", "wb").write(mask)
    if args.listing:
        rows = []
        for o in objs:
            mod = modules[o]
            for sect, loc, op, src in mod.listing:
                a = place.get((o, sect))
                if a is not None:
                    rows.append([f"{a + loc:08x}", f"{op:04x}", src[0], src[1], src[2][:80]])
        # code pointers: 32-bit relocations whose value resolves to a listed instruction start
        starts = {r[0] for r in rows}
        ptrs = []
        for o in objs:
            mod = modules[o]
            for sname, s in mod.sections.items():
                a = place.get((o, sname))
                if a is None:
                    continue
                for off, kind, v in s.relocs:
                    if kind != "L":
                        continue
                    try:
                        val = resolve(v, o, place, gsyms)
                    except AsmError:
                        continue
                    if f"{val:08x}" in starts:
                        ptrs.append([f"{a + off:08x}", f"{val:08x}"])
        json.dump({"instructions": sorted(rows), "code_pointers": sorted(ptrs)}, open(args.listing, "w"))
    if args.map:
        json.dump({"sections": {f"{m}:{s}": f"{a:08x}" for (m, s), a in sorted(place.items(), key=lambda x: x[1])},
                   "globals": {k: f"{v:08x}" for k, v in sorted(gsyms.items(), key=lambda x: x[1])}},
                  open(args.map, "w"), indent=0)
    report = {"objects": len(objs), "module_errors": {k: len(v) for k, v in errors.items()},
              "first_errors": {k: v[:8] for k, v in errors.items()}, "unplaced": [f"{m}:{s}" for m, s in unplaced],
              "link_problems": problems[:50], "unresolved_externals": unresolved,
              "includes_used": sorted(files.used)}
    if args.report:
        json.dump(report, open(args.report, "w"), indent=1)
    print(json.dumps({k: v for k, v in report.items() if k != "first_errors"}, indent=1)[:4000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
