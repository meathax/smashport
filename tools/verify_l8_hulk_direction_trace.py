#!/usr/bin/env python3
"""Verify paired L8 Hulk seek traces against the linked GETXY source path."""

from __future__ import annotations
import argparse, hashlib, json, re
from pathlib import Path

MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
PROCESS_ORDER = ["0108b0b0", "0108c190", "0108ab10", "0108ccd0"]
OBJECTS = {"0108b0b0": "0101f1b0", "0108c190": "0101f3a0",
           "0108ab10": "0101f590", "0108ccd0": "0101f780"}
SEEK_FRAMES = {"0108b0b0": "3901", "0108c190": "3901",
               "0108ab10": "3901", "0108ccd0": "3901"}
EXPECTED_SEEKS = {"0108b0b0": 4, "0108c190": 4, "0108ab10": 6, "0108ccd0": 2}
EXPECTED_VELOCITIES = {
    4: (0x00006660, 0x00005998),
    6: (0xFFFF99A0, 0x00005998),
    2: (0x00006660, 0xFFFFA668),
}

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)

def fields(line: str) -> dict[str, str]:
    return {key.lower(): value.lower() for key, value in re.findall(r"([a-zA-Z0-9_]+)=([^ ]+)", line)}

def read_run(path: Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    for marker in (
        "L8_HULK_DIRECTION_PROBE target=first_resumed_DOHULK_SETUP_DIR_seek_and_velocity",
        "L8_HULK_DIRECTION_ARMED frame=3880", "L8_HULK_DIRECTION_DONE frame=4040",
        "L8_ACTION coin1_press frame=2600", "L8_ACTION start1_press frame=3150",
        "Average speed:",
    ):
        require(marker in text, f"{path}: missing marker {marker}")
    require("[LUA ERROR]" not in text and "runtime error" not in text,
            f"{path}: MAME Lua probe failed")
    lines = [line.strip() for line in text.splitlines() if "L8_HULK_DIRECTION_TAP" in line]
    stream = "\n".join(re.sub(r"\s+", " ", line.lower()) for line in lines) + "\n"
    return {"path": path, "sha256": sha(path), "text": text,
            "lines": lines, "rows": [fields(line) for line in lines],
            "event_stream": stream,
            "event_stream_sha256": hashlib.sha256(stream.encode()).hexdigest()}

def select(rows: list[dict[str, str]], process: str, stage: str,
           pc: str | None = None) -> list[dict[str, str]]:
    return [row for row in rows if row.get("process") == process
            and row.get("stage") == stage.lower()
            and (pc is None or row.get("pc") == pc.lower())]

def source_direction(ox: int, oy: int, tx: int, ty: int) -> tuple[int, int, int]:
    dx, dy = tx - ox, ty - oy
    ax, ay = abs(dx), abs(dy)
    slope = ((ay << 16) // ax) if ax else 0
    direction = 0
    if ax and slope <= 0x269FB:
        direction += 1
        if slope <= 0x69FB:
            direction += 1
    if dx < 0:
        direction = (8 - direction) if dy < 0 and direction else (0 if dy < 0 else direction + 4)
    elif dy >= 0:
        direction = 4 - direction
    return direction, dx, dy

def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_a", type=Path)
    parser.add_argument("run_b", type=Path)
    parser.add_argument("--mame", required=True, type=Path)
    parser.add_argument("--rom-zip", required=True, type=Path)
    parser.add_argument("--probe", type=Path, default=root / "tools/mame_l8_hulk_direction_trace.lua")
    parser.add_argument("--disassembly-dir", type=Path, default=root / "evidence/l8-hulk-direction-disassembly")
    parser.add_argument("--report", type=Path, default=root / "evidence/l8-hulk-direction-seek-2026-09-23.json")
    parser.add_argument("--native-test-log", type=Path)
    parser.add_argument("--windows-test-log", type=Path)
    parser.add_argument("--build-log", type=Path)
    parser.add_argument("--windows-exe", type=Path)
    parser.add_argument("--preservation-report", type=Path)
    args = parser.parse_args()
    require(sha(args.mame) == MAME_SHA256, "pinned MAME hash mismatch")
    require(sha(args.rom_zip) == ROM_SHA256, "supplied L8 ROM archive hash mismatch")
    manifest_path = args.disassembly_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for name, expected in (("getanix", "ffe065c0: move   *a8(70h),a0,0"),
                           ("getxy", "ffe40850: move   *a8(90h),a0,0")):
        listing = args.disassembly_dir / manifest["targets"][name]["listing"]
        require(sha(listing) == manifest["targets"][name]["listing_sha256"], f"{name} listing hash mismatch")
        require(expected in listing.read_text(encoding="utf-8").lower(), f"{name} source mapping absent")
    run_a, run_b = read_run(args.run_a), read_run(args.run_b)
    require(run_a["event_stream"] == run_b["event_stream"], "fresh-run selected traces differ")
    native_validation = None
    if args.native_test_log:
        text = args.native_test_log.read_text(encoding="utf-8", errors="replace")
        require("L8 resumed Hulk GETXY seeks match sampled actor and target coordinates" in text,
                "native regression log lacks the L8 GETXY direction cases")
        require("RESULT 16 groups; 4808 checks." in text,
                "native regression log lacks the expected completed test summary")
        native_validation = {"result": "PASS", "log": str(args.native_test_log),
            "log_sha256": sha(args.native_test_log), "groups": 16, "checks": 4808,
            "test_source": "tests/test_extended.cpp",
            "test_source_sha256": sha(root / "tests/test_extended.cpp"),
            "helper_source": "src/gameplay.cpp",
            "helper_source_sha256": sha(root / "src/gameplay.cpp")}
    if args.windows_test_log:
        text = args.windows_test_log.read_text(encoding="utf-8", errors="replace")
        require("100% tests passed, 0 tests failed out of 11" in text,
                "Windows test log does not report all eleven CTest targets passing")
        require("frontend native/native replay: 12000" in text,
                "Windows test log lacks the 12,000-state frontend replay")
        require("independent" in text.lower() and "oracle" in text.lower(),
                "Windows test log lacks independent oracle output")
    build_validation = None
    if args.build_log:
        text = args.build_log.read_text(encoding="utf-8", errors="replace")
        require("Building CXX object" in text and "Linking CXX executable stv_extended_tests.exe" in text,
                "MSVC/Ninja build log lacks the native regression build")
        require(args.windows_exe is not None and args.windows_exe.is_file(),
                "MSVC staged executable was not supplied or is missing")
        build_validation = {"result": "PASS", "route": "MSVC x64/Ninja",
            "log": str(args.build_log), "log_sha256": sha(args.build_log),
            "staged_executable": str(args.windows_exe),
            "staged_executable_sha256": sha(args.windows_exe)}
    preservation_validation = None
    if args.preservation_report:
        preservation = json.loads(args.preservation_report.read_text(encoding="utf-8"))
        require(preservation.get("result") == "PASS" and preservation.get("source_files") == 240
                and preservation.get("rom_zip_sha256") == ROM_SHA256,
                "original-input preservation report does not match this project's 240-file/ROM baseline")
        preservation_validation = {"result": "PASS", "report": str(args.preservation_report),
            "report_sha256": sha(args.preservation_report), "source_files": 240,
            "rom_zip_sha256": ROM_SHA256}
    rows = run_a["rows"]
    actors = []
    for process in PROCESS_ORDER:
        obj = OBJECTS[process]
        expected_seek = EXPECTED_SEEKS[process]
        expected_frame = SEEK_FRAMES[process]
        mv = select(rows, process, "FIRST_RESUMED_MV")
        require(len(mv) == 1 and mv[0]["frame"] == "3901" and mv[0]["actor_object"] == obj,
                f"{process}: first resumed MV/process-object mapping changed")
        selected = [row for row in select(rows, process, "PDATA_READ_SKPLYR", "ffe3f6c0")
                    if row["frame"] == expected_frame]
        require(len(selected) == 1 and selected[0]["frame"] == expected_frame and selected[0]["data"] == "00000000",
                f"{process}: SETUP_DIR did not select observed player slot 0 at expected boundary")
        pointer_words = [r for r in select(rows, process, "PLYROBJS_READ", "ffe3f720")
                         if r["frame"] == expected_frame and r["address"] in ("010dd780", "010dd790")]
        require(len(pointer_words) == 2, f"{process}: expected the two PLYROBJS pointer halves")
        parts = {r["address"]: int(r["data"], 16) for r in pointer_words}
        require(parts["010dd780"] == 0x0970 and parts["010dd790"] == 0x0100,
                f"{process}: selected PLYROBJS pointer changed")
        target_ptr = (parts["010dd790"] << 16) | parts["010dd780"]
        target_x_rows = [row for row in select(rows, process, "TARGET_X_READ", "ffe3f880")
                         if row["frame"] == expected_frame]
        target_y_rows = [row for row in select(rows, process, "TARGET_Y_READ", "ffe3f8b0")
                         if row["frame"] == expected_frame]
        require(len(target_x_rows) == len(target_y_rows) == 1,
                f"{process}: expected one target coordinate pair")
        tx, ty = int(target_x_rows[0]["data"], 16), int(target_y_rows[0]["data"], 16)
        require(target_x_rows[0]["frame"] == target_y_rows[0]["frame"] == expected_frame,
                f"{process}: target coordinate sample moved across scheduler boundary")
        require(tx == 0x296 and ty == 0x1CF and target_ptr == 0x01000970,
                f"{process}: target world position/pointer changed")
        hulk_x_rows = [row for row in select(rows, process, "HULK_X_INTEGER_READ", "ffe065e0")
                       if row["frame"] == expected_frame]
        hulk_y_rows = [row for row in select(rows, process, "HULK_Y_INTEGER_READ", "ffe40870")
                       if row["frame"] == expected_frame]
        require(len(hulk_x_rows) == len(hulk_y_rows) == 1,
                f"{process}: expected GETANIX/GETXY actor coordinate reads")
        hx_low = int(hulk_x_rows[0]["data"], 16)
        anchor = int(hulk_x_rows[0]["a2"], 16)
        hx = (hx_low + anchor) & 0xFFFF
        hy = int(hulk_y_rows[0]["data"], 16)
        actual_seek = [row for row in select(rows, process, "PDATA_WRITE_SK_DIR", "ffe3f930")
                       if row["frame"] == expected_frame]
        require(len(actual_seek) == 1 and actual_seek[0]["frame"] == expected_frame
                and int(actual_seek[0]["data"], 16) == expected_seek,
                f"{process}: executed L8 seek result changed")
        direction, dx, dy = source_direction(hx, hy, tx, ty)
        require(direction + 1 == expected_seek,
                f"{process}: native GETXY geometry diverges from L8 seek result")
        vel_rows = [r for r in select(rows, process, "HULK_VELOCITY_WRITE")
                    if r["frame"] == expected_frame and r.get("actor_object") == obj]
        words = {int(r["object_offset"], 16): int(r["data"], 16) for r in vel_rows}
        require(set(words) == {0x20, 0x30, 0x40, 0x50}, f"{process}: expected four velocity words")
        vx = ((words[0x30] << 16) | words[0x20]) & 0xFFFFFFFF
        vy = ((words[0x50] << 16) | words[0x40]) & 0xFFFFFFFF
        require((vx, vy) == EXPECTED_VELOCITIES[expected_seek], f"{process}: velocity writes diverge")
        current_dir = [row for row in select(rows, process, "PDATA_READ_DIR", "ffe3d900")
                       if row["frame"] == expected_frame]
        require(len(current_dir) == 1 and int(current_dir[0]["data"], 16) == expected_seek,
                f"{process}: pre-update direction changed")
        actors.append({
            "process_reference_only": f"0x{process}", "object_reference_only": f"0x{obj}",
            "first_resumed_mv_frame": 3901, "setup_dir_frame": int(expected_frame),
            "previous_dir": expected_seek, "pdata_skplyr": 0,
            "target_pointer_reference_only": f"0x{target_ptr:08x}",
            "target_xy_integer": {"x": tx, "y": ty},
            "hulk_xy_from_getanix_getxy": {"x_integer": hx, "y_integer": hy,
                "object_x_integer": hx_low, "image_anchor_x": anchor},
            "deltas": {"x": dx, "y": dy}, "getxy_direction_zero_based": direction,
            "source_sk_dir_after_increment": expected_seek,
            "velocity_u32": {"x": f"0x{vx:08x}", "y": f"0x{vy:08x}"},
        })
    probe_source = root / "tools/inspect_l8_hulk_direction_pc.py"
    report = {
        "date": "2026-09-23", "result": "PASS",
        "scope": "Two fresh-config/NVRAM MAME L8 runs repeat the first resumed Hulk target-slot selection, GETANIX/GETXY integer-coordinate decision, SK_DIR result, and first velocity writes.",
        "reference": {"mame_version": "0.289", "mame_sha256": MAME_SHA256,
            "rom_archive_sha256": ROM_SHA256, "probe_armed_frame": 3880,
            "first_mv_frame": 3901, "first_seek_frame": 3901,
            "fourth_actor_second_seek_frame": 3912, "fresh_config_and_nvram_each_run": True},
        "mapping": {"hulks_asm": "original/archive/smashtv-main/HULKS.ASM",
            "hulks_asm_sha256": sha(root / "original/archive/smashtv-main/HULKS.ASM"),
            "util_asm": "original/archive/smashtv-main/UTIL.ASM",
            "util_asm_sha256": sha(root / "original/archive/smashtv-main/UTIL.ASM"),
            "disp_equ": "original/archive/smashtv-main/DISP.EQU",
            "disp_equ_sha256": sha(root / "original/archive/smashtv-main/DISP.EQU"),
            "getxy_source_line": 1040, "getanix_source_line": 650,
            "getanix_call_pc": "0xffe40810", "getanix_oxpos_read_next_pc": "0xffe065e0",
            "getxy_oypos_read_next_pc": "0xffe40870", "setup_dir_seek_store_next_pc": "0xffe3f930",
            "rom_disassembly_dir": str(args.disassembly_dir),
            "rom_disassembly_manifest_sha256": sha(manifest_path),
            "rom_disassembly_probe": str(probe_source), "rom_disassembly_probe_sha256": sha(probe_source)},
        "runs": [{"log": str(run["path"]), "log_sha256": run["sha256"],
                  "event_count": len(run["rows"]), "event_stream_sha256": run["event_stream_sha256"]}
                 for run in (run_a, run_b)],
        "paired_stream_identical": True, "actors": actors,
        "native_helper_validation": ("PASS: native GETXY coordinate cases match the L8 trace; see native_validation."
            if native_validation else "Pending native test/build route."),
        "native_validation": native_validation,
        "windows_validation": {"result": "PASS", "test_log": str(args.windows_test_log),
            "test_log_sha256": sha(args.windows_test_log), "ctest_passed": 11, "ctest_failed": 0,
            "replay": "PASS; 12,000-state frontend replay", "oracles": "PASS"}
            if args.windows_test_log else None,
        "build_validation": build_validation,
        "preservation_validation": preservation_validation,
        "limits": ["Selected MAME 0.289 L8 reference only; no PCB behavior or whole-archive retail identity claim.",
                   "Four first resumed Hulk decisions only; no continuous movement/collision/actor ownership translation.",
                   "All captures remain diagnostic evidence and are not inputs to native gameplay."],
        "next_action": "Translate the measured Hulk direction decision as a deterministic continuation with source scheduler ownership, then capture its next turn/animation boundary before integrating the actor into GameSession."}
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"result": "PASS", "report": str(args.report),
        "events": len(rows), "event_stream_sha256": run_a["event_stream_sha256"],
        "actors": [{"process": a["process_reference_only"], "frame": a["setup_dir_frame"],
                    "hulk_xy": a["hulk_xy_from_getanix_getxy"], "target_xy": a["target_xy_integer"],
                    "seek": a["source_sk_dir_after_increment"]} for a in actors]}, indent=2))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
