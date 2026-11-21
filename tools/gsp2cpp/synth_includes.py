#!/usr/bin/env python3
"""Synthesize LOADIMG global-declaration includes absent from the archive (P1).

IMGTBL.GLO and BGNDTBL.GLO are not in the source archive. Every module that
uses image or background tables includes them, so they must declare as
.GLOBAL every label defined by the generated table sources that the build
links (BACKUP/IMGTBL.ASM + IMGPAL.ASM, BACKUP/BGNDTBL.ASM + BGNDPAL.ASM).
Output: generated/gsp2cpp/synth/*.GLO (deterministic, sorted).
"""
import os, re

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
BACKUP = os.path.join(ROOT, "original", "archive", "smashtv-main", "BACKUP")
OUT = os.path.join(ROOT, "generated", "gsp2cpp", "synth")
# BGNDPAL references image palettes (e.g. NUARENAP), so both declaration files
# cover all four generated table sources.
ALL = ["IMGTBL.ASM", "IMGPAL.ASM", "BGNDTBL.ASM", "BGNDPAL.ASM"]
SETS = {"IMGTBL.GLO": ALL, "BGNDTBL.GLO": ALL}


def labels(path):
    out = []
    for line in open(path, "rb").read().decode("latin-1").splitlines():
        m = re.match(r"^([A-Za-z_$][\w$]*):?(\s|$)", line)
        if m and not line.startswith("*"):
            out.append(m.group(1))
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    for glo, srcs in SETS.items():
        names = sorted({n for s in srcs for n in labels(os.path.join(BACKUP, s))})
        with open(os.path.join(OUT, glo), "w", newline="\n") as f:
            f.write(f"* SYNTHESIZED by tools/gsp2cpp/synth_includes.py from BACKUP/{' + BACKUP/'.join(srcs)}\n")
            for i in range(0, len(names), 8):
                f.write("\t.GLOBAL\t" + ",".join(names[i:i + 8]) + "\n")
        print(glo, len(names))


if __name__ == "__main__":
    main()
