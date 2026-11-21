#!/usr/bin/env python3
"""Disassemble the pinned L8 GETXY/GETANIX direction calculation path."""

from __future__ import annotations
import argparse, hashlib, json, subprocess, zipfile
from pathlib import Path

ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
UNIDASM_SHA256 = "5d27d3c7bc349d63d3511d86d9eb30ce348eb4f0e61f2bd2609f87aa5a5a98e0"
ROM_REGION_SIZE = 0x100000
ROM_REGION_OFFSET = 0xC0000
CPU_ROM_MAP_BASE = 0xFF800000
CONTEXT_BYTES = 0x100
ROM_MEMBERS = {
    "la8_smash_tv_game_rom_u105.u105": 0x48CD793F,
    "la8_smash_tv_game_rom_u89.u89": 0x8E7FE463,
}
TARGETS = {
    "getanix": (0xFFE064D0, (
        "ffe064d0: mmtm   sp,a1,a2",
        "ffe065c0: move   *a8(70h),a0,0",
        "ffe065e0: add    a2,a0",
    )),
    "getxy": (0xFFE407B0, (
        "ffe40810: calla  ffe064d0h",
        "ffe40850: move   *a8(90h),a0,0",
        "ffe40870: sub    a0,a3",
        "ffe408e0: divu   a4,a5",
        "ffe40900: cmpi   269fbh,a5",
        "ffe40950: cmpi   69fbh,a5",
    )),
}

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-zip", required=True, type=Path)
    parser.add_argument("--unidasm", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    rom_bytes = args.rom_zip.read_bytes()
    unidasm_bytes = args.unidasm.read_bytes()
    if sha(rom_bytes) != ROM_SHA256:
        raise SystemExit("supplied L8 ROM archive hash mismatch")
    if sha(unidasm_bytes) != UNIDASM_SHA256:
        raise SystemExit("pinned unidasm hash mismatch")
    region = bytearray(ROM_REGION_SIZE)
    with zipfile.ZipFile(args.rom_zip) as archive:
        for name, expected_crc in ROM_MEMBERS.items():
            item = archive.getinfo(name)
            data = archive.read(name)
            if item.CRC != expected_crc or len(data) != 0x20000:
                raise SystemExit(f"unexpected L8 ROM metadata: {name}")
            lane = 0 if name.endswith("u105.u105") else 1
            region[ROM_REGION_OFFSET + lane:ROM_REGION_SIZE:2] = data
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "smashtv-la8-maindata.bin").write_bytes(region)
    targets = {}
    for name, (pc, expected) in TARGETS.items():
        offset = (pc - CPU_ROM_MAP_BASE) // 8
        if not (ROM_REGION_OFFSET <= offset < ROM_REGION_SIZE):
            raise SystemExit(f"{name} PC outside L8 ROM region")
        start = max(ROM_REGION_OFFSET, offset - CONTEXT_BYTES)
        end = min(ROM_REGION_SIZE, offset + CONTEXT_BYTES)
        binary = args.output_dir / f"{name}.bin"
        listing = args.output_dir / f"{name}.txt"
        binary.write_bytes(region[start:end])
        base_pc = CPU_ROM_MAP_BASE + start * 8
        result = subprocess.run(
            [str(args.unidasm), str(binary), "-arch", "tms34010", "-basepc",
             f"{base_pc:08x}", "-norawbytes", "-count", str((end - start) * 8)],
            check=True, capture_output=True, text=True,
        ).stdout
        normalized = result.lower()
        missing = [instruction for instruction in expected if instruction not in normalized]
        if missing:
            raise SystemExit(f"{name} L8 listing mismatch; missing: {missing}")
        listing.write_text(result, encoding="utf-8", newline="\n")
        targets[name] = {"pc": f"0x{pc:08x}", "listing": listing.name,
                         "listing_sha256": sha(result.encode()),
                         "verified_instructions": list(expected)}
    manifest = {"mame_version": "0.289", "rom_sha256": ROM_SHA256,
                "unidasm_sha256": UNIDASM_SHA256, "rom_region_sha256": sha(region),
                "lane_order": "U105 even bytes; U89 odd bytes", "targets": targets}
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(manifest, indent=2))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
