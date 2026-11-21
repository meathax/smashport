#!/usr/bin/env python3
"""Verify one bounded L8 MAME coin bounce and tilt/slam interaction trace."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def lines_for(lines: list[str], prefix: str) -> list[str]:
    return [line for line in lines if line.startswith(prefix)]


def one(lines: list[str], pattern: str, name: str) -> re.Match[str]:
    matches = [re.search(pattern, line) for line in lines]
    matches = [match for match in matches if match]
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one {name}, found {len(matches)}")
    return matches[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
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
        raise ValueError("MAME trace did not complete")
    one(lines, r"^L8_COIN_BOUNCE_INIT fresh_nvram_required=1 baseline_bounce_slam_and_recovery=1$", "fresh-run marker")
    one(lines, r"^L8_COIN_BOUNCE_SUMMARY end=2820$", "end marker")

    driver = args.driver.read_text(encoding="utf-8", errors="replace")
    if ('PORT_BIT( 0x0001, IP_ACTIVE_LOW, IPT_COIN1 )' not in driver or
            'PORT_BIT( 0x0008, IP_ACTIVE_LOW, IPT_TILT ) // Slam Switch' not in driver):
        raise ValueError("MAME L8 input mapping for coin1/tilt changed")
    audit = (args.source / "AUDIT.ASM").read_text(encoding="latin-1")
    if not re.search(r"\.WORD\s+LC_PID\s*;PROCESS ID SWITCH 16.*?\n\s*\.LONG\s+LCOIN", audit, re.I | re.S):
        raise ValueError("Source switch 16 no longer routes to LCOIN")
    if not re.search(r"\.WORD\s+SLAM_PID\s*;PROCESS ID SWITCH 19.*?\n\s*\.LONG\s+SLAM_SW", audit, re.I | re.S):
        raise ValueError("Source switch 19 no longer routes to SLAM_SW")

    actions = lines_for(lines, "L8_COIN_BOUNCE_ACTION ")
    expected_actions = {
        "baseline_coin1_press": ("press", 2600), "baseline_coin1_release": ("release", 2601),
        "bounce_coin1_press": ("press", 2603), "bounce_coin1_release": ("release", 2604),
        "slam_coin1_press": ("press", 2700), "slam_coin1_release": ("release", 2701),
        "slam_press": ("press", 2702), "during_slam_coin1_press": ("press", 2710),
        "during_slam_coin1_release": ("release", 2711), "slam_release": ("release", 2718),
        "recovery_coin1_press": ("press", 2800), "recovery_coin1_release": ("release", 2801),
    }
    if len(actions) != len(expected_actions):
        raise ValueError(f"Expected {len(expected_actions)} input actions, found {len(actions)}")
    for label, (kind, frame) in expected_actions.items():
        one(actions, rf"^L8_COIN_BOUNCE_ACTION label={label} action={kind} frame={frame}$", label)

    flag_lines = lines_for(lines, "L8_COIN_BOUNCE_FLAG ")
    flag_events = []
    for line in flag_lines:
        match = re.search(r"frame=(\d+) value=(\d+) old=(\?|\d+) .*pc=([0-9a-f]+)$", line)
        if not match:
            raise ValueError(f"Malformed flag trace: {line}")
        flag_events.append((int(match[1]), int(match[2]), match[4]))
    if flag_events != [(2601, 30, "ffeb0150"), (2605, 0, "ffe0eb80"),
                       (2701, 30, "ffeb0150"), (2703, 0, "ffe0eb80"),
                       (2801, 30, "ffeb0150"), (2807, 0, "ffe0eb80")]:
        raise ValueError(f"Unexpected validator flag lifecycle: {flag_events}")

    audit_lines = lines_for(lines, "L8_COIN_BOUNCE_AUDIT ")
    credit_lines = lines_for(lines, "L8_COIN_BOUNCE_CREDIT ")
    if len(audit_lines) != 2 or len(credit_lines) != 2:
        raise ValueError("Expected only baseline and post-slam-recovery audit/credit transactions")
    for prefix, observed in (("audit", audit_lines), ("credit", credit_lines)):
        expected = [(2606, 1), (2808, 2)]
        actual = []
        for line in observed:
            match = re.search(r"frame=(\d+) total=(\d+) pc=([0-9a-f]+)$", line)
            if not match:
                raise ValueError(f"Malformed {prefix} trace: {line}")
            actual.append((int(match[1]), int(match[2])))
        if actual != expected:
            raise ValueError(f"Unexpected {prefix} sequence: {actual}")

    reads = lines_for(lines, "L8_COIN_BOUNCE_INPUT ")
    # MAME shows the two switches low together (0xfff6) at both switch-scan PCs.
    for pc in ("ffe0d660", "ffe0d6c0"):
        one(reads, rf"^L8_COIN_BOUNCE_INPUT frame=2710 address=01c00010 data=0000fff6 pc={pc}$",
            f"coin1+slam active-low read at {pc}")
    one(reads, r"^L8_COIN_BOUNCE_INPUT frame=2702 address=01c00010 data=0000fff7 pc=ffe0d660$", "tilt active-low scan read")

    report = {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Fresh-NVRAM executed MAME 0.289 L8 baseline, short coin1 bounce, tilt/slam suppression and recovery trace; selected input timing only, not full process equivalence or PCB validation",
        "reference": {
            "mame_version": version,
            "mame_executable_sha256": sha256(args.mame),
            "rom_zip_sha256": sha256(args.rom),
            "driver": str(args.driver.resolve()).replace("\\", "/"),
            "driver_sha256": sha256(args.driver),
            "source_audit_sha256": sha256(args.source / "AUDIT.ASM"),
            "fresh_nvram_and_config": True,
        },
        "input_mapping": {"coin1": "IN1 0x0001 active-low", "tilt_slam": "IN1 0x0008 active-low",
                          "surviving_source_routes": {"switch16": "LC_PID/LCOIN", "switch19": "SLAM_PID/SLAM_SW"}},
        "observations": {
            "bounce": {"first_coin_press_frame": 2600, "second_one_frame_press_frame": 2603,
                       "first_flag_cleared_frame": 2605, "validator_initializations": 1,
                       "AUDLCOIN_increments": 1, "credit_increments": 1,
                       "interpretation": "The second pulse arrived while the first flag was active and produced no second validator initialization or credit."},
            "slam": {"coin_press_frame": 2700, "flag_initialized_frame": 2701,
                     "tilt_press_frame": 2702, "tilt_active_low_read": "0xfff7",
                     "coin1_and_tilt_active_low_read": "0xfff6", "flag_cleared_frame": 2703,
                     "audit_or_credit_emitted_for_this_pulse": False,
                     "coin_pulse_while_tilt_held_frame": 2710,
                     "coin_pulse_while_tilt_held_started_flag": False,
                     "tilt_release_frame": 2718,
                     "recovery_coin_press_frame": 2800,
                     "recovery_audit_credit_frame": 2808},
            "flag_events": [{"frame": frame, "value": value, "pc": pc} for frame, value, pc in flag_events],
            "audit_frames_and_totals": [[2606, 1], [2808, 2]],
            "credit_frames_and_totals": [[2606, 1], [2808, 2]],
        },
        "logs": {"path": str(args.log).replace("\\", "/"),
                 "sha256": sha256(args.log), "script": str(args.script).replace("\\", "/"),
                 "script_sha256": sha256(args.script)},
        "limitations": [
            "The bounce case is one short retrigger interval at one scan schedule; it does not define a general bounce threshold.",
            "The selected MAME tilt field was observed active-low at input-read PCs and coin accounting was suppressed in this run; timing is not generalized to all schedules.",
            "No diagnostics, service-credit, simultaneous-chute, nonzero coin3 adjustment, or physical PCB behavior was tested.",
            "The MAME driver and source correlation are development references; native dispatch and credit-state comparison remain unimplemented.",
        ],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS: short bounce did not double-credit; selected tilt/slam run suppressed coin audit/credit and recovered")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
