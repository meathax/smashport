#!/usr/bin/env python3
"""Locate every linked global symbol of our image inside a retail ROM (P1 revision mapping).

For each global in ROM space, take our words from the symbol to the next symbol
(relocated words are wildcards) and search the retail image for positions where
all fixed words agree. Unique hits give the retail address of that symbol;
consecutive symbols reveal the retail module layout and where code changed.

  python tools/gsp2cpp/locate.py build/p1/robo.bin --rev la5 --json build/p1/locate_la5.json
"""
import argparse, json, os, struct, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare

N = 4  # anchor n-gram length (words)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--rev", default="la5")
    ap.add_argument("--map", default="build/p1/robo.map.json")
    ap.add_argument("--json")
    ap.add_argument("--maxlen", type=int, default=48, help="words compared per symbol")
    a = ap.parse_args()
    ours = open(a.image, "rb").read()
    mb = open(a.image + ".relmask", "rb").read()
    A = struct.unpack("<%dH" % (len(ours) // 2), ours)
    M = [mb[2 * k] or mb[2 * k + 1] for k in range(len(A))]
    ref = compare.retail(a.rev)
    B = struct.unpack("<%dH" % (len(ref) // 2), ref)
    index = defaultdict(list)
    for j in range(len(B) - N):
        index[B[j:j + N]].append(j)
    m = json.load(open(a.map))
    syms = sorted((int(v, 16), k) for k, v in m["globals"].items() if int(v, 16) >= 0xFFE00000)
    sec_starts = sorted(int(v, 16) for v in m["sections"].values() if int(v, 16) >= 0xFFE00000)
    points = sorted(set([s[0] for s in syms] + sec_starts))
    result = []
    for n, (addr, name) in enumerate(syms):
        if addr % 16:
            continue
        i0 = (addr - compare.BASE) // 16
        nxt = next((p for p in points if p > addr), 0x100000000)
        ln = min((nxt - addr) // 16, a.maxlen)
        if ln < N:
            result.append((name, addr, None, "short"))
            continue
        anchor = None
        for k in range(0, ln - N + 1):
            if not any(M[i0 + k:i0 + k + N]):
                anchor = k
                break
        if anchor is None:
            result.append((name, addr, None, "no-anchor"))
            continue
        hits = []
        for j in index.get(A[i0 + anchor:i0 + anchor + N], []):
            j0 = j - anchor
            if j0 < 0 or j0 + ln > len(B):
                continue
            if all(M[i0 + k] or A[i0 + k] == B[j0 + k] for k in range(ln)):
                hits.append(j0)
        if len(hits) == 1:
            result.append((name, addr, compare.BASE + hits[0] * 16, "unique"))
        elif hits:
            best = min(hits, key=lambda j0: abs(j0 - i0))
            result.append((name, addr, compare.BASE + best * 16, f"multi{len(hits)}"))
        else:
            result.append((name, addr, None, "none"))
    found = sum(1 for r in result if r[2] is not None)
    print(f"{a.rev}: {found}/{len(result)} symbols located "
          f"({sum(1 for r in result if r[3] == 'unique')} unique)")
    if a.json:
        json.dump([{"symbol": s, "ours": f"{o:08x}", "ref": f"{r:08x}" if r else None, "how": h}
                   for s, o, r, h in result], open(a.json, "w"), indent=0)


if __name__ == "__main__":
    main()
