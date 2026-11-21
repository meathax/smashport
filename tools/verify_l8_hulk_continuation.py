#!/usr/bin/env python3
"""Verify paired paid-start L8 traces from the first OHULK dispatch to PRCSLP."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path


MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
PROCESSES = ["0108b0b0", "0108c190", "0108ab10", "0108ccd0"]
EXPECTED_DISPATCH_ORDER = PROCESSES
REQUIRED_FIXED_STAGES = [
    "IH1_ENTRY",
    "BEGINOBJ2_ENTRY",
    "BEGINOBJ2_RETURN",
    "SETUP_HULK_ENTRY",
    "SETUP_HULK_RETURN",
    "DO_DIFF_ENTRY",
    "DO_DIFF_RETURN",
    "JUST_SK_ENTRY",
    "JUST_SK_RETURN",
    "GET_STAND_ENTRY",
    "GET_STAND_RETURN",
    "LUP_TOP_FIRST_ENTRY",
    "BUYINPID_EXISTS_BRANCH",
    "GET_STAND_ENTRY",
    "SLEEP2_LOAD",
    "SLEEP2_CALL",
    "PRCSLP_ENTRY",
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key.lower()] = value.lower()
    if "stage" in fields:
        fields["stage"] = fields["stage"].upper()
    return fields


def read_json(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def required_text(path: Path, patterns: list[str], description: str) -> None:
    text = path.read_text(encoding="utf-8", errors="replace").lower()
    missing = [pattern for pattern in patterns if pattern.lower() not in text]
    if missing:
        raise ValueError(f"{description} listing/source is missing {missing}")


def parse_run(path: Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    required_markers = [
        "L8_ACTION coin1_press frame=2600",
        "L8_ACTION coin1_release frame=2604",
        "L8_ACTION start1_press frame=3150",
        "L8_ACTION start1_release frame=3170",
        "L8_HULK_CONT_ARMED frame=3528",
        "L8_HULK_CONT_DONE frame=3535",
        "Average speed:",
    ]
    for marker in required_markers:
        if marker not in text:
            raise ValueError(f"incomplete paid-start trace {path}: missing {marker!r}")
    if "[LUA ERROR]" in text or "L8_HULK_CONT_ERROR" in text:
        raise ValueError(f"MAME Lua/debugger trace failed: {path}")

    lines = text.splitlines()
    rows: list[dict[str, object]] = []
    tap_rows: dict[str, list[dict[str, str]]] = {process: [] for process in PROCESSES}
    for index, line in enumerate(lines):
        if "L8_DEBUGGER" in line and "L8_HULK_CONT stage=" in line:
            values = parse_fields(line)
            if "stage" in values and "process" in values:
                rows.append({"line_index": index, "line": line, "fields": values})
        elif "L8_HULK_CONT_TAP stage=FIRST_CFLAGS_READ" in line:
            values = parse_fields(line)
            if values.get("process") in tap_rows:
                tap_rows[values["process"]].append(values)

    dispatches = [row for row in rows if row["fields"].get("stage") == "FIRST_OHULK_DISPATCH"]
    dispatch_order = [row["fields"]["process"] for row in dispatches]
    if dispatch_order != EXPECTED_DISPATCH_ORDER:
        raise ValueError(f"unexpected first OHULK dispatch order in {path}: {dispatch_order}")
    if any(len(tap_rows[process]) != 1 for process in PROCESSES):
        raise ValueError(f"expected one first CFLAGS read for each Hulk process in {path}")

    sequences: dict[str, list[dict[str, object]]] = {}
    selected_line_indices: set[int] = set()
    for process in PROCESSES:
        start = next(i for i, row in enumerate(rows)
                     if row["fields"].get("stage") == "FIRST_OHULK_DISPATCH" and
                     row["fields"].get("process") == process)
        sequence: list[dict[str, object]] = []
        for row in rows[start:]:
            fields = row["fields"]
            if fields.get("process") != process:
                continue
            sequence.append(row)
            selected_line_indices.add(row["line_index"])
            if fields.get("stage") == "PRCSLP_ENTRY":
                break
        if not sequence or sequence[0]["fields"].get("stage") != "FIRST_OHULK_DISPATCH":
            raise ValueError(f"missing initial OHULK dispatch for process {process} in {path}")
        if sequence[-1]["fields"].get("stage") != "PRCSLP_ENTRY":
            raise ValueError(f"process {process} did not reach PRCSLP in {path}")
        sequences[process] = sequence
        if len([r for r in sequence if r["fields"].get("stage") == "GET_VALID_XY_RETURN"]) != 1:
            raise ValueError(f"process {process} lacks exactly one placement return in {path}")
        attempts = [r for r in sequence if r["fields"].get("stage") == "GET_VALID_XY_ENTRY"]
        if not attempts:
            raise ValueError(f"process {process} has no GET_VALID_XY entry/retry in {path}")
        stages = [r["fields"]["stage"] for r in sequence]
        positions = [i for i, stage in enumerate(stages) if stage == "GET_VALID_XY_RETURN"]
        if not positions:
            raise ValueError(f"process {process} has no GET_VALID_XY return in {path}")
        after_return = stages[positions[0] + 1:]
        cursor = 0
        for expected in REQUIRED_FIXED_STAGES:
            try:
                found = after_return.index(expected, cursor)
            except ValueError as exc:
                raise ValueError(f"process {process} expected {expected} after placement in {path}: {stages}") from exc
            cursor = found + 1

    selected_tap_indices = {
        index for index, line in enumerate(lines)
        if "L8_HULK_CONT_TAP stage=FIRST_CFLAGS_READ" in line
    }
    selected_line_indices |= selected_tap_indices
    event_stream = "\n".join(lines[index].strip().lower()
                              for index in sorted(selected_line_indices)) + "\n"

    history_begin = next((i for i, line in enumerate(lines)
                          if "L8_HULK_CONT_HISTORY_BEGIN" in line), None)
    history_end = next((i for i, line in enumerate(lines)
                        if history_begin is not None and i > history_begin and
                        "L8_HULK_CONT_HISTORY_END" in line), None)
    if history_begin is None or history_end is None:
        raise ValueError(f"missing first-yield instruction history in {path}")
    history_lines = [line.strip().lower() for line in lines[history_begin + 1:history_end]
                     if re.search(r"\bffe[0-9a-f]{5}:\s", line.lower())]
    history_text = "\n".join(history_lines) + "\n"
    for expected in (
        "ffe3d500: jrne   ffe3d650h",
        "ffe3d650: movi   2h,a0",
        "ffe3d670: calla  ffe0a490h",
        "ffe0a490: move   *sp+,a7,1",
    ):
        if expected not in history_text:
            raise ValueError(f"first-yield history in {path} is missing {expected!r}")

    return {
        "path": path,
        "text": text,
        "sha256": sha256(path),
        "sequences": sequences,
        "tap_rows": tap_rows,
        "dispatch_order": dispatch_order,
        "event_stream": event_stream,
        "event_stream_sha256": hashlib.sha256(event_stream.encode("utf-8")).hexdigest(),
        "history_text": history_text,
        "history_sha256": hashlib.sha256(history_text.encode("utf-8")).hexdigest(),
        "history_instruction_count": len(history_lines),
    }


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_a", type=Path)
    parser.add_argument("run_b", type=Path)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--probe", type=Path, default=root / "tools/mame_l8_hulk_continuation.lua")
    parser.add_argument("--disassembly-helper", type=Path,
                        default=root / "tools/inspect_l8_hulk_continuation_pc.py")
    parser.add_argument("--disassembly-dir", type=Path,
                        default=root / "evidence/l8-hulk-continuation-disassembly")
    parser.add_argument("--source-dir", type=Path,
                        default=root / "original/archive/smashtv-main")
    parser.add_argument("--scheduler-report", type=Path,
                        default=root / "evidence/l8-hulk-scheduler-2026-09-23.json")
    parser.add_argument("--placement-report", type=Path,
                        default=root / "evidence/l8-hulk-placement-flow-2026-09-23.json")
    parser.add_argument("--object-report", type=Path,
                        default=root / "evidence/l8-hulk-object-init-2026-09-23.json")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    if sha256(args.mame) != MAME_SHA256:
        raise SystemExit("pinned MAME executable hash mismatch")
    if sha256(args.rom_zip) != ROM_SHA256:
        raise SystemExit("supplied L8 ROM archive hash mismatch")
    with zipfile.ZipFile(args.rom_zip) as archive:
        for name, crc in (("la8_smash_tv_game_rom_u105.u105", 0x48CD793F),
                          ("la8_smash_tv_game_rom_u89.u89", 0x8E7FE463)):
            if archive.getinfo(name).CRC != crc:
                raise SystemExit(f"L8 program ROM member CRC mismatch: {name}")

    scheduler = read_json(args.scheduler_report)
    placement = read_json(args.placement_report)
    objects = read_json(args.object_report)
    if scheduler.get("result") != "PASS" or placement.get("result") != "PASS" or objects.get("result") != "PASS":
        raise SystemExit("one or more prerequisite L8 reference reports did not pass")
    if scheduler.get("first_dispatch_order") != [f"0x{x}" for x in EXPECTED_DISPATCH_ORDER]:
        raise SystemExit("scheduler prerequisite has a different first OHULK process order")

    disassembly_dir = args.disassembly_dir
    manifest_path = disassembly_dir / "manifest.json"
    manifest = read_json(manifest_path)
    targets = manifest["targets"]
    listing_checks = {
        "ohulk-initialization": [
            "ffe3b8e0: move   *a13(130h),a0,0",
            "ffe3b970: calla  ffe41920h",
            "ffe3baa0: callr  ffe41630h",
            "ffe3bac0: calla  ffe04170h",
            "ffe3bb30: callr  ffe3e670h",
            "ffe3bb50: callr  ffe3ec30h",
            "ffe3bb70: callr  ffe3ebe0h",
            "ffe3bb90: callr  ffe405d0h",
            "ffe3bbb0: jr     ffe3c760h",
        ],
        "ohulk-loop-top": [
            "ffe3d4d0: calla  ffe0b100h",
            "ffe3d500: jrne   ffe3d650h",
            "ffe3d650: movi   2h,a0",
            "ffe3d670: calla  ffe0a490h",
        ],
        "setup-hulk": ["ffe3e670: movk   5h,a0", "ffe3e7f0: move   @10de5d8h,a0,0"],
        "existp": ["ffe0b100: mmtm   sp,a1,a2,a4"],
        "prcslp-first-yield": [
            "ffe0a490: move   *sp+,a7,1",
            "ffe0a4f0: move   a0,-*a1,0",
        ],
        "beginobj2": [
            "ffe04170: callr  ffe03f60h",
            "ffe04190: move   a13,*a0(160h),1",
        ],
    }
    listing_reports: dict[str, dict[str, object]] = {}
    for target_name, snippets in listing_checks.items():
        target = targets[target_name]
        listing_path = disassembly_dir / target["listing"]
        required_text(listing_path, snippets, target_name)
        listing_reports[target_name] = {
            "path": str(listing_path),
            "sha256": sha256(listing_path),
            "verified_instructions": snippets,
        }

    hulk_source = args.source_dir / "HULKS.ASM"
    mproc_source = args.source_dir / "MPROC.ASM"
    mproc_equ = args.source_dir / "MPROC.EQU"
    ndsp_source = args.source_dir / "NDSP1.ASM"
    robo_equ = args.source_dir / "ROBO.EQU"
    hulk_text = hulk_source.read_text(encoding="utf-8", errors="replace")
    mproc_text = mproc_source.read_text(encoding="utf-8", errors="replace")
    mproc_equ_text = mproc_equ.read_text(encoding="utf-8", errors="replace")
    ndsp_text = ndsp_source.read_text(encoding="utf-8", errors="replace")
    robo_equ_text = robo_equ.read_text(encoding="utf-8", errors="replace")
    if not re.search(
        r"OHULK:[\s\S]*?CALLR\s+IH1[\s\S]*?CALLA\s+BEGINOBJ2[\s\S]*?"
        r"CALLR\s+SETUP_HULK[\s\S]*?CALLR\s+DO_DIFF[\s\S]*?"
        r"CALLR\s+JUST_SK[\s\S]*?CALLR\s+GET_STAND[\s\S]*?JRUC\s+LUP_TOP",
        hulk_text, re.IGNORECASE,
    ):
        raise SystemExit("HULKS.ASM OHULK initialization sequence changed")
    if not re.search(
        r"DOLOGIC:[\s\S]*?MOVI\s+BUYINPID,A0\s*\n\s*CALLA\s+EXISTP\s*\n\s*"
        r"JRNZ\s+DONE_SPIN[\s\S]*?DONE_SPIN:\s*SLEEP\s+2",
        hulk_text, re.IGNORECASE,
    ):
        raise SystemExit("HULKS.ASM buy-in guard/SLEEP 2 path changed")
    if not re.search(r"SETUP_HULK:[\s\S]*?MOVE\s+@HULK_CNT,A0[\s\S]*?INC\s+A0[\s\S]*?MOVE\s+A0,@HULK_CNT", hulk_text, re.IGNORECASE):
        raise SystemExit("HULKS.ASM SETUP_HULK count increment not found")
    if not re.search(r"SLEEP\s+\$MACRO[\s\S]*?CALLA\s+PRCSLP", mproc_equ_text, re.IGNORECASE):
        raise SystemExit("MPROC.EQU SLEEP-to-PRCSLP macro not found")
    if not re.search(r"EXISTP:\s", mproc_text, re.IGNORECASE):
        raise SystemExit("MPROC.ASM EXISTP source label not found")
    if not re.search(r"BEGINOBJ2\s", ndsp_text, re.IGNORECASE):
        raise SystemExit("NDSP1.ASM BEGINOBJ2 source label not found")
    if not re.search(r"BUYINPID\s+\.EQU\s+216[\s\S]*?HULKPID\s+\.EQU\s+236", robo_equ_text, re.IGNORECASE):
        raise SystemExit("ROBO.EQU BUYINPID/HULKPID identifiers changed")

    run_a = parse_run(args.run_a)
    run_b = parse_run(args.run_b)
    if run_a["event_stream"] != run_b["event_stream"]:
        raise SystemExit("paired first-OHULK continuation event streams differ")
    if run_a["history_text"] != run_b["history_text"]:
        raise SystemExit("paired first-yield CPU history differs")

    placement_by_process = {x["process_pointer"].lower().removeprefix("0x"): x
                            for x in placement["first_paid_room_hulks"]}
    object_by_process = {x["process_pointer"].lower().removeprefix("0x"): x
                         for x in objects["initial_objects"]}
    scheduler_by_process = {x["process_pointer"].lower().removeprefix("0x"): x
                            for x in scheduler["processes"]}
    actor_reports = []
    for process in EXPECTED_DISPATCH_ORDER:
        sequence = run_a["sequences"][process]
        rows = [row["fields"] for row in sequence]
        by_stage: dict[str, list[dict[str, str]]] = {}
        for row in rows:
            by_stage.setdefault(row["stage"], []).append(row)
        placement_actor = placement_by_process[process]
        object_actor = object_by_process[process]
        scheduler_actor = scheduler_by_process[process]
        valid_return = by_stage["GET_VALID_XY_RETURN"][0]
        expected_xy = placement_actor["get_valid_xy_return"]
        if int(valid_return["a0"], 16) != int(expected_xy["x"], 16) or int(valid_return["a1"], 16) != int(expected_xy["y"], 16):
            raise SystemExit(f"GET_VALID_XY return differs from placement-flow witness for {process}")
        ih1 = by_stage["IH1_ENTRY"][0]
        expected_input = placement_actor["strtobj_input_after_waveyx"]
        if int(ih1["a0"], 16) != int(expected_input["x"], 16) or int(ih1["a1"], 16) != int(expected_input["y"], 16):
            raise SystemExit(f"OHULK-to-IH1 coordinates differ from placement-flow witness for {process}")
        begin_entry = by_stage["BEGINOBJ2_ENTRY"][0]
        if (begin_entry["a0"], begin_entry["a1"]) != (ih1["a0"], ih1["a1"]):
            raise SystemExit(f"IH1/BEGINOBJ2 incoming coordinates disagree for {process}")
        begin_return = by_stage["BEGINOBJ2_RETURN"][0]
        object_pointer = object_actor["object_pointer"].lower().removeprefix("0x")
        if begin_return["a0"].removeprefix("0x") != object_pointer or begin_return["a8"].removeprefix("0x") != object_pointer:
            raise SystemExit(f"BEGINOBJ2 object does not match prior object-init witness for {process}")
        setup_entry = by_stage["SETUP_HULK_ENTRY"][0]
        if setup_entry["a8"].removeprefix("0x") != object_pointer:
            raise SystemExit(f"SETUP_HULK does not own the expected object for {process}")
        sleep_call = by_stage["SLEEP2_CALL"][0]
        first_sleep = by_stage["PRCSLP_ENTRY"][0]
        sleep_value = first_sleep.get("sleep", first_sleep.get("a0", ""))
        if int(sleep_call["a0"], 16) != 2 or int(sleep_value, 16) != 2:
            raise SystemExit(f"first source sleep is not SLEEP 2 for {process}")
        cflags = run_a["tap_rows"][process][0]
        if int(cflags["data"], 16) != int(scheduler_actor["cflags_first_read_value"], 16):
            raise SystemExit(f"CFLAGS read differs from preceding scheduler trace for {process}")
        attempts = [row for row in by_stage["GET_VALID_XY_ENTRY"]]
        actor_reports.append({
            "process_pointer": f"0x{process}",
            "process_id": 236,
            "first_dispatch_frame": int(by_stage["FIRST_OHULK_DISPATCH"][0]["frame"]),
            "first_cflags_read": {
                "frame": int(cflags["frame"]),
                "emutime_seconds": float(cflags["emutime"]),
                "address": f"0x{cflags['address']}",
                "data": f"0x{cflags['data']}",
                "pc": f"0x{cflags['pc']}",
            },
            "get_valid_xy_entry_attempt_count": len(attempts),
            "get_valid_xy_entry_a0_values": [f"0x{row['a0']}" for row in attempts],
            "get_valid_xy_return": {"x": f"0x{valid_return['a0']}", "y": f"0x{valid_return['a1']}"},
            "post_wavyx_ih1_beginobj2_input": {"x": f"0x{ih1['a0']}", "y": f"0x{ih1['a1']}"},
            "beginobj2_object_pointer": f"0x{object_pointer}",
            "object_init_report_matches": True,
            "setup_hulk_and_get_stand_sequence_verified": True,
            "buyinpid_branch_pc": f"0x{by_stage['BUYINPID_EXISTS_BRANCH'][0]['pc']}",
            "first_sleep": {
                "sleep_value": int(sleep_value, 16),
                "prcslp_pc": f"0x{first_sleep['pc']}",
                "observed_frame_label": int(first_sleep["frame"]),
                "frame_label_is_instruction_timing": False,
            },
        })

    source_hashes = {
        path.name: sha256(path)
        for path in (hulk_source, mproc_source, mproc_equ, ndsp_source, robo_equ)
    }
    event_stream = run_a["event_stream"]
    report = {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Two paired fresh-config/NVRAM MAME L8 paid-start runs trace the first four OHULK processes from their first dispatch through GET_VALID_XY attempts, IH1/BEGINOBJ2, SETUP_HULK, DO_DIFF/JUST_SK/GET_STAND, the BUYINPID guard and first SLEEP 2 -> PRCSLP call.",
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": sha256(args.mame),
            "rom_archive_sha256": sha256(args.rom_zip),
            "fresh_config_and_nvram_each_run": True,
            "dispatch_frame": 3533,
        },
        "probe": {
            "lua_probe": str(args.probe),
            "lua_probe_sha256": sha256(args.probe),
            "disassembly_helper": str(args.disassembly_helper),
            "disassembly_helper_sha256": sha256(args.disassembly_helper),
            "disassembly_manifest": str(manifest_path),
            "disassembly_manifest_sha256": sha256(manifest_path),
            "verified_listings": listing_reports,
        },
        "source_hashes": source_hashes,
        "prerequisite_reports": {
            "scheduler": {"path": str(args.scheduler_report), "sha256": sha256(args.scheduler_report)},
            "placement_flow": {"path": str(args.placement_report), "sha256": sha256(args.placement_report)},
            "object_initialization": {"path": str(args.object_report), "sha256": sha256(args.object_report)},
        },
        "runs": [
            {"log": str(args.run_a), "log_sha256": run_a["sha256"]},
            {"log": str(args.run_b), "log_sha256": run_b["sha256"]},
        ],
        "reference_event_stream_sha256": run_a["event_stream_sha256"],
        "first_dispatch_order": [f"0x{x}" for x in EXPECTED_DISPATCH_ORDER],
        "first_yield_history": {
            "instruction_count": run_a["history_instruction_count"],
            "history_sha256": run_a["history_sha256"],
            "verified_instructions": [
                "FFE3D500: JRNE FFE3D650",
                "FFE3D650: MOVI 2,A0",
                "FFE3D670: CALLA PRCSLP",
                "FFE0A490: PRCSLP saves the process continuation and sleep parameter",
            ],
        },
        "actors": actor_reports,
        "limits": [
            "This is selected MAME L8 reference evidence only; it is not physical-PCB validation or whole-source retail identity.",
            "Only the first four paid-first-room OHULK processes and their first continuation/sleep are traced; later actor behavior and other creation paths remain uncovered.",
            "The timestamp taps are precise for the CFLAGS read; debugger-stage frame labels are callback labels, not per-instruction event times or exact interrupt phases.",
            "The GET_VALID_XY function-entry breakpoint records retry inputs but does not individually attribute every HCOUNT read to a random draw or rejection test.",
            "The coordinate values remain L8 source/world fixed-point words; they are not monitor pixels or 16:9 native coordinates.",
            "No captured address, pointer, random input, actor state or sleep value is connected to native gameplay.",
        ],
        "next_action": "Trace PRCSLP's per-process state writes and the matching PRCDSP wake/countdown for these four Hulks, then follow the first resumed movement update before designing a pointer-free native actor continuation.",
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "result": report["result"],
        "report": str(args.report),
        "report_sha256": sha256(args.report),
        "event_stream_sha256": report["reference_event_stream_sha256"],
        "history_instruction_count": report["first_yield_history"]["instruction_count"],
        "actors": [{
            "process": actor["process_pointer"],
            "placement_attempts": actor["get_valid_xy_entry_attempt_count"],
            "object": actor["beginobj2_object_pointer"],
            "sleep": actor["first_sleep"]["sleep_value"],
            "frame_label": actor["first_sleep"]["observed_frame_label"],
        } for actor in actor_reports],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
