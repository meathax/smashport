#!/usr/bin/env python3
"""Show the L8 instructions emitted for source lines (P9 patch-point work).

  python tools/gsp2cpp/srcsites.py MUTOID.ASM:315 CHASER.ASM:172 ...
  python tools/gsp2cpp/srcsites.py --range MUTOID.ASM:310-330
Prints l8 address, instruction text and the first native line. Private output.
"""
import glob, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
pat = re.compile(r"^L_([0-9a-f]{8}): // (.*?)  \[([A-Z0-9_]+\.[A-Z]+):(\d+)\]")
refs = {}
for path in glob.glob(os.path.join(ROOT, "generated", "private", "l8", "l8_chunk_*.cpp")):
    lines = open(path).read().splitlines()
    for i, line in enumerate(lines):
        m = pat.match(line)
        if m:
            j = i + 2 if i + 1 < len(lines) and lines[i + 1].strip().startswith("l8_cov") else i + 1
            refs.setdefault((m.group(3), int(m.group(4))), []).append((m.group(1), m.group(2), lines[j].strip()[:140]))
args = sys.argv[1:]
want = []
if args and args[0] == "--range":
    for a in args[1:]:
        f, r = a.split(":")
        lo, hi = (int(x) for x in r.split("-"))
        want += [(f, n) for n in range(lo, hi + 1)]
else:
    for a in args:
        f, n = a.split(":")
        want.append((f, int(n)))
for key in want:
    for l8, insn, native in refs.get(key, []):
        print(f"{key[0]}:{key[1]}  {l8}  {insn:34s} | {native}")
