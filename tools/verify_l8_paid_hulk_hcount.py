#!/usr/bin/env python3
"""Verify a repeatable paid-L8 first-room HCOUNT trace from two clean MAME runs."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


EXPECTED_MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
EXPECTED_UNIDASM_SHA256 = "5d27d3c7bc349d63d3511d86d9eb30ce348eb4f0e61f2bd2609f87aa5a5a98e0"
EXPECTED_ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
EXPECTED_ROOM_IMAGE_SHA256 = "eeafd1ac5f42afdef8d464787d7631c78f9eb25032fb4b0946bb472c295c3a24"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", line))


def read_run(path: Path) -> tuple[list[str], list[dict[str, str]], list[dict[str, str]]]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if any("[LUA ERROR]" in line for line in lines):
        raise ValueError(f"MAME reported a Lua error: {path}")
    if not any(line.startswith("Average speed:") for line in lines):
        raise ValueError(f"MAME did not finish: {path}")
    event_lines = [line for line in lines if line.startswith("L8_")]
    credits = [fields(line) for line in lines if line.startswith("L8_CREDIT_WRITE ")]
    hcounts = [fields(line) for line in lines if line.startswith("L8_RAND_HCOUNT ")]
    return event_lines, credits, hcounts


def verify(args: argparse.Namespace) -> dict[str, object]:
    if sha256(args.mame) != EXPECTED_MAME_SHA256:
        raise ValueError("MAME executable hash differs from the pinned 0.289 reference")
    if sha256(args.unidasm) != EXPECTED_UNIDASM_SHA256:
        raise ValueError("unidasm executable hash differs from the pinned 0.289 reference")
    if sha256(args.rom_zip) != EXPECTED_ROM_SHA256:
        raise ValueError("supplied ROM archive hash differs from the recorded L8 archive")
    if sha256(args.reference_room) != EXPECTED_ROOM_IMAGE_SHA256:
        raise ValueError("frame-4200 image differs from the previously verified paid first room")
    run_room_hashes = (sha256(args.run_a_room), sha256(args.run_b_room))
    if run_room_hashes != (EXPECTED_ROOM_IMAGE_SHA256, EXPECTED_ROOM_IMAGE_SHA256):
        raise ValueError("one or both clean-run room snapshots differ from the paid first-room image")

    events_a, credits_a, hcounts_a = read_run(args.log_a)
    events_b, credits_b, hcounts_b = read_run(args.log_b)
    if events_a != events_b or credits_a != credits_b or hcounts_a != hcounts_b:
        raise ValueError("Clean MAME runs produced different observed source events")
    if len(credits_a) != 3:
        raise ValueError("Expected CMOS initialization, coin credit, and P1 start debit writes")
    expected_credit = ("2606", "014010e0", "00000001", "ffeaa1f0")
    expected_debit = ("3151", "014010e0", "00000000", "ffeaa1f0")
    if (credits_a[1].get("frame"), credits_a[1].get("address"), credits_a[1].get("value"), credits_a[1].get("pc")) != expected_credit:
        raise ValueError("Four-frame coin1 pulse did not produce the expected mapped credit write")
    if (credits_a[2].get("frame"), credits_a[2].get("address"), credits_a[2].get("value"), credits_a[2].get("pc")) != expected_debit:
        raise ValueError("P1 Start did not produce the expected mapped credit debit")

    if len(hcounts_a) != 56:
        raise ValueError("Expected the bounded HCOUNT sample window around first-room entry")
    room_samples = [row for row in hcounts_a if row.get("frame") == "3533"]
    if len(room_samples) != 35 or any(
        row.get("address") != "c00001c0" or row.get("pc") != "ffe06c60" for row in room_samples
    ):
        raise ValueError("First-room HCOUNT burst differs from the captured 35-read event")
    summary = next((line for line in events_a if line.startswith("L8_SUMMARY ")), "")
    if summary != "L8_SUMMARY coin_hcount_reads=1519 mapped_credit_writes=3":
        raise ValueError("HCOUNT or mapped-credit trace summary differs")

    listing = args.disassembly.read_text(encoding="utf-8", errors="replace").lower()
    required = (
        "ffe06c30: move   @c00001c0h,a1,0",
        "ffe06c60: rl     a1,a0",
    )
    if any(instruction not in listing for instruction in required):
        raise ValueError("L8 ROM disassembly does not map the observed tap to RANDOM's HCOUNT load")

    data = {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Two byte-identical fresh-NVRAM/config MAME 0.289 paid-start runs; first-room HCOUNT bus trace only, not native game equivalence.",
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": EXPECTED_MAME_SHA256,
            "unidasm_executable_sha256": EXPECTED_UNIDASM_SHA256,
            "rom_archive_sha256": EXPECTED_ROM_SHA256,
            "fresh_config_and_nvram_per_run": True,
        },
        "runs": [
            {"log": args.log_a.as_posix(), "log_sha256": sha256(args.log_a)},
            {"log": args.log_b.as_posix(), "log_sha256": sha256(args.log_b)},
        ],
        "reproducibility": {
            "all_l8_trace_lines_identical": True,
            "total_hcount_reads_in_frames_3000_4300": 1519,
            "same_source_coin_and_start_writes": True,
            "first_room_image_matches_prior_paid_reference": {
                "frame": 4200,
                "sha256": EXPECTED_ROOM_IMAGE_SHA256,
                "matches_both_runs": True,
                "run_a_image": {"file": args.run_a_room.as_posix(), "sha256": run_room_hashes[0]},
                "run_b_image": {"file": args.run_b_room.as_posix(), "sha256": run_room_hashes[1]},
            },
        },
        "paid_start": {
            "coin1_pressed_frame": 2600,
            "coin1_released_frame": 2604,
            "credit_written_frame": 2606,
            "credit_address": "0x014010e0",
            "credit_value": 1,
            "start1_pressed_frame": 3150,
            "start1_released_frame": 3170,
            "credit_debited_frame": 3151,
            "debit_value": 0,
        },
        "hcount": {
            "read_address": "0xc00001c0",
            "observed_cpu_pc": "0xffe06c60",
            "read_count_frame_3533": 35,
            "frame_3533_samples": room_samples,
            "rom_disassembly": {
                "listing": args.disassembly.as_posix(),
                "listing_sha256": sha256(args.disassembly),
                "instruction_before_tap_pc": "0xffe06c30 MOVE @0xc00001c0,A1",
                "next_instruction": "0xffe06c60 RL A1,A0",
                "source_mapping": "original/archive/smashtv-main/UTIL.ASM: RANDOM, lines 888-903; MOVE @HCOUNT,A1 then RL A1,A0",
                "source_sha256": sha256(args.util_source),
                "rom_disassembly_helper": "tools/inspect_l8_coin_pc.py",
                "helper_sha256": sha256(args.disassembly_helper),
            },
        },
        "limits": [
            "The HCOUNT burst is correlated to RANDOM's source and L8 ROM instructions; the 35 reads are not assigned individually to a specific number of Hulks or placement retries.",
            "The captured values are evidence only and are not substituted into native gameplay.",
            "The MAME visual match and CMOS writes establish this selected L8 paid-start schedule; they do not prove complete L8/source5.00 equivalence or PCB behavior.",
            "The native source fixture still has no retail OHULK process ownership, exact HCOUNT event-time path, first-room actor graph, or complete playable progression.",
        ],
    }
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_a", type=Path)
    parser.add_argument("log_b", type=Path)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--unidasm", type=Path, required=True)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--reference-room", type=Path, required=True)
    parser.add_argument("--run-a-room", type=Path, required=True)
    parser.add_argument("--run-b-room", type=Path, required=True)
    parser.add_argument("--disassembly", type=Path, required=True)
    parser.add_argument("--disassembly-helper", type=Path, default=Path("tools/inspect_l8_coin_pc.py"))
    parser.add_argument("--util-source", type=Path, default=Path("original/archive/smashtv-main/UTIL.ASM"))
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Report exists; refusing to overwrite it")
    report = verify(args)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("PASS: clean L8 paid-start/HCOUNT runs match mapped CMOS writes, ROM source mapping and first-room image")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
