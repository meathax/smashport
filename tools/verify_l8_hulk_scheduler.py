#!/usr/bin/env python3
"""Verify paired live L8 Hulk CREATE/GETPRC/PRCDSP reference traces."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path


MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
ALLOC_ORDER = ["0108ccd0", "0108ab10", "0108c190", "0108b0b0"]
DISPATCH_ORDER = list(reversed(ALLOC_ORDER))
OLD_CREATOR_LINK = "01089490"
CREATE_PCS = {
    "GETPRC_ENTRY": "ffe0aba0",
    "FREE_HEAD_READ": "ffe0abf0",
    "FREE_NEXT_READ": "ffe0ac10",
    "FREE_HEAD_POPPED": "ffe0ac40",
    "NEW_LINK_SET": "ffe0ac50",
    "ACTIVE_LINK_SET": "ffe0ac90",
    "PROCESS_IMAGE_WRITE": "ffe0adf0",
    "WAKE_WORD_READY": "ffe0ae20",
    "WAKE_WORD_WRITTEN": "ffe0ae30",
    "ID_WORD_WRITTEN": "ffe0ae40",
    "GETPRC_RETURN": "ffe0ae60",
}
EXPECTED_TAP_COUNTS = {
    "FREE_HEAD_POP": 8,
    "NEW_PROCESS_LINK": 8,
    "CREATOR_ACTIVE_LINK": 8,
    "PROCESS_IMAGE": 48,
    "PTIME_INITIALIZED": 4,
    "PROCID_INITIALIZED": 4,
    "PTIME_DISPATCH_DECREMENT": 4,
    "WAKE_PC_READ": 4,
    "FIRST_OHULK_CFLAGS_READ": 4,
}
EXPECTED_SCHEDULER_HISTORY = (
    "ffe0a500: move   *a13,a13,1",
    "ffe0a520: move   *a13(30h),a0,0",
    "ffe0a550: move   a0,*a13(30h),0",
    "ffe0a580: move   a13,a1",
    "ffe0a590: addi   40h,a1",
    "ffe0a5b0: mmfm   a1,a7-a12",
    "ffe0a5d0: jump   a7",
    "ffe3b8e0: move   *a13(130h),a0,0",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fields(line: str) -> dict[str, str]:
    result = dict(re.findall(r"([a-z0-9_]+)=([^\s]+)", line.lower()))
    if "stage" in result:
        result["stage"] = result["stage"].upper()
    return result


def read_run(path: Path) -> tuple[str, list[str], list[dict[str, str]], list[str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    for marker in (
        "L8_ACTION coin1_press frame=2600",
        "L8_ACTION start1_release frame=3170",
        "L8_HULK_SCHED_DONE frame=4300",
        "Average speed:",
    ):
        if marker not in text:
            raise ValueError(f"run is incomplete or missing {marker!r}: {path}")
    if "[LUA ERROR]" in text or "L8_HULK_SCHED_ERROR" in text:
        raise ValueError(f"MAME Lua/debugger probe failed: {path}")
    raw_lines = text.splitlines()
    tap_lines = [line for line in raw_lines if line.startswith("L8_HULK_TAP ")]
    tap_rows = [fields(line) for line in tap_lines]
    counter = Counter(row.get("stage", "") for row in tap_rows)
    if counter != Counter(EXPECTED_TAP_COUNTS):
        raise ValueError(f"unexpected tap event counts in {path}: {dict(counter)}")
    debug_lines = [line for line in raw_lines if line.startswith("L8_DEBUGGER ")]
    return text, tap_lines, tap_rows, debug_lines


def one(rows: list[dict[str, str]], stage: str, process: str) -> dict[str, str]:
    matches = [row for row in rows if row.get("stage") == stage and
               process in (row.get("process"), row.get("a0"), row.get("a13"))]
    if len(matches) != 1:
        raise ValueError(f"expected one {stage} event for process {process}, got {len(matches)}")
    return matches[0]


def process_pointer(row: dict[str, str]) -> str:
    return (row.get("process") or row.get("a0") or "").lower().removeprefix("0x")


def history_instructions(lines: list[str]) -> list[str]:
    begin = next((i for i, line in enumerate(lines)
                  if "L8_HULK_SCHED_HISTORY_BEGIN" in line and "process=0108b0b0" in line.lower()), None)
    if begin is None:
        raise ValueError("no first OHULK dispatch history for the last-created HULK process")
    end = next((i for i in range(begin + 1, len(lines))
                if "L8_HULK_SCHED_HISTORY_END" in lines[i]), None)
    if end is None:
        raise ValueError("first OHULK dispatch history block is unterminated")
    result: list[str] = []
    for line in lines[begin + 1:end]:
        match = re.match(r"L8_DEBUGGER frame=\d+ emutime=[0-9.]+ (FFE[0-9A-F]+:.*)$", line)
        if match:
            result.append(match.group(1).lower())
    cursor = 0
    for expected in EXPECTED_SCHEDULER_HISTORY:
        index = next((i for i in range(cursor, len(result)) if result[i] == expected), None)
        if index is None:
            raise ValueError(f"first-dispatch history omits/reorders {expected}")
        cursor = index + 1
    return result


def verify(args: argparse.Namespace) -> dict[str, object]:
    if sha256(args.mame) != MAME_SHA256:
        raise ValueError("MAME executable hash differs from pinned 0.289 reference")
    if sha256(args.rom_zip) != ROM_SHA256:
        raise ValueError("supplied L8 archive hash differs from verified identity")

    manifest_path = args.disassembly_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("rom_archive_sha256") != ROM_SHA256:
        raise ValueError("scheduler disassembly used a different ROM archive")
    if manifest.get("reference", {}).get("mame_executable_sha256") != MAME_SHA256:
        raise ValueError("scheduler disassembly names a different MAME reference")
    listing_hashes: dict[str, str] = {}
    for name, details in manifest.get("targets", {}).items():
        listing = args.disassembly_dir / details["listing"]
        if sha256(listing) != details.get("listing_sha256"):
            raise ValueError(f"listing digest mismatch for {name}")
        listing_text = listing.read_text(encoding="utf-8").lower()
        for instruction in details.get("verified_instructions", []):
            if instruction not in listing_text:
                raise ValueError(f"unmapped instruction in {name}: {instruction}")
        listing_hashes[name] = details["listing_sha256"]
    if set(listing_hashes) != {"oh1-create-hulk", "mproc-getprc-xfer", "mproc-prcdsp-first-dispatch"}:
        raise ValueError("scheduler disassembly target set changed")

    source_hulks = args.hulks_source.read_text(encoding="latin-1")
    source_mproc = args.mproc_source.read_text(encoding="latin-1").upper()
    source_equ = args.mproc_equ.read_text(encoding="latin-1")
    if not re.search(r"(?im)^OH1\s+PUSH\s+A2\s*;CREATE A BUNCH OF THEM", source_hulks):
        raise ValueError("HULKS.ASM OH1 setup no longer maps to the expected create loop")
    if not re.search(r"(?im)^\s*CREATE\s+HULKPID\s*,\s*OHULK\b", source_hulks):
        raise ValueError("HULKS.ASM no longer creates OHULK with HULKPID")
    create_macro = source_equ[source_equ.upper().find("CREATE\t$MACRO"):]
    for fragment in ("MOVI\t:P1:,A1", "MOVI\t:P2:,A7,L", "CALLA\tGETPRC"):
        if fragment not in create_macro:
            raise ValueError(f"MPROC.EQU CREATE macro omits {fragment}")
    getprc_start = source_mproc.upper().find("GETPRC:")
    allocator_end = source_mproc.find("\nKILALL:", getprc_start)
    dispatcher_start = source_mproc.upper().find("PRCDSP:")
    suicide_start = source_mproc.upper().find("*PROCESS SUICIDE", dispatcher_start)
    if min(getprc_start, allocator_end, dispatcher_start, suicide_start) < 0:
        raise ValueError("MPROC.ASM GETPRC/PRCDSP source boundaries are missing")
    getprc_source = source_mproc[getprc_start:allocator_end]
    dispatcher_source = source_mproc[dispatcher_start:suicide_start]
    for fragment in ("MOVE\t@FREE,A0,L", "MOVE\t*A0,A2,L", "MOVE\tA2,@FREE,L",
                     "MOVE\t*A13,*A0,L", "MOVE\tA0,*A13,L", "MMTM\tA2,A7,A8,A9,A10,A11,A12",
                     "MOVE\tA12,-*A2,W", "MOVE\tA1,-*A2,W"):
        if fragment not in getprc_source:
            raise ValueError(f"GETPRC source omits {fragment}")
    for fragment in ("MOVE\t*A13,A13,L", "MOVE\t*A13(PTIME),A0", "DEC\tA0",
                     "MOVE\tA0,*A13(PTIME)", "MMFM\tA1,A7,A8,A9,A10,A11,A12", "JUMP\tA7"):
        if fragment not in dispatcher_source:
            raise ValueError(f"PRCDSP source omits {fragment}")
    for field, value in (("PLINK", "0"), ("PROCID", "20H"), ("PTIME", "30H"),
                         ("PSPTR", "40H"), ("PA11", "60H"), ("PA10", "80H"),
                         ("PA9", "0A0H"), ("PA8", "0C0H"), ("PWAKE", "0E0H"),
                         ("PRCSIZ", "5A0H")):
        if not re.search(rf"(?im)^\s*{field}\s*\.SET\s+{value}\b", source_equ):
            raise ValueError(f"MPROC.EQU process field {field} no longer equals {value}")

    text_a, taps_a, rows_a, debug_a = read_run(args.log_a)
    text_b, taps_b, rows_b, debug_b = read_run(args.log_b)
    if taps_a != taps_b or rows_a != rows_b:
        raise ValueError("fresh MAME runs disagree on process memory tap stream")
    if any("L8_HULK_SCHED_ERROR" in text for text in (text_a, text_b)):
        raise ValueError("debugger trace initialization failed")

    allocator_rows = [fields(line) for line in debug_a
                      if "L8_HULK_SCHED stage=GETPRC_ENTRY" in line]
    returned_rows = [fields(line) for line in debug_a
                     if "L8_HULK_SCHED stage=GETPRC_RETURN" in line]
    if len(allocator_rows) != 4 or len(returned_rows) != 4:
        raise ValueError("expected exactly four OHULK GETPRC entries and returns")
    if [row["a0"] for row in returned_rows] != ALLOC_ORDER:
        raise ValueError("GETPRC free-list allocation order changed")
    if any(row.get("id") != "000000ec" or row.get("a7") != "ffe3b8e0" for row in allocator_rows):
        raise ValueError("allocator call does not match HULKPID/OHULK parameters")
    for stage, pc in CREATE_PCS.items():
        matches = [fields(line) for line in debug_a if f"L8_HULK_SCHED stage={stage}" in line]
        if len(matches) != 4 or any(row.get("pc") != pc for row in matches):
            raise ValueError(f"wrong/missing live GETPRC boundary {stage}")

    free_order = []
    for row in rows_a:
        if row["stage"] == "FREE_HEAD_POP" and row["address"] == "01000020":
            free_order.append(row["a0"][-8:])
    if free_order != ALLOC_ORDER:
        raise ValueError(f"free-list head observations do not match allocation: {free_order}")

    pointer_rows: list[dict[str, object]] = []
    old_links = [OLD_CREATOR_LINK, ALLOC_ORDER[0], ALLOC_ORDER[1], ALLOC_ORDER[2]]
    for process, expected_next in zip(ALLOC_ORDER, old_links, strict=True):
        low = next((row for row in rows_a if row["stage"] == "NEW_PROCESS_LINK" and
                    row["address"] == process and row["a0"][-8:] == process), None)
        high = next((row for row in rows_a if row["stage"] == "NEW_PROCESS_LINK" and
                     row["address"] == f"{int(process, 16) + 0x10:08x}" and
                     row["a0"][-8:] == process), None)
        if low is None or high is None or low["data"][-4:] != expected_next[-4:] or high["data"] != "00000108":
            raise ValueError(f"GETPRC active-list predecessor link is wrong for {process}")

    creator_links = [row for row in rows_a if row["stage"] == "CREATOR_ACTIVE_LINK" and
                     row["address"] == "0108d810"]
    creator_link_highs = [row for row in rows_a if row["stage"] == "CREATOR_ACTIVE_LINK" and
                          row["address"] == "0108d820"]
    if ([row["data"][-4:] for row in creator_links] != [process[-4:] for process in ALLOC_ORDER] or
            len(creator_link_highs) != 4 or any(row["data"] != "00000108" for row in creator_link_highs)):
        raise ValueError("GETPRC did not link the four new processes after the observed creator")

    for allocation_index, process in enumerate(ALLOC_ORDER, 1):
        image = [row for row in rows_a if row["stage"] == "PROCESS_IMAGE" and row["a0"][-8:] == process]
        ptime = one(rows_a, "PTIME_INITIALIZED", process)
        procid = one(rows_a, "PROCID_INITIALIZED", process)
        if ptime["address"] != f"{int(process, 16) + 0x30:08x}" or ptime["data"] != "00000001":
            raise ValueError(f"GETPRC PTIME initialization is wrong for {process}")
        if procid["address"] != f"{int(process, 16) + 0x20:08x}" or procid["data"] != "000000ec":
            raise ValueError(f"GETPRC PROCID initialization is wrong for {process}")
        wake_low = next((row for row in image if row["address"] == f"{int(process, 16) + 0xe0:08x}"), None)
        wake_high = next((row for row in image if row["address"] == f"{int(process, 16) + 0xf0:08x}"), None)
        if wake_low is None or wake_high is None or wake_low["data"] != "0000b8e0" or wake_high["data"] != "0000ffe3":
            raise ValueError(f"GETPRC saved wake PC is wrong for {process}")
        decrement = one(rows_a, "PTIME_DISPATCH_DECREMENT", process)
        wake_read = one(rows_a, "WAKE_PC_READ", process)
        cflags = one(rows_a, "FIRST_OHULK_CFLAGS_READ", process)
        if decrement["address"] != ptime["address"] or decrement["data"] != "00000000":
            raise ValueError(f"scheduler did not decrement the initial PTIME=1 for {process}")
        if wake_read["data"] != "0000b8e0" or wake_read["pc"] != "ffe0a5d0":
            raise ValueError(f"scheduler wake-PC load did not reach PWAKE for {process}")
        if cflags["pc"] != "ffe3b900" or cflags["a13"][-8:] != process:
            raise ValueError(f"first OHULK instruction read is misattributed for {process}")
        creator = next((row for row in creator_links if row["a0"][-8:] == process), None)
        if creator is None:
            raise ValueError(f"creator active-list link is missing for {process}")
        if creator["address"] != "0108d810" or creator["data"][-4:] != process[-4:]:
            raise ValueError(f"active-list insertion after the observed creator is wrong for {process}")
        pointer_rows.append({
            "allocation_order": allocation_index,
            "first_dispatch_order": DISPATCH_ORDER.index(process) + 1,
            "process_pointer": f"0x{process}",
            "process_id": 236,
            "wake_pc": "0xffe3b8e0",
            "initial_ptime": 1,
            "first_dispatch_ptime": 0,
            "ptime_initialized_emutime_seconds": float(ptime["emutime"]),
            "ptime_decrement_emutime_seconds": float(decrement["emutime"]),
            "wake_pc_read_emutime_seconds": float(wake_read["emutime"]),
            "first_ohulk_cflags_read_emutime_seconds": float(cflags["emutime"]),
            "ptimer_to_first_ohulk_seconds": round(float(cflags["emutime"]) - float(ptime["emutime"]), 9),
            "cflags_first_read_value": f"0x{int(cflags['data'], 16):04x}",
        })

    dispatch_rows = [fields(line) for line in debug_a
                     if "L8_HULK_SCHED_DISPATCH stage=OHULK_FIRST_DISPATCH" in line]
    history_line = next((line for line in debug_a
                         if "L8_HULK_SCHED_HISTORY_BEGIN" in line), None)
    if history_line is None:
        raise ValueError("missing first OHULK dispatch breakpoint history")
    history_begin = fields(history_line)
    dispatched = [history_begin.get("process", "")] + [row.get("process", "") for row in dispatch_rows]
    if dispatched != DISPATCH_ORDER:
        raise ValueError(f"first dispatch order changed: {dispatched}")
    instructions = history_instructions(debug_a)
    history_verified = [instruction for instruction in EXPECTED_SCHEDULER_HISTORY]

    event_stream_sha = hashlib.sha256(("\n".join(taps_a) + "\n").encode("utf-8")).hexdigest()
    result = {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Two identical fresh paid-start MAME L8 runs trace the first-room OH1 HULKPID/OHULK CREATE calls through the free/active lists, initial process image, PTIME gate, first PRCDSP wake and first OHULK CFLAGS read.",
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": MAME_SHA256,
            "rom_archive_sha256": ROM_SHA256,
            "fresh_config_and_nvram_each_run": True,
            "run_to_frame": 4300,
            "creation_and_dispatch_frame": 3533,
        },
        "probe": {
            "lua_probe": str(args.probe),
            "lua_probe_sha256": sha256(args.probe),
            "disassembly_helper": str(args.disassembly_helper),
            "disassembly_helper_sha256": sha256(args.disassembly_helper),
            "disassembly_manifest": str(manifest_path),
            "disassembly_manifest_sha256": sha256(manifest_path),
            "verified_listing_sha256": listing_hashes,
        },
        "runs": [
            {"log": str(args.log_a), "log_sha256": sha256(args.log_a)},
            {"log": str(args.log_b), "log_sha256": sha256(args.log_b)},
        ],
        "reference_event_stream_sha256": event_stream_sha,
        "source_mapping": {
            "HULKS.ASM:OH1": "FFE3B720 pushes the loop count; FFE3B730 loads HULKPID (0x00EC); FFE3B750 loads OHULK wake PC 0xFFE3B8E0; FFE3B780 calls GETPRC.",
            "MPROC.EQU:CREATE": "Expands to ID in A1, entry PC in A7, then CALLA GETPRC.",
            "MPROC.ASM:GETPRC": "FFE0ABA0 consumes @FREE (0x01000020), inserts the new process after current A13, stores A7-A12 process image and initializes PTIME=1 plus PROCID.",
            "MPROC.ASM:PRCDSP": "FFE0A500 follows the active list; FFE0A520/FFE0A550 decrement PTIME; FFE0A5B0 restores A7-A12 and FFE0A5D0 jumps through A7.",
            "MPROC.EQU:PROC": "PLINK=0, PROCID=0x20, PTIME=0x30, PSPTR=0x40, PA11=0x60, PA10=0x80, PA9=0xA0, PA8=0xC0 and PWAKE=0xE0.",
        },
        "allocation_order": [f"0x{process}" for process in ALLOC_ORDER],
        "active_chain_after_creator": [
            "0x0108d810", "0x0108b0b0", "0x0108c190", "0x0108ab10", "0x0108ccd0", "0x" + OLD_CREATOR_LINK,
        ],
        "first_dispatch_order": [f"0x{process}" for process in DISPATCH_ORDER],
        "processes": pointer_rows,
        "scheduler_history_verified": history_verified,
        "history_instruction_count": len(instructions),
        "limits": [
            "This is reference evidence only; no captured pointer or state value is connected to native gameplay.",
            "The trace covers the first four ordinary OHULK processes in the paid first room; it does not cover all process creation sites or actor lifetime.",
            "The first wake proves the observed PTIME=1 decrement, PWAKE restore and entry jump, not the complete scheduler under all interrupt/overload paths.",
            "World coordinates and process state are not converted to the enhanced 16:9 native simulation.",
        ],
        "next_action": "Trace the first OHULK continuation from its CFLAGS read through GET_VALID_XY, BEGINOBJ2, SETUP_HULK and the first yield/sleep; join those boundaries to the verified placement/object-write reports before designing the first pointer-free native actor continuation.",
    }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_a", type=Path)
    parser.add_argument("log_b", type=Path)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--probe", type=Path, default=Path("tools/mame_l8_hulk_scheduler.lua"))
    parser.add_argument("--disassembly-helper", type=Path, default=Path("tools/inspect_l8_hulk_scheduler_pc.py"))
    parser.add_argument("--disassembly-dir", type=Path, default=Path("evidence/l8-hulk-scheduler-disassembly"))
    parser.add_argument("--hulks-source", type=Path, default=Path("original/archive/smashtv-main/HULKS.ASM"))
    parser.add_argument("--mproc-source", type=Path, default=Path("original/archive/smashtv-main/MPROC.ASM"))
    parser.add_argument("--mproc-equ", type=Path, default=Path("original/archive/smashtv-main/MPROC.EQU"))
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = verify(args)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"PASS: {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
