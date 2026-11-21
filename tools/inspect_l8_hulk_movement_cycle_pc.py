#!/usr/bin/env python3
"""Disassemble bounded L8 Hulk turn and FRANIM continuation code."""

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
CONTEXT_BYTES = 0x240
ROM_MEMBERS = {
    "la8_smash_tv_game_rom_u105.u105": 0x48CD793F,
    "la8_smash_tv_game_rom_u89.u89": 0x8E7FE463,
}
TARGETS = {
    "franim-sleep-resume": 0xFFE08490,
    "hulk-turn-state": 0xFFE3E500,
    "hulk-way": 0xFFE3F0A0,
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

    for path, expected, label in ((args.rom_zip, ROM_SHA256, "ROM archive"),
                                  (args.mame, MAME_SHA256, "MAME"),
                                  (args.unidasm, UNIDASM_SHA256, "unidasm")):
        actual = sha256(path)
        if actual != expected:
            raise SystemExit(f"unexpected {label} hash: {actual}")

    rom = bytearray(ROM_REGION_SIZE)
    with zipfile.ZipFile(args.rom_zip) as archive:
        for member, expected_crc in ROM_MEMBERS.items():
            payload = archive.read(member)
            if archive.getinfo(member).CRC != expected_crc or len(payload) != 0x20000:
                raise SystemExit(f"unexpected L8 program ROM metadata: {member}")
            lane = 0 if member.endswith("u105.u105") else 1
            rom[ROM_REGION_OFFSET + lane:ROM_REGION_SIZE:2] = payload

    args.output_dir.mkdir(parents=True, exist_ok=True)
    region = args.output_dir / "smashtv-la8-maindata.bin"
    region.write_bytes(rom)
    out_targets: dict[str, object] = {}
    for name, pc in TARGETS.items():
        region_offset = (pc - CPU_ROM_MAP_BASE) // 8
        if not ROM_REGION_OFFSET <= region_offset < ROM_REGION_SIZE:
            raise SystemExit(f"target outside supplied L8 program ROM: {name}")
        start = max(ROM_REGION_OFFSET, region_offset - CONTEXT_BYTES)
        end = min(ROM_REGION_SIZE, region_offset + CONTEXT_BYTES)
        snippet = args.output_dir / f"{name}.bin"
        snippet.write_bytes(rom[start:end])
        base_pc = CPU_ROM_MAP_BASE + start * 8
        count = (end - start) * 8
        listing_text = subprocess.run(
            [str(args.unidasm), str(snippet), "-arch", "tms34010", "-basepc",
             f"{base_pc:08x}", "-norawbytes", "-count", str(count)],
            check=True, capture_output=True, text=True,
        ).stdout
        listing = args.output_dir / f"{name}.txt"
        listing.write_text(listing_text, encoding="utf-8", newline="\n")
        out_targets[name] = {
            "pc": f"0x{pc:08x}",
            "listing": listing.name,
            "listing_sha256": hashlib.sha256(listing_text.encode("utf-8")).hexdigest(),
        }

    manifest = {
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": MAME_SHA256,
            "unidasm_executable_sha256": UNIDASM_SHA256,
        },
        "rom_archive_sha256": ROM_SHA256,
        "program_rom_lane_order": "U105 even bytes; U89 odd bytes, per selected L8 MAME ROM_LOAD16_BYTE map",
        "context_bytes_each_side": CONTEXT_BYTES,
        "program_region_sha256": hashlib.sha256(rom).hexdigest(),
        "targets": out_targets,
    }
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
