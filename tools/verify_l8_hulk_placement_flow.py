#!/usr/bin/env python3
"""Verify repeated L8 OHULK placement values and their live source path."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
FLOW_STAGES = (
    "GET_VALID_XY_RETURN",
    "WAVEYX_LOADED",
    "WAVEYX_APPLIED",
    "STRTOBJ_INPUT",
    "STRTOBJ_OBJECT_COORDS",
)
EXPECTED_PCS = {
    "GET_VALID_XY_RETURN": "ffe3b9a0",
    "WAVEYX_LOADED": "ffe3ba30",
    "WAVEYX_APPLIED": "ffe3baa0",
    "STRTOBJ_INPUT": "ffe03f60",
    "STRTOBJ_OBJECT_COORDS": "ffe043d0",
}
EXPECTED_HISTORY = (
    "ffe41920: cmpi   6h,a11",
    "ffe41a20: btst   a0,a1",
    "ffe41a30: jreq   ffe41c40h",
    "ffe41c40: movi   c10000h,b1",
    "ffe41ca0: calla  ffe06df0h",
    "ffe41d00: movi   2a0000h,b0",
    "ffe41d30: movi   1570000h,b1",
    "ffe41d60: calla  ffe06df0h",
    "ffe06c30: move   @c00001c0h,a1,0",
    "ffe41fe0: movi   10dd8b0h,a2",
    "ffe421c0: move   @10dd7f0h,a4,1",
    "ffe421f0: move   a3,*a4+,1",
    "ffe42200: move   a4,@10dd7f0h,1",
    "ffe42230: rets",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_fields(line: str) -> dict[str, str]:
    result = dict(re.findall(r"([a-z0-9_]+)=([^\s]+)", line.lower()))
    if "stage" in result:
        result["stage"] = result["stage"].upper()
    return result


def flow_run(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    if "[LUA ERROR]" in text or "L8_HULK_FLOW_ERROR" in text:
        raise ValueError(f"MAME probe failed: {path}")
    if "Average speed:" not in text:
        raise ValueError(f"MAME did not finish: {path}")
    for marker in ("L8_ACTION coin1_press frame=2600", "L8_ACTION start1_release frame=3170",
                   "L8_HULK_FLOW_SNAPSHOT frame=4200 error=nil", "L8_HULK_FLOW_DONE frame=4300"):
        if marker not in text:
            raise ValueError(f"MAME run is missing {marker!r}: {path}")
    lines = [line for line in text.splitlines() if "L8_DEBUGGER L8_HULK_FLOW stage=" in line]
    rows = [parse_fields(line) for line in lines]
    if len(rows) != 35:
        raise ValueError(f"expected the same 35 observed flow events, got {len(rows)} in {path}")
    if any(row.get("stage") not in FLOW_STAGES for row in rows):
        raise ValueError(f"unexpected placement stage in {path}")
    return lines, rows


def first_stage(rows: list[dict[str, str]], start_index: int, process: str, stage: str) -> dict[str, str]:
    for row in rows[start_index:]:
        if row.get("process") == process and row.get("stage") == stage:
            return row
    raise ValueError(f"missing {stage} for process {process}")


def history_block(path: Path, process: str, final_x: str, final_y: str) -> list[str]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if "[LUA ERROR]" in "\n".join(lines) or "Average speed:" not in "\n".join(lines):
        raise ValueError("MAME history probe did not finish cleanly")
    start = next((i for i, line in enumerate(lines)
                  if "L8_ACTION start1_release frame=3170" in line), None)
    if start is None:
        raise ValueError("history probe did not reach paid P1 start")
    for index in range(start + 1, len(lines)):
        begin = lines[index]
        if "L8_DEBUGGER L8_HULK_HISTORY_BEGIN" not in begin:
            continue
        fields = parse_fields(begin.lower())
        if (fields.get("process") != process or fields.get("a0") != final_x or
                fields.get("a1") != final_y):
            continue
        end = next((j for j in range(index + 1, len(lines))
                    if "L8_DEBUGGER L8_HULK_HISTORY_END" in lines[j]), None)
        if end is None:
            raise ValueError("unterminated MAME history block")
        instructions = [line.replace("L8_DEBUGGER ", "", 1).strip().lower()
                       for line in lines[index + 1:end]
                       if line.startswith("L8_DEBUGGER FFE")]
        for instruction in EXPECTED_HISTORY:
            if instruction not in instructions:
                raise ValueError(f"history omits executed path instruction: {instruction}")
        branch = instructions.index("ffe41a30: jreq   ffe41c40h")
        if "ffe41c40: movi   c10000h,b1" not in instructions[branch + 1:]:
            raise ValueError("GET_VALID_XY did not take the observed GV range-random path")
        if instructions.count("ffe06c30: move   @c00001c0h,a1,0") < 2:
            raise ValueError("history lacks both selected RANGRAND HCOUNT reads")
        return instructions
    raise ValueError(f"missing paid placement history for {process} ({final_x}, {final_y})")


def verify(args: argparse.Namespace) -> dict[str, object]:
    if sha256(args.mame) != MAME_SHA256:
        raise ValueError("MAME executable hash differs from the pinned 0.289 reference")
    if sha256(args.rom_zip) != ROM_SHA256:
        raise ValueError("supplied ROM archive differs from the verified L8 identity")

    prior = json.loads(args.object_report.read_text(encoding="utf-8"))
    if prior.get("result") != "PASS" or len(prior.get("initial_objects", [])) != 4:
        raise ValueError("prerequisite object/process initialization evidence is not passing")
    manifest_path = args.disassembly_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("rom_archive_sha256") != ROM_SHA256:
        raise ValueError("flow disassembly used a different ROM archive")
    if manifest.get("reference", {}).get("mame_executable_sha256") != MAME_SHA256:
        raise ValueError("flow disassembly names a different MAME reference")
    listings: dict[str, str] = {}
    for target, details in manifest.get("targets", {}).items():
        listing = args.disassembly_dir / details["listing"]
        if sha256(listing) != details.get("listing_sha256"):
            raise ValueError(f"disassembly listing hash mismatch: {target}")
        text = listing.read_text(encoding="utf-8").lower()
        for instruction in details.get("verified_instructions", []):
            if instruction not in text:
                raise ValueError(f"listing no longer maps {target}: {instruction}")
        listings[target] = details["listing_sha256"]

    lines_a, rows_a = flow_run(args.log_a)
    lines_b, rows_b = flow_run(args.log_b)
    if lines_a != lines_b or rows_a != rows_b:
        raise ValueError("fresh MAME runs disagree on live Hulk placement values")
    log_text_a = args.log_a.read_text(encoding="utf-8", errors="replace")
    start_index = next(i for i, line in enumerate(log_text_a.splitlines())
                       if "L8_ACTION start1_release frame=3170" in line)
    after_start = [parse_fields(line) for line in log_text_a.splitlines()[start_index + 1:]
                   if "L8_DEBUGGER L8_HULK_FLOW stage=" in line]

    actor_by_process = {actor["process_pointer"].lower().removeprefix("0x"): actor
                        for actor in prior["initial_objects"]}
    if len(actor_by_process) != 4:
        raise ValueError("prerequisite report does not contain four unique HULKPID records")

    hulk_source = args.hulks_source.read_text(encoding="latin-1")
    waves_source = args.waves_source.read_text(encoding="latin-1")
    disp_source = args.disp_source.read_text(encoding="latin-1")
    ndsp_source = args.ndsp_source.read_text(encoding="latin-1")
    required_source = (
        (hulk_source, "CALLA\tGET_VALID_XY", "HULKS.ASM:OHULK GET_VALID_XY call"),
        (hulk_source, "MOVE\t@WAVEYX,A2,L", "HULKS.ASM:OHULK WAVEYX load"),
        (waves_source, "WAVEYX,32", "WAVES.ASM WAVEYX storage"),
        (ndsp_source, "STRTOBJ", "NDSP1.ASM STRTOBJ entry"),
        (disp_source, "OXVAL\t.EQU   \t60H", "DISP.EQU OXVAL field"),
        (disp_source, "OYVAL\t.EQU   \t080H", "DISP.EQU OYVAL field"),
    )
    for source, fragment, label in required_source:
        if fragment not in source:
            raise ValueError(f"preserved source no longer contains {label}")
    active_strtobj_operations = (
        (r"(?m)^\s*JRUC\tSTFOBJ0\s*$", "STRTOBJ reaches STFOBJ0"),
        (r"(?m)^\s*CALLR\tGSAGOF\b", "STFOBJ0 calls GSAGOF"),
        (r"(?m)^\s*CALLR\tGANIOF\b", "STFOBJ0 calls GANIOF"),
        (r"(?m)^\s*SUB\tA6,A9\s*;SUBTRACT X ANIOFF", "active X animation-anchor subtraction"),
        (r"(?m)^\s*SUB\tA7,A10\s*;SUBTRACT Y ANIOFF", "active Y animation-anchor subtraction"),
        (r"(?m)^\s*MOVE\tA9,\*A0\(OXVAL\),L", "active OXVAL store"),
        (r"(?m)^\s*MOVE\tA10,\*A0\(OYVAL\),L", "active OYVAL store"),
    )
    for pattern, label in active_strtobj_operations:
        if not re.search(pattern, ndsp_source):
            raise ValueError(f"preserved NDSP1.ASM no longer contains {label}")

    result_actors: list[dict[str, object]] = []
    hulk_history: list[str] | None = None
    for process, actor in actor_by_process.items():
        valid_index = next((i for i, row in enumerate(after_start)
                            if row.get("process") == process and
                            row.get("stage") == "GET_VALID_XY_RETURN"), None)
        if valid_index is None:
            raise ValueError(f"missing first paid GET_VALID_XY return for {process}")
        ordered_stages: list[str] = []
        seen_stages: set[str] = set()
        for row in after_start[valid_index:]:
            if row.get("process") == process and row.get("stage") in FLOW_STAGES:
                stage = row["stage"]
                if stage not in seen_stages:
                    ordered_stages.append(stage)
                    seen_stages.add(stage)
        if ordered_stages[:len(FLOW_STAGES)] != list(FLOW_STAGES):
            raise ValueError(f"initial placement stages are missing or reordered for {process}: {ordered_stages}")
        stages = {stage: first_stage(after_start, valid_index, process, stage) for stage in FLOW_STAGES}
        if any(stages[stage]["pc"] != EXPECTED_PCS[stage] for stage in FLOW_STAGES):
            raise ValueError(f"unexpected live code boundary for {process}")
        valid = stages["GET_VALID_XY_RETURN"]
        wave = stages["WAVEYX_LOADED"]
        applied = stages["WAVEYX_APPLIED"]
        init = stages["STRTOBJ_INPUT"]
        obj = stages["STRTOBJ_OBJECT_COORDS"]
        get_x, get_y = int(valid["x"], 16), int(valid["y"], 16)
        packed_wave = int(wave["waveyx"], 16)
        wave_x = (packed_wave & 0xFFFF) << 16
        wave_y = packed_wave & 0xFFFF0000
        applied_x = (get_x + wave_x) & 0xFFFFFFFF
        applied_y = (get_y + wave_y) & 0xFFFFFFFF
        if (int(wave["x"], 16), int(wave["y"], 16)) != (get_x, get_y):
            raise ValueError(f"A0/A1 changed before the packed WAVEYX read for {process}")
        if (int(applied["x"], 16), int(applied["y"], 16),
                int(init["x"], 16), int(init["y"], 16)) != (
                applied_x, applied_y, applied_x, applied_y):
            raise ValueError(f"WAVEYX arithmetic/STRTOBJ input mismatch for {process}")

        x_offset, y_offset = int(obj["x_anim_offset"], 16), int(obj["y_anim_offset"], 16)
        object_x = (applied_x - x_offset) & 0xFFFFFFFF
        object_y = (applied_y - y_offset) & 0xFFFFFFFF
        recorded_x = (int(actor["raw_source_position_words"]["x_integer_high_word"]) << 16) | int(
            actor["raw_source_position_words"]["x_fraction_low_word"])
        recorded_y = (int(actor["raw_source_position_words"]["y_integer_high_word"]) << 16) | int(
            actor["raw_source_position_words"]["y_fraction_low_word"])
        if (int(obj["x"], 16), int(obj["y"], 16), object_x, object_y) != (
                recorded_x, recorded_y, recorded_x, recorded_y):
            raise ValueError(f"computed STRTOBJ fields differ from prior live object writes for {process}")

        if hulk_history is None:
            hulk_history = history_block(args.history_log, process,
                                         f"{applied_x:08x}", f"{applied_y:08x}")
        result_actors.append({
            "count": actor["count"],
            "process_pointer": actor["process_pointer"],
            "object_pointer": actor["object_pointer"],
            "executed_pcs": {stage: stages[stage]["pc"] for stage in FLOW_STAGES},
            "get_valid_xy_return": {"x": f"0x{get_x:08x}", "y": f"0x{get_y:08x}"},
            "waveyx": {
                "packed_word": f"0x{packed_wave:08x}",
                "x_add": f"0x{wave_x:08x}",
                "y_add": f"0x{wave_y:08x}",
            },
            "strtobj_input_after_waveyx": {
                "x": f"0x{applied_x:08x}", "y": f"0x{applied_y:08x}",
            },
            "strtobj_animation_offsets": {
                "x": f"0x{x_offset:08x}", "y": f"0x{y_offset:08x}",
            },
            "initial_object_fields": {
                "x": f"0x{object_x:08x}", "y": f"0x{object_y:08x}",
                "match_prior_live_object_write_trace": True,
            },
        })

    if hulk_history is None:
        raise ValueError("no initial Hulk process histories were verified")

    image_a = sha256(args.room_a)
    image_b = sha256(args.room_b)
    expected_room = prior.get("room_image", {}).get("sha256")
    if image_a != image_b or image_a != expected_room:
        raise ValueError("frame-4200 screenshots differ from each other or the paid-room reference")

    def rel(path: Path) -> str:
        return path.as_posix()

    return {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Two identical fresh MAME L8 paid-start runs capture GET_VALID_XY return values, packed WAVEYX, post-offset STRTOBJ inputs, animation-anchor subtractions and the initial object coordinates already mapped by the preceding write trace.",
        "reference": {
            "mame_version": "0.289",
            "mame_executable_sha256": MAME_SHA256,
            "rom_archive_sha256": ROM_SHA256,
            "fresh_config_and_nvram_each_run": True,
            "history_trace_run_is_fresh": True,
        },
        "probe": {
            "flow_helper": rel(args.flow_probe),
            "flow_helper_sha256": sha256(args.flow_probe),
            "history_helper": rel(args.history_probe),
            "history_helper_sha256": sha256(args.history_probe),
            "history_log_sha256": sha256(args.history_log),
            "disassembly_helper": rel(args.disassembly_helper),
            "disassembly_helper_sha256": sha256(args.disassembly_helper),
            "disassembly_manifest": rel(manifest_path),
            "disassembly_manifest_sha256": sha256(manifest_path),
            "verified_listings": listings,
        },
        "runs": [
            {"log": rel(args.log_a), "log_sha256": sha256(args.log_a),
             "room_image": rel(args.room_a), "room_image_sha256": image_a},
            {"log": rel(args.log_b), "log_sha256": sha256(args.log_b),
             "room_image": rel(args.room_b), "room_image_sha256": image_b},
        ],
        "prerequisite_object_report": rel(args.object_report),
        "source_mapping": {
            "HULKS.ASM:OHULK": "FFE3B950 selects HULK; FFE3B970 calls GET_VALID_XY; FFE3B9A0 returns to OHULK; FFE3BA00 loads WAVEYX; FFE3BA30-BA90 applies packed X/Y; FFE3BAC0 calls BEGINOBJ2.",
            "HULKS.ASM:GET_VALID_XY": "FFE41920 is the executed L8 function entry. Paid first-room history takes the GV branch at FFE41A30 to FFE41C40, then performs two RANGRAND calls through UTIL.ASM:RANDOM's HCOUNT read at FFE06C30.",
            "WAVES.ASM:WAVEYX": "The runtime word read by the L8 instruction at FFE3BA00 is captured as the packed XY offset; MOVX selects X and the high word supplies Y.",
            "NDSP1.ASM:STRTOBJ": "A0/A1 enter at FFE03F60. FFE043B0/B0 subtract A6/A7 anchor offsets from A9/A10 before FFE043D0/F0 store OXVAL/OYVAL.",
            "DISP.EQU": "OXVAL at object bit offset 0x60 and OYVAL at 0x80; the final values agree with the preceding object write trace.",
        },
        "first_paid_room_hulks": result_actors,
        "paid_get_valid_xy_history": {
            "history_instruction_count": len(hulk_history),
            "takes_gv_range_random_path": True,
            "captured_selected_hcount_reads": hulk_history.count(
                "ffe06c30: move   @c00001c0h,a1,0"),
            "selected_hcount_reads_are_full_rng_model": False,
        },
        "room_image": {
            "frame": 4200,
            "sha256": image_a,
            "matches_prior_paid_first_room_image": True,
        },
        "limits": [
            "This identifies the values returned from GET_VALID_XY and their consumers; it does not reconstruct the random seed/counter timing or every placement retry across all waves.",
            "The MAME paid-run GV/first-room trace is reference evidence only; no saved trace values or pointers feed native gameplay.",
            "The trace does not map CREATE/GETPRC allocation, process initialization/return, dispatch wake timing or native actor ownership.",
            "The raw object coordinates remain L8 world values and are not converted to monitor pixels or enhanced 16:9 coordinates.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_a", type=Path)
    parser.add_argument("log_b", type=Path)
    parser.add_argument("history_log", type=Path)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--flow-probe", type=Path, default=Path("tools/mame_l8_hulk_placement_flow.lua"))
    parser.add_argument("--history-probe", type=Path, default=Path("tools/mame_l8_hulk_placement_history.lua"))
    parser.add_argument("--disassembly-helper", type=Path, default=Path("tools/inspect_l8_hulk_flow_pc.py"))
    parser.add_argument("--disassembly-dir", type=Path, default=Path("evidence/l8-hulk-placement-disassembly"))
    parser.add_argument("--object-report", type=Path, default=Path("evidence/l8-hulk-object-init-2026-09-23.json"))
    parser.add_argument("--hulks-source", type=Path, default=Path("original/archive/smashtv-main/HULKS.ASM"))
    parser.add_argument("--waves-source", type=Path, default=Path("original/archive/smashtv-main/WAVES.ASM"))
    parser.add_argument("--ndsp-source", type=Path, default=Path("original/archive/smashtv-main/NDSP1.ASM"))
    parser.add_argument("--disp-source", type=Path, default=Path("original/archive/smashtv-main/DISP.EQU"))
    parser.add_argument("--room-a", type=Path, required=True)
    parser.add_argument("--room-b", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = verify(args)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{report['result']}: {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
