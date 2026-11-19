#!/usr/bin/env python3
"""Derive the TMS34010 encoding table from the pinned MAME unidasm (development only).

Every 16-bit opcode is disassembled followed by five marker operand words that
are themselves one-word ADD instructions, so a consumed or unconsumed operand
never desynchronizes the stream. A second pass at another base PC exposes
PC-relative operands. Output: isa_table.json, {opcode: [words, template]},
where template slots are {W1..W5}, {~W1..}, {L1..L4}, {~L1..} and {REL}.

  python tools/gsp2cpp/isa_derive.py [--unidasm PATH] [--out PATH]
"""
import argparse, hashlib, json, os, re, struct, subprocess, tempfile

MARKERS = [0x4123, 0x4145, 0x4167, 0x4189, 0x41AB]
UNIDASM_SHA256 = "5d27d3c7bc349d63d3511d86d9eb30ce348eb4f0e61f2bd2609f87aa5a5a98e0"
BASE1 = 0x1000000


def run(unidasm, image, base):
    out = subprocess.run([unidasm, image, "-arch", "tms34010", "-basepc", f"{base:x}"],
                         check=True, capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        m = re.match(r"([0-9a-f]+): ((?:[0-9a-f]{4} )+)\s*(.*)$", line)
        if m:
            table[int(m.group(1), 16)] = (len(m.group(2).split()), m.group(3).strip())
    return table


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--unidasm", default=r"D:\Arcade\AI\mameexe\unidasm.exe")
    ap.add_argument("--out", default=os.path.join(here, "isa_table.json"))
    args = ap.parse_args()
    digest = hashlib.sha256(open(args.unidasm, "rb").read()).hexdigest()
    if digest != UNIDASM_SHA256:
        raise SystemExit(f"unpinned unidasm {digest}")
    with tempfile.TemporaryDirectory() as tmp:
        image = os.path.join(tmp, "all.bin")
        with open(image, "wb") as f:
            for op in range(65536):
                f.write(struct.pack("<6H", op, *MARKERS))
        b0 = run(args.unidasm, image, 0)
        b1 = run(args.unidasm, image, BASE1)
    values = {}
    for i, p in enumerate(MARKERS):
        values[p] = f"W{i + 1}"
        values[~p & 0xFFFF] = f"~W{i + 1}"
    for i in range(4):
        v = (MARKERS[i + 1] << 16) | MARKERS[i]
        values[v] = f"L{i + 1}"
        values[~v & 0xFFFFFFFF] = f"~L{i + 1}"
    table = {}
    for op in range(65536):
        n, t0 = b0[op * 96]
        _, t1 = b1[op * 96 + BASE1]
        if t0.startswith("DW "):
            continue
        parts0 = re.split(r"([0-9A-F]+h)", t0)
        parts1 = re.split(r"([0-9A-F]+h)", t1)
        out = []
        for a, b in zip(parts0, parts1):
            if re.fullmatch(r"[0-9A-F]+h", a):
                v = int(a[:-1], 16)
                out.append("{REL}" if a != b else "{" + values[v] + "}" if v in values else a)
            else:
                out.append(a)
        text = re.sub(r"\s+", " ", "".join(out)).strip()
        # MAME 0.289 34010dsm prints opcodes 0x07E0-0x07FF as "MOVE @a,Rd", but
        # its execution table (34010tbl.hxx row 0x0700) dispatches movb_ar_a/b:
        # the instruction is MOVB @SAddress,Rd. Keep the executed semantics.
        if (op & 0xFFE0) == 0x07E0 and text.startswith("MOVE @"):
            text = "MOVB" + text[4:]
        table[f"{op:04x}"] = [n, text]
    with open(args.out, "w") as f:
        json.dump({"source": "MAME 0.289 unidasm", "unidasm_sha256": digest, "markers": MARKERS,
                   "opcodes": table}, f, indent=0, sort_keys=True)
    print(f"{len(table)} opcodes -> {args.out}")


if __name__ == "__main__":
    main()
