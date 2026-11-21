#!/usr/bin/env python3
"""Verify repeatable L8 HULK process/object context at HULK_CNT increments."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
COUNT_REPORT = Path("evidence/l8-paid-hulk-count-2026-09-23.json")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z0-9_]+)=([^\s]+)", line))


def read_run(path: Path) -> tuple[list[str], list[dict[str, str]], list[dict[str, str]]]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if any("[LUA ERROR]" in line for line in lines):
        raise ValueError(f"MAME Lua probe failed: {path}")
    if not any(line.startswith("Average speed:") for line in lines):
        raise ValueError(f"MAME did not finish: {path}")
    events = [line for line in lines if line.startswith("L8_")]
    contexts = [fields(line) for line in lines if line.startswith("L8_HULK_CONTEXT sample=")]
    objects = [fields(line) for line in lines if line.startswith("L8_HULK_OBJECT sample=")]
    return events, contexts, objects


def verify(args: argparse.Namespace) -> dict[str, object]:
    if sha256(args.mame) != MAME_SHA256:
        raise ValueError("MAME executable hash differs from the pinned 0.289 reference")
    if sha256(args.rom_zip) != ROM_SHA256:
        raise ValueError("supplied ROM archive differs from the verified L8 identity")

    hulk_source = args.hulks_source.read_text(encoding="latin-1")
    mproc_source = args.mproc_source.read_text(encoding="latin-1")
    robo_source = args.robo_source.read_text(encoding="latin-1")
    disp_source = args.disp_source.read_text(encoding="latin-1")
    source_fragments = (
        (hulk_source, "ONSCRN_HULKS:"),
        (hulk_source, "\tCREATE\tHULKPID,OHULK"),
        (hulk_source, "SETUP_HULK:"),
        (hulk_source, "MOVE\t@HULK_CNT,A0"),
        (hulk_source, "INC\tA0"),
        (hulk_source, "MOVE\tA0,@HULK_CNT"),
        (mproc_source, "PROCID\t.SET\t20H"),
        (mproc_source, "PSPTR\t.SET    40H"),
        (robo_source, "HULKPID\t\t.EQU\t236"),
        (disp_source, "OXPOS\t.EQU   \t70H"),
        (disp_source, "OYPOS\t.EQU   \t090H"),
        (disp_source, "OPLINK\t.EQU   \t160H"),
    )
    if any(fragment not in source for source, fragment in source_fragments):
        raise ValueError("one or more preserved L8 actor/process layout definitions differ")

    count_report = json.loads(args.count_report.read_text(encoding="utf-8"))
    if count_report.get("result") != "PASS":
        raise ValueError("the prerequisite HULK_CNT source/PC report is not passing")

    events_a, contexts_a, objects_a = read_run(args.log_a)
    events_b, contexts_b, objects_b = read_run(args.log_b)
    if events_a != events_b or contexts_a != contexts_b or objects_a != objects_b:
        raise ValueError("fresh MAME runs disagree on the HULK process/object context")
    if len(contexts_a) != 25 or len(objects_a) != 5:
        raise ValueError("the trace did not retain 25 count writes and five paired context reads")
    summary = next((event for event in events_a if event.startswith("L8_SUMMARY ")), "")
    if summary != "L8_SUMMARY hulk_context_writes=25":
        raise ValueError("the context trace summary differs")

    context_by_sample = {int(row["sample"]): row for row in contexts_a}
    object_by_sample = {int(row["sample"]): row for row in objects_a}
    if set(context_by_sample) != set(range(25)) or set(object_by_sample) != set(range(5)):
        raise ValueError("sample numbering is incomplete or duplicated")
    if object_by_sample[0].get("procid") != "000000df":
        raise ValueError("the counter reset context did not retain its non-HULK caller process ID")

    initial_hulks: list[dict[str, str | int]] = []
    process_pointers: set[str] = set()
    object_pointers: set[str] = set()
    for sample in range(1, 5):
        context = context_by_sample[sample]
        obj = object_by_sample[sample]
        expected_count = f"{sample * 0x100:08x}"
        if (context.get("frame") != "3533" or context.get("pc") != "ffe3e860" or
                context.get("address") != "010de5d0" or context.get("value") != expected_count or
                obj.get("procid") != "000000ec"):
            raise ValueError(f"sample {sample} is not a HULKPID SETUP_HULK count increment")
        if context.get("a13") != obj.get("object_process_link") or context.get("a8") != obj.get("object"):
            raise ValueError(f"sample {sample} process/object pointers do not match their source links")
        if context["a13"] in process_pointers or context["a8"] in object_pointers:
            raise ValueError("two counter increments reused a live process or object pointer")
        process_pointers.add(context["a13"])
        object_pointers.add(context["a8"])
        initial_hulks.append({
            "count": sample,
            "frame": int(context["frame"]),
            "pc": "0x" + context["pc"],
            "process_id": int(obj["procid"], 16),
            "process_pointer": "0x" + context["a13"],
            "object_pointer": "0x" + context["a8"],
            "object_process_link": "0x" + obj["object_process_link"],
            "source_integer_x_word": int(obj["xpos"], 16),
            "source_integer_y_word": int(obj["ypos"], 16),
            "source_fields": {"x": "DISP.EQU:OXPOS +0x70", "y": "DISP.EQU:OYPOS +0x90",
                              "process_link": "DISP.EQU:OPLINK +0x160"},
        })

    return {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Two identical fresh MAME L8 paid-first-room runs map the first four HULK_CNT increments to four distinct HULKPID process records and linked game-object records.",
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": MAME_SHA256,
            "rom_archive_sha256": ROM_SHA256,
            "fresh_config_and_nvram_each_run": True,
        },
        "runs": [
            {"log": args.log_a.as_posix(), "log_sha256": sha256(args.log_a)},
            {"log": args.log_b.as_posix(), "log_sha256": sha256(args.log_b)},
        ],
        "prerequisite_count_report": args.count_report.as_posix(),
        "source_mapping": {
            "actor_process_id": 236,
            "actor_process_symbol": "ROBO.EQU:HULKPID",
            "process_id_offset": "MPROC.EQU:PROCID +0x20",
            "object_pointer_register": "A8",
            "process_pointer_register": "A13",
            "object_process_link_offset": "DISP.EQU:OPLINK +0x160",
            "process_object_link_matches": True,
            "coordinate_offsets": {"x": "DISP.EQU:OXPOS +0x70", "y": "DISP.EQU:OYPOS +0x90"},
            "coordinate_values_are_raw_source_integer_words": True,
        },
        "initial_hulks": initial_hulks,
        "source_hashes": {
            "HULKS.ASM": sha256(args.hulks_source),
            "MPROC.EQU": sha256(args.mproc_source),
            "ROBO.EQU": sha256(args.robo_source),
            "DISP.EQU": sha256(args.disp_source),
        },
        "probe": {"script": args.probe.as_posix(), "sha256": sha256(args.probe)},
        "limits": [
            "This identifies live HULKPID process records and their linked display-object records at SETUP_HULK; it does not yet map the CREATE/HULK process allocator's full continuation or scheduler timing.",
            "The captured X/Y values are source integer fields, not yet converted to monitor-pixel coordinates or widened-arena coordinates.",
            "No process/object pointer or captured coordinate is consumed by native gameplay.",
            "The 35 HCOUNT reads in frame 3533 remain unassigned to individual random placement branches or retries.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_a", type=Path)
    parser.add_argument("log_b", type=Path)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--hulks-source", type=Path, default=Path("original/archive/smashtv-main/HULKS.ASM"))
    parser.add_argument("--mproc-source", type=Path, default=Path("original/archive/smashtv-main/MPROC.EQU"))
    parser.add_argument("--robo-source", type=Path, default=Path("original/archive/smashtv-main/ROBO.EQU"))
    parser.add_argument("--disp-source", type=Path, default=Path("original/archive/smashtv-main/DISP.EQU"))
    parser.add_argument("--count-report", type=Path, default=COUNT_REPORT)
    parser.add_argument("--probe", type=Path, default=Path("tools/mame_l8_hulk_count_context.lua"))
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Report exists; refusing to overwrite it")
    result = verify(args)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("PASS: repeated L8 count writes identify four HULKPID processes and linked object records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
