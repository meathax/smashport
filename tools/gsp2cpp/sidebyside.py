#!/usr/bin/env python3
"""Disassemble our linked image and a retail ROM side by side around an address (P1 debugging).

  python tools/gsp2cpp/sidebyside.py build/p1/robo.bin 0xffe037c0 [--rev la5] [--before 0x40] [--after 0x100]
"""
import argparse, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare

UNIDASM = r"D:\Arcade\AI\mameexe\unidasm.exe"


def dis(data, base):
    with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
        f.write(data)
        name = f.name
    try:
        out = subprocess.run([UNIDASM, name, "-arch", "tms34010", "-basepc", f"{base:x}"],
                             capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(name)
    return [re.sub(r"\s+", " ", l.strip()) for l in out.splitlines() if l.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("addr", type=lambda x: int(x, 16))
    ap.add_argument("--rev", default="la5")
    ap.add_argument("--before", type=lambda x: int(x, 16), default=0x40)
    ap.add_argument("--after", type=lambda x: int(x, 16), default=0x100)
    a = ap.parse_args()
    ours = open(a.image, "rb").read()
    ref = compare.retail(a.rev)
    s = ((a.addr - compare.BASE) // 8 - a.before) & ~1
    e = (a.addr - compare.BASE) // 8 + a.after
    base = compare.BASE + s * 8
    A, B = dis(ours[s:e], base), dis(ref[s:e], base)
    for i in range(max(len(A), len(B))):
        x = A[i] if i < len(A) else ""
        y = B[i] if i < len(B) else ""
        print(f"{'  ' if x == y else '!='} {x[:58]:58s} | {y[:58]}")


if __name__ == "__main__":
    main()
