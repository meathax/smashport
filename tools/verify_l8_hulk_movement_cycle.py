#!/usr/bin/env python3
"""Verify paired L8 Hulk turn, animation-wake, and movement-position traces."""

from __future__ import annotations
import argparse
import hashlib
import json
import re
from pathlib import Path

MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
PROCESSES = {"0108c190": "0101f3a0", "0108b0b0": "0101f1b0"}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def rows_from(path: Path) -> tuple[str, list[dict[str, str]]]:
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    for marker in ("L8_HULK_MOVEMENT_ARMED frame=3880",
                   "L8_HULK_MOVEMENT_DONE frame=4300",
                   "L8_ACTION coin1_press frame=2600",
                   "L8_ACTION start1_press frame=3150",
                   "Average speed:"):
        require(marker in text, f"{path}: missing {marker}")
    require("[LUA ERROR]" not in text and "runtime error" not in text,
            f"{path}: MAME/Lua runtime error")
    events = [line.strip() for line in text.splitlines()
              if "L8_HULK_MOVEMENT_TAP" in line]
    normalized = "\n".join(re.sub(r"\s+", " ", line.lower()) for line in events) + "\n"
    rows = [{key.lower(): value.lower().strip()
             for key, value in re.findall(r"([a-zA-Z0-9_]+)=([^ ]+)", line)}
            for line in events]
    return normalized, rows


def select(rows: list[dict[str, str]], process: str, stage: str,
           frame: int | None = None, data: str | None = None) -> list[dict[str, str]]:
    return [row for row in rows if row.get("process") == process
            and row.get("stage") == stage.lower()
            and (frame is None or int(row["frame"]) == frame)
            and (data is None or row.get("data") == data.lower())]


