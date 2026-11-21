#!/usr/bin/env python3
"""Recover the three missing C3 input schedules from pinned historical recipes.

This only reproduces native input/artifact bytes. It never creates an oracle or
claims new MAME equivalence, room clearance, route closure, or content closure.
"""
import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import re
import sys
import threading

import rebaseline as runner

ROOT = Path(__file__).resolve().parents[2]
CASES = (
    ("bonus", "evidence/c3-bonus-route-2026-10-02.json"),
    ("lower", "evidence/c3-lower-route-2026-10-02.json"),
    ("scorpion", "evidence/c3-scorpion-route-2026-10-02.json"),
)
BASE = ROOT / "tests/scenarios/two_player_full_game_440k.inputs"
BOT = ROOT / "tools/lockstep/bot.hpp"
OBSERVER = ROOT / "tools/lockstep/content_observer.hpp"
ENV = {"STV_LS_COMPACT": "1", "STV_LS_SUMMARY": "1", "STV_LS_DUMP_EVERY": "60"}
ARTIFACTS = ("native.bin", "native.snd", "native.cov", "content.jsonl", "recorded.inputs")
require = runner.require


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check_pin(data, expected, name):
    require(type(expected) is dict and type(expected.get("bytes")) is int
            and expected["bytes"] >= 0 and type(expected.get("sha256")) is str
            and re.fullmatch(r"[0-9a-f]{64}", expected["sha256"]), f"{name}: malformed pin")
    require(len(data) == expected["bytes"] and digest(data) == expected["sha256"],
            f"{name}: bytes/hash differ from historical pin")


def receipt_pin(receipt, suffix):
    suffix = suffix.replace("\\", "/").lower()
    matches = [pin for pin in receipt["pins"].values()
               if pin["path"].replace("\\", "/").lower().endswith("/" + suffix)]
    require(len(matches) == 1, f"receipt pin missing/ambiguous: {suffix}")
    return matches[0]


def recipe(receipt):
    require(receipt.get("gate_proof") is False, "receipt must preserve the open C3 gate")
    steps = [s for s in receipt["steps"] if s["name"] == "native-variant"]
    require(len(steps) == 1, "receipt must contain exactly one native-variant step")
    step = steps[0]
    require(step["state"] == "PASS" and step["exit_code"] == 0,
            "historical native step is not terminal PASS")
    require(step["environment"] == ENV, "historical native environment differs")
    cmd = step["command"]
    require(type(cmd) is list and len(cmd) == 20 and all(type(x) is str for x in cmd),
            "historical native command shape differs")
    flags = {4: "--native-hcount", 5: "--inputs", 7: "--bot", 9: "--bot2",
             10: "--bot-after", 12: "--bot-seed", 14: "--snd",
             16: "--coverage", 18: "--content"}
    require(all(cmd[index] == flag for index, flag in flags.items()),
            "unrecognized historical command options")
    names = {0: "stv_l8_lockstep.exe", 1: "smashtv.zip", 2: "native.bin",
             6: "prefix.inputs", 8: "recorded.inputs", 15: "native.snd",
             17: "native.cov", 19: "content.jsonl"}
    require(all(PureWindowsPath(cmd[index]).name == name for index, name in names.items()),
            "historical command file roles differ")
    require(all(re.fullmatch(r"[0-9]+", cmd[i]) for i in (3, 11, 13)), "invalid frame/seed")
    final, after, seed = (int(cmd[i]) for i in (3, 11, 13))
    require(0 <= after <= final <= 2000000 and seed <= 0xffffffff, "frame/seed out of bounds")
    endpoint = step["proof"]["endpoint"]
    require(endpoint == {"first_frame": 0, "last_frame": final, "frames": final + 1,
                         "summaries": final + 1, "dump_every": 60},
            "historical native endpoint does not match recipe")
    artifacts = step["proof"]["artifacts"]
    require(set(artifacts) == set(ARTIFACTS), "historical artifact roles differ")
    for name, item in artifacts.items():
        require(PureWindowsPath(item["path"]).name == name, "artifact path/role mismatch")
        require(type(item.get("bytes")) is int and item["bytes"] >= 0
                and re.fullmatch(r"[0-9a-f]{64}", item.get("sha256", "")), "malformed artifact pin")
    return {"final": final, "bot_after": after, "bot_seed": seed, "artifacts": artifacts,
            "prefix": receipt_pin(receipt, "prefix.inputs"),
            "native_protected_source_commit": receipt.get("native_protected_source_commit")}


def events(data, final=None):
    try:
        rows = data.decode("ascii").splitlines()
    except UnicodeError as error:
        raise runner.ProofError("input schedule is not ASCII") from error
    previous = -1
    parsed = []
    for number, line in enumerate(rows, 1):
        match = re.fullmatch(r"([0-9]+) (:IN[01]) ([0-9a-fA-F]+) ([01])", line)
        require(match is not None, f"input line {number}: malformed event")
        frame, port, mask, value = match.groups()
        frame, mask, value = int(frame), int(mask, 16), int(value)
        require(previous <= frame and 0 < mask <= 0xffff, f"input line {number}: ordering/mask invalid")
        require(final is None or frame <= final, f"input line {number}: outside requested endpoint")
        previous = frame
        parsed.append((frame, port, mask, value, line))
    require(parsed, "empty input schedule")
    return parsed


