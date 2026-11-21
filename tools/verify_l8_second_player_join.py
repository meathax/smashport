"""Check an observed paid P2 join in an active L8 P1 game under MAME 0.289."""
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
    parser.add_argument("credit_image", type=Path)
    parser.add_argument("two_player_image", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    events = [
        "L8_ACTION coin1_press frame=2600",
        "L8_CREDIT_WRITE frame=2606 time=47.651739800 address=014010e0 data=00000001 mask=0000ffff pc=ffeaa1f0",
        "L8_ACTION start1_press frame=3150",
        "L8_CREDIT_WRITE frame=3151 time=57.611923000 address=014010e0 data=00000000 mask=0000ffff pc=ffeaa1f0",
        "L8_ACTION coin2_press frame=4200",
        "L8_CREDIT_WRITE frame=4202 time=76.827024800 address=014010e0 data=00000001 mask=0000ffff pc=ffeaa1f0",
        "L8_ACTION coin2_release frame=4204",
        "L8_SNAPSHOT frame=4250 error=nil",
        "L8_ACTION start2_press frame=4500",
        "L8_CREDIT_WRITE frame=4501 time=82.290055200 address=014010e0 data=00000000 mask=0000ffff pc=ffeaa1f0",
        "L8_SNAPSHOT frame=4700 error=nil",
    ]
    positions = [lines.index(event) for event in events]
    if positions != sorted(positions) or not any(line.startswith("Average speed:") for line in lines):
        raise ValueError("Second-player join reference sequence incomplete or out of order")
    report = {
        "result": "PASS",
        "reference": "MAME 0.289 retail L8 with fresh isolated NVRAM/config",
        "log_sha256": digest(args.log), "script_sha256": digest(args.script),
        "credit_image_sha256": digest(args.credit_image),
        "two_player_image_sha256": digest(args.two_player_image),
        "mapped_cmos_credit_address": "0x014010e0",
        "p1": {"coin_credit_frame": 2606, "start_debit_frame": 3151},
        "p2": {"coin_credit_frame": 4202, "start_debit_frame": 4501},
        "manual_observation": "P2 HUD changes from INSERT COIN to PRESS START after coin2; frame 4700 shows both player sprites and separate P1/P2 HUDs in the first room",
        "limitation": "One observed local paid join, not full two-player progression, native behavior or online joining.",
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
