#!/usr/bin/env python3
"""Show the first differing sound access between a MAME and a native STVSND01 stream, with context.

  snd_ctx.py <mame.bin> <native.bin> [before] [after]
"""
import sys
sys.path.insert(0, __import__("os").path.dirname(__file__))
from sndfile import records, fmt

A = list(records(sys.argv[1])); B = list(records(sys.argv[2]))
nb = int(sys.argv[3]) if len(sys.argv) > 3 else 12
na = int(sys.argv[4]) if len(sys.argv) > 4 else 4
a = [x for x in A if x[0] in (1, 2)]; b = [x for x in B if x[0] in (1, 2)]
strip = lambda r: {k: v for k, v in r.items() if k != "pc"}
for i, (p, q) in enumerate(zip(a, b)):
    if p[0] != q[0] or strip(p[2]) != strip(q[2]):
        for j in range(max(0, i - nb), min(i + na, len(a), len(b))):
            print(("> " if j == i else "  ") + "M " + fmt(*a[j]) + "  |  N " + fmt(*b[j]))
        t0, t1 = a[max(0, i - nb)][2]["t"], a[min(i + na, len(a) - 1)][2]["t"]
        print("main writes:", [fmt(*x) for x in A if x[0] == 3 and t0 - 2e14 <= x[2]["t"] <= t1])
        break
else:
    print("identical over", min(len(a), len(b)))
