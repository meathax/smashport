#!/usr/bin/env python3
"""Compare extracted SOUNDS.ASM tables to the verified L8 34010 program ROM bytes."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "original/archive/smashtv-main/SOUNDS.ASM"
CATALOG = ROOT / "generated/source_sound_tables.json"
DEFAULT_ROM = ROOT / "original/user-supplied-roms/smashtv.zip"
EXPECTED_ARCHIVE_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
PROGRAM_ROMS = {
    "la8_smash_tv_game_rom_u105.u105": "7d0d9edccf0610f57e40934ab33e32315369656d",
    "la8_smash_tv_game_rom_u89.u89": "629332be706cda26f8b170b8e2877355230119ee",
}
PROGRAM_ROM_BYTES = 0x20000
REGION_BYTES = 0x100000
LOAD_OFFSET = 0xC0000


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-archive", type=pathlib.Path, default=DEFAULT_ROM)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()

    archive_bytes = args.rom_archive.read_bytes()
    archive_sha256 = sha256(archive_bytes)
    if archive_sha256 != EXPECTED_ARCHIVE_SHA256:
        raise ValueError("Supplied ROM archive differs from the verified L8 input")
    source_bytes = SOURCE.read_bytes()
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    if sha256(source_bytes) != catalog["source_sha256"]:
        raise ValueError("Generated sound catalog does not match preserved SOUNDS.ASM")

    with zipfile.ZipFile(args.rom_archive) as archive:
        program = {}
        for filename, expected_sha1 in PROGRAM_ROMS.items():
            data = archive.read(filename)
            if len(data) != PROGRAM_ROM_BYTES or hashlib.sha1(data).hexdigest() != expected_sha1:
                raise ValueError(f"Verified L8 program ROM mismatch: {filename}")
            program[filename] = data

    # MAME midyunit.cpp declares a ROM_REGION16_LE and interleaves U105/U89
    # at 0xC0000/0xC0001 with ROM_LOAD16_BYTE.
    region = bytearray(REGION_BYTES)
    even = program["la8_smash_tv_game_rom_u105.u105"]
    odd = program["la8_smash_tv_game_rom_u89.u89"]
    region[LOAD_OFFSET:REGION_BYTES:2] = even
    region[LOAD_OFFSET + 1:REGION_BYTES:2] = odd
    image = bytes(region)

    missing = []
    duplicates = []
    ordered_matches = []
    excluded_gaps = []
    cursor = LOAD_OFFSET
    previous_label = None
    for table in catalog["tables"]:
        sequence = b"".join(int(word).to_bytes(2, "little") for word in table["words"])
        offset = image.find(sequence, cursor)
        count = image.count(sequence)
        if offset < 0:
            missing.append(table["label"])
            continue
        if ordered_matches and offset > cursor:
            excluded_gaps.append({
                "after_label": previous_label,
                "before_label": table["label"],
                "region_offset": cursor,
                "byte_count": offset - cursor,
            })
        ordered_matches.append({
            "label": table["label"],
            "source_line": table["line"],
            "region_offset": offset,
            "byte_count": len(sequence),
        })
        cursor = offset + len(sequence)
        previous_label = table["label"]
        if count > 1:
            duplicates.append({"label": table["label"], "occurrences": count})
    if missing:
        raise ValueError("L8 program ROM is missing source command table bytes: " + ", ".join(missing))
    if len(ordered_matches) != len(catalog["tables"]):
        raise ValueError("Not all command tables were matched in source order")
    if (len(excluded_gaps) != 1 or excluded_gaps[0]["after_label"] != "CROWD2"
            or excluded_gaps[0]["before_label"] != "GOGO" or excluded_gaps[0]["byte_count"] != 48):
        raise ValueError(f"Unexpected gap in ordered command tables: {excluded_gaps}")

    result = {
        "result": "PASS",
        "source_file": "original/archive/smashtv-main/SOUNDS.ASM",
        "source_sha256": catalog["source_sha256"],
        "rom_archive_sha256": archive_sha256,
        "program_roms": {
            name: {"bytes": len(program[name]), "sha1": hashlib.sha1(program[name]).hexdigest()}
            for name in PROGRAM_ROMS
        },
        "program_region": {
            "bytes": REGION_BYTES,
            "layout": "MAME ROM_REGION16_LE; ROM_LOAD16_BYTE even/odd",
            "load_offset": LOAD_OFFSET,
        },
        "literal_tables_checked": len(catalog["tables"]),
        "literal_tables_found": len(catalog["tables"]),
        "literal_tables_matched_in_source_order": len(ordered_matches),
        "source_ordered_matches": ordered_matches,
        "excluded_intervening_data": {
            "byte_count": 48,
            "description": "PZLSND, PZ1SND, PZ2SND and PZ3SND .LONG pointer tables; not .WORD command tables",
        },
        "duplicate_byte_sequences": duplicates,
        "scope": "Exact source command-table byte sequences found in the verified L8 program region; not whole-source identity, CPU execution, sound-board behavior, or gameplay equivalence.",
    }
    encoded = json.dumps(result, indent=2) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
