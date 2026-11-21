#!/usr/bin/env python3
"""Pixel-compare two PNG images (8-bit RGB/RGBA, any filters). Development only.
  pngdiff.py a.png b.png  -> prints size, differing pixel count, first differences"""
import struct, sys, zlib


def load(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, t = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif t == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = {2: 3, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    out, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(line)); prev = line
    return w, h, bpp, out


def main():
    wa, ha, ba, A = load(sys.argv[1]); wb, hb, bb, B = load(sys.argv[2])
    print(f"a {wa}x{ha} b {wb}x{hb}")
    if (wa, ha) != (wb, hb):
        return 1
    diff = 0; first = []
    for y in range(ha):
        for x in range(wa):
            pa = A[y][x * ba:x * ba + 3]; pb = B[y][x * bb:x * bb + 3]
            if pa != pb:
                diff += 1
                if len(first) < 5: first.append((x, y, pa.hex(), pb.hex()))
    print(f"differing pixels: {diff}", first)
    return 0 if diff == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
