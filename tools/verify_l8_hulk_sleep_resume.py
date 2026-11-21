#!/usr/bin/env python3
"""Verify paired fresh paid-start PRCSLP/PRCDSP-to-MV L8 traces."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
UNIDASM_SHA256 = "5d27d3c7bc349d63d3511d86d9eb30ce348eb4f0e61f2bd2609f87aa5a5a98e0"
PROCESSES = ["0108b0b0", "0108c190", "0108ab10", "0108ccd0"]
CONTEXT_OFFSETS = [0xe0, 0xf0, 0xc0, 0xd0, 0xa0, 0xb0, 0x80, 0x90, 0x60, 0x70, 0x40, 0x50]
OBJECT_OFFSETS = [0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fields(line: str) -> dict[str, str]:
    out = {}
    for item in line.split():
        if "=" in item:
            key, value = item.split("=", 1)
            out[key.lower()] = value.lower()
    return out


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def load_run(path: Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    markers = [
        "L8_HULK_WAKE_PROBE target=paid_PRCSLP_state_PRCDSP_wake_first_MV",
        "L8_HULK_WAKE_ARMED frame=3528",
        "L8_HULK_WAKE_DONE frame=4300",
        "L8_ACTION coin1_press frame=2600",
        "L8_ACTION coin1_release frame=2604",
        "L8_ACTION start1_press frame=3150",
        "L8_ACTION start1_release frame=3170",
        "Average speed:",
    ]
    for marker in markers:
        require(marker in text, f"{path}: missing {marker!r}")
    require("[LUA ERROR]" not in text and "L8_HULK_WAKE_ERROR" not in text,
            f"{path}: MAME Lua error")
    raw = [line for line in text.splitlines() if "L8_HULK_WAKE_TAP" in line]
    parsed = [(line, fields(line)) for line in raw]
    event_stream = "\n".join(re.sub(r"\s+", " ", line.strip().lower()) for line, _ in parsed) + "\n"
    return {
        "path": path,
        "text": text,
        "sha256": sha256(path),
        "rows": [item[1] for item in parsed],
        "event_stream": event_stream,
        "event_stream_sha256": hashlib.sha256(event_stream.encode()).hexdigest(),
        "tap_count": len(parsed),
    }


def select(rows: list[dict[str, str]], stage: str, process: str) -> list[dict[str, str]]:
    return [row for row in rows if row.get("stage") == stage.lower() and row.get("process") == process]


def check_source(root: Path, disasm: Path) -> dict[str, str]:
    paths = {
        "mproc_asm": root / "original/archive/smashtv-main/MPROC.ASM",
        "mproc_equ": root / "original/archive/smashtv-main/MPROC.EQU",
        "hulks_asm": root / "original/archive/smashtv-main/HULKS.ASM",
        "disp_equ": root / "original/archive/smashtv-main/DISP.EQU",
    }
    content = {name: path.read_text(encoding="latin1") for name, path in paths.items()}
    checks = {
        "mproc_asm": ["PRCSLP:", "MMTM\tA1,A7,A8,A9,A10,A11,A12", "MOVE\tA0,-*A1,W", "PRCDSP", "MOVE\t*A13(PTIME),A0", "MOVE\tA0,*A13(PTIME)"],
        "mproc_equ": ["PTIME\t.SET", "PWAKE\t.SET", "SLEEP\t$MACRO"],
        "hulks_asm": ["DOLOGIC:", "MV:", "MOVE\t@EHALT,A0", "CALLR\tDO_DIFF", "CALLR\tGET_RUN"],
        "disp_equ": ["OXVEL\t.EQU", "OYVEL\t.EQU", "OXVAL\t.EQU", "OYVAL\t.EQU"],
    }
    for name, patterns in checks.items():
        upper = content[name].upper()
        for pattern in patterns:
            require(pattern.upper() in upper, f"source check failed: {name} missing {pattern!r}")
    expected = {
        "mproc_asm": ["ffe0a4d0: mmtm   a1,a7-a12", "ffe0a4f0: move   a0,-*a1,0", "ffe0a550: move   a0,*a13(30h),0", "ffe0a5b0: mmfm   a1,a7-a12", "ffe0a5d0: jump   a7"],
        "ohulk_resumed_movement": ["ffe3d6a0: jr     ffe3c760h", "ffe3d710: move   *a13(100h),a0,0", "ffe3d730: andi   3ffh,a0"],
        "ohulk_velocity": ["ffe3fe50: move   a1,*a8(20h),1", "ffe3fe70: move   a0,*a8(40h),1", "ffe3ff60: move   a1,*a8(20h),1", "ffe3ff80: move   a0,*a8(40h),1", "ffe40080: move   a1,*a8(20h),1", "ffe400a0: move   a0,*a8(40h),1"],
        "object_positions": ["ffe04b80: move   a6,*a8(80h),1", "ffe04bd0: move   a7,*a8(60h),1"],
    }
    listings = {
        "mproc_asm": disasm / "prcslp-prcdsp.txt",
        "ohulk_resumed_movement": disasm / "ohulk-resumed-movement.txt",
        "ohulk_velocity": disasm / "ohulk-resumed-velocity.txt",
        "object_positions": disasm / "object-position-store.txt",
    }
    for name, patterns in expected.items():
        listing = listings[name].read_text(encoding="utf-8").lower()
        for pattern in patterns:
            require(pattern in listing, f"disassembly check failed: {name} missing {pattern!r}")
    return {name: sha256(path) for name, path in paths.items()}


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_a", type=Path)
    parser.add_argument("run_b", type=Path)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--probe", type=Path, default=root / "tools/mame_l8_hulk_sleep_resume.lua")
    parser.add_argument("--disassembly-helper", type=Path, default=root / "tools/inspect_l8_hulk_sleep_resume_pc.py")
    parser.add_argument("--disassembly-dir", type=Path, default=root / "evidence/l8-hulk-sleep-resume-disassembly")
    parser.add_argument("--object-report", type=Path, default=root / "evidence/l8-hulk-object-init-2026-09-23.json")
    parser.add_argument("--source-root", type=Path, default=root)
    parser.add_argument("--repeat-dir", type=Path, required=True, help="fresh disassembly output directory to compare byte-for-byte")
    parser.add_argument("--report", type=Path, default=root / "evidence/l8-hulk-sleep-resume-2026-09-23.json")
    args = parser.parse_args()

    require(sha256(args.mame) == MAME_SHA256, "pinned MAME hash mismatch")
    require(sha256(args.rom_zip) == ROM_SHA256, "supplied ROM archive hash mismatch")
    with zipfile.ZipFile(args.rom_zip) as archive:
        require(archive.testzip() is None, "ROM archive CRC check failed")
    manifest_path = args.disassembly_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    require(manifest["rom_archive_sha256"] == ROM_SHA256, "disassembly ROM identity mismatch")
    require(manifest["reference"]["mame_executable_sha256"] == MAME_SHA256, "disassembly MAME identity mismatch")
    require(manifest["reference"]["unidasm_executable_sha256"] == UNIDASM_SHA256, "disassembly unidasm identity mismatch")
    require(manifest["program_region_sha256"] == "55432384624396b41f2cfc528e96284cc2e076cd222114a6def0b71ec34676c3", "program region hash mismatch")
    for target in manifest["targets"].values():
        listing = args.disassembly_dir / target["listing"]
        require(sha256(listing) == target["listing_sha256"], f"listing hash mismatch: {listing}")
    source_hashes = check_source(args.source_root, args.disassembly_dir)
    primary_files = {p.relative_to(args.disassembly_dir).as_posix(): sha256(p)
                     for p in args.disassembly_dir.rglob("*") if p.is_file()}
    repeat_files = {p.relative_to(args.repeat_dir).as_posix(): sha256(p)
                    for p in args.repeat_dir.rglob("*") if p.is_file()}
    require(primary_files == repeat_files and len(primary_files) == 10,
            "fresh bounded disassembly output does not reproduce all 10 files byte-for-byte")
    object_report = json.loads(args.object_report.read_text(encoding="utf-8"))
    expected_objects = {item["process_pointer"].lower().removeprefix("0x"): item["object_pointer"].lower().removeprefix("0x") for item in object_report["initial_objects"]}

    a, b = load_run(args.run_a), load_run(args.run_b)
    require(a["event_stream"] == b["event_stream"], "paired sleep/resume event streams differ")
    rows = a["rows"]
    actors = []
    for process in PROCESSES:
        base = int(process, 16)
        context = select(rows, "PRCSLP_CONTEXT_WRITE", process)
        require(len(context) == 12, f"{process}: expected 12 first PRCSLP context halfword writes, got {len(context)}")
        context_by_offset = {int(row["address"], 16) - base: row for row in context}
        require(sorted(context_by_offset) == sorted(CONTEXT_OFFSETS), f"{process}: unexpected saved-context addresses")
        require([int(row["address"], 16) - base for row in context] == CONTEXT_OFFSETS,
                f"{process}: saved-context transfer order changed")
        pwa_low = int(context_by_offset[0xe0]["data"], 16)
        pwa_high = int(context_by_offset[0xf0]["data"], 16)
        wake_pc = (pwa_high << 16) | pwa_low
        obj_low = int(context_by_offset[0xc0]["data"], 16)
        obj_high = int(context_by_offset[0xd0]["data"], 16)
        object_pointer = f"{((obj_high << 16) | obj_low):08x}"
        require(object_pointer == expected_objects[process], f"{process}: saved A8/object pointer mismatch")
        sleep = select(rows, "PRCSLP_SLEEP_VALUE_WRITE", process)
        require(len(sleep) == 1 and int(sleep[0]["address"], 16) == base + 0x30 and int(sleep[0]["data"], 16) == 2,
                f"{process}: first SLEEP 2 did not store in PTIME")
        countdown = select(rows, "PRCDSP_PTIME_DECREMENT", process)
        require([int(row["data"], 16) for row in countdown] == [1, 0],
                f"{process}: PTIME countdown is not exactly 2->1->0: {[row['data'] for row in countdown]}")
        require([int(row["frame"]) for row in countdown] == [3534, 3535],
                f"{process}: first PTIME decrement frame labels differ")
        restored = select(rows, "PRCDSP_REGISTER_RESTORE_READ", process)
        require(len(restored) == 12, f"{process}: expected 12 restored context words")
        restored_by_offset = {int(row["address"], 16) - base: row for row in restored}
        require({off: row["data"] for off, row in restored_by_offset.items()} ==
                {off: row["data"] for off, row in context_by_offset.items()},
                f"{process}: PRCDSP restore differs from PRCSLP saved register image")
        wake = select(rows, "FIRST_RESUMED_WAKE_PC", process)
        require(len(wake) == 1 and wake[0]["data"] == "ffe3d6a0" and wake_pc == 0xffe3d6a0,
                f"{process}: PWAKE did not restore the post-SLEEP continuation")
        mv = select(rows, "FIRST_RESUMED_MV_ENTRY", process)
        require(len(mv) == 1 and mv[0]["pc"] == "ffe3d730" and
                int(mv[0]["address"], 16) == base + 0x100 and int(mv[0]["frame"]) == 3901,
                f"{process}: first resumed MV/HAGE read missing or changed")
        obj_writes = select(rows, "FIRST_POST_MV_OBJECT_WRITE", process)
        require(len(obj_writes) == 8, f"{process}: expected first velocity/position word writes")
        offsets = [int(row["address"], 16) - int(object_pointer, 16) for row in obj_writes]
        require(sorted(offsets) == OBJECT_OFFSETS, f"{process}: object field offsets changed: {offsets}")
        by_offset = {off: row for off, row in zip(offsets, obj_writes)}
        require(all(by_offset[off]["frame"] == "3901" for off in (0x20,0x30,0x40,0x50)),
                f"{process}: first OXVEL/OYVEL writes did not follow MV in frame 3901")
        require(all(by_offset[off]["frame"] == "3902" for off in (0x60,0x70,0x80,0x90)),
                f"{process}: first OXVAL/OYVAL word writes did not follow in frame 3902")
        actors.append({
            "process_pointer": f"0x{process}",
            "process_id": 236,
            "object_pointer": f"0x{object_pointer}",
            "saved_context_word_offsets": [f"0x{off:02x}" for off in CONTEXT_OFFSETS],
            "sleep": {"value": 2, "ptime_address": f"0x{base+0x30:08x}", "frame": int(sleep[0]["frame"]), "pc_after_store": sleep[0]["pc"]},
            "ptime_decrement": [{"frame": int(row["frame"]), "emutime_seconds": float(row["emutime"]), "value": int(row["data"],16)} for row in countdown],
            "wake_pc": f"0x{wake_pc:08x}",
            "wake_frame": int(wake[0]["frame"]),
            "first_mv": {"frame": int(mv[0]["frame"]), "pc_after_hage_read": mv[0]["pc"], "hage_word": mv[0]["data"]},
            "first_object_field_writes": [
                {"offset": f"0x{off:02x}", "frame": int(by_offset[off]["frame"]), "data_word": by_offset[off]["data"], "pc_after_store": by_offset[off]["pc"]}
                for off in OBJECT_OFFSETS
            ],
        })

    require(a["tap_count"] == 148, f"unexpected observed event count: {a['tap_count']}")
    disasm_helper_hash = sha256(args.disassembly_helper)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Two paired fresh-config/NVRAM MAME L8 paid-start runs capture the first PRCSLP process image and SLEEP 2 store, PRCDSP PTIME countdown/restoration, the first restored OHULK PC and first resumed MV/HAGE read plus object velocity/position field writes for four initial Hulks.",
        "reference": {"mame_version": "0.289", "mame_executable_sha256": MAME_SHA256, "rom_archive_sha256": ROM_SHA256, "fresh_config_and_nvram_each_run": True, "capture_armed_frame": 3528, "first_sleep_frame": 3533, "first_wake_frame": 3535, "first_mv_frame": 3901},
        "probe": {"lua_probe": str(args.probe), "lua_probe_sha256": sha256(args.probe), "disassembly_helper": str(args.disassembly_helper), "disassembly_helper_sha256": disasm_helper_hash, "disassembly_manifest": str(manifest_path), "disassembly_manifest_sha256": sha256(manifest_path), "program_region_sha256": manifest["program_region_sha256"], "targets": manifest["targets"]},
        "source_hashes": source_hashes,
        "prerequisite_reports": {"scheduler": {"path": "evidence/l8-hulk-scheduler-2026-09-23.json", "sha256": sha256(root / "evidence/l8-hulk-scheduler-2026-09-23.json")}, "continuation": {"path": "evidence/l8-hulk-continuation-2026-09-23.json", "sha256": sha256(root / "evidence/l8-hulk-continuation-2026-09-23.json")}, "object_initialization": {"path": str(args.object_report), "sha256": sha256(args.object_report)}},
        "runs": [{"log": str(run["path"]), "log_sha256": run["sha256"], "event_count": run["tap_count"], "event_stream_sha256": run["event_stream_sha256"]} for run in (a,b)],
        "reference_event_stream_sha256": a["event_stream_sha256"],
        "disassembly_reproduction": {"rerun_directory": str(args.repeat_dir), "file_count": len(primary_files), "byte_identical": True, "rerun_manifest_sha256": sha256(args.repeat_dir / "manifest.json")},
        "paired_stream_identical": True,
        "actors": actors,
        "limits": [
            "Selected MAME L8 reference only; not physical-PCB validation or full arcade/source identity.",
            "The trace captures the first SLEEP 2/countdown/wake only; intervening repeated BUYINPID waits between frame 3535 and the first MV entry at frame 3901 are not individually captured.",
            "Frame labels are MAME frame-done labels; exact memory-tap emutimes are recorded for tapped stores and reads, but these labels do not define a new instruction clock.",
            "HAGE and object words remain source-space fields; positions have not been converted to monitor pixels or the expanded 16:9 coordinate system.",
            "The trace does not implement or connect native actor continuations; captured process/object pointers and values remain evidence only.",
        ],
        "next_action": "Trace the first resumed DOHULK/SETUP_DIR/GET_RUN path and subsequent per-frame velocity/position deltas after this initial MV entry; then translate only the source-owned continuation states that can be verified against paired L8 traces.",
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": "PASS", "report": str(args.report), "event_stream_sha256": a["event_stream_sha256"], "tap_count": a["tap_count"], "actors": [{"process": actor["process_pointer"], "object": actor["object_pointer"], "sleep": actor["sleep"]["value"], "wake": actor["wake_pc"], "mv_frame": actor["first_mv"]["frame"], "field_writes": len(actor["first_object_field_writes"])} for actor in actors]}, indent=2))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
