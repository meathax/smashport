#!/usr/bin/env python3
"""Disassemble captured L8 coin-path PCs from the supplied program ROM pair.

This is a development evidence helper, not part of the game runtime. It follows
MAME's smashtv ROM_REGION16_LE / ROM_LOAD16_BYTE layout and TMS34010 bit-PC
addressing, then asks the pinned MAME unidasm build to decode bounded windows.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import zipfile
from pathlib import Path


EXPECTED_ARCHIVE_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
EXPECTED_MAME_EXE_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
EXPECTED_UNIDASM_SHA256 = "5d27d3c7bc349d63d3511d86d9eb30ce348eb4f0e61f2bd2609f87aa5a5a98e0"
EXPECTED_MEMBER_CRC32 = {
    "la8_smash_tv_game_rom_u105.u105": 0x48CD793F,
    "la8_smash_tv_game_rom_u89.u89": 0x8E7FE463,
}
REGION_SIZE = 0x100000
ROM_REGION_OFFSET = 0xC0000
CPU_ROM_MAP_BASE = 0xFF800000
CONTEXT_BYTES = 0x100
TARGETS = {
    "timer-initialization": 0xFFEB0150,
    "timer-clear": 0xFFE0EB80,
    "left-coin-audit": 0xFFEAA1F0,
    "random-hcount-read": 0xFFE06C60,
    "hulk-count-setup": 0xFFE3E7F0,
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom-zip", required=True, type=Path)
    parser.add_argument("--unidasm", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    archive_bytes = args.rom_zip.read_bytes()
    archive_sha = sha256(archive_bytes)
    if archive_sha != EXPECTED_ARCHIVE_SHA256:
        raise SystemExit(f"ROM archive SHA-256 mismatch: {archive_sha}")
    if not args.unidasm.is_file():
        raise SystemExit(f"unidasm executable not found: {args.unidasm}")
    unidasm_sha = sha256(args.unidasm.read_bytes())
    if unidasm_sha != EXPECTED_UNIDASM_SHA256:
        raise SystemExit(f"unidasm SHA-256 mismatch: {unidasm_sha}")

    members: dict[str, bytes] = {}
    with zipfile.ZipFile(args.rom_zip) as archive:
        for name, expected_crc in EXPECTED_MEMBER_CRC32.items():
            info = archive.getinfo(name)
            payload = archive.read(name)
            if info.CRC != expected_crc or len(payload) != 0x20000:
                raise SystemExit(f"unexpected MAME L8 ROM member metadata: {name}")
            members[name] = payload

    # MAME declares ROM_REGION16_LE and loads U105 on even bytes, U89 on odd.
    region = bytearray(REGION_SIZE)
    region[ROM_REGION_OFFSET:REGION_SIZE:2] = members["la8_smash_tv_game_rom_u105.u105"]
    region[ROM_REGION_OFFSET + 1:REGION_SIZE:2] = members["la8_smash_tv_game_rom_u89.u89"]

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "smashtv-la8-maindata.bin").write_bytes(region)
    manifest: dict[str, object] = {
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": EXPECTED_MAME_EXE_SHA256,
            "unidasm_executable_sha256": unidasm_sha,
        },
        "rom_archive_sha256": archive_sha,
        "mame_rom_pair": {
            name: {"length": len(payload), "sha256": sha256(payload),
                  "crc32": f"{EXPECTED_MEMBER_CRC32[name]:08x}"}
            for name, payload in members.items()
        },
        "mapping": {
            "rom_region": "ROM_REGION16_LE 0x100000, loaded at region offset 0xC0000",
            "lane_order": "U105 even byte addresses, U89 odd byte addresses",
            "cpu_map": "map(0xFF800000, 0xFFFFFFFF).rom().region(maindata, 0)",
            "cpu_pc_units": "TMS34010 bit addresses; region byte offset=(PC-0xFF800000)/8",
            "context_bytes_each_side": CONTEXT_BYTES,
        },
        "targets": {},
    }

    targets_out: dict[str, object] = {}
    for name, pc in TARGETS.items():
        delta_bits = pc - CPU_ROM_MAP_BASE
        if delta_bits < 0 or delta_bits % 8:
            raise SystemExit(f"target PC does not map to an aligned ROM byte: {name}")
        region_offset = delta_bits // 8
        if not ROM_REGION_OFFSET <= region_offset < REGION_SIZE:
            raise SystemExit(f"target PC is outside loaded L8 ROM pair: {name}")
        start = max(ROM_REGION_OFFSET, region_offset - CONTEXT_BYTES)
        end = min(REGION_SIZE, region_offset + CONTEXT_BYTES)
        snippet = args.output_dir / f"{name}.bin"
        snippet.write_bytes(region[start:end])
        base_pc = CPU_ROM_MAP_BASE + start * 8
        output = subprocess.run(
            [str(args.unidasm), str(snippet), "-arch", "tms34010",
             "-basepc", f"{base_pc:08x}", "-norawbytes", "-count",
             str((end - start) * 8)],
            check=True, capture_output=True, text=True,
        ).stdout
        normalized = output.lower()
        expected_instructions = {
            "timer-initialization": (
                "ffeafdc0: movi   10ee680h,a10",
                "ffeb0140: movb   a0,*a10",
            ),
            "timer-clear": (
                "ffe0eab0: move   @10ee680h,a0,1",
                "ffe0eaf0: movi   10ee698h,a2",
                "ffe0eb70: movb   a0,*a2",
            ),
            "left-coin-audit": ("ffeaa1e0: movb   a0,*a7",),
            "random-hcount-read": (
                "ffe06c30: move   @c00001c0h,a1,0",
                "ffe06c60: rl     a1,a0",
            ),
            "hulk-count-setup": (
                "ffe3e7f0: move   @10de5d8h,a0,0",
                "ffe3e820: inc    a0",
                "ffe3e830: move   a0,@10de5d8h,0",
                "ffe3e860: andi   1h,a0",
                "ffe3e8c0: sll    5h,a1",
            ),
        }[name]
        if any(instruction not in normalized for instruction in expected_instructions):
            raise SystemExit(f"L8 disassembly instruction-window check failed: {name}")
        listing = args.output_dir / f"{name}.txt"
        listing.write_text(output, encoding="utf-8", newline="\n")
        targets_out[name] = {
            "pc": f"0x{pc:08x}",
            "region_byte_offset": f"0x{region_offset:06x}",
            "program_rom_pair_offset": f"0x{region_offset - ROM_REGION_OFFSET:06x}",
            "context_region_byte_offset": f"0x{start:06x}",
            "context_base_pc": f"0x{base_pc:08x}",
            "listing": listing.name,
            "listing_sha256": sha256(output.encode("utf-8")),
        }
    manifest["targets"] = targets_out
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
