#!/usr/bin/env python3
"""Compare MAME screen snapshots (record.lua STV_LS_SNAPS -> mame_F.png) with
native snapshots (stv_l8_lockstep --snap -> native_F.png), pixel for pixel.

  python tools/lockstep/snapcmp.py MAME_DIR NATIVE_DIR F [F ...]
Prints one line per frame and a JSON summary; exit 1 on any difference.
"""
import json, struct, sys, zlib


def load_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path
    i, idat, w = 8, b'', 0
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]
        t = d[i + 4:i + 8]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', d[i + 8:i + 18])
            assert depth == 8 and ctype in (2, 6), f'{path}: unsupported PNG type {ctype}/{depth}'
            bpp = 3 if ctype == 2 else 4
        elif t == b'IDAT':
            idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([bytes(line[k * bpp:k * bpp + 3]) for k in range(w)])
        prev = line
    return w, h, rows


mame_dir, native_dir, frames = sys.argv[1], sys.argv[2], sys.argv[3:]
results, bad = [], 0
for f in frames:
    mw, mh, m = load_png(f'{mame_dir}/mame_{f}.png')
    nw, nh, n = load_png(f'{native_dir}/native_{f}.png')
    if (mw, mh) != (nw, nh):
        results.append({'frame': int(f), 'result': 'SIZE', 'mame': [mw, mh], 'native': [nw, nh]})
        bad += 1
        continue
    diff = sum(1 for y in range(mh) for x in range(mw) if m[y][x] != n[y][x])
    results.append({'frame': int(f), 'size': [mw, mh], 'differing_pixels': diff})
    bad += diff != 0
    print(f'frame {f}: {mw}x{mh}, {diff} differing pixels')
print(json.dumps({'frames': len(frames), 'identical': len(frames) - bad, 'results': results}))
sys.exit(1 if bad else 0)
