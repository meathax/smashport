#!/usr/bin/env python3
"""Contact sheet of native snapshots (development): half-size tiles, 5 per row.

  python tools/lockstep/sheet.py OUT.png DIR FRAME [FRAME ...]
reads DIR/native_FRAME.png (stv_l8_lockstep --snap) and writes OUT.png.
"""
import struct, sys, zlib


def load(p):
    d = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]; t = d[i + 4:i + 8]
        if t == b'IHDR': w, h = struct.unpack('>II', d[i + 8:i + 16])
        if t == b'IDAT': idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat); s = w * 3 + 1
    return w, h, [raw[y * s + 1:(y + 1) * s] for y in range(h)]


def save(p, rows, w):
    raw = b''.join(b'\0' + r for r in rows)
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(p, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, len(rows), 8, 2, 0, 0, 0)) +
                        ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))


out, folder, frames = sys.argv[1], sys.argv[2], sys.argv[3:]
imgs = [load(f'{folder}/native_{f}.png') for f in frames]
w, h = imgs[0][0], imgs[0][1]
cols = 5; sw, sh = w // 2, h // 2
sheet = [bytearray(sw * cols * 3) for _ in range(sh * ((len(imgs) + cols - 1) // cols))]
for k, (_, _, rows) in enumerate(imgs):
    cx, cy = (k % cols) * sw, (k // cols) * sh
    for y in range(sh):
        r = rows[y * 2]
        for x in range(sw):
            sheet[cy + y][(cx + x) * 3:(cx + x) * 3 + 3] = r[x * 6:x * 6 + 3]
save(out, [bytes(r) for r in sheet], sw * cols)