def prefix_bytes(data, after):
    require(type(after) is int and after >= 0, "invalid prefix endpoint")
    selected = [row[4] for row in events(data) if row[0] < after]
    require(selected, "prefix contains no events")
    return ("\n".join(selected) + "\n").encode("ascii")


def prepare(name, receipt_path, output, base_data, rom):
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    spec = recipe(receipt)
    check_pin(base_data, receipt_pin(receipt, "tests/scenarios/two_player_full_game_440k.inputs"), "base inputs")
    for path, suffix in ((BOT, "tools/lockstep/bot.hpp"), (OBSERVER, "tools/lockstep/content_observer.hpp"),
                         (rom, "original/user-supplied-roms/smashtv.zip")):
        check_pin(path.read_bytes(), receipt_pin(receipt, suffix), suffix)
    prefix = prefix_bytes(base_data, spec["bot_after"])
    check_pin(prefix, spec["prefix"], name + " prefix")
    directory = output / name
    directory.mkdir()
    (directory / "prefix.inputs").write_bytes(prefix)
    spec.update(name=name, directory=str(directory), receipt=runner.pin(receipt_path),
                recovered_prefix=runner.pin(directory / "prefix.inputs"))
    return (name, spec)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--rom", type=Path, default=ROOT / "original/user-supplied-roms/smashtv.zip")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=2, choices=(1, 2, 3))
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args(argv)
    output = args.output.resolve()
    require(not output.exists(), f"refusing to overwrite evidence: {output}")
    require(1 <= args.timeout_seconds <= 7200, "timeout must be 1..7200 seconds")
    pins = {str(p): runner.pin(p) for p in (Path(__file__), ROOT / "tools/scenarios/rebaseline.py",
                                          BASE, BOT, OBSERVER, args.exe, args.rom)}
    output.mkdir(parents=True)
    report = runner.Report(output, {"schema": "stv.c3.input_recovery.v1", "result": "RUNNING",
        "started_at_utc": runner.now(), "gate_proof": False, "fresh_oracle_proof": False,
        "pins": pins, "steps": [], "recipes": [],
        "scope": "Historical prefix and native-recorded-input recovery only; no content/clearance/oracle gate.",
        "timeout_seconds": args.timeout_seconds})
    timer = None
    try:
        base_data = BASE.read_bytes()
        cases = [prepare(name, ROOT / path, output, base_data, args.rom) for name, path in CASES]
        report.data["recipes"] = [spec for _, spec in cases]
        report.save()
        if args.prepare_only:
            report.update(report.data, result="PASS_PREFIX_RECOVERY_ONLY")
            return 0
        timer = threading.Timer(args.timeout_seconds, report.cancel_children, args=("recovery deadline expired",))
        timer.daemon = True
        timer.start()

        def run_case(case):
            name, spec = case
            directory = Path(spec["directory"])
            command = [args.exe.resolve(), args.rom.resolve(), directory / "native.bin", str(spec["final"]),
                       "--native-hcount", "--inputs", directory / "prefix.inputs",
                       "--bot", directory / "recorded.inputs", "--bot2",
                       "--bot-after", str(spec["bot_after"]), "--bot-seed", str(spec["bot_seed"]),
                       "--snd", directory / "native.snd", "--coverage", directory / "native.cov",
                       "--content", directory / "content.jsonl"]

            def verify(tail):
                current = {file: runner.pin(directory / file) for file in ARTIFACTS}
                same = {file: current[file]["bytes"] == spec["artifacts"][file]["bytes"]
                        and current[file]["sha256"] == spec["artifacts"][file]["sha256"] for file in ARTIFACTS}
                proof = {"endpoint": runner.main_endpoint(directory / "native.bin", spec["final"]),
                         "sound_endpoint": runner.sound_endpoint(directory / "native.snd", spec["final"]),
                         "samples": runner.content_samples(directory / "content.jsonl", spec["final"]),
                         "artifacts": current, "historical_hash_match": same,
                         "input_events": len(events((directory / "recorded.inputs").read_bytes(), spec["final"])),
                         "fresh_oracle_proof": False}
                report.update(spec, reproduction=proof)
                require(same["recorded.inputs"], name + ": recorded input hash differs; preserve as new evidence, not recovered identity")
                return proof

            report.run("native-" + name, command, directory, verify)
            return {"name": name, "recorded_inputs": spec["reproduction"]["artifacts"]["recorded.inputs"],
                    "historical_hash_match": spec["reproduction"]["historical_hash_match"]}

        results = runner.run_main_group(report, cases, args.workers, run_case)
        require(not report.cancelled.is_set(), "recovery cancelled/deadline expired")
        report.update(report.data, result="PASS_EXACT_INPUT_RECOVERY", recovered=results)
        return 0
    except BaseException as error:
        report.cancel_children(str(error) or type(error).__name__)
        report.update(report.data, result="FAIL", error=str(error) or type(error).__name__)
        raise
    finally:
        if timer:
            timer.cancel()
        report.update(report.data, finished_at_utc=runner.now())
        print(json.dumps({"result": report.data["result"], "report": str(report.path),
                          "owned_live_pids": report.data["owned_live_pids"], "fresh_oracle_proof": False}))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (runner.ProofError, OSError, KeyError, TypeError, ValueError) as error:
        print("C3 input recovery failed: " + str(error), file=sys.stderr)
        sys.exit(2)
