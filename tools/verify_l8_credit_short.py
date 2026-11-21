"""Check one post-POST four-frame L8 coin pulse in the pinned MAME reference."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("script", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    log = args.log.read_text(encoding="utf-8", errors="replace")
    ordered = [
        "L8_ACTION coin_press frame=2600",
        "L8_ACTION coin_release frame=2604",
        "L8_CREDIT_WRITE frame=2606 time=47.651739800 address=014010e0 data=00000001 mask=0000ffff pc=ffeaa1f0",
        "L8_SNAPSHOT frame=2750 error=nil",
    ]
    lines = log.splitlines()
    indices = [lines.index(event) for event in ordered]
    if indices != sorted(indices) or "Average speed:" not in log:
        raise ValueError("Short-pulse reference events are incomplete or out of order")
    report = {
        "result": "PASS",
        "reference": "MAME 0.289 retail L8 with fresh isolated NVRAM/config",
        "log_sha256": digest(args.log),
        "script_sha256": digest(args.script),
        "image_sha256": digest(args.image),
        "coin_held_frames": 4,
        "credit_write_frame": 2606,
        "credit_write_mapped_cmos_address": "0x014010e0",
        "credit_write_value": 1,
        "credit_screen_frame": 2750,
        "manual_screen_observation": "1 COIN / 1 PLAY; CREDITS: 1; READY FOR 1 PLAYER; PRESS START",
        "limitation": "One post-POST short-pulse witness; does not establish every debounce phase, audit write, source5.00 equivalence or native behavior.",
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
