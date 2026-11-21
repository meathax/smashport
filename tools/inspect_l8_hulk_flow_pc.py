#!/usr/bin/env python3
"""Disassemble the executed L8 OHULK -> GET_VALID_XY -> STRTOBJ path."""

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
CONTEXT_BYTES = 0x100
ROM_MEMBERS = {
    "la8_smash_tv_game_rom_u105.u105": 0x48CD793F,
    "la8_smash_tv_game_rom_u89.u89": 0x8E7FE463,
}
TARGETS = {
    "ohulk-value-path": {
        "pc": 0xFFE3B970,
        "expected": (
            "ffe3b950: movi   6h,a11",
            "ffe3b970: calla  ffe41920h",
            "ffe3b9a0: move   @10865a0h,a2,0",
            "ffe3ba00: move   @10ee080h,a2,1",
            "ffe3ba30: movx   a2,a3",
            "ffe3ba40: sll    10h,a3",
            "ffe3ba50: add    a3,a0",
            "ffe3ba60: andi   ffff0000h,a2",
            "ffe3ba90: add    a2,a1",
            "ffe3baa0: callr  ffe41630h",
            "ffe3bac0: calla  ffe04170h",
        ),
    },
    "get-valid-xy-gv-route": {
        "pc": 0xFFE41920,
        "expected": (
            "ffe41920: cmpi   6h,a11",
            "ffe41940: jrne   ffe41c40h",
            "ffe41950: move   @10865a0h,a0,0",
            "ffe41980: cmpi   30h,a0",
            "ffe419a0: jreq   ffe41c40h",
            "ffe419b0: move   @10eb630h,a0,0",
            "ffe419e0: dec    a0",
            "ffe419f0: move   @ffe417b0h,a1,0",
            "ffe41a20: btst   a0,a1",
            "ffe41a30: jreq   ffe41c40h",
        ),
    },
    "range-random-hcount": {
        "pc": 0xFFE06DF0,
        "expected": (
            "ffe06df0: move   a1,-*sp,1",
            "ffe06e30: callr  ffe06cb0h",
            "ffe06c30: move   @c00001c0h,a1,0",
            "ffe06c60: rl     a1,a0",
            "ffe06e50: move   b0,a1",
            "ffe06e60: add    a1,a0",
        ),
    },
    "strtobj-coordinate-write": {
        "pc": 0xFFE043B0,
        "expected": (
            "ffe043b0: sub    a6,a9",
            "ffe043c0: sub    a7,a10",
            "ffe043d0: move   a9,*a0(60h),1",
            "ffe043f0: move   a10,*a0(80h),1",
        ),
    },
    "beginobj2-link": {
        "pc": 0xFFE04170,
        "expected": (
            "ffe04170: callr  ffe03f60h",
            "ffe04190: move   a13,*a0(160h),1",
            "ffe041b0: calla  ffe03030h",
            "ffe041e0: move   a0,a8",
            "ffe041f0: rets",
        ),
    },
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--unidasm", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    rom_sha = sha256(args.rom_zip)
    if rom_sha != ROM_SHA256:
        raise SystemExit(f"unexpected supplied L8 archive hash: {rom_sha}")
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
        if not (ROM_REGION_OFFSET <= region_offset < ROM_REGION_SIZE):
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
            "mame_executable_sha256": MAME_SHA256,
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
