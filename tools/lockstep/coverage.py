#!/usr/bin/env python3
"""Translated-code coverage over scenario runs (PLAN P5).

  coverage.py <l8_translation_index.csv> <run1.cov> [run2.cov ...] [--forms N] [--files N]

Each .cov file comes from `stv_l8_lockstep --coverage`; runs are OR-merged.
Prints executed/total instructions, the instruction forms never executed
(for the reduced op fuzz) and the source files with the most unexecuted code.
"""
import argparse, collections, csv, re

ap = argparse.ArgumentParser()
ap.add_argument("index")
ap.add_argument("cov", nargs="+")
ap.add_argument("--forms", type=int, default=40)
ap.add_argument("--files", type=int, default=15)
a = ap.parse_args()
# rows: address,instruction,source,chunk,status -- the instruction text itself contains commas
rows = []
for line in open(a.index).read().splitlines()[1:]:
    head, note, _chunk, _status = line.rsplit(",", 3)
    addr, text = head.split(",", 1)
    rows.append((addr, text, note))
hit = bytearray(len(rows))
for p in a.cov:
    b = open(p, "rb").read()
    if len(b) != len(hit):
        ap.error(f"{p}: coverage size mismatch; expected {len(hit)} bytes, got {len(b)}")
    for i, value in enumerate(b):
        if value not in (0, 1):
            ap.error(f"{p}: invalid coverage byte {value} at index {i}; expected 0 or 1")
        hit[i] |= value
n = len(rows)
done = sum(1 for x in hit if x)
print(f"executed {done}/{n} instructions ({100.0 * done / n:.1f}%) over {len(a.cov)} run(s)")
form = lambda text: re.sub(r"[-0-9A-Fa-fx]+h\b|\{[A-Z0-9]+\}|\b\d+\b", "#", text).split()[0] + " " + \
    re.sub(r"[AB]\d+", "R", re.sub(r"[-0-9A-Fa-fx]+h\b|\{[A-Z0-9]+\}|\b\d+\b", "#", " ".join(text.split()[1:])))
seen, never = set(), collections.Counter()
for r, h in zip(rows, hit):
    if h:
        seen.add(form(r[1]))
for r, h in zip(rows, hit):
    f = form(r[1])
    if not h and f not in seen:
        never[f] += 1
print(f"instruction forms never executed: {len(never)}")
for f, c in never.most_common(a.forms):
    print(f"  {c:5d}  {f}")
files = collections.Counter()
for r, h in zip(rows, hit):
    if not h:
        files[(r[2].split(":")[0] or "(no source)")] += 1
print("unexecuted instructions by source file:")
for f, c in files.most_common(a.files):
    print(f"  {c:5d}  {f}")
