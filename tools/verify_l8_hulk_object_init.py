#!/usr/bin/env python3
"""Verify repeated L8 writes linking initial Hulk process and object state."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


MAME_SHA256 = "af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256 = "7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
PROCESS_REPORT = Path("evidence/l8-hulk-process-context-2026-09-23.json")
DISASSEMBLY_DIR = Path("evidence/l8-hulk-object-disassembly")
EXPECTED_STORES = {
    "hulk-object-link-zero": "ffe02de0: move   a2,*a0(160h),1",
    "hulk-object-link-assign": "ffe04190: move   a13,*a0(160h),1",
    "hulk-object-init-x": "ffe043d0: move   a9,*a0(60h),1",
    "hulk-object-init-y": "ffe043f0: move   a10,*a0(80h),1",
    "hulk-object-motion-y": "ffe04b80: move   a6,*a8(80h),1",
    "hulk-object-motion-x": "ffe04bd0: move   a7,*a8(60h),1",
}


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
    writes = [fields(line) for line in lines if line.startswith("L8_HULK_OBJECT_WRITE ")]
    counts = [fields(line) for line in lines if line.startswith("L8_HULK_COUNT ")]
    return events, writes, counts


def first_write(writes: list[dict[str, str]], address: int, field: str, pc: str) -> dict[str, str]:
    address_text = f"{address:08x}"
    matches = [row for row in writes if row.get("address") == address_text and row.get("field") == field
               and row.get("pc") == pc]
    if not matches:
        raise ValueError(f"missing {field} write at 0x{address_text} from captured PC {pc}")
    return matches[0]


def verify(args: argparse.Namespace) -> dict[str, object]:
    if sha256(args.mame) != MAME_SHA256:
        raise ValueError("MAME executable hash differs from the pinned 0.289 reference")
    if sha256(args.rom_zip) != ROM_SHA256:
        raise ValueError("supplied ROM archive differs from the verified L8 identity")

    process_report = json.loads(args.process_report.read_text(encoding="utf-8"))
    if process_report.get("result") != "PASS" or len(process_report.get("initial_hulks", [])) != 4:
        raise ValueError("the prerequisite live HULK process/object mapping is not passing")
    actors = process_report["initial_hulks"]

    hulk_source = args.hulks_source.read_text(encoding="latin-1")
    disp_source = args.disp_source.read_text(encoding="latin-1")
    if not ("CALLA\tBEGINOBJ2" in hulk_source and "CALLR\tSETUP_HULK" in hulk_source):
        raise ValueError("HULKS.ASM no longer puts BEGINOBJ2 before SETUP_HULK")
    order = (hulk_source.index("CALLA\tBEGINOBJ2"), hulk_source.index("CALLR\tSETUP_HULK"))
    if order[0] >= order[1]:
        raise ValueError("HULK source object setup/count order changed")
    for fragment in ("OXVAL\t.EQU   \t60H", "OXPOS\t.EQU   \t70H", "OYVAL\t.EQU   \t080H",
                     "OYPOS\t.EQU   \t090H", "OPLINK\t.EQU   \t160H"):
        if fragment not in disp_source:
            raise ValueError(f"preserved DISP.EQU object layout no longer contains {fragment!r}")

    manifest_path = args.disassembly_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("rom_archive_sha256") != ROM_SHA256 or
            manifest.get("reference", {}).get("mame_executable_sha256") != MAME_SHA256):
        raise ValueError("object-field disassembly manifest has a different ROM or MAME identity")
    listing_hashes: dict[str, str] = {}
    for target, instruction in EXPECTED_STORES.items():
        listing_info = manifest.get("targets", {}).get(target)
        if not listing_info:
            raise ValueError(f"disassembly manifest omits {target}")
        listing_path = args.disassembly_dir / listing_info["listing"]
        if sha256(listing_path) != listing_info.get("listing_sha256"):
            raise ValueError(f"disassembly listing hash mismatch for {target}")
        listing_text = listing_path.read_text(encoding="utf-8").lower()
        if instruction not in listing_text:
            raise ValueError(f"ROM listing does not map {target} to the source object field store")
        listing_hashes[target] = listing_info["listing_sha256"]

    events_a, writes_a, counts_a = read_run(args.log_a)
    events_b, writes_b, counts_b = read_run(args.log_b)
    if events_a != events_b or writes_a != writes_b or counts_a != counts_b:
        raise ValueError("fresh MAME runs disagree on first-room object setup events")
    snapshot_event = "L8_SNAPSHOT frame=4200 error=nil"
    if snapshot_event not in events_a:
        raise ValueError("the trace did not capture its paid-room comparison screenshot")
    summary = next((event for event in events_a if event.startswith("L8_SUMMARY ")), "")
    if summary != f"L8_SUMMARY object_writes={len(writes_a)}":
        raise ValueError("object write summary differs from the raw trace")
    if len(counts_a) != 5:
        raise ValueError("expected one HULK_CNT reset and four first-room count increments")

    image_hashes = []
    for image in (args.run_a_room, args.run_b_room, args.reference_room):
        image_hashes.append(sha256(image))
    if len(set(image_hashes)) != 1:
        raise ValueError("fresh-run frame-4200 screens differ from each other or the paid-room reference")

    count_rows = {int(row["value"], 16) // 0x100: row for row in counts_a if int(row["value"], 16) > 0}
    if set(count_rows) != {1, 2, 3, 4}:
        raise ValueError("the frame-3533 counter trace is missing initial Hulk counts 1..4")

    initial_objects: list[dict[str, object]] = []
    all_writes = sorted(writes_a, key=lambda row: (float(row["emutime"]), int(row["sample"])))
    for actor in actors:
        number = int(actor["count"])
        object_ptr = int(actor["object_pointer"], 16)
        process_ptr = int(actor["process_pointer"], 16)
        x_low = first_write(all_writes, object_ptr + 0x60, "x", "ffe043f0")
        x_high = first_write(all_writes, object_ptr + 0x70, "x", "ffe043f0")
        y_low = first_write(all_writes, object_ptr + 0x80, "y", "ffe04410")
        y_high = first_write(all_writes, object_ptr + 0x90, "y", "ffe04410")
        link_low_zero = first_write(all_writes, object_ptr + 0x160, "oplink", "ffe02e00")
        link_high_zero = first_write(all_writes, object_ptr + 0x170, "oplink", "ffe02e00")
        link_low = first_write(all_writes, object_ptr + 0x160, "oplink", "ffe041b0")
        link_high = first_write(all_writes, object_ptr + 0x170, "oplink", "ffe041b0")

        if (int(x_high["data"], 16) != actor["source_integer_x_word"] or
                int(y_high["data"], 16) != actor["source_integer_y_word"]):
            raise ValueError(f"actor {number} initial object words disagree with the HULK process-context report")
        if (int(link_low_zero["data"], 16) != 0 or int(link_high_zero["data"], 16) != 0 or
                int(link_low["data"], 16) != (process_ptr & 0xffff) or
                int(link_high["data"], 16) != (process_ptr >> 16)):
            raise ValueError(f"actor {number} OPLINK clear/reassignment does not identify its HULK process")

        ordered = [x_low, x_high, y_low, y_high, link_low, link_high]
        if not max(float(row["emutime"]) for row in ordered[:4]) < min(float(link_low["emutime"]), float(link_high["emutime"])):
            raise ValueError(f"actor {number} process link was assigned before both initial coordinates")
        count = count_rows[number]
        if (count.get("a8") != actor["object_pointer"][2:] or count.get("a13") != actor["process_pointer"][2:] or
                float(count["emutime"]) <= max(float(link_low["emutime"]), float(link_high["emutime"]))):
            raise ValueError(f"actor {number} HULK_CNT increment did not follow the linked object initialization")

        motion_x = next((row for row in all_writes if row.get("address") in {
            f"{object_ptr + 0x60:08x}", f"{object_ptr + 0x70:08x}"} and row.get("field") == "x" and
            row.get("pc") == "ffe04bf0" and float(row["emutime"]) > float(count["emutime"])), None)
        motion_y = next((row for row in all_writes if row.get("address") in {
            f"{object_ptr + 0x80:08x}", f"{object_ptr + 0x90:08x}"} and row.get("field") == "y" and
            row.get("pc") == "ffe04ba0" and float(row["emutime"]) > float(count["emutime"])), None)
        if not motion_x or not motion_y or motion_x.get("a8") != actor["object_pointer"][2:] or motion_x.get("a13") != actor["process_pointer"][2:] or motion_y.get("a8") != actor["object_pointer"][2:] or motion_y.get("a13") != actor["process_pointer"][2:]:
            raise ValueError(f"actor {number} post-setup motion writes do not target its linked object/process")

        initial_objects.append({
            "count": number,
            "frame": 3533,
            "process_id": actor["process_id"],
            "process_pointer": actor["process_pointer"],
            "object_pointer": actor["object_pointer"],
            "initial_object_setup_emutime": {
                "x": float(x_low["emutime"]), "y": float(y_low["emutime"]),
                "oplink": float(link_low["emutime"]), "hulk_count_increment": float(count["emutime"]),
            },
            "raw_source_position_words": {
                "x_fraction_low_word": int(x_low["data"], 16),
                "x_integer_high_word": int(x_high["data"], 16),
                "y_fraction_low_word": int(y_low["data"], 16),
                "y_integer_high_word": int(y_high["data"], 16),
            },
            "process_link_was_cleared_then_set_to_a13": True,
            "post_setup_x_store_pc": "0xffe04bf0",
            "post_setup_y_store_pc": "0xffe04ba0",
        })

    return {
        "date": "2026-09-23",
        "result": "PASS",
        "scope": "Two identical fresh paid-first-room runs show initial X/Y object field writes, OPLINK clear/reassignment to four HULKPID processes, then HULK_CNT increments; selected post-setup coordinate writes target those same A8/A13 pairs.",
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
        "prerequisite_process_context_report": args.process_report.as_posix(),
        "disassembly": {
            "manifest": manifest_path.as_posix(),
            "manifest_sha256": sha256(manifest_path),
            "helper": args.disassembly_helper.as_posix(),
            "helper_sha256": sha256(args.disassembly_helper),
            "verified_store_listings": listing_hashes,
            "captured_pc_note": "For each tapped object store, the ROM listing was checked at the preceding instruction address; the trace retains MAME's simultaneously sampled PC register value.",
        },
        "source_layout": {
            "x_long_word": "DISP.EQU:OXVAL +0x60; integer high word OXPOS +0x70",
            "y_long_word": "DISP.EQU:OYVAL +0x80; integer high word OYPOS +0x90",
            "object_process_link": "DISP.EQU:OPLINK +0x160",
            "hulk_source_order": "HULKS.ASM:OHULK calls BEGINOBJ2 before SETUP_HULK increments HULK_CNT",
        },
        "initial_objects": initial_objects,
        "room_image": {
            "frame": 4200,
            "sha256": image_hashes[0],
            "matches_paid_first_room_reference": True,
        },
        "limits": [
            "The trace identifies object-field stores and process links but does not reconstruct the full BEGINOBJ2 call stack, GETPRC initialization/return path or dispatch scheduler.",
            "The raw X/Y words have not been converted from L8 world coordinates to monitor pixels or widened-arena coordinates.",
            "The observed later X/Y stores are not yet a full source HULK movement model; native actor/process ownership remains disconnected.",
            "HCOUNT reads are still not individually attributed to placement draws or retries, and no captured coordinates or pointers are used by native gameplay.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_a", type=Path)
    parser.add_argument("log_b", type=Path)
    parser.add_argument("--mame", type=Path, required=True)
    parser.add_argument("--rom-zip", type=Path, required=True)
    parser.add_argument("--hulks-source", type=Path, default=Path("original/archive/smashtv-main/HULKS.ASM"))
    parser.add_argument("--disp-source", type=Path, default=Path("original/archive/smashtv-main/DISP.EQU"))
    parser.add_argument("--process-report", type=Path, default=PROCESS_REPORT)
    parser.add_argument("--disassembly-dir", type=Path, default=DISASSEMBLY_DIR)
    parser.add_argument("--disassembly-helper", type=Path, default=Path("tools/inspect_l8_hulk_pc.py"))
    parser.add_argument("--run-a-room", type=Path, required=True)
    parser.add_argument("--run-b-room", type=Path, required=True)
    parser.add_argument("--reference-room", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Report exists; refusing to overwrite it")
    result = verify(args)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("PASS: repeated L8 traces map initial object coordinates and OPLINK to four HULKPID processes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
