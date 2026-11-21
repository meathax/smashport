#!/usr/bin/env python3
"""Build the L8 code map: instruction starts, source annotations, entries, unresolved sites (P2).

Sources of truth, in order:
 1. Transfer: every instruction of the P1 source listing is moved to L8 through the
    enclosing global symbol's located address; it is accepted only if the L8 word
    equals the source opcode (then it carries FILE.ASM:line).
 2. Code pointers: source 32-bit relocations to code are transferred the same way;
    the L8 long at that place becomes an entry if it is an address in ROM.
 3. Recursive descent over L8 from all accepted starts, the trap vectors and the
    pointers, following static branches/calls and fall-through. JSRP-style
    GETPC/ADDI return addresses and MOVI-loaded code addresses feeding
    CALL/JUMP/EXGPC in the same block are resolved by local constant tracking.
Output: generated/private/l8_codemap.json (derived from the user's ROM; not redistributable).

  python tools/gsp2cpp/codemap.py [--rev l8]
"""
import argparse, bisect, json, os, sys
from collections import defaultdict, deque
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compare
from decode import Decoder

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
ROM_LO, ROM_HI = 0xFFE00000, 0x100000000
UNCOND_END = {"RETS", "RETI", "JUMP", "EXGPC"}


def track_constants(ins, nxt, consts):
    """Register facts shared by discovery and the conservative metadata pass."""
    m = ins.mnem
    regs = [o.reg for o in ins.ops if o.kind == "R"]
    if m == "MOVI" and ins.ops[0].kind == "IMM":
        consts[ins.ops[1].reg] = ins.ops[0].value
    elif m == "GETPC":
        consts[regs[0]] = nxt
    elif m == "ADDI" and ins.ops[0].kind == "IMM" and ins.ops[1].reg in consts:
        consts[ins.ops[1].reg] = (consts[ins.ops[1].reg] + ins.ops[0].value) & 0xFFFFFFFF
    elif m == "MOVE" and ins.ops and ins.ops[-1].kind == "R" or (
            m == "MOVE" and len(ins.ops) >= 2 and ins.ops[1].kind == "R"):
        dst = [o for o in ins.ops if o.kind == "R"][-1].reg
        src = ins.ops[0]
        if src.kind == "R" and src.reg in consts and src.reg != dst:
            consts[dst] = consts[src.reg]
        else:
            consts.pop(dst, None)
    elif m == "MMFM":
        # The LIST extension restores registers that are not R operands.
        # A retained constant here can hide a real indirect return.
        pointer, mask = ins.ops[0].reg, ins.ops[1].value
        consts.pop(pointer, None)
        file = pointer[0] if pointer != "SP" else ("B" if ins.op & 0x10 else "A")
        for n in range(16):
            if mask & (1 << n):
                consts.pop("SP" if n == 15 else f"{file}{n}", None)
    elif regs and m not in ("CALL", "JUMP", "EXGPC", "MOVE", "CMP", "BTST", "CMPI"):
        consts.pop(regs[-1], None)


