#!/usr/bin/env python3
"""Show the source lines assembled near an absolute bit address of the linked image (P1 debugging).

  python tools/gsp2cpp/where.py 0xffe07bd8 [--map build/p1/robo.map.json] [--span 0x40]
"""
import argparse, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", type=lambda x: int(x, 16))
    ap.add_argument("--map", default="build/p1/robo.map.json")
    ap.add_argument("--span", type=lambda x: int(x, 16), default=0x40)
    ap.add_argument("--variant", default="SYS.INC=SYS.RBO")
    a = ap.parse_args()
    secs = {k: int(v, 16) for k, v in json.load(open(a.map))["sections"].items()}
    best = max(((v, k) for k, v in secs.items() if v <= a.addr), default=None)
    base, key = best
    mod, sect = key.split(":", 1)
    files = asm.Files(variants=dict([a.variant.split("=", 1)]))
    m = asm.Module(mod + ".ASM", files, asm.ISA(), {}).assemble()
    for s, loc, op, src in m.listing:
        addr = base + loc
        if s == sect and abs(addr - a.addr) <= a.span * 8:
            print(f"{addr:08x} {op:04x} {m.isa.text.get(op, '?'):28s} {src[0]}:{src[1]}  {src[2][:60]}")


if __name__ == "__main__":
    main()
