#!/usr/bin/env python3
"""Sound CPU code map for the Smash TV CVSD board (PLAN P6).

Pages: the 6809 sees one 32 KiB ROM page at $8000-$FFFF, chosen by the bank
latch at $7800 (MAME williams_cvsd_sound_device::bank_select_w):
  region offset = 0x10000 + 0x8000*((bank>>2)&3) + 0x20000*(bank&3)
with U4 at 0x10000 (reloaded at 0x20000), U19 at 0x30000/0x40000 and U20 at
0x50000/0x60000. Unique pages are keyed "u4.0" .. "u20.1".

Recursive descent from each page's vectors plus explicit seeds; instructions
that leave the known graph (indirect jumps, bank switches, RAM targets) are
listed so the emitter can route them through the runtime dispatcher, which
faults by name on any (page, pc) it lacks.

  python tools/m6809_aot/codemap6809.py <smashtv.zip> [--seeds file] [--out json]
"""
import argparse, json, os, sys, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode6809 import decode

ROMS = {"u4": "sl2_smash_tv_sound_rom_u4.u4", "u19": "sl2_smash_tv_sound_rom_u19.u19",
        "u20": "sl2_smash_tv_sound_rom_u20.u20"}
CHIPS = ("u4", "u19", "u20")


def page_of_bank(bank):
    chip = bank & 3
    if chip == 3:
        return None                      # unpopulated region (reads 0)
    return f"{CHIPS[chip]}.{(bank >> 2) & 1}"


# RAM-resident code: the boot path at u4.0 $8158 copies ROM $8192-$8219 to RAM
# $0000 (LDX #$0000 / LDY #$8192 / copy loop to $821A) and jumps to $0000.
# The runtime checks each specialized RAM instruction's bytes before running it.
RAM_IMAGES = [("u4.0", 0x8192, 0x821A, 0x0000)]


class Page:
    def __init__(self, base, data):
        self.base, self.data = base, data

    def __getitem__(self, a):
        return self.data[a - self.base] if self.base <= a < self.base + len(self.data) else 0

    def contains(self, a):
        return self.base <= a < self.base + len(self.data)


def load(zpath):
    z = zipfile.ZipFile(zpath)
    pages = {}
    for chip, name in ROMS.items():
        d = z.read(name)
        pages[f"{chip}.0"] = Page(0x8000, d[:0x8000])
        pages[f"{chip}.1"] = Page(0x8000, d[0x8000:])
    img = bytearray(0x800)
    for src, start, end, dst in RAM_IMAGES:
        img[dst:dst + end - start] = pages[src].data[start - 0x8000:end - 0x8000]
    pages["ram"] = Page(0x0000, bytes(img))
    return pages


def common_ranges(pages):
    """Addresses whose bytes are identical in every page (bank-independent code)."""
    keys = [k for k in pages if k != "ram"]
    same = [all(pages[k].data[i] == pages[keys[0]].data[i] for k in keys) for i in range(0x8000)]
    return same


FLOW_END = {"RTS", "RTI", "BRA", "LBRA", "JMP", "SWI", "SWI2", "SWI3"}
BANK = 0x7800


def _writes(ins):
    """Index registers (X Y U S) an instruction may overwrite."""
    mn = ins.mnem
    out = set()
    for r in "XYUS":
        if mn in ("LD" + r, "LEA" + r):
            out.add(r)
    if mn in ("PULS", "PULU"):
        for bit, r in ((0x10, "X"), (0x20, "Y"), (0x40, "U" if mn == "PULS" else "S")):
            if ins.value & bit:
                out.add(r)
    if mn in ("TFR", "EXG"):
        names = {1: "X", 2: "Y", 3: "U", 4: "S"}
        for n in (ins.value & 0x0F, ins.value >> 4):
            if n in names:
                out.add(names[n])
    if ins.idx and ins.idx.kind in ("inc1", "inc2", "dec1", "dec2"):
        out.add(ins.idx.reg)
    return out


def table_entries(data, base, limit):
    out = []
    for i in range(limit):
        a = base + 2 * i
        if not 0x8000 <= a <= 0xFFFE:
            break
        v = (data[a] << 8) | data[a + 1]
        out.append(v)
    return out


