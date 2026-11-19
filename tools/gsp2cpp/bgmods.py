#!/usr/bin/env python3
"""Inspect L8 background modules (PLAN P9): list a module's blocks with sizes and
the vertical cut lines no block crosses.  python tools/gsp2cpp/bgmods.py <zip> <module_addr_hex>"""
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
e = z.read('la8_smash_tv_game_rom_u105.u105'); o = z.read('la8_smash_tv_game_rom_u89.u89')
w = [e[i] | (o[i] << 8) for i in range(len(e))]
B = 0xFFE00000
W = lambda a: w[(a - B) // 16]
L = lambda a: W(a) | (W(a + 16) << 16)
m = int(sys.argv[2], 16)
xs, ys, n = W(m), W(m + 16), W(m + 32)
blk, hdr = L(m + 48), L(m + 80)
print(f"module {m:08x}: size {xs}x{ys}, {n} blocks @ {blk:08x}, headers @ {hdr:08x}")
spans = []
for k in range(n):
    a = blk + k * 64
    flags, x, y, h = W(a), W(a + 16), W(a + 32), W(a + 48)
    ha = hdr + (h & 0x0fff) * 32      # MAP_HDR counts 32-bit longs into the header table
    bw, bh = W(ha), W(ha + 16)
    spans.append((x, x + bw))
    print(f"  {k:3d} x={x:4d} y={y:4d} w={bw:4d} h={bh:4d} flags={flags:04x} hdr={h:04x}")
cuts = [c for c in range(0, xs + 1) if not any(a < c < b for a, b in spans)]
print("cut lines:", cuts)
