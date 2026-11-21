#!/usr/bin/env python3
"""Verify an isolated 16-phase, one-frame MAME L8 coin1 sweep."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def event(rows: list[str], pattern: str, label: str) -> tuple[int, re.Match[str]]:
    found = [(i, re.search(pattern, line)) for i, line in enumerate(rows)]
    found = [(i, match) for i, match in found if match]
    if len(found) != 1:
        raise ValueError(f"Expected one {label}, found {len(found)}")
    return found[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--script", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Report already exists; refusing to overwrite it")

    lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    version = subprocess.run([str(args.mame), "-version"], capture_output=True,
                             text=True, timeout=20, check=True).stdout.strip()
    if not version.startswith("0.289"):
        raise ValueError(f"Expected MAME 0.289, got {version}")
    if not any(line.startswith("Average speed:") for line in lines):
        raise ValueError("Phase sweep did not complete through the final frame")
    init_i, _ = event(lines, r"^L8_COIN_PHASE_SWEEP_INIT fresh_nvram_required=1 chute=coin1 pulse=1_frame trials=16$", "sweep init")
    summary_i, _ = event(lines, r"^L8_COIN_PHASE_SUMMARY end=3227 flag_writes=32 audit_writes=16 credit_writes=16$", "sweep summary")
    if init_i >= summary_i:
        raise ValueError("Sweep initialization followed its summary")

    driver_text = args.driver.read_text(encoding="utf-8", errors="replace")
    if 'PORT_BIT( 0x0001, IP_ACTIVE_LOW, IPT_COIN1 )' not in driver_text:
        raise ValueError("Pinned MAME driver does not map IN1 bit 0x0001 to active-low coin1")

    trials = []
    phases = set()
    for trial in range(16):
        label = f"{trial:02d}"
        press_i, press = event(lines, rf"^L8_COIN_PHASE_ACTION trial={label} action=press frame=(\d+) phase=(\d+)$", f"trial {trial} press")
        release_i, release = event(lines, rf"^L8_COIN_PHASE_ACTION trial={label} action=release frame=(\d+) phase=(\d+)$", f"trial {trial} release")
        frame, phase = int(press[1]), int(press[2])
        release_frame, release_phase = int(release[1]), int(release[2])
        if release_frame != frame + 1 or release_phase != (phase + 1) % 16:
            raise ValueError(f"Trial {trial} did not produce exactly one emulated frame of coin input")
        phases.add(phase)
        if press_i >= release_i:
            raise ValueError(f"Trial {trial} release preceded press")

        flag_start_i, flag_start = event(lines, rf"^L8_COIN_PHASE_FLAG trial={label} frame=(\d+) offset=(-?\d+) old=(?:\?|0) value=30 mask=0000ffff pc=ffeb0150$", f"trial {trial} C_FLAGS start")
        flag_clear_i, flag_clear = event(lines, rf"^L8_COIN_PHASE_FLAG trial={label} frame=(\d+) offset=(-?\d+) old=30 value=0 mask=0000ffff pc=ffe0eb80$", f"trial {trial} C_FLAGS clear")
        audit_i, audit = event(lines, rf"^L8_COIN_PHASE_AUDIT trial={label} frame=(\d+) units=(\d+) data=([0-9a-f]+) pc=ffeaa1f0$", f"trial {trial} left audit")
        credit_i, credit = event(lines, rf"^L8_COIN_PHASE_CREDIT trial={label} frame=(\d+) credits=(\d+) data=([0-9a-f]+) pc=ffeaa1f0$", f"trial {trial} credit write")
        start_frame, start_offset = int(flag_start[1]), int(flag_start[2])
        clear_frame, clear_offset = int(flag_clear[1]), int(flag_clear[2])
        audit_frame, audit_units, audit_data = int(audit[1]), int(audit[2]), int(audit[3], 16)
        credit_frame, credit_total, credit_data = int(credit[1]), int(credit[2]), int(credit[3], 16)
        expected_count = trial + 1
        if start_frame != frame + 1 or start_offset != 1:
            raise ValueError(f"Trial {trial} did not initialize its validator byte on the observed next frame")
        if not (flag_start_i < release_i < flag_clear_i < audit_i < credit_i):
            raise ValueError(f"Trial {trial} flag/audit/credit order changed")
        if not (frame <= clear_frame <= frame + 8 and audit_frame == clear_frame + 1 and credit_frame == audit_frame):
            raise ValueError(f"Trial {trial} timer-to-accounting frames are outside the observed bound")
        if audit_units != expected_count or audit_data != expected_count or credit_total != expected_count or credit_data != expected_count:
            raise ValueError(f"Trial {trial} audit/credit count did not increment exactly once")
        trials.append({"trial": trial, "frame_phase_modulo_16": phase, "press_frame": frame,
                       "release_frame": release_frame, "flag_initialized_frame": start_frame,
                       "flag_cleared_frame": clear_frame, "clear_offset_from_press": clear_offset,
                       "audit_frame": audit_frame, "left_coin_audit_total": audit_units,
                       "credit_write_frame": credit_frame, "credit_total": credit_total,
                       "accepted_once": True})

    if phases != set(range(16)):
        raise ValueError(f"Expected all 16 frame-number phases exactly once, got {sorted(phases)}")

    report = {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Executed MAME 0.289 L8 coin1 one-frame pulses across every frame-number phase modulo 16; not a generic electrical phase proof, debounce minimum, PCB test, or native timing differential",
        "reference": {
            "mame_version": version,
            "mame_executable_sha256": sha256(args.mame),
            "rom_zip_sha256": sha256(args.rom),
            "mame_driver": str(args.driver.resolve()).replace("\\", "/"),
            "mame_driver_sha256": sha256(args.driver),
            "isolated_fresh_nvram_and_config": True,
            "mame_input": "IN1 mask 0x0001, IP_ACTIVE_LOW, IPT_COIN1",
        },
        "stimulus": {"chute": "coin1", "pulse_frames": 1, "phase_modulus_frames": 16,
                     "phases_covered": sorted(phases), "trials": len(trials),
                     "spacing": "41 + trial frames; start phase advances by 9 modulo 16"},
        "observations": {
            "runtime_flag_byte_candidate": "0x010ee680",
            "flag_init_value": 30,
            "flag_init_pc": "0xffeb0150",
            "flag_clear_pc": "0xffe0eb80",
            "flag_clear_offset_from_press_frames": [t["clear_offset_from_press"] for t in trials],
            "accepted_trials": sum(t["accepted_once"] for t in trials),
            "all_trials_one_credit_and_one_left_audit_increment": True,
            "trials": trials,
        },
        "logs": {"path": str(args.log).replace("\\", "/"),
                 "sha256": sha256(args.log), "script": str(args.script).replace("\\", "/"),
                 "script_sha256": sha256(args.script)},
        "limitations": [
            "Frame-number modulo 16 is a scheduling label, not a measured SWSCAN or interrupt phase.",
            "Only one-frame coin1 pulses during this attract-state run were tested; bounce, simultaneous chutes, slam, diagnostics, service, different adjustments, and other pulse widths remain open.",
            "The flag-byte address and executed code are MAME reference observations correlated with surviving source; they do not authenticate full source-retail identity or a physical PCB.",
            "MAME execution is development evidence and is not shipped with the native game.",
        ],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(trials)} one-frame L8 coin1 pulses accepted across phases 0..15; timer clear ranged {min(t['clear_offset_from_press'] for t in trials)}..{max(t['clear_offset_from_press'] for t in trials)} frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
