#!/usr/bin/env python3
"""Anchored alignment: align each global symbol's span from its located retail address (P1).

Uses locate.py output. For every located symbol, walks our words to the next
symbol against the retail ROM from the located address (relocated words are
wildcards) and counts equal words; unlocated spans count as unmatched. Output:
per-module identity and the list of spans that differ, for docs/revision_map.md.

  python tools/gsp2cpp/anchored.py build/p1/robo.bin --rev la5 --json build/p1/anchored_la5.json
"""
import argparse, json, os, struct, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare, align


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--rev", default="la5")
    ap.add_argument("--map", default="build/p1/robo.map.json")
    ap.add_argument("--locate")
    ap.add_argument("--json")
    a = ap.parse_args()
    loc = json.load(open(a.locate or f"build/p1/locate_{a.rev}.json"))
    ours = open(a.image, "rb").read()
    mb = open(a.image + ".relmask", "rb").read()
    A = list(struct.unpack("<%dH" % (len(ours) // 2), ours))
    M = [mb[2 * k] or mb[2 * k + 1] for k in range(len(A))]
    B = list(struct.unpack("<%dH" % (len(compare.retail(a.rev)) // 2), compare.retail(a.rev)))
    m = json.load(open(a.map))
    secs = sorted((int(v, 16), k) for k, v in m["sections"].items() if int(v, 16) >= 0xFFE00000)
    owner = lambda addr: max((s for s in secs if s[0] <= addr), default=(0, "?"))[1]
    bounds = sorted(set([int(x["ours"], 16) for x in loc] + [s[0] for s in secs] + [0x100000000]))
    per = defaultdict(lambda: [0, 0])
    spans = []
    # Unlocated (short or unanchored) symbols inherit the offset of the preceding
    # located symbol in the same section; the walk then decides whether they match.
    delta, sect_prev = None, None
    for x in sorted(loc, key=lambda x: int(x["ours"], 16)):
        sec = owner(int(x["ours"], 16))
        if sec != sect_prev:
            delta, sect_prev = None, sec
        if x["ref"] is not None:
            delta = int(x["ref"], 16) - int(x["ours"], 16)
        elif delta is not None:
            x["ref"], x["how"] = f"{int(x['ours'], 16) + delta:08x}", "inferred"
    delta, sect_prev = None, None
    for x in sorted(loc, key=lambda x: -int(x["ours"], 16)):
        sec = owner(int(x["ours"], 16))
        if sec != sect_prev:
            delta, sect_prev = None, sec
        if x["ref"] is not None:
            delta = int(x["ref"], 16) - int(x["ours"], 16)
        elif delta is not None:
            x["ref"], x["how"] = f"{int(x['ours'], 16) + delta:08x}", "inferred-back"
    for x in loc:
        o = int(x["ours"], 16)
        if o % 16:
            continue
        end = next(b for b in bounds if b > o)
        i0, i1 = (o - compare.BASE) // 16, (min(end, 0x100000000) - compare.BASE) // 16
        mod = owner(o).split(":")[0]
        n = i1 - i0
        if x["ref"] is None:
            per[mod][1] += n
            spans.append({"symbol": x["symbol"], "ours": x["ours"], "words": n, "matched": 0, "why": x["how"]})
            continue
        j0 = (int(x["ref"], 16) - compare.BASE) // 16
        matched, edits, _ = align.walk(A, M, B, i0, i1, j0)
        per[mod][0] += matched
        per[mod][1] += n
        if matched != n:
            spans.append({"symbol": x["symbol"], "ours": x["ours"], "ref": x["ref"], "words": n, "matched": matched,
                          "edits": [[f"{compare.BASE + i * 16:08x}", na, f"{compare.BASE + j * 16:08x}", nb]
                                    for i, na, j, nb in edits]})
    tm = sum(v[0] for v in per.values())
    tn = sum(v[1] for v in per.values())
    print(f"{a.rev}: {tm}/{tn} words in symbol spans equal ({100.0 * tm / tn:.2f}%), {len(spans)} spans differ")
    worst = sorted(per.items(), key=lambda kv: kv[1][0] / max(kv[1][1], 1))[:12]
    for k, (mm, nn) in worst:
        print(f"  {k:10s} {mm:6d}/{nn:6d} ({100.0 * mm / max(nn, 1):.1f}%)")
    if a.json:
        json.dump({"rev": a.rev, "matched": tm, "words": tn,
                   "modules": {k: {"matched": v[0], "words": v[1]} for k, v in sorted(per.items())},
                   "differing_spans": spans}, open(a.json, "w"), indent=0)


if __name__ == "__main__":
    main()
