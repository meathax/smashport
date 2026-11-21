"""Bounded check of the development-only MAME 0.289 L8 paid-start witness."""
import argparse
import hashlib
import json
from pathlib import Path


EVENTS = (
    "L8_ACTION coin_press frame=2600",
    "L8_CMOS_WRITE sample=24 frame=2606 time=47.651739800 address=014010e0 data=00000001 mask=0000ffff pc=ffeaa1f0",
    "L8_ACTION coin_release frame=2850",
    "L8_SNAPSHOT frame=2870 name=l8-credit-attract-2870.png error=nil",
    "L8_ACTION start_press frame=3150",
    "L8_CMOS_WRITE sample=26 frame=3151 time=57.611923000 address=014010e0 data=00000000 mask=0000ffff pc=ffeaa1f0",
    "L8_ACTION start_release frame=3170",
    "L8_SNAPSHOT frame=4200 name=l8-credit-attract-4200.png error=nil",
    "L8_SUMMARY cmos_writes=70",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("script", type=Path)
    parser.add_argument("credit_image", type=Path)
    parser.add_argument("room_image", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    log = args.log.read_text(encoding="utf-8", errors="replace")
    lines = log.splitlines()
    indices = [lines.index(event) for event in EVENTS]
    if indices != sorted(indices):
        raise ValueError("Coin, CMOS, start and snapshot events are out of order")
    if "Average speed:" not in log:
        raise ValueError("MAME did not finish the requested run")
    result = {
        "result": "PASS",
        "reference": "MAME 0.289 retail L8, fresh isolated NVRAM/config, headless graphics snapshots",
        "log_sha256": digest(args.log),
        "script_sha256": digest(args.script),
        "coin_accepted": {"frame": 2606, "mapped_cmos_address": "0x014010e0", "written_value": 1},
        "p1_start_spent_credit": {"frame": 3151, "mapped_cmos_address": "0x014010e0", "written_value": 0},
        "screenshots": {
            "credit_screen": {"file": args.credit_image.name, "sha256": digest(args.credit_image), "frame": 2870,
                              "manual_observation": "1 COIN / 1 PLAY; CREDITS: 1; READY FOR 1 PLAYER"},
            "paid_first_room": {"file": args.room_image.name, "sha256": digest(args.room_image), "frame": 4200,
                                "manual_observation": "P1 alive with active enemies; P2 INSERT COIN TO PLAY"},
        },
        "correction": "The older frame-1050 coin/frame-1100 start probe ran during POST and did not demonstrate paid play; its frame-1700 room was attract footage. Its tank-path memory read remains an executed reference read.",
        "limitation": "CMOS tap and two visually inspected snapshots prove this observed MAME sequence, not source5.00 coin timing, all L8 credit cases, native port behavior or PCB behavior.",
    }
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
