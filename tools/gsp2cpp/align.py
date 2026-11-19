#!/usr/bin/env python3
"""Align the linked source image with a retail program ROM and list edits (P1 gate).

Word-level walk with resynchronisation: words at relocated fields of our image
(image.relmask) match anything, because they depend on the layout. On a mismatch
the walker searches for the smallest skip (i on ours, j on ref) after which K
words agree again. Each skip is an edit: a place where the source differs from
the retail build (or where the assembler is still wrong). Edits are reported
with the linked section, nearest symbol and source lines.

  python tools/gsp2cpp/align.py build/p1/robo.bin --rev la5 --map build/p1/robo.map.json --json build/p1/align.json
"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare

K = 8          # words that must agree to resynchronise
LOOK = 400     # maximum skip searched on each side, in words


def words(b):
    return list(struct.unpack("<%dH" % (len(b) // 2), b))


def walk(A, M, B, a0, a1, b0):
    """Align A[a0:a1] (mask M) against B from b0. Returns (matched, edits, end_b)."""
    i, j, matched, edits = a0, b0, 0, []

    def eq(x, y):
        return M[x] or A[x] == B[y]

    def agree(x, y):
        if x + K > a1 or y + K > len(B):
            return x >= a1 or y >= len(B)
        return all(eq(x + k, y + k) for k in range(K))

    while i < a1 and j < len(B):
        if eq(i, j):
            i += 1
            j += 1
            matched += 1
            continue
        best = None
        for total in range(1, 2 * LOOK):
            for di in range(0, min(total, LOOK) + 1):
                dj = total - di
                if dj > LOOK:
                    continue
                if agree(i + di, j + dj):
                    best = (di, dj)
                    break
            if best:
                break
        if not best:
            edits.append((i, a1 - i, j, 0))
            return matched, edits, j
        edits.append((i, best[0], j, best[1]))
        i += best[0]
        j += best[1]
    return matched, edits, j


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--rev", default="la5")
    ap.add_argument("--map", default="build/p1/robo.map.json")
    ap.add_argument("--json")
    ap.add_argument("--show", type=int, default=30)
    a = ap.parse_args()
    ours = open(a.image, "rb").read()
    maskb = open(a.image + ".relmask", "rb").read()
    ref = compare.retail(a.rev)
    A, B = words(ours), words(ref)
    M = [maskb[2 * k] or maskb[2 * k + 1] for k in range(len(A))]
    label = compare.labeler(a.map)
    sections = sorted((int(v, 16), k) for k, v in json.load(open(a.map))["sections"].items()
                      if 0xFFE00000 <= int(v, 16) < 0x100000000)
    total_matched, all_edits, j = 0, [], 0
    per = []
    for n, (addr, key) in enumerate(sections):
        s = (addr - compare.BASE) // 16
        e = ((sections[n + 1][0] - compare.BASE) // 16) if n + 1 < len(sections) else len(A)
        if e <= s:
            continue
        if n == 0:
            j = s
        m, ed, j = walk(A, M, B, s, e, j)
        total_matched += m
        per.append({"section": key, "words": e - s, "matched": m, "edits": len(ed)})
        all_edits.extend(ed)
    base = compare.BASE
    print(f"{a.rev}: {total_matched}/{len(A)} words aligned-equal; {len(all_edits)} edits")
    for i, n_a, j, n_b in all_edits[:a.show]:
        print(f"  ours {base + i * 16:08x} +{n_a:4d}w  ref {base + j * 16:08x} +{n_b:4d}w  {label(base + i * 16)}")
    if a.json:
        json.dump({"rev": a.rev, "matched_words": total_matched, "words": len(A), "sections": per,
                   "edits": [{"ours": f"{base + i * 16:08x}", "ours_words": n_a, "ref": f"{base + j * 16:08x}",
                              "ref_words": n_b, "at": label(base + i * 16)} for i, n_a, j, n_b in all_edits]},
                  open(a.json, "w"), indent=0)


if __name__ == "__main__":
    main()
