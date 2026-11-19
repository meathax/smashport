#!/usr/bin/env python3
"""Resolve enhanced-profile patch points (data/enhanced_patchpoints.json) to L8
instruction addresses using the source references in the generated chunks.

  python tools/gsp2cpp/patchpoints.py   -> generated/private/enhanced_patchpoints_l8.json
Private: contains translated code excerpts. Unresolved points are listed.
"""
import glob, json, os, re

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
spec = json.load(open(os.path.join(ROOT, "data", "enhanced_patchpoints.json")))
refs, by_pc = {}, {}
pat = re.compile(r"^L_([0-9a-f]{8}): // (.*?)(?:  \[(.*?)\])?$")
for path in glob.glob(os.path.join(ROOT, "generated", "private", "l8", "l8_chunk_*.cpp")):
    lines = open(path).read().splitlines()
    for i, line in enumerate(lines):
        m = pat.match(line)
        if m:
            body = lines[i + 2].strip() if i + 2 < len(lines) and lines[i + 1].strip().startswith("l8_cov") else lines[i + 1].strip()
            site = {"l8": "0x" + m.group(1), "insn": m.group(2), "native": body[:160]}
            by_pc[int(m.group(1), 16)] = site
            if m.group(3):
                refs.setdefault(m.group(3), []).append(site)
out, missing = [], []
for p in spec["points"]:
    if "pc" in p:
        site = by_pc.get(int(p["pc"], 16))
        hit = [site] if site and site["insn"] == p["instruction"] else None
    else:
        hit = refs.get(p["src"])
    if hit:
        out.append(dict(p, sites=hit))
    else:
        missing.append(p["name"])
json.dump({"resolved": out, "unresolved": missing}, open(os.path.join(ROOT, "generated", "private", "enhanced_patchpoints_l8.json"), "w"), indent=1)
print(f"{len(out)} resolved, {len(missing)} unresolved: {missing}")