def unresolved_transfers(instructions):
    """Report unknowns for every accepted transfer, independently of seed order.

    Constants are local basic-block facts. Direct branch/call destinations and
    discontinuities begin new blocks; calls can clobber caller registers.
    Weak-seeded data remains visible pending explicit data/entry proof.
    """
    boundaries = {ins.target for ins in instructions.values() if ins.target is not None}
    consts, following, unresolved = {}, None, []
    for pc, ins in sorted(instructions.items()):
        if pc != following or pc in boundaries:
            consts.clear()
        nxt = pc + ins.words * 16
        track_constants(ins, nxt, consts)
        if ins.mnem in ("CALL", "JUMP", "EXGPC") and ins.ops[0].reg not in consts:
            unresolved.append(pc)
        uncond_jr = (ins.op & 0xFF00) == 0xC000 or ins.op == 0xC080
        if ins.mnem in UNCOND_END or ins.mnem == "JA" or uncond_jr:
            consts.clear()
            following = None
        else:
            following = nxt
            if ins.mnem in ("CALL", "CALLA", "CALLR", "TRAP"):
                consts.clear()
    return unresolved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rev", default="l8")
    ap.add_argument("--listing", default="build/p1/listing.json")
    ap.add_argument("--map", default="build/p1/robo.map.json")
    ap.add_argument("--out", default=os.path.join(ROOT, "generated", "private", "l8_codemap.json"))
    a = ap.parse_args()
    rom = compare.retail(a.rev)
    W = [rom[k] | (rom[k + 1] << 8) for k in range(0, len(rom), 2)]
    idx = lambda addr: (addr - ROM_LO) // 16
    dec = Decoder()
    with open(os.path.join(ROOT, "generated", "gsp2cpp", f"symbols_{a.rev}.json")) as stream:
        syms = json.load(stream)["symbols"]
    with open(a.map) as stream:
        gl = json.load(stream)["globals"]
    located = sorted((int(gl[k], 16), int(v, 16), k) for k, v in syms.items() if k in gl)
    ours_keys = [x[0] for x in located]
    with open(a.map) as stream:
        secs = sorted(int(v, 16) for v in json.load(stream)["sections"].values())

    def transfer(addr):
        i = bisect.bisect_right(ours_keys, addr) - 1
        if i < 0:
            return None
        o, r, name = located[i]
        # stay inside the same linked section
        nxt_sec = next((s for s in secs if s > o), ROM_HI)
        if addr >= nxt_sec:
            return None
        return r + (addr - o), name

    with open(a.listing) as stream:
        lst = json.load(stream)
    starts = {}          # L8 addr -> annotation
    rejected = 0
    for ours, op, f, line, text in lst["instructions"]:
        t = transfer(int(ours, 16))
        if not t:
            rejected += 1
            continue
        l8, sym = t
        if ROM_LO <= l8 < ROM_HI and W[idx(l8)] == int(op, 16):
            starts[l8] = {"src": f"{f}:{line}", "text": text, "sym": sym}
        else:
            rejected += 1
    seeds = set(starts)
    ptr_seeds = set()
    for place, target in lst["code_pointers"]:
        t = transfer(int(place, 16))
        if t and ROM_LO <= t[0] < ROM_HI - 16:
            v = W[idx(t[0])] | (W[idx(t[0]) + 1] << 16)
            if v in starts:   # only pointers to verified transferred instruction starts
                ptr_seeds.add(v)
    vec = []
    for n in range(32):
        at = 0xFFFFFFE0 - n * 32
        v = W[idx(at)] | (W[idx(at) + 1] << 16)
        if ROM_LO <= v < ROM_HI and v % 16 == 0:
            vec.append(v)
    seeds |= ptr_seeds | set(vec)

    insn = {}
    owner = {}           # word index -> instruction start
    conflicts = []
    indirect = []
    work = deque(sorted(seeds))
    parent = {}
    cur = [None]

    def add(target):
        parent.setdefault(target, cur[0])
        work.append(target)
    while work:
        pc = work.popleft()
        if pc in insn or not (ROM_LO <= pc < ROM_HI):
            continue
        consts = {}
        while ROM_LO <= pc < ROM_HI and pc not in insn:
            i = idx(pc)
            if i in owner:
                conflicts.append((pc, owner[i]))
                break
            ins = dec.decode(W, i, pc)
            if ins is None:
                conflicts.append((pc, "undecodable"))
                break
            insn[pc] = ins
            cur[0] = pc
            for k in range(ins.words):
                owner[i + k] = pc
            nxt = pc + ins.words * 16
            m = ins.mnem
            track_constants(ins, nxt, consts)
            # process wake / JSRP return addresses travel in A7 into CALLA/JA or onto the A12 stack
            wake = consts.get("A7")
            if wake is not None and ROM_LO <= wake < ROM_HI and wake % 16 == 0 and (
                    m in ("CALLA", "JA") or (m == "MOVE" and ins.ops[0].kind == "R" and ins.ops[0].reg == "A7"
                                              and ins.ops[1].kind == "DEC" and ins.ops[1].reg == "A12")):
                add(wake)
            if ins.target is not None and ROM_LO <= ins.target < ROM_HI:
                add(ins.target)
            if m in ("CALL", "JUMP", "EXGPC"):
                r = ins.ops[0].reg
                if r in consts:
                    add(consts[r])
                else:
                    indirect.append(pc)
            uncond_jr = (ins.op & 0xFF00) == 0xC000 or ins.op == 0xC080
            if m in UNCOND_END or uncond_jr:
                break
            pc = nxt
    # ---- weak seeds: EXGPC fall-throughs (coroutine returns) and ROM longs that
    # point into ROM. Each is accepted only if its complete descent decodes
    # without touching data or conflicting with accepted code (sandboxed).
    weak = set()
    for pc0, ins in list(insn.items()):
        if ins.mnem == "EXGPC":
            weak.add(pc0 + ins.words * 16)
    for k in range(0, len(W) - 1):
        v = W[k] | (W[k + 1] << 16)
        if ROM_LO <= v < ROM_HI and v % 16 == 0 and idx(v) not in owner:
            weak.add(v)
    accepted_weak = 0
    for seed in sorted(weak):
        if seed in insn or idx(seed) in owner:
            continue
        local, lowner, ok, stack = {}, {}, True, [seed]
        while stack and ok:
            pc = stack.pop()
            while ROM_LO <= pc < ROM_HI and pc not in insn and pc not in local:
                i = idx(pc)
                if i in owner or i in lowner:
                    ok = False
                    break
                ins = dec.decode(W, i, pc)
                if ins is None or ins.mnem in ("TRAP", "EMU", "REV", "PIXBLT", "FILL", "LINE"):
                    ok = False
                    break
                local[pc] = ins
                for kk in range(ins.words):
                    lowner[i + kk] = pc
                if ins.target is not None and ROM_LO <= ins.target < ROM_HI:
                    stack.append(ins.target)
                uncond_jr = (ins.op & 0xFF00) == 0xC000 or ins.op == 0xC080
                if ins.mnem in UNCOND_END or uncond_jr:
                    break
                pc += ins.words * 16
            if len(local) > 4000:
                ok = False
        if ok and local:
            insn.update(local)
            owner.update(lowner)
            accepted_weak += 1
    print(f"weak seeds: {len(weak)} candidates, {accepted_weak} accepted")
    indirect = unresolved_transfers(insn)
    code_words = len(owner)
    src_ann = sum(1 for p in insn if p in starts)
    missing_transfer = [p for p in starts if p not in insn]
    print(f"{a.rev}: {len(insn)} instructions ({code_words} words, {100.0 * code_words / len(W):.1f}% of ROM); "
          f"{src_ann} with source lines; seeds {len(seeds)} (vectors {len(vec)}, pointers {len(ptr_seeds)}); "
          f"transfer rejected {rejected}; indirect unresolved {len(indirect)}; conflicts {len(conflicts)}")
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w") as stream:
        json.dump({"rev": a.rev, "note": "derived from the user's ROM; private",
                   "instructions": {f"{p:08x}": [insn[p].words, insn[p].op] + ([starts[p]["src"]] if p in starts else [])
                                    for p in sorted(insn)},
                   "vectors": [f"{v:08x}" for v in vec],
                   "indirect_sites": [f"{p:08x}" for p in sorted(set(indirect))],
                   "conflicts": [[f"{p:08x}", o if isinstance(o, str) else f"{o:08x}",
                                  f"{parent[p]:08x}" if parent.get(p) else None] for p, o in conflicts]},
                  stream)


if __name__ == "__main__":
    main()