class Map:
    def __init__(self, pages):
        self.pages = pages
        self.code = {}
        self.exits = set()
        self.tables = {}

    def run(self, seeds, weak):
        self._walk(seeds, self.code, self.exits, strict=True)
        for s in weak:
            if s in self.code:
                continue
            tmp, tex = {}, set()
            ok = self._walk([s], tmp, tex, strict=False, known=self.code)
            if ok:
                self.code.update(tmp)
                self.exits |= tex

    def _walk(self, seeds, code, exits, strict, known=None):
        known = known or {}
        work = list(seeds)
        newweak = []
        while work:
            page, pc = work.pop()
            consts = {}
            mask = None
            while True:
                if (page, pc) in code or (page, pc) in known:
                    break
                data = self.pages[page]
                if not data.contains(pc):
                    if page != "ram" and pc < 0x800:
                        work.append(("ram", pc))
                        exits.add((page, pc, "ram-target"))
                    elif page == "ram" and pc >= 0x8000:
                        # RAM code returning to ROM: bank is whatever the RAM routine left
                        exits.add((page, pc, "rom-target"))
                        newweak += [(p, pc) for p in self.pages if p != "ram"]
                    elif not strict:
                        return False
                    else:
                        exits.add((page, pc, "unmapped-target"))
                    break
                ins = decode(data.__getitem__, pc)
                if ins is None:
                    if not strict:
                        return False
                    exits.add((page, pc, "undecodable"))
                    break
                code[(page, pc)] = ins
                mn = ins.mnem
                nxt = (pc + ins.length) & 0xFFFF
                if ins.target is not None:
                    work.append((page, ins.target))
                if mn in ("ANDA", "ANDB") and ins.mode == "imm8":
                    mask = ins.value
                ix = ins.idx
                if ins.mode == "idx" and mn in ("JMP", "JSR"):
                    base = consts.get(ix.reg)
                    if ix.indirect and ix.kind in ("accA", "accB", "accD") and isinstance(base, int):
                        n = (mask + 1) if mask is not None and ix.kind != "accD" else 128
                        n = n // 2 if n > 128 else n
                        self.tables[(page, pc)] = (base, n)
                        newweak += [(page, v) for v in table_entries(data, base, n) if v >= 0x8000]
                        exits.add((page, pc, "table"))
                    elif not ix.indirect and ix.kind == "zero" and isinstance(base, tuple):
                        self.tables[(page, pc)] = (base[1], 128)
                        newweak += [(page, v) for v in table_entries(data, base[1], 128) if v >= 0x8000]
                        exits.add((page, pc, "table"))
                    else:
                        exits.add((page, pc, "indirect"))
                src = consts.get(ix.reg) if ix else None
                for r in _writes(ins):
                    consts.pop(r, None)
                if ins.mode == "imm16" and mn[:2] == "LD" and mn[2] in "XYUS":
                    consts[mn[2]] = ins.value
                if mn[:2] == "LD" and len(mn) == 3 and mn[2] in "XYUS" and ix and not ix.indirect                         and ix.kind in ("accA", "accB", "accD") and isinstance(src, int):
                    consts[mn[2]] = ("tab", src)
                if mn in ("PULS", "PULU") and ins.value & 0x80:
                    break
                if mn in ("TFR", "EXG") and (ins.value & 0x0F == 5 or ins.value >> 4 == 5):
                    exits.add((page, pc, "reg-pc"))
                    if mn == "TFR" or ins.value & 0x0F == 5:
                        break
                if mn in ("JMP", "JSR") and ins.mode == "dir":
                    exits.add((page, pc, "direct-page"))
                if mn.startswith("ST") and ins.mode == "ext" and ins.value == BANK:
                    exits.add((page, pc, "bank-switch"))
                    newweak += [(p, nxt) for p in self.pages if p not in (page, "ram")]
                if mn in FLOW_END:
                    break
                pc = nxt
        if strict:
            for s in newweak:
                self._pending.append(s)
        else:
            self._pending_weak.extend(newweak)
        return True


def descend(pages, seeds):
    m = Map(pages)
    m._pending, m._pending_weak = [], []
    m.run(seeds, [])
    rounds = 0
    while m._pending or m._pending_weak:
        weak = m._pending + m._pending_weak
        m._pending, m._pending_weak = [], []
        m.run([], weak)
        rounds += 1
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip")
    ap.add_argument("--seeds")
    ap.add_argument("--out")
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
    if a.seeds:
        for line in open(a.seeds):
            line = line.split("#")[0].split()
            if line:
                seeds.append((line[0], int(line[1], 16)))
    m = descend(pages, seeds)
    code, exits = m.code, m.exits
    same = common_ranges(pages)
    per = {}
    for (p, pc) in code:
        per[p] = per.get(p, 0) + 1
    print("instructions", len(code), per)
    kinds = {}
    for e in exits:
        kinds[e[2]] = kinds.get(e[2], 0) + 1
    print("exits", kinds)
    if a.out:
        json.dump({"code": sorted(f"{p}:{pc:04x}:{code[(p, pc)].length}" for (p, pc) in code),
                   "exits": [f"{p}:{pc:04x}:{k}" for p, pc, k in sorted(set(exits))],
                   "tables": [f"{p}:{pc:04x}:{b:04x}:{n}" for (p, pc), (b, n) in sorted(m.tables.items())],
                   "common": [i + 0x8000 for i in range(0x8000) if same[i]][:1] and
                             _ranges([i for i in range(0x8000) if same[i]])},
                  open(a.out, "w"), indent=0)


def _ranges(idx):
    out, start, prev = [], None, None
    for i in idx:
        if start is None:
            start = prev = i
        elif i == prev + 1:
            prev = i
        else:
            out.append([start + 0x8000, prev + 0x8000]); start = prev = i
    if start is not None:
        out.append([start + 0x8000, prev + 0x8000])
    return out


if __name__ == "__main__":
    main()
