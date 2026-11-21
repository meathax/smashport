#!/usr/bin/env python3
"""PLAN P5 reduced fuzz: test vectors for the L8 instruction forms no scenario
executes. Each vector is one real L8 instance (address) with a random CPU state.

  python tools/lockstep/opfuzz_vectors.py FORMS.txt N > vectors.txt

FORMS.txt lines: "FORM<TAB>count<TAB>addr:text addr:text ..." (see NEXT_ACTIONS).
Vector line: pc st sp a0..a14 b0..b14 (hex). Address-like registers point into
main RAM (bit addresses 0x01000000..0x010FFFFF), mostly word aligned; ST has
random flags and field sizes with interrupts disabled. RAM holds a fixed pattern
(see opfuzz.lua / stv_op_fuzz) so both sides start identical.
"""
import random, sys

forms, n = sys.argv[1], int(sys.argv[2])
rng = random.Random(0x5EED)
pcs = []
for line in open(forms):
    parts = line.rstrip("\n").split("\t")
    if len(parts) < 3:
        continue
    pixel = parts[0].startswith(("PIXT", "DRAV"))
    pcs += [(p.split(":")[0], pixel) for p in parts[2].split()]


def xy():
    return (rng.randrange(0, 12) << 16) | rng.randrange(0, 40)   # small Y:X so XY targets stay in RAM


def ram_ptr():
    a = 0x01000000 + rng.randrange(0x400, 0xFC00) * 16    # word aligned inside main RAM (64K words), away from the ends
    if rng.random() < 0.25:
        a += rng.randrange(16)                           # unaligned bit address
    return a


for pc, pixel in pcs:
    for _ in range(n):
        st = rng.getrandbits(32) & ~(1 << 21) & ~(0x3 << 22) & 0xF3FFFFFF   # IE clear
        regs = [ram_ptr() for _ in range(30)]
        if rng.random() < 0.3 and not pixel:             # some plain data registers
            for k in rng.sample(range(30), 8):
                regs[k] = rng.getrandbits(32)
        sp = ram_ptr() & ~15
        if pixel:
            # XY operands small; B3 DPTCH a modest pitch; B4 OFFSET in RAM; B5/B6 window wide
            for k in range(15):
                regs[k] = xy() if rng.random() < 0.5 else ram_ptr()
            regs[15 + 3] = rng.choice([0x100, 0x200, 0x400, 0x800, 0x1000])
            regs[15 + 4] = ram_ptr() & ~15
            regs[15 + 5], regs[15 + 6] = 0, (0x1ff << 16) | 0x1ff
            sp = xy()
        print(pc, f"{st:08x}", f"{sp:08x}", " ".join(f"{r:08x}" for r in regs))
