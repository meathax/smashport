#!/usr/bin/env python3
"""Verify repeatable first-room HULK_CNT updates against the L8 ROM and source."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


EXPECTED_MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
EXPECTED_UNIDASM_SHA256 = "5d27d3c7bc349d63d3511d86d9eb30ce348eb4f0e61f2bd2609f87aa5a5a98e0"
EXPECTED_ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
EXPECTED_HULKS_SHA256 = "715563e9893198c0742798d3db0c77b446d64c94733e4c3c22e8df27f4d52d3a"
EXPECTED_ROOM_SHA256 = "eeafd1ac5f42afdef8d464787d7631c78f9eb25032fb4b0946bb472c295c3a24"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", line))


def run(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if any("[LUA ERROR]" in line for line in lines):
        raise ValueError(f"MAME Lua probe failed: {path}")
    if not any(line.startswith("Average speed:") for line in lines):
        raise ValueError(f"MAME did not finish: {path}")
    return [line for line in lines if line.startswith("L8_")], [
        fields(line) for line in lines if line.startswith("L8_HULK_CNT_WRITE ")
    ]


def verify(args: argparse.Namespace) -> dict[str, object]:
    expected_hashes = (
        (args.mame, EXPECTED_MAME_SHA256, "MAME 0.289"),
        (args.unidasm, EXPECTED_UNIDASM_SHA256, "unidasm"),
        (args.rom_zip, EXPECTED_ROM_SHA256, "supplied ROM archive"),
        (args.hulks_source, EXPECTED_HULKS_SHA256, "preserved HULKS.ASM"),
        (args.reference_room, EXPECTED_ROOM_SHA256, "previously verified paid-room image"),
        (args.run_a_room, EXPECTED_ROOM_SHA256, "run A frame-4200 image"),
        (args.run_b_room, EXPECTED_ROOM_SHA256, "run B frame-4200 image"),
    )
    for path, expected, label in expected_hashes:
        if digest(path) != expected:
            raise ValueError(f"{label} hash differs from its verified L8 identity")

    paid = json.loads(args.paid_start_report.read_text(encoding="utf-8"))
    if (paid.get("result") != "PASS" or paid.get("paid_start", {}).get("credit_written_frame") != 2606 or
            paid.get("paid_start", {}).get("credit_debited_frame") != 3151):
        raise ValueError("The cross-referenced paid-start proof is missing or invalid")

    events_a, writes_a = run(args.log_a)
    events_b, writes_b = run(args.log_b)
    if events_a != events_b or writes_a != writes_b:
        raise ValueError("Clean MAME runs produced different HULK count or screen events")
    if len(writes_a) != 25:
        raise ValueError("Expected 25 target-word writes in the captured paid first-room interval")
    initial = writes_a[:5]
    if [row.get("frame") for row in initial] != ["3533"] * 5:
        raise ValueError("The initial five HULK_CNT writes did not occur in frame 3533")
    if [row.get("value") for row in initial] != ["00000000", "00000100", "00000200", "00000300", "00000400"]:
        raise ValueError("HULK_CNT did not reset and advance through four initial actors")
    if [row.get("pc") for row in initial[1:]] != ["ffe3e860"] * 4:
        raise ValueError("The four count increments did not execute at the mapped SETUP_HULK continuation")
    if any(row.get("address") != "010de5d0" or row.get("mask") != "0000ffff" for row in initial):
        raise ValueError("The HULK_CNT word tap address/mask differs")
    release = [int(row["value"], 16) // 0x100 for row in writes_a
               if row.get("pc") == "ffe3e860" and row.get("frame") in {str(i) for i in range(3987, 4097)}]
    if release != list(range(5, 22)):
        raise ValueError("Observed later HULK_CNT release increments differ from the captured sequence 5..21")
    summary = next((line for line in events_a if line.startswith("L8_SUMMARY ")), "")
    if summary != "L8_SUMMARY hulk_count_writes=25 hcount_reads_frame3533=35":
        raise ValueError("HULK count/HCOUNT summary differs")

    disassembly = args.disassembly.read_text(encoding="utf-8", errors="replace").lower()
    required = (
        "ffe3e7f0: move   @10de5d8h,a0,0",
        "ffe3e820: inc    a0",
        "ffe3e830: move   a0,@10de5d8h,0",
        "ffe3e860: andi   1h,a0",
        "ffe3e8c0: sll    5h,a1",
        "ffe3e8d0: addi   10dd780h,a1",
    )
    if any(instruction not in disassembly for instruction in required):
        raise ValueError("L8 disassembly does not map the count update and player-selection tail")
    source_text = args.hulks_source.read_text(encoding="latin-1")
    source_required = (
        "SETUP_HULK:", "MOVE\t@HULK_CNT,A0", "INC\tA0", "MOVE\tA0,@HULK_CNT",
        "ANDI\t01,A0", "SLL\t5,A1", "ADDI\tPLYROBJS,A1",
    )
    if any(fragment not in source_text for fragment in source_required):
        raise ValueError("Preserved HULKS.ASM SETUP_HULK source sequence differs")

    return {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Two identical clean MAME 0.289 paid-first-room runs; live HULK_CNT word updates mapped to SETUP_HULK. Not a native actor implementation.",
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": EXPECTED_MAME_SHA256,
            "unidasm_executable_sha256": EXPECTED_UNIDASM_SHA256,
            "rom_archive_sha256": EXPECTED_ROM_SHA256,
            "hulks_source_sha256": EXPECTED_HULKS_SHA256,
            "fresh_config_and_nvram_each_run": True,
        },
        "runs": [
            {"log": args.log_a.as_posix(), "log_sha256": digest(args.log_a)},
            {"log": args.log_b.as_posix(), "log_sha256": digest(args.log_b)},
        ],
        "paid_start_reference": args.paid_start_report.as_posix(),
        "hulk_count": {
            "source_symbol": "HULKS.ASM:HULK_CNT",
            "source_symbol_definition": "HULKS.ASM:68, .BSS HULK_CNT,16",
            "source_setup_sequence": "HULKS.ASM:659-697, SETUP_HULK increments HULK_CNT then selects a player through PLYROBJS",
            "source_bit_address": "0x010de5d8",
            "mame_word_tap_address": "0x010de5d0",
            "mame_tap_mask": "0x0000ffff",
            "initial_room_frame": 3533,
            "initial_reset_and_count_words": ["0x00000000", "0x00000100", "0x00000200", "0x00000300", "0x00000400"],
            "mapped_increment_pc": "0xffe3e860",
            "count_normalization": "MAME reports the enclosing 16-bit word at aligned address 0x010de5d0; the source bit address is +8 and its byte occupies the high 8 bits of the reported word.",
            "later_release_counts": list(range(5, 22)),
            "later_release_pc": "0xffe3e860",
            "later_release_frames": [int(row["frame"]) for row in writes_a
                                      if row.get("pc") == "ffe3e860" and row.get("frame") in {str(i) for i in range(3987, 4097)}],
        },
        "hcount_correlation": {
            "source": "UTIL.ASM:RANDOM reads @HCOUNT then rotates the seed",
            "hcount_reads_same_frame": 35,
            "hcount_read_pc": "0xffe06c60",
            "trace_reference": "evidence/l8-paid-hulk-hcount-2026-09-23.json",
            "limit": "Same-frame co-occurrence is established; individual HCOUNT reads have not been assigned to placement retries versus SETUP_HULK random draws.",
        },
        "rom_disassembly": {
            "listing": args.disassembly.as_posix(),
            "listing_sha256": digest(args.disassembly),
            "helper": "tools/inspect_l8_coin_pc.py",
            "helper_sha256": digest(args.disassembly_helper),
        },
        "room_image": {
            "frame": 4200,
            "reference_sha256": EXPECTED_ROOM_SHA256,
            "run_a_sha256": digest(args.run_a_room),
            "run_b_sha256": digest(args.run_b_room),
            "matches_paid_first_room_reference": True,
        },
        "limits": [
            "The trace maps HULK_CNT setup and release counts, not Hulk coordinates, movement or full process state.",
            "It does not attribute all 35 frame-3533 HCOUNT reads to individual actors or retries.",
            "No HULK_CNT values or captured HCOUNT sequence are used by native gameplay.",
            "The source fixture still lacks retail OHULK/INIT/GET_VALID_XY actor ownership, native process timing, and a complete playable L8 room.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_a", type=Path)
    parser.add_argument("log_b", type=Path)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--unidasm", type=Path, required=True)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--hulks-source", type=Path, default=Path("original/archive/smashtv-main/HULKS.ASM"))
    parser.add_argument("--reference-room", type=Path, required=True)
    parser.add_argument("--run-a-room", type=Path, required=True)
    parser.add_argument("--run-b-room", type=Path, required=True)
    parser.add_argument("--paid-start-report", type=Path, required=True)
    parser.add_argument("--disassembly", type=Path, required=True)
    parser.add_argument("--disassembly-helper", type=Path, default=Path("tools/inspect_l8_coin_pc.py"))
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Report exists; refusing to overwrite it")
    result = verify(args)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("PASS: two clean L8 runs map four initial HULK_CNT increments to SETUP_HULK and match first-room pixels")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
