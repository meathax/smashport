#!/usr/bin/env python3
"""Model of the LOADIMG 4.50 header generator (P1: reconstruct missing .TBL includes).

The archive lacks six LOADIMG outputs (COBRA/FINGRNT/FLATFACE/GALAGAS/MNTR/
MUTEXP.TBL). LOADIMG read a .LOD script and IMG libraries, allocated 8bpp image
data in IROM and wrote one header per selected image to the current ASM> file:
    label: .word w,h,anchor_x,anchor_y / .long irom_bit_address [/ .long palette]
Grammar, from IMG/ROBOY.LOD and verified against BACKUP/IMGTBL.ASM:
    ***> hex   IROM allocation base (bit address)
    ASM> file  append subsequent headers to file
    GLO> file  global-declaration file (ignored: synth_includes.py builds it)
    path.IMG   select the image library for following ---> lines
    ---> a,b+TAG,...  images in allocation order; NAME+TAG shares the pixel data
                      of the first image carrying TAG
    POF>/PON>  omit/include the palette long in each header
    BBB> path  background blocks (not modeled; BGNDTBL.ASM is in the archive)
Row data is padded to a multiple of 4 pixels. Development-only.

  python tools/gsp2cpp/loadimg.py [--lod ROBOY.LOD] [--check] [--write]
"""
import argparse, os, re, struct, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ARCHIVE = os.path.join(ROOT, "original", "archive", "smashtv-main")
IMG = os.path.join(ARCHIVE, "IMG")
SYNTH = os.path.join(ROOT, "generated", "gsp2cpp", "synth")
TBLS = ["COBRA.TBL", "FINGRNT.TBL", "FLATFACE.TBL", "GALAGAS.TBL", "MNTR.TBL", "MUTEXP.TBL"]


def load_library(name):
    path = None
    for n in os.listdir(IMG):
        if n.upper() == name.upper():
            path = os.path.join(IMG, n)
    if not path:
        raise SystemExit(f"IMG library {name} missing")
    raw = open(path, "rb").read()
    count, next_palette, directory = struct.unpack_from("<HHI", raw)
    images = {}
    for i in range(count):
        h = directory + i * 42
        nm = raw[h:h + 16].split(b"\0")[0].decode("ascii")
        ax, ay, w, hh, sel, _ = struct.unpack_from("<hhHHHI", raw, h + 16)
        images.setdefault(nm.upper(), (nm, w, hh, ax, ay, sel))
    pals = []
    for p in range(next_palette - 3):
        ph = directory + count * 42 + p * 26
        pals.append(raw[ph:ph + 12].split(b"\0")[0].decode("ascii"))
    return images, pals


def run(lod):
    lines = open(os.path.join(IMG, lod), "rb").read().decode("latin-1").replace("\r", "").split("\n")
    base = None
    out = None
    palettes_on = True
    lib = None
    tags = {}
    files = {}
    problems = []
    for line in lines:
        s = line.strip()
        if not s:
            continue
        if s.startswith("***>"):
            base = int(s[4:].strip(), 16)
        elif s.startswith("ASM>"):
            out = s[4:].strip().upper()
            files.setdefault(out, [])
        elif s.startswith("GLO>") or s.startswith("BBB>"):
            continue
        elif s.startswith("POF>"):
            palettes_on = False
        elif s.startswith("PON>"):
            palettes_on = True
        elif s.startswith("--->"):
            for item in s[4:].split(","):
                item = item.strip()
                if not item:
                    continue
                name, _, tag = item.partition("+")
                img = lib[0].get(name.upper())
                if img is None:
                    problems.append(f"{out}: image {name} not in library")
                    continue
                nm, w, h, ax, ay, sel = img
                if tag and tag.upper() in tags:
                    addr = tags[tag.upper()]
                else:
                    addr = base
                    base += ((w + 3) // 4 * 4) * h * 8
                    if tag:
                        tags[tag.upper()] = addr
                pal = lib[1][(sel & 0xFF) - 3] if palettes_on else None
                files[out].append((name, w, h, ax, ay, addr, pal))
        elif s.upper().endswith(".IMG"):
            lib = load_library(re.split(r"[\\/]", s)[-1])
        elif s != "\x1a":
            problems.append(f"unrecognized LOD line {s!r}")
    return files, problems


def parse_imgtbl():
    text = open(os.path.join(ARCHIVE, "BACKUP", "IMGTBL.ASM"), "rb").read().decode("latin-1").replace("\r", "")
    recs = {}
    for m in re.finditer(r"^(\w+):\s*\n\s*\.word\s+([-\d]+),([-\d]+),([-\d]+),([-\d]+)\s*\n\s*\.long\s+>([0-9A-Fa-f]+)"
                         r"(?:\s*\n\s*\.long\s+(\w+))?", text, re.M):
        recs[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)),
                            int(m.group(6), 16), m.group(7))
    return recs


def emit(records):
    # Same prologue as the archive's LOADIMG output BACKUP/IMGTBL.ASM; LA5 shows the
    # headers are not in the including module's .text.
    out = ["* SYNTHESIZED by tools/gsp2cpp/loadimg.py from IMG/ROBOY.LOD and IMG libraries",
           "	.DATA", "	.even"]
    for name, w, h, ax, ay, addr, pal in records:
        out.append(f"{name}:")
        out.append(f"\t.word\t{w},{h},{ax},{ay}")
        out.append(f"\t.long\t>{addr:X}")
        if pal:
            out.append(f"\t.long\t{pal}")
    # RBOSS.ASM includes FLATFACE.TBL and never issues .TEXT, yet LA5 holds its
    # code and tables in the ROM text flow: the included table returns to .TEXT.
    out.append("\t.TEXT")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lod", default="ROBOY.LOD")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()
    files, problems = run(args.lod)
    for p in problems:
        print("PROBLEM", p)
    if args.check:
        ref = parse_imgtbl()
        model = {r[0]: r[1:] for r in files.get("IMGTBL.ASM", [])}
        same = diff = 0
        missing = [k for k in ref if k not in model]
        extra = [k for k in model if k not in ref]
        first = []
        for k, v in ref.items():
            if k in model:
                if tuple(model[k]) == v:
                    same += 1
                else:
                    diff += 1
                    if len(first) < 8:
                        first.append((k, v, model[k]))
        print(f"IMGTBL.ASM check: {same} identical, {diff} differ, {len(missing)} missing, {len(extra)} extra")
        for f in first:
            print("  ", f)
        print("  missing:", missing[:10], " extra:", extra[:10])
    for t in TBLS:
        print(t, len(files.get(t, [])), "records")
    if args.write:
        os.makedirs(SYNTH, exist_ok=True)
        for t in TBLS:
            with open(os.path.join(SYNTH, t), "w", newline="\n") as f:
                f.write(emit(files[t]))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
