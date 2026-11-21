#!/usr/bin/env python3
"""Validate one bounded MAME 0.289 L8 coin timer/audit write witness."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_line(lines: list[str], pattern: str, label: str) -> tuple[int, str]:
    regex = re.compile(pattern)
    matches = [(index, line) for index, line in enumerate(lines) if regex.search(line)]
    if len(matches) != 1:
        raise ValueError(f"Expected one {label}, found {len(matches)}")
    return matches[0]


def equ_number(source: str, name: str) -> int:
    match = re.search(rf"^\s*{re.escape(name)}\s+EQU\s+([0-9A-F]+)(H?)\b", source, re.I | re.M)
    if not match:
        raise ValueError(f"Could not resolve source EQU {name}")
    digits, is_hex = match.groups()
    return int(digits, 16 if is_hex.upper() == "H" else 10)


def verify(args: argparse.Namespace) -> dict:
    log_text = args.log.read_text(encoding="utf-8", errors="replace")
    lines = log_text.splitlines()
    if "Average speed:" not in log_text:
        raise ValueError("MAME did not finish the bounded witness")

    reference = json.loads(args.reference_report.read_text(encoding="utf-8"))
    if sha256(args.mame).lower() != reference["mame_executable_sha256"].lower():
        raise ValueError("MAME executable differs from the recorded L8 reference binary")
    if sha256(args.rom_zip).lower() != reference["archive_sha256"].lower():
        raise ValueError("ROM archive differs from the recorded L8 reference archive")
    version = subprocess.run([str(args.mame), "-version"], capture_output=True,
                             text=True, timeout=20, check=True).stdout.strip()
    if not version.startswith("0.289"):
        raise ValueError(f"Expected MAME 0.289, got {version}")

    audit_equ = args.source / "AUDIT.EQU"
    link_equ = args.source / "LINK.EQU"
    audit_source = audit_equ.read_text(encoding="latin-1")
    link_source = link_equ.read_text(encoding="latin-1")
    audit_number = equ_number(audit_source, "AUDLCOIN")
    byte_units = equ_number(link_source, "C_BYTE_SIZE")
    word_units = equ_number(link_source, "C_WORD_SIZE")
    audit_count = equ_number(link_source, "N_AUDITS")
    cmos_base = equ_number(link_source, "CMOS")
    audit_size = 4 * byte_units + word_units
    audit_address = cmos_base + audit_number * audit_size
    credits_address = cmos_base + audit_count * audit_size
    if (audit_number, byte_units, word_units, audit_count, cmos_base) != (1, 16, 32, 45, 0x01400000):
        raise ValueError("Surviving source audit layout differs from the expected L8 witness mapping")
    if audit_address != 0x01400060 or credits_address != 0x014010e0:
        raise ValueError("Source-derived audit/credit addresses do not match the witnessed map")

    press = require_line(lines, r"^L8_COIN_RAM_ACTION press frame=2600$", "coin press")[0]
    candidate = require_line(lines, r"^L8_COIN_TIMER_CANDIDATE frame=2601 address=010ee680 lane=0 value=30\b", "timer-byte candidate initialization")[0]
    release = require_line(lines, r"^L8_COIN_RAM_ACTION release frame=2604$", "coin release")[0]
    cleared = require_line(lines, r"^L8_COIN_TIMER_WORD_WRITE frame=2605 address=010ee680 lane=0 old=30 new=0\b", "timer-byte candidate clear")[0]
    if not (press < candidate < release < cleared):
        raise ValueError("Coin input and candidate-byte events are out of order")

    audit_events = []
    for index in range(4):
        address = audit_address + index * byte_units
        expected_value = 1 if index == 3 else 0
        event_index, line = require_line(
            lines,
            rf"^L8_COIN_CMOS_WRITE kind=other frame=2606 address={address:08x} data={expected_value:08x} mask=0000ffff pc=ffeaa1f0$",
            f"left-coin audit data unit {index}",
        )
        audit_events.append((event_index, address, expected_value))
    checksum_events = []
    for address in (audit_address + 4 * byte_units, audit_address + 4 * byte_units + byte_units):
        event_index, _ = require_line(
            lines,
            rf"^L8_COIN_CMOS_WRITE kind=other frame=2606 address={address:08x} data=000000ff mask=0000ffff pc=ffeaa1f0$",
            f"left-coin audit checksum byte at {address:08x}",
        )
        checksum_events.append(event_index)
    credit_index, _ = require_line(
        lines,
        rf"^L8_COIN_CMOS_WRITE kind=credit frame=2606 address={credits_address:08x} data=00000001 mask=0000ffff pc=ffeaa1f0$",
        "credit write",
    )
    if not (cleared < min(index for index, *_ in audit_events) <= max(index for index, *_ in audit_events) < credit_index):
        raise ValueError("Timer-byte clear, coin audit and credit writes are out of order")
    if max(checksum_events) >= credit_index:
        raise ValueError("Audit checksum writes did not precede the credit write")

    return {
        "result": "PASS",
        "scope": "One executed MAME L8 short-pulse timer-byte candidate, left-coin audit and credit transaction; not full source-retail or PCB equivalence",
        "reference": {
            "mame_version": version,
            "mame_executable_sha256": sha256(args.mame),
            "rom_zip_sha256": sha256(args.rom_zip),
            "isolated_nvram_and_config": True,
        },
        "trace": {
            "log_sha256": sha256(args.log),
            "script_sha256": sha256(args.script),
            "coin_press_frame": 2600,
            "coin_release_frame": 2604,
            "timer_byte_candidate": {
                "runtime_address": "0x010ee680",
                "initial_value": 30,
                "initial_frame": 2601,
                "clear_value": 0,
                "clear_frame": 2605,
                "initial_pc": "0xffeb0150",
                "clear_pc": "0xffe0eb80",
                "symbol_mapping": "candidate only; L8 runtime address is not mapped to a source5.00 symbol",
            },
            "direct_coin_audit": {
                "source_audit_number": audit_number,
                "source_record_address": f"0x{audit_address:08x}",
                "data_units": [
                    {"address": f"0x{address:08x}", "value": value}
                    for _, address, value in audit_events
                ],
                "checksum_units": ["0xff", "0xff"],
                "write_frame": 2606,
                "write_pc": "0xffeaa1f0",
            },
            "credit": {"address": f"0x{credits_address:08x}", "value": 1, "write_frame": 2606},
        },
        "source_layout": {
            "audit_equ_sha256": sha256(audit_equ),
            "link_equ_sha256": sha256(link_equ),
            "c_byte_size_address_units": byte_units,
            "c_word_size_address_units": word_units,
            "audit_record_size_address_units": audit_size,
            "audit_record_count": audit_count,
        },
        "limitations": [
            "The captured runtime RAM address is a timer-byte candidate, not a linked source-symbol mapping.",
            "One fresh-NVRAM left-chute pulse only; other chutes, slam, diagnostics, bounce, all debounce phases and mechanical counter modes remain untested.",
            "This reference evidence does not connect raw PC coin input to GameSession or implement start/player creation.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("script", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("rom_zip", type=Path)
    parser.add_argument("mame", type=Path)
    parser.add_argument("reference_report", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Report already exists; refusing to overwrite it")
    result = verify(args)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("PASS: observed L8 timer-byte candidate clear, direct left-coin audit and credit write")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
