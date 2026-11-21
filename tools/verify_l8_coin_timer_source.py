"""Record source5.00 coin timer operations beside one executed L8 coin witness.

The source register transfer is preserved literally; this does not infer a
countdown duration or claim that the source and retail L8 paths are equivalent.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(text: str, pattern: str, label: str) -> None:
    if re.search(pattern, text, re.MULTILINE | re.IGNORECASE) is None:
        raise ValueError(f"Source operation missing: {label}")


def require_order(text: str, steps: list[tuple[str, str]]) -> None:
    position = 0
    for label, pattern in steps:
        match = re.search(pattern, text[position:], re.MULTILINE | re.IGNORECASE)
        if match is None:
            raise ValueError(f"Source operation missing or out of order: {label}")
        position += match.end()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("witness", type=Path)
    parser.add_argument("reference_log", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    main_source = (args.source / "MAIN.ASM").read_text(encoding="latin-1")
    audit_source = (args.source / "AUDIT.ASM").read_text(encoding="latin-1")
    timing = json.loads(args.witness.read_text(encoding="utf-8"))
    reference_log = args.reference_log.read_text(encoding="utf-8", errors="replace")
    if timing.get("result") != "PASS" or timing.get("coin_held_frames") != 4 or timing.get("credit_write_frame") != 2606:
        raise ValueError("Expected executed four-frame L8 coin witness is unavailable")
    if sha256(args.reference_log) != timing["log_sha256"]:
        raise ValueError("Reference log does not match the checked witness")
    pressed = re.search(r"^L8_ACTION coin_press frame=(\d+)$", reference_log, re.MULTILINE)
    released = re.search(r"^L8_ACTION coin_release frame=(\d+)$", reference_log, re.MULTILINE)
    if not pressed or not released or int(released[1]) - int(pressed[1]) != timing["coin_held_frames"]:
        raise ValueError("Reference coin pulse does not match checked witness")
    require(audit_source, r"VALID_COIN:[\s\S]*?MOVK\s+30,A0\s*;HIT TIMER", "VALID_COIN timer load")
    require(audit_source, r"COIN_LOOP:\s*SLEEP\s+1[\s\S]*?JRGT\s+COIN_LOOP", "coin process waits on flag")
    validator = audit_source.split("VALID_COIN:", 1)[1].split("COIN_LOOP:", 1)[0]
    require(validator, r"CMPI\s+INDIAG,A0\s*\n\s*JREQ\s+CRX", "diagnostic guard")
    require(validator, r"MOVE\s+A8,A0[\s\S]*?CALLA\s+GET_ADJ[\s\S]*?MOVE\s+A0,A8[\s\S]*?JRZ\s+CRX", "zero-multiplier guard")
    require(validator, r"CALLR\s+SLAM_P[^\n]*\n\s*JRNZ\s+CRX", "initial slam guard")
    require(validator, r"MOVI\s+LC_PID,A0[\s\S]*?MOVE\s+A0,\*A13\(PROCID\),W", "shared coin process ID")
    require(validator, r"MOVB\s+\*A10,A0[\s\S]*?JRNZ\s+CRX[\s\S]*?MOVK\s+30,A0[\s\S]*?MOVB\s+A0,\*A10", "bounce guard and flag initialization")
    require(audit_source, r"COIN_LOOP:\s*SLEEP\s+1[\s\S]*?MOVB\s+\*A10,A0[\s\S]*?JRGT\s+COIN_LOOP[\s\S]*?CALLR\s+SLAM_P[^\n]*\n\s*JRNZ\s+CRX", "post-timer slam guard")
    require(audit_source, r"SLAM_P:[\s\S]*?MOVI\s+SLAM_PID,A0[\s\S]*?MOVI\s+0FFFFH,A1[\s\S]*?CALLA\s+EXISTP[\s\S]*?RETS", "SLAM_P checks the active process ID")
    require(audit_source, r"SLAM_SW:[\s\S]*?MOVI\s+SLAM_PID,A0[\s\S]*?MOVE\s+A0,\*A13\(PROCID\),W[\s\S]*?MOVI\s+0FFFFH,A1[\s\S]*?CALLA\s+KILALL[\s\S]*?SLEEP\s+40H[\s\S]*?JAUC\s+SUCIDE", "SLAM_SW replaces prior process and sleeps 40H")
    post_timer = audit_source.split("COIN_LOOP:", 1)[1].split("CREDSP:", 1)[0]
    output_steps = [
        ("sound suppression clear", r"MOVE\s+A0,@SOUNDSUP,W"),
        ("coin sound request", r"SOUND1\s+COINSND"),
        ("buy-in timer", r"MOVE\s+A0,@BTIME"),
        ("countdown refresh", r"CALLR\s+DOCNT"),
        ("coin fraction timer", r"MOVE\s+A0,@C_FRAC,W"),
        ("direct chute audit", r"CALLR\s+AUD1"),
        ("mechanical counter adjustment", r"MOVI\s+ADJCNTR,A0"),
        ("mechanical counter increment", r"MOVB\s+A0,\*A1"),
        ("credit adjustment", r"CALLR\s+ADJUST_CREDITS"),
        ("default page", r"CALLR\s+DEF_PAGE"),
    ]
    require_order(post_timer, output_steps)
    require(audit_source, r"MECH1L[\s\S]*?CMPI\s+10,A0[\s\S]*?MECH10[\s\S]*?MOVK\s+2,A0[\s\S]*?MECH20[\s\S]*?CMPI\s+2,A0[\s\S]*?MECH3B[\s\S]*?DINT[\s\S]*?MOVB\s+\*A1,A0[\s\S]*?ADD\s+A2,A0[\s\S]*?MOVB\s+A0,\*A1", "mechanical counter mode and byte increment")
    require(main_source, r"INC\s+A0\s*\n\s*SLL\s+29,A0\s*\n\s*SRL\s+29,A0[\s\S]*?JRNE\s+DIRQX", "DIRQ1 modulo-eight gate")
    require(main_source, r"CTIMLP[\s\S]*?MOVE\s+A0,A1\s*\n\s*SRL\s+24,A1[\s\S]*?DEC\s+A1\s*\n\s*MOVB\s+A0,\*A2", "decrement A1 then write A0")
    require(main_source, r"C_CNTR[\s\S]*?CCNTR0L[\s\S]*?MOVB\s+\*A5\(16\),A2[\s\S]*?MOVK\s+2,A2[\s\S]*?DEC\s+A1[\s\S]*?C_CNTR1L[\s\S]*?DEC\s+A0", "mechanical pulse service")
    report = {
        "result": "PASS",
        "finding": "Source5.00 loads 30 into the coin flag and gates service every eighth end-screen interrupt; the service loop decrements A1 but writes A0. No 30-times-eight-frame countdown is inferred.",
        "source_initial_flag_value": 30,
        "source_service_gate_end_screen_interrupts": 8,
        "source_decrement_register": "A1",
        "source_store_register": "A0",
        "source_guard_sequence": ["diagnostic", "zero multiplier", "initial slam", "shared LC_PID", "bounce flag", "load 30", "SLEEP 1 flag wait", "post-timer slam"],
        "source_slam_process": {"id": "SLAM_PID", "retrigger": "KILALL prior incarnation", "sleep_literal": "40H", "completion": "SUCIDE"},
        "source_output_sequence": [label for label, _ in output_steps],
        "l8_coin_held_frames": timing["coin_held_frames"],
        "l8_frames_press_to_credit_write": timing["credit_write_frame"] - int(pressed[1]),
        "l8_credit_write_frame": timing["credit_write_frame"],
        "source_sha256": {"MAIN.ASM": sha256(args.source / "MAIN.ASM"), "AUDIT.ASM": sha256(args.source / "AUDIT.ASM")},
        "reference_report_sha256": sha256(args.witness),
        "reference_log_sha256": sha256(args.reference_log),
        "limitation": "Literal source operations and one executed L8 credit write only. A separate bounded L8 PC map covers selected MOVB writes; this verifier does not establish whole-process timing, shared-process scheduling or native credit/audio/display output.",
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
