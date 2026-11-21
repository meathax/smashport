#!/usr/bin/env python3
"""Disassemble L8 OH1/CREATE, GETPRC and first-dispatch instructions."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import zipfile
from pathlib import Path


ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
UNIDASM_SHA256 = "5d27d3c7bc349d63d3511d86d9eb30ce348eb4f0e61f2bd2609f87aa5a5a98e0"
ROM_REGION_SIZE = 0x100000
ROM_REGION_OFFSET = 0xC0000
CPU_ROM_MAP_BASE = 0xFF800000
CONTEXT_BYTES = 0x180
ROM_MEMBERS = {
    "la8_smash_tv_game_rom_u105.u105": 0x48CD793F,
    "la8_smash_tv_game_rom_u89.u89": 0x8E7FE463,
}
TARGETS = {
    "oh1-create-hulk": {
        "pc": 0xFFE3B720,
        "expected": (
            "ffe3b720: move   a2,-*sp,1",
            "ffe3b730: movi   ech,a1",
            "ffe3b750: movi   ffe3b8e0h,a7",
            "ffe3b780: calla  ffe0aba0h",
            "ffe3b7b0: mmfm   sp,a2",
            "ffe3b7d0: dsjne  a2,ffe3b720h",
            "ffe3b7f0: rets",
        ),
    },
    "mproc-getprc-xfer": {
        "pc": 0xFFE0ABA0,
        "expected": (
            "ffe0aba0: mmtm   sp,a2,a12",
            "ffe0abc0: move   @1000020h,a0,1",
            "ffe0abf0: jreq   ffe0ae70h",
            "ffe0ac00: move   *a0,a2,1",
            "ffe0ac10: move   a2,@1000020h,1",
            "ffe0ac40: move   *a13,*a0,1",
            "ffe0ac50: move   a0,*a13,1",
            "ffe0ac60: jr     ffe0ac90h",
            "ffe0ac90: cmpi   ffe00000h,a7",
            "ffe0acc0: jrnc   ffe0ad90h",
            "ffe0ad90: move   a0,a2",
            "ffe0ada0: addi   100h,a2",
            "ffe0adc0: move   a0,a12",
            "ffe0add0: addi   5a0h,a12",
            "ffe0adf0: mmtm   a2,a7-a12",
            "ffe0ae10: movk   1h,a12",
            "ffe0ae20: move   a12,-*a2,0",
            "ffe0ae30: move   a1,-*a2,0",
            "ffe0ae40: mmfm   sp,a2,a12",
            "ffe0ae60: rets",
        ),
    },
    "mproc-prcdsp-first-dispatch": {
        "pc": 0xFFE0A500,
        "expected": (
            "ffe0a500: move   *a13,a13,1",
            "ffe0a510: jreq   ffe0a5e0h",
            "ffe0a520: move   *a13(30h),a0,0",
            "ffe0a540: dec    a0",
            "ffe0a550: move   a0,*a13(30h),0",
            "ffe0a570: jrgt   ffe0a500h",
            "ffe0a580: move   a13,a1",
            "ffe0a590: addi   40h,a1",
            "ffe0a5b0: mmfm   a1,a7-a12",
            "ffe0a5d0: jump   a7",
            "ffe0a5e0: rets",
        ),
    },
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--unidasm", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    rom_sha = sha256(args.rom_zip)
    if rom_sha != ROM_SHA256:
        raise SystemExit(f"unexpected supplied L8 archive hash: {rom_sha}")
    mame_sha = sha256(args.mame)
    if mame_sha != MAME_SHA256:
        raise SystemExit(f"unexpected pinned MAME hash: {mame_sha}")
    unidasm_sha = sha256(args.unidasm)
    if unidasm_sha != UNIDASM_SHA256:
        raise SystemExit(f"unexpected unidasm binary hash: {unidasm_sha}")

    rom = bytearray(ROM_REGION_SIZE)
    with zipfile.ZipFile(args.rom_zip) as archive:
        for member, expected_crc in ROM_MEMBERS.items():
            info = archive.getinfo(member)
            payload = archive.read(member)
            if info.CRC != expected_crc or len(payload) != 0x20000:
                raise SystemExit(f"unexpected L8 program ROM metadata: {member}")
            lane = 0 if member.endswith("u105.u105") else 1
            rom[ROM_REGION_OFFSET + lane:ROM_REGION_SIZE:2] = payload

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "smashtv-la8-maindata.bin").write_bytes(rom)
    out_targets: dict[str, object] = {}
    for name, target in TARGETS.items():
        pc = int(target["pc"])
        region_offset = (pc - CPU_ROM_MAP_BASE) // 8
        if not ROM_REGION_OFFSET <= region_offset < ROM_REGION_SIZE:
            raise SystemExit(f"target outside supplied L8 program ROM: {name}")
        start = max(ROM_REGION_OFFSET, region_offset - CONTEXT_BYTES)
        end = min(ROM_REGION_SIZE, region_offset + CONTEXT_BYTES)
        snippet = args.output_dir / f"{name}.bin"
        snippet.write_bytes(rom[start:end])
        base_pc = CPU_ROM_MAP_BASE + start * 8
        listing_text = subprocess.run(
            [str(args.unidasm), str(snippet), "-arch", "tms34010", "-basepc",
             f"{base_pc:08x}", "-norawbytes", "-count", str((end - start) * 8)],
            check=True, capture_output=True, text=True,
        ).stdout
        normalized = listing_text.lower()
        missing = [line for line in target["expected"] if line not in normalized]
        if missing:
            raise SystemExit(f"{name} disassembly mismatch; missing: {missing}")
        listing = args.output_dir / f"{name}.txt"
        listing.write_text(listing_text, encoding="utf-8", newline="\n")
        out_targets[name] = {
            "pc": f"0x{pc:08x}",
            "listing": listing.name,
            "listing_sha256": hashlib.sha256(listing_text.encode("utf-8")).hexdigest(),
            "verified_instructions": list(target["expected"]),
        }

    manifest = {
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": mame_sha,
            "unidasm_executable_sha256": unidasm_sha,
        },
        "rom_archive_sha256": rom_sha,
        "program_rom_lane_order": "U105 even bytes; U89 odd bytes, per selected L8 MAME ROM_LOAD16_BYTE map",
        "targets": out_targets,
    }
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
