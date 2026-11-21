#!/usr/bin/env python3
"""Compare a linked program image with a retail program ROM pair (P1 gate).

  python tools/gsp2cpp/compare.py build/p1/robo.bin --rev la5 --map build/p1/robo.map.json [--ranges N]

Retail images come from the user's ZIP (U105 even bytes, U89 odd bytes), mapped
at bit address 0xFFE00000. Output: identical-byte ratio and the first differing
ranges, each labelled with the nearest linked section/global symbol.
"""
import argparse, bisect, json, os, sys, zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ZIP = os.path.join(ROOT, "original", "user-supplied-roms", "smashtv.zip")
PREFIX = {"l8": "la8_smash_tv_game_rom_", "la5": "smashtv5/la5_smash_tv_game_rom_",
          "la6": "smashtv6/la6_smash_tv_game_rom_", "la4": "smashtv4/la4_smash_tv_game_rom_",
          "la3": "smashtv3/la3_smash_tv_game_rom_"}
BASE = 0xFFE00000


def retail(rev):
    z = zipfile.ZipFile(ZIP)
    even, odd = z.read(PREFIX[rev] + "u105.u105"), z.read(PREFIX[rev] + "u89.u89")
    img = bytearray(len(even) * 2)
    img[0::2], img[1::2] = even, odd
    return bytes(img)


def labeler(mapfile):
    if not mapfile:
        return lambda a: ""
    m = json.load(open(mapfile))
    points = sorted([(int(v, 16), k) for k, v in m["globals"].items()] +
                    [(int(v, 16), "[" + k + "]") for k, v in m["sections"].items()])
    keys = [p[0] for p in points]

    def label(addr):
        i = bisect.bisect_right(keys, addr) - 1
        return f"{points[i][1]}+0x{(addr - points[i][0]) // 8:x}" if i >= 0 else ""
    return label


def ranges(a, b, gap=16):
    out, i, n = [], 0, min(len(a), len(b))
    while i < n:
        if a[i] != b[i]:
            j = i
            last = i
            while j < n and j - last <= gap:
                if a[j] != b[j]:
                    last = j
                j += 1
            out.append((i, last + 1))
            i = last + 1
        else:
            i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--rev", default="la5", choices=sorted(PREFIX))
    ap.add_argument("--map")
    ap.add_argument("--ranges", type=int, default=25)
    ap.add_argument("--json")
    ap.add_argument("--structural", action="store_true", help="ignore relocated address fields (image.relmask)")
    args = ap.parse_args()
    ours = open(args.image, "rb").read()
    ref = retail(args.rev)
    if args.structural:
        mask = open(args.image + ".relmask", "rb").read()
        ours = bytes(r if m else o for o, r, m in zip(ours, ref, mask))
    same = sum(1 for x, y in zip(ours, ref) if x == y)
    rs = ranges(ours, ref)
    label = labeler(args.map)
    print(f"{args.rev}: {same}/{len(ref)} bytes identical ({100.0 * same / len(ref):.3f}%), {len(rs)} differing ranges")
    for s, e in rs[:args.ranges]:
        a = BASE + s * 8
        print(f"  0x{a:08x} +{e - s:5d}B  {label(a):40s} ours {ours[s:s + 8].hex()} ref {ref[s:s + 8].hex()}")
    if args.json:
        json.dump({"rev": args.rev, "identical_bytes": same, "total": len(ref),
                   "ranges": [[f"{BASE + s * 8:08x}", e - s, label(BASE + s * 8)] for s, e in rs]},
                  open(args.json, "w"), indent=0)
    return 0 if not rs else 1


if __name__ == "__main__":
    sys.exit(main())