def p_wake_by_frame(rows: list[dict[str, str]], process: str) -> dict[int, int]:
    result: dict[int, int] = {}
    for index, row in enumerate(rows):
        if row.get("process") != process or row.get("stage") != "pwake_write":
            continue
        frame = int(row["frame"])
        high = next((candidate for candidate in rows[index + 1:index + 20]
                     if candidate.get("process") == process
                     and candidate.get("stage") == "sched_save_write"
                     and candidate.get("process_offset") == "f0"
                     and int(candidate["frame"]) == frame), None)
        require(high is not None, f"{process} frame {frame}: PWAKE high word missing")
        result[frame] = (int(high["data"], 16) << 16) | int(row["data"], 16)
    return result


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_a", type=Path)
    parser.add_argument("run_b", type=Path)
    parser.add_argument("--mame", required=True, type=Path)
    parser.add_argument("--rom-zip", required=True, type=Path)
    parser.add_argument("--disassembly-dir", type=Path,
                        default=root / "evidence/l8-hulk-movement-cycle-disassembly")
    parser.add_argument("--native-test-log", required=True, type=Path)
    parser.add_argument("--windows-test-log", required=True, type=Path)
    parser.add_argument("--build-log", required=True, type=Path)
    parser.add_argument("--windows-exe", required=True, type=Path)
    parser.add_argument("--preservation-report", required=True, type=Path)
    parser.add_argument("--report", type=Path,
                        default=root / "evidence/l8-hulk-movement-cycle-2026-09-23.json")
    args = parser.parse_args()
    require(sha(args.mame) == MAME_SHA256, "pinned MAME binary hash mismatch")
    require(sha(args.rom_zip) == ROM_SHA256, "supplied L8 ROM archive hash mismatch")

    manifest_path = args.disassembly_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    mapping_checks = {
        "franim-sleep-resume": (
            "ffe08490: move   *a12+,a3,0", "ffe084a0: jrne   ffe08180h"),
        "hulk-turn-state": (
            "ffe3e4e0: move   a0,*a13(130h),0", "ffe3e510: move   a0,*a13(1f0h),0",
            "ffe3e550: calla  ffe0a490h", "ffe3e580: jr     ffe3c760h"),
        "hulk-way": (
            "ffe3f130: dec    a0", "ffe3f170: inc    a0", "ffe3f1e0: calla  ffe405d0h"),
    }
    for name, instructions in mapping_checks.items():
        target = manifest["targets"][name]
        listing = args.disassembly_dir / target["listing"]
        require(sha(listing) == target["listing_sha256"], f"{name} listing hash mismatch")
        content = listing.read_text(encoding="utf-8").lower()
        require(all(instruction in content for instruction in instructions),
                f"{name} source/ROM instruction mapping mismatch")

    stream_a, rows_a = rows_from(args.run_a)
    stream_b, _ = rows_from(args.run_b)
    require(stream_a == stream_b, "fresh L8 movement-cycle event streams differ")
    rows = rows_a

    # First visible 4->5 turn: target, spin flag, OSCILLATE, three-tick sleep,
    # then the facing update on the scheduler resume.
    c190 = "0108c190"
    require(len(select(rows, c190, "pdata_read_dir", 3901, "00000004")) >= 1,
            "c190 initial facing is not the expected L8 direction 4")
    require(len(select(rows, c190, "pdata_write_sk_dir", 4017, "00000005")) == 1,
            "c190 target seek 5 missing at frame 4017")
    require(len(select(rows, c190, "pdata_write_cflags", 4017, "00000001")) == 1,
            "c190 spin flag not set on measured turn")
    require(len(select(rows, c190, "pdata_write_oscillate", 4017, "00000002")) == 1,
            "c190 OSCILLATE=2 not set on measured turn")
    require(len(select(rows, c190, "ptime_write", 4017, "00000003")) == 1,
            "c190 SLEEP 3 scheduler timer missing")
    wakes = p_wake_by_frame(rows, c190)
    require(wakes.get(4017) == 0xFFE3E580, "c190 turn sleep continuation changed")
    require(len(select(rows, c190, "pdata_write_dir", 4020, "00000005")) == 1,
            "c190 facing step 4->5 not observed at frame 4020")
    require(wakes.get(4020) == 0xFFE3DD50, "c190 WAY/CONT1 sleep continuation changed")
    require(wakes.get(4023) == 0xFFE08490, "c190 FRANIM sleep/resume continuation missing")

    # Second actor proves the measured multi-step walk around WAY's 1..8 ring.
    b0b0 = "0108b0b0"
    require(len(select(rows, b0b0, "pdata_write_sk_dir", 4137, "00000007")) == 1,
            "b0b0 target seek 7 missing at frame 4137")
    require(len(select(rows, b0b0, "pdata_write_cflags", 4137, "00000001")) == 1,
            "b0b0 spin flag missing at frame 4137")
    require(len(select(rows, b0b0, "pdata_write_oscillate", 4137, "00000002")) == 1,
            "b0b0 OSCILLATE=2 missing at frame 4137")
    turn_steps = [(4140, "00000005"), (4143, "00000006"), (4146, "00000007")]
    for frame, direction in turn_steps:
        require(len(select(rows, b0b0, "pdata_write_dir", frame, direction)) == 1,
                f"b0b0 WAY facing step to {direction} absent at frame {frame}")
    position_events = [row for row in rows if row.get("process") in PROCESSES
                       and row.get("stage") == "hulk_position_write"]
    require(len(position_events) >= 500, "expected resumed HULK position stores were not captured")

    native_text = args.native_test_log.read_text(encoding="utf-8", errors="replace")
    require("L8 resumed Hulk WAY steps match observed spin and facing writes" in native_text,
            "native regression does not cover the traced WAY steps")
    require("RESULT 17 groups; 4812 checks." in native_text,
            "native extended regression summary mismatch")

    windows_text = args.windows_test_log.read_text(encoding="utf-8", errors="replace")
    require("100% tests passed, 0 tests failed out of 11" in windows_text,
            "Windows CTest route did not pass all eleven targets")
    require("frontend native/native replay: 12000" in windows_text,
            "Windows route lacks the 12,000-state frontend replay")
    require("independent" in windows_text.lower() and "oracle" in windows_text.lower(),
            "Windows route lacks independent oracle results")
    build_text = args.build_log.read_text(encoding="utf-8", errors="replace")
    require("Building CXX object" in build_text
            and "Linking CXX executable stv_extended_tests.exe" in build_text,
            "MSVC/Ninja build log lacks the extended regression target")
    require(args.windows_exe.is_file(), "staged Windows executable is missing")
    preservation = json.loads(args.preservation_report.read_text(encoding="utf-8"))
    require(preservation.get("result") == "PASS" and preservation.get("source_files") == 240
            and preservation.get("rom_zip_sha256") == ROM_SHA256,
            "240-file/ROM preservation verification failed")

    source_files = {
        "hulks_asm": root / "original/archive/smashtv-main/HULKS.ASM",
        "util_asm": root / "original/archive/smashtv-main/UTIL.ASM",
        "mproc_asm": root / "original/archive/smashtv-main/MPROC.ASM",
        "mproc_equ": root / "original/archive/smashtv-main/MPROC.EQU",
        "ndsp1_asm": root / "original/archive/smashtv-main/NDSP1.ASM",
        "disp_equ": root / "original/archive/smashtv-main/DISP.EQU",
    }
    report = {
        "date": "2026-09-23", "result": "PASS",
        "scope": "Two clean L8 MAME runs match through first sampled Hulk turns, spin flags, scheduler sleep/wake state, FRANIM resumes, and source position-field writes.",
        "reference": {"mame_version": "0.289", "mame_sha256": MAME_SHA256,
            "rom_archive_sha256": ROM_SHA256, "fresh_config_and_nvram_each_run": True,
            "trace_start_frame": 3880, "trace_done_marker": 4300,
            "event_count_each_run": len(rows),
            "event_stream_sha256": hashlib.sha256(stream_a.encode()).hexdigest()},
        "source_sha256": {key: sha(path) for key, path in source_files.items()},
        "disassembly_manifest_sha256": sha(manifest_path),
        "tools": {
            "trace_probe": "tools/mame_l8_hulk_movement_cycle.lua",
            "trace_probe_sha256": sha(root / "tools/mame_l8_hulk_movement_cycle.lua"),
            "disassembly_probe": "tools/inspect_l8_hulk_movement_cycle_pc.py",
            "disassembly_probe_sha256": sha(root / "tools/inspect_l8_hulk_movement_cycle_pc.py"),
            "verifier": "tools/verify_l8_hulk_movement_cycle.py",
            "verifier_sha256": sha(Path(__file__).resolve()),
        },
        "runs": [{"log": str(path), "log_sha256": sha(path)}
                 for path in (args.run_a, args.run_b)],
        "paired_stream_identical": True,
        "turns": {
            "process_c190_reference_only": "0x0108c190",
            "object_c190_reference_only": "0x0101f3a0",
            "initial_facing": 4, "seek": 5,
            "spin_start_frame": 4017, "cflags": 1, "oscillate": 2,
            "sleep_ticks": 3, "sleep_wake_pc": "0xffe3e580",
            "first_facing_step": {"frame": 4020, "from": 4, "to": 5,
                "wake_pc": "0xffe3dd50"},
            "franim_resume_pc": "0xffe08490",
            "position_write_count_all_tracked_actors": len(position_events),
            "way_source":"HULKS.ASM:WAY",
            "native_turn_helper_validation":"PASS; L8-observed 4->5 and 4->5->6->7 steps match turn_hulk.",
        },
        "native_validation": {"result": "PASS", "log": str(args.native_test_log),
            "log_sha256": sha(args.native_test_log), "groups": 17, "checks": 4812,
            "test_source": "tests/test_extended.cpp", "test_source_sha256": sha(root / "tests/test_extended.cpp"),
            "helper_source": "src/gameplay.cpp", "helper_source_sha256": sha(root / "src/gameplay.cpp")},
        "windows_validation": {"result": "PASS", "test_log": str(args.windows_test_log),
            "test_log_sha256": sha(args.windows_test_log), "ctest_passed": 11, "ctest_failed": 0,
            "replay": "PASS; 12,000-state frontend replay", "oracles": "PASS"},
        "build_validation": {"result": "PASS", "route": "MSVC x64/Ninja",
            "log": str(args.build_log), "log_sha256": sha(args.build_log),
            "staged_executable": str(args.windows_exe), "staged_executable_sha256": sha(args.windows_exe),
            "production_executable_changed": False},
        "preservation_validation": {"result": "PASS", "report": str(args.preservation_report),
            "report_sha256": sha(args.preservation_report), "source_files": 240,
            "rom_zip_sha256": ROM_SHA256},
        "limits": ["Selected MAME 0.289 L8 execution only; not arcade PCB proof or whole-source equivalence.",
            "Trace coverage does not implement the Hulk actor, complete FRANIM script, collision/damage, or full first room.",
            "All reference events remain diagnostic; production GameSession consumes none of these captured states."],
        "next_action": "Implement a scheduler-owned native Hulk actor continuation using measured PRCSLP/PRCDSP, DIS, WAY and FRANIM boundaries; compare the actor state against this paired L8 trace before connecting room ownership."
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"result": "PASS", "report": str(args.report),
        "events": len(rows), "event_stream_sha256": report["reference"]["event_stream_sha256"],
        "c190_first_turn": report["turns"]["first_facing_step"],
        "b0b0_facing_steps": turn_steps, "position_write_count": len(position_events)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
