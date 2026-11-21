#!/usr/bin/env python3
"""Verify bounded MAME L8 chute pulses and the live disabled XCOIN adjustment."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def one(lines: list[str], pattern: str, label: str) -> tuple[int, str]:
    found = [(i, line) for i, line in enumerate(lines) if re.search(pattern, line)]
    if len(found) != 1:
        raise ValueError(f"Expected exactly one {label}, found {len(found)}")
    return found[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sweep-log", type=Path, required=True)
    parser.add_argument("--coin3-read-log", type=Path, required=True)
    parser.add_argument("--adjustment-log", type=Path, required=True)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--mame-driver", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    sweep = args.sweep_log.read_text(encoding="utf-8", errors="replace").splitlines()
    coin3_read = args.coin3_read_log.read_text(encoding="utf-8", errors="replace").splitlines()
    adjustment = args.adjustment_log.read_text(encoding="utf-8", errors="replace").splitlines()
    if not any(line.startswith("Average speed:") for line in sweep):
        raise ValueError("The bounded chute sweep did not finish")
    if not any(line.startswith("Average speed:") for line in coin3_read):
        raise ValueError("The bounded coin3 input-read witness did not finish")
    if not any(line.startswith("Average speed:") for line in adjustment):
        raise ValueError("The bounded XCOIN adjustment witness did not finish")

    version = subprocess.run([str(args.mame), "-version"], capture_output=True,
                             text=True, timeout=20, check=True).stdout.strip()
    if not version.startswith("0.289"):
        raise ValueError(f"Expected MAME 0.289, got {version}")
    driver = args.mame_driver.read_text(encoding="utf-8", errors="replace")
    for token in (
        "PORT_BIT( 0x0001, IP_ACTIVE_LOW, IPT_COIN1 )",
        "PORT_BIT( 0x0002, IP_ACTIVE_LOW, IPT_COIN2 )",
        "PORT_BIT( 0x0080, IP_ACTIVE_LOW, IPT_BILL1 )",
        "PORT_BIT( 0x0200, IP_ACTIVE_LOW, IPT_COIN3 )",
    ):
        if token not in driver:
            raise ValueError(f"MAME L8 driver input mapping is missing {token}")

    audit_equ = (args.source / "AUDIT.EQU").read_text(encoding="latin-1")
    link_equ = (args.source / "LINK.EQU").read_text(encoding="latin-1")
    def equ(text: str, name: str) -> int:
        match = re.search(rf"^\s*{re.escape(name)}\s+EQU\s+([0-9A-F]+)(H?)\b", text, re.I | re.M)
        if not match:
            raise ValueError(f"Could not resolve source EQU {name}")
        digits, suffix = match.groups()
        return int(digits, 16 if suffix.upper() == "H" else 10)

    byte_bits = equ(link_equ, "C_BYTE_SIZE")
    long_bits = equ(link_equ, "C_LONG_SIZE")
    cmos = equ(link_equ, "CMOS")
    wpcmos = equ(link_equ, "WPCMOS")
    xcoin_adjustment = equ(audit_equ, "ADJXMULT")
    coin_audit_numbers = {name: equ(audit_equ, name) for name in ("AUDLCOIN", "AUDRCOIN", "AUDCCOIN")}
    audit_size = 4 * byte_bits + equ(link_equ, "C_WORD_SIZE")
    audit_count = equ(link_equ, "N_AUDITS")
    audit_records = {name: cmos + number * audit_size for name, number in coin_audit_numbers.items()}
    credits_address = cmos + audit_count * audit_size
    expected_adjustment_address = wpcmos + xcoin_adjustment * long_bits
    if (byte_bits, long_bits, cmos, wpcmos, xcoin_adjustment, expected_adjustment_address) != (
        16, 64, 0x01400000, 0x01404000, 5, 0x01404140
    ):
        raise ValueError("Surviving source adjustment/CMOS address formula changed")
    if (audit_size, audit_records, credits_address) != (
        96, {"AUDLCOIN": 0x01400060, "AUDRCOIN": 0x014000c0, "AUDCCOIN": 0x01400120}, 0x014010e0
    ):
        raise ValueError("Surviving source audit/credit address formula changed")

    pulses = []
    for label, press, release, initialized, cleared, credit in (
        ("coin1-1f", 2600, 2601, 2601, 2605, 1),
        ("coin1-4f", 2700, 2704, 2701, 2703, 2),
        ("coin1-8f", 2800, 2808, 2801, 2807, 3),
        ("coin1-20f", 2900, 2920, 2901, 2903, 4),
    ):
        press_i, _ = one(sweep, rf"^L8_COIN_ACTION label={label} action=press frame={press} ", f"{label} press")
        release_i, _ = one(sweep, rf"^L8_COIN_ACTION label={label} action=release frame={release} ", f"{label} release")
        init_i, _ = one(sweep, rf"^L8_COIN_FLAG frame={initialized} source=C_FLAGS\+0 address=010ee680 .*value=30 ", f"{label} timer start")
        clear_i, _ = one(sweep, rf"^L8_COIN_FLAG frame={cleared} source=C_FLAGS\+0 address=010ee680 .*value=0 ", f"{label} timer clear")
        audit_i, _ = one(sweep, rf"^L8_COIN_CMOS frame={cleared + 1} address={audit_records['AUDLCOIN'] + 3 * byte_bits:08x} data=0000000{credit} ", f"{label} AUDLCOIN count")
        credit_i, _ = one(sweep, rf"^L8_COIN_CMOS frame={cleared + 1} address={credits_address:08x} data=0000000{credit} ", f"{label} credits")
        if not (press_i < init_i and press_i < release_i and clear_i < audit_i < credit_i):
            raise ValueError(f"{label} timer, release, audit and credit order changed")
        pulses.append({"frames_held": release - press, "press_frame": press,
                       "release_frame": release, "flag_start_frame": initialized,
                       "flag_clear_frame": cleared, "accepted": True,
                       "running_credit_total": credit, "audit_credit_write_frame": cleared + 1})

    coin2_press, _ = one(sweep, r"^L8_COIN_ACTION label=coin2-4f action=press frame=3000 ", "coin2 press")
    coin2_flag, _ = one(sweep, r"^L8_COIN_FLAG frame=3001 source=C_FLAGS\+8 address=010ee688 .*value=30 ", "coin2 flag")
    coin2_audit, _ = one(sweep, rf"^L8_COIN_CMOS frame=3008 address={audit_records['AUDRCOIN'] + 3 * byte_bits:08x} data=00000001 ", "coin2 AUDRCOIN unit")
    coin2_credit, _ = one(sweep, rf"^L8_COIN_CMOS frame=3008 address={credits_address:08x} data=00000005 ", "coin2 credit total")
    if not coin2_press < coin2_flag < coin2_audit < coin2_credit:
        raise ValueError("Coin2 flag/audit/credit sequence changed")

    coin3_press, _ = one(sweep, r"^L8_COIN_ACTION label=coin3-4f action=press frame=3100 .*mask=0200$", "coin3 press")
    one(sweep, r"^L8_COIN_ACTION label=coin3-4f action=release frame=3104 mask=0200$", "coin3 release")
    one(coin3_read, r"^L8_COIN_INPUT frame=3100 address=01c00010 data=0000fdff .*pc=ffe0d660$", "coin3 active-low read")
    for pc in ("ffe0d660", "ffe0d6c0"):
        one(coin3_read, rf"^L8_COIN_INPUT frame=3100 address=01c00010 data=0000fdff .*pc={pc}$", f"coin3 scanner read at {pc}")
    if any(re.search(r"^L8_COIN_FLAG frame=31\d\d source=C_FLAGS\+24 address=010ee698 .*value=30 ", line) for line in sweep):
        raise ValueError("Disabled coin3 unexpectedly started the XCOIN flag")
    if any(re.search(r"^L8_COIN_CMOS frame=31\d\d ", line) for line in sweep):
        raise ValueError("Disabled coin3 unexpectedly wrote an audit or credit in its test window")

    bill_press, _ = one(sweep, r"^L8_COIN_ACTION label=bill1-4f action=press frame=3200 .*mask=0080$", "MAME bill1 press")
    bill_flag, _ = one(sweep, r"^L8_COIN_FLAG frame=3201 source=C_FLAGS\+16 address=010ee690 .*value=30 ", "MAME bill1 source flag")
    bill_audit, _ = one(sweep, rf"^L8_COIN_CMOS frame=3208 address={audit_records['AUDCCOIN'] + 3 * byte_bits:08x} data=00000001 ", "AUDCCOIN count")
    bill_credit, _ = one(sweep, rf"^L8_COIN_CMOS frame=3208 address={credits_address:08x} data=00000009 ", "bill1 priced credit total")
    if not bill_press < bill_flag < bill_audit < bill_credit:
        raise ValueError("MAME bill1 to CCOIN/AUDCCOIN route changed")

    p2_start, _ = one(adjustment, r"^L8_ACTION start2_press frame=4500$", "P2 start press")
    one(adjustment, r"^L8_CREDIT frame=4501 address=014010e0 data=00000000 ", "P2 start debit")
    one(adjustment, r"^L8_COIN3_INPUT frame=4800 data=0000fdff pc=ffe0d660$", "coin3 active-low read in the later session")
    adjustment_reads = []
    for address in ("01404140", "01404150", "01404160", "01404170"):
        _, line = one(adjustment, rf"^L8_XCOIN_ADJUSTMENT frame=4801 address={address} data=00000000 ", f"zero XCOIN adjustment read {address}")
        adjustment_reads.append(line)
    if any(re.search(r"^L8_COIN_FLAG frame=48\d\d source=C_FLAGS\+24 address=010ee698 .*value=30 ", line) for line in adjustment):
        raise ValueError("XCOIN timer started despite the zero ADJXMULT read")

    mame_driver = args.mame_driver
    report = {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Executed MAME 0.289 L8 chute-route and pulse-width evidence; selected reference configuration only, not PCB timing or native coin-dispatch equivalence",
        "reference": {
            "mame_version": version,
            "mame_executable_sha256": sha256(args.mame),
            "rom_zip_sha256": sha256(args.rom),
            "mame_driver": str(mame_driver).replace("\\", "/"),
            "mame_driver_sha256": sha256(mame_driver),
            "fresh_isolated_nvram_and_config": True,
        },
        "logs": {
            "chute_sweep": {"path": "evidence/logs/l8-mame-coin-chute-sweep-2026-09-23.log", "sha256": sha256(args.sweep_log)},
            "coin3_active_low_input": {"path": "evidence/logs/l8-mame-coin-chute-input-readback-2026-09-23.log", "sha256": sha256(args.coin3_read_log)},
            "coin3_adjustment": {"path": "evidence/logs/l8-mame-coin3-adjustment-zero-2026-09-23.log", "sha256": sha256(args.adjustment_log)},
            "chute_script_sha256": sha256(Path("tools/mame_l8_coin_chute_sweep.lua")),
            "coin3_script_sha256": sha256(Path("tools/mame_l8_coin3_active_p2.lua")),
        },
        "coin1_pulse_sweep": pulses,
        "additional_routes": {
            "coin2": {"mame_mask": "0x0002", "flag": "C_FLAGS+8 at 0x010ee688", "audit_record": "AUDRCOIN at 0x014000c0", "counter_unit_address": "0x014000f0", "credit_total_after": 5, "credit_audit_frame": 3008},
            "bill1": {"mame_mask": "0x0080", "source_route": "CCOIN -> C_FLAGS+16 -> AUDCCOIN", "flag_address": "0x010ee690", "audit_record": "0x01400120", "counter_unit_address": "0x01400150", "credit_total_after": 9, "credit_audit_frame": 3208},
            "coin3": {"mame_mask": "0x0200", "active_low_value": "0x0000fdff", "source_route": "XCOIN -> ADJXMULT index 5", "flag_address": "0x010ee698", "started_flag": False,
                      "adjustment_address": f"0x{expected_adjustment_address:08x}", "adjustment_value": 0,
                      "adjustment_read_frame": 4801, "input_read_frames": [3100, 4800], "accepted": False},
        },
        "source_address_formula": {
            "WPCMOS": f"0x{wpcmos:08x}", "ADJXMULT_index": xcoin_adjustment,
            "C_LONG_SIZE_bits": long_bits, "resolved_address": f"0x{expected_adjustment_address:08x}",
            "audit_record_stride_bits": audit_size,
            "audit_record_addresses": {name: f"0x{address:08x}" for name, address in audit_records.items()},
            "credits_address": f"0x{credits_address:08x}",
        },
        "limitations": [
            "Pulse sweep was run at selected frame phases; it does not establish a general minimum coin-pulse duration or bounce threshold.",
            "The bill1 field is the MAME input name; the executed source5.00 route enters CCOIN/AUDCCOIN. No physical acceptor wiring is asserted.",
            "The coin3 rejection is explained by the live zero ADJXMULT read; this run does not test coin3 after changing its operator adjustment.",
            "MAME execution and ROM/source data correlation are reference evidence, not PCB bus proof, full source-retail identity, or native process timing.",
        ],
    }
    rendered = json.dumps(report, indent=2) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
