#!/usr/bin/env python3
"""Verify simultaneous chutes, service credit, and diagnostic suppression in L8 MAME."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exactly(lines: list[str], pattern: str, label: str) -> re.Match[str]:
    found = [re.search(pattern, line) for line in lines]
    found = [match for match in found if match]
    if len(found) != 1:
        raise ValueError(f"Expected one {label}, found {len(found)}")
    return found[0]


def equ(text: str, name: str) -> int:
    match = re.search(rf"^\s*{re.escape(name)}\s+EQU\s+([0-9A-F]+)(H?)\b", text, re.I | re.M)
    if not match:
        raise ValueError(f"Missing source EQU {name}")
    digits, suffix = match.groups()
    return int(digits, 16 if suffix.upper() == "H" else 10)


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
        raise ValueError("Route-matrix trace did not reach its end frame")
    exactly(lines, r"^L8_ROUTE_MATRIX_INIT fresh_nvram_required=1 simultaneous_service_diagnostic=1$", "fresh-run marker")
    exactly(lines, r"^L8_ROUTE_SUMMARY end=2840$", "run summary")

    driver = args.driver.read_text(encoding="utf-8", errors="replace")
    for token in (
        'PORT_BIT( 0x0001, IP_ACTIVE_LOW, IPT_COIN1 )',
        'PORT_BIT( 0x0002, IP_ACTIVE_LOW, IPT_COIN2 )',
        'PORT_SERVICE_NO_TOGGLE( 0x0010, IP_ACTIVE_LOW )',
        'PORT_BIT( 0x0040, IP_ACTIVE_LOW, IPT_SERVICE1 )',
    ):
        if token not in driver:
            raise ValueError(f"Pinned MAME L8 input map lacks {token}")

    audit = (args.source / "AUDIT.ASM").read_text(encoding="latin-1")
    equs = (args.source / "AUDIT.EQU").read_text(encoding="latin-1")
    link = (args.source / "LINK.EQU").read_text(encoding="latin-1")
    routes = ((16, "LC_PID", "LCOIN"), (17, "RC_PID", "RCOIN"),
              (20, "DIAG_PID", "DIAG"), (22, "DIAG_PID", "SERVICE"))
    for switch, pid, routine in routes:
        pattern = rf"\.WORD\s+{pid}\s*;PROCESS ID SWITCH\s+{switch}.*?\n\s*\.LONG\s+{routine}\b"
        if not re.search(pattern, audit, re.I | re.S):
            raise ValueError(f"Source SWTAB switch {switch} no longer routes to {routine}")
    if equ(equs, "AUDRCOIN") != 2 or equ(equs, "AUDSERV") != 5 or equ(equs, "AUDSTAT") != 33:
        raise ValueError("Source audit identifiers changed")
    if (equ(link, "CMOS"), equ(link, "C_BYTE_SIZE"), equ(link, "C_WORD_SIZE"), equ(link, "N_AUDITS")) != (
        0x01400000, 16, 32, 45
    ):
        raise ValueError("Source CMOS/audit layout changed")
    test_source = (args.source / "TEST.ASM").read_text(encoding="latin-1")
    if not re.search(r"DIAG_STATE:.*?MOVI\s+INDIAG,A0\s+MOVE\s+A0,@GAMSTATE,W", test_source, re.I | re.S):
        raise ValueError("Source DIAG_STATE no longer enters INDIAG")
    if not re.search(r"VALID_COIN:.*?CMPI\s+INDIAG,A0\s+JREQ\s+CRX", audit, re.I | re.S):
        raise ValueError("Source VALID_COIN diagnostic suppression changed")
    if not re.search(r"SERVICE\s+MOVE\s+@GAMSTATE,A0,W.*?CMPI\s+INDIAG,A0\s+JREQ\s+SERVICX", audit, re.I | re.S):
        raise ValueError("Source SERVICE diagnostic suppression changed")

    actions = lines[[line.startswith("L8_ROUTE_ACTION ") for line in lines].index(True):]
    action_patterns = (
        r"^L8_ROUTE_ACTION label=simultaneous_coin1_press action=press frame=2600$",
        r"^L8_ROUTE_ACTION label=simultaneous_coin2_press action=press frame=2600$",
        r"^L8_ROUTE_ACTION label=simultaneous_coin1_release action=release frame=2604$",
        r"^L8_ROUTE_ACTION label=simultaneous_coin2_release action=release frame=2604$",
        r"^L8_ROUTE_ACTION label=service_credit_press action=press frame=2700$",
        r"^L8_ROUTE_ACTION label=service_credit_release action=release frame=2704$",
        r"^L8_ROUTE_ACTION label=diagnostic_test_press action=press frame=2800$",
        r"^L8_ROUTE_ACTION label=diagnostic_test_release action=release frame=2804$",
        r"^L8_ROUTE_ACTION label=coin_during_diagnostic_press action=press frame=2810$",
        r"^L8_ROUTE_ACTION label=coin_during_diagnostic_release action=release frame=2811$",
    )
    if len([line for line in lines if line.startswith("L8_ROUTE_ACTION ")]) != len(action_patterns):
        raise ValueError("Unexpected input-action count")
    for pattern in action_patterns:
        exactly(actions, pattern, pattern.split("label=")[1].split(" ")[0])

    flag_lines = [line for line in lines if line.startswith("L8_ROUTE_FLAG ")]
    expected_flags = [
        (2601, "C_FLAGS+0", 30), (2601, "C_FLAGS+8", 0),
        (2602, "C_FLAGS+8", 30), (2605, "C_FLAGS+8", 0), (2605, "C_FLAGS+0", 0),
    ]
    actual_flags = []
    for line in flag_lines:
        match = re.search(r"frame=(\d+) name=(C_FLAGS\+\d+) value=(\d+) ", line)
        if not match:
            raise ValueError(f"Malformed flag write {line}")
        actual_flags.append((int(match[1]), match[2], int(match[3])))
    if actual_flags != expected_flags:
        raise ValueError(f"Unexpected coin-flag sequence {actual_flags}")

    cmos_lines = [line for line in lines if line.startswith("L8_ROUTE_CMOS ")]
    expected_cmos = [
        (2606, "AUDRCOIN", 0x014000f0, 1), (2606, "CREDITS", 0x014010e0, 1),
        (2701, "AUDSERV", 0x01400210, 1), (2701, "CREDITS", 0x014010e0, 2),
        (2801, "AUDSTAT", 0x01400c90, 0),
    ]
    actual_cmos = []
    for line in cmos_lines:
        match = re.search(r"frame=(\d+) name=(\w+) address=([0-9a-f]+) value=(\d+) ", line)
        if not match:
            raise ValueError(f"Malformed CMOS write {line}")
        actual_cmos.append((int(match[1]), match[2], int(match[3], 16), int(match[4])))
    if actual_cmos != expected_cmos:
        raise ValueError(f"Unexpected watched CMOS sequence {actual_cmos}")

    input_lines = [line for line in lines if line.startswith("L8_ROUTE_INPUT ")]
    for frame, data, label in ((2600, "0000fffc", "simultaneous coin switches"),
                               (2700, "0000ffbf", "service-credit switch"),
                               (2800, "0000ffef", "test/diagnostic switch"),
                               (2810, "0000fffe", "coin1 during diagnostic")):
        for pc in ("ffe0d660", "ffe0d6c0"):
            exactly(input_lines, rf"^L8_ROUTE_INPUT frame={frame} data={data} pc={pc}$", f"{label} read at {pc}")
    if any(re.search(r"^L8_ROUTE_FLAG frame=28\d\d .*value=30 ", line) for line in flag_lines):
        raise ValueError("A chute flag started during the diagnostic input window")
    if any(re.search(r"^L8_ROUTE_CMOS frame=28\d\d name=(AUDLCOIN|AUDRCOIN|AUDSERV|CREDITS) ", line) for line in cmos_lines):
        raise ValueError("Coin/service accounting changed after diagnostic entry")

    report = {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Fresh-NVRAM MAME 0.289 L8 simultaneous-chute, service-credit and diagnostic coin-suppression reference trace; one run only, not native process equivalence or PCB validation",
        "reference": {"mame_version": version, "mame_executable_sha256": sha256(args.mame),
                      "rom_zip_sha256": sha256(args.rom), "driver": str(args.driver.resolve()).replace("\\", "/"),
                      "driver_sha256": sha256(args.driver), "fresh_nvram_and_config": True},
        "source_checked": {"swtabs": {str(switch): f"{pid}/{routine}" for switch, pid, routine in routes},
                           "audit_identifiers": {"AUDRCOIN": 2, "AUDSERV": 5, "AUDSTAT": 33},
                           "test_diag_state_entry": "TEST.ASM DIAG_STATE stores INDIAG in GAMSTATE",
                           "coin_and_service_diagnostic_guards": "AUDIT.ASM VALID_COIN and SERVICE return when GAMSTATE is INDIAG"},
        "observations": {
            "simultaneous_coin1_coin2": {
                "press_frame": 2600, "release_frame": 2604, "active_low_readback": "0xfffc",
                "flag_initializations": [[2601, "C_FLAGS+0", 30], [2602, "C_FLAGS+8", 30]],
                "flag_clear_frame": 2605, "accepted_audit": "AUDRCOIN", "AUDLCOIN_incremented": False,
                "credit_write": [2606, 1],
                "interpretation": "Both chute flags started, but only the right-chute audit and one credit were written in this observed dispatch order."
            },
            "service_credit": {"press_frame": 2700, "release_frame": 2704,
                               "active_low_readback": "0xffbf", "AUDSERV_write": [2701, 1],
                               "credit_write": [2701, 2]},
            "diagnostics": {"test_switch_press_frame": 2800, "AUDSTAT_write": [2801, 0],
                            "coin1_input_frame": 2810, "coin1_active_low_readback": "0xfffe",
                            "scanner_read_pcs": ["0xffe0d660", "0xffe0d6c0"],
                            "new_coin_flag_or_accounting": False,
                            "direct_GAMSTATE_memory_read": False},
        },
        "logs": {"path": str(args.log).replace("\\", "/"),
                 "sha256": sha256(args.log), "script": str(args.script).replace("\\", "/"),
                 "script_sha256": sha256(args.script)},
        "limitations": [
            "The simultaneous-chute winner is this run's observed result; other frame schedules and coin combinations are not established.",
            "AUDSTAT plus the tested DIAG path confirms diagnostics were entered, but GAMSTATE itself was not directly read from MAME memory.",
            "Only the selected service switch event is covered; service audio/display behavior and process-owner scheduling remain unimplemented natively.",
            "The source 5.00 mapping and MAME L8 execution are not proof of full source-retail equivalence or physical PCB behavior.",
            "No coin3 adjustment was changed; no memory pokes were used.",
        ],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS: simultaneous chute result, service credit, and diagnostic coin suppression matched the captured route trace")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
