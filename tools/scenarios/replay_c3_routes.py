#!/usr/bin/env python3
"""Replay three preserved legal C3 schedules on the reference-qualified executable.

Runs sequentially with bounded owned children. MAME is an immutable cache here;
no interpreter is linked or launched, and no game RAM/ROM is changed.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import threading

import coverage_gap_manifest as gaps
import rebaseline as runner

ROOT = Path(__file__).resolve().parents[2]
V8_SHA = "5f312c7d11412f8efc6fc42e638b4a978b5037bfeaf2f2adafabd54475d52393"
CASES = {"bonus": (55000, 180), "lower": (110000, 300), "scorpion": (160000, 420)}
RESULT = "PASS_CURRENT_REFERENCE_ROUTES_CONTENT_OPEN"
require = runner.require


def terminal(text, final):
    matches = re.findall(r"(?m)^native: frames=(\d+) cycles=(\d+) pc=([0-9a-f]+) faults=(\d+) "
                         r"hcount_left=(\d+) input_left=(\d+) input_mismatch=(\d+) big_field=(\d+)\s*$", text)
    require(len(matches) == 1, "native terminal marker missing/ambiguous")
    frames, cycles, pc, faults, hcount, inputs, mismatch, big = matches[0]
    require(int(frames) == final and all(int(v) == 0 for v in (hcount, inputs, mismatch, big)),
            "native endpoint/input/hcount mismatch")
    return dict(frames=int(frames), cycles=int(cycles), pc=pc, observed_fault_count=int(faults),
                hcount_left=0, input_left=0, input_mismatch=0, big_field=0)


def strict_marker(text, final):
    require(f"NO_DIVERGENCE over {final + 1} complete frames (both streams ended)" in text,
            "strict complete comparison marker missing")
    return dict(frames=final + 1, allow_prefix=False)


def bounded(report, name, command, directory, timeout, verify=None):
    require(0 < timeout <= 900, "invalid child deadline")
    timer = threading.Timer(timeout, report.cancel_children,
                            args=(f"{name}: {timeout}-second deadline expired",))
    timer.daemon = True
    timer.start()
    try:
        report.run(name, command, directory, verify)
        require(not report.cancelled.is_set(), "batch cancelled")
    finally:
        timer.cancel()


def reference_executable(exe, proof):
    recorded = proof.get("pins", {}).get(str(exe.resolve()))
    require(recorded is not None and recorded == runner.pin(exe),
            "executable differs from the completed reference report")
    return recorded


def prepare(exe, reference):
    proof = json.loads(reference.read_text(encoding="utf-8"))
    gaps.complete_steps(proof)
    reference_executable(exe, proof)
    runner.check_d10_sources(ROOT, proof["verified_at"])
    schedules = gaps.legal_schedules(ROOT / "evidence/c3-historical-oracle-restoration-2026-11-08.json")
    require({r["name"] for r in schedules["schedules"]} == set(CASES), "route schedule set differs")
    for row in schedules["schedules"]:
        require(row["final_frame"] == CASES[row["name"]][0], "route endpoint differs")
    rom = ROOT / "original/user-supplied-roms/smashtv.zip"
    recorded_rom = proof["pins"].get(str(rom.resolve()))
    require(recorded_rom is not None and gaps.checked(recorded_rom) == rom.resolve(),
            "ROM differs from current reference proof")
    return proof["verified_at"], schedules, dict(exe=runner.pin(exe), rom=runner.pin(rom),
        reference=runner.pin(reference), controller=runner.pin(__file__),
        runner=runner.pin(ROOT / "tools/scenarios/rebaseline.py"),
        gap_reader=runner.pin(ROOT / "tools/scenarios/coverage_gap_manifest.py"),
        comparator=runner.pin(ROOT / "tools/lockstep/lsfile.py"))


def validate_completed(doc):
    require(doc.get("schema") in {"stv.c3.route_replay.v1", "stv.c3.route_replay.v2"} and doc.get("result") == RESULT
            and doc.get("owned_live_pids") == [], "route batch is not terminal PASS")
    if doc["schema"] == "stv.c3.route_replay.v1":
        require(doc.get("pins", {}).get("exe", {}).get("sha256") == V8_SHA,
                "historical route batch executable is not frozen v8")
    else:
        require(all(k in doc.get("pins", {}) for k in ("exe", "reference")),
                "route batch lacks executable/reference binding")
    require(len(doc.get("cases", [])) == 3 and {c["name"] for c in doc["cases"]} == set(CASES)
            and all(c.get("state") == "PASS" and c["final_frame"] == CASES[c["name"]][0]
                    for c in doc["cases"]), "route cases incomplete/duplicate")
    expected = {prefix + name for name in CASES for prefix in ("native-", "diff-")}
    steps = {s["name"]: s for s in doc.get("steps", [])}
    require(len(doc["steps"]) == len(expected) and set(steps) == expected
            and all(s.get("state") == "PASS" and s.get("exit_code") == 0 for s in steps.values()),
            "route proof steps incomplete/duplicate/failed")
    return steps


def read_completed(path):
    doc = json.loads(path.read_text(encoding="utf-8"))
    steps = validate_completed(doc)
    runner.check_d10_sources(ROOT, doc["verified_at"])
    for pin in doc["pins"].values():
        gaps.checked(pin)
    if doc["schema"] == "stv.c3.route_replay.v2":
        proof = json.loads(Path(doc["pins"]["reference"]["path"]).read_text(encoding="utf-8"))
        gaps.complete_steps(proof)
        require(proof["verified_at"] == doc["verified_at"], "route/reference source identity differs")
        reference_executable(Path(doc["pins"]["exe"]["path"]), proof)
    captures = []
    for row in doc["cases"]:
        name, final = row["name"], row["final_frame"]
        native, diff = steps["native-" + name], steps["diff-" + name]
        for step in (native, diff):
            require(runner.sha(step["log"]) == step["log_sha256"], "route log changed")
        inputs, oracle = gaps.checked(row["inputs"]), gaps.checked(row["oracle"])
        paths = {name: gaps.checked(pin) for name, pin in native["proof"]["artifacts"].items()}
        expected_native = [doc["pins"]["exe"]["path"], doc["pins"]["rom"]["path"],
            str(paths["native.bin"]), str(final), "--native-hcount", "--inputs", str(inputs),
            "--snd", str(paths["native.snd"]), "--coverage", str(paths["native.cov"]),
            "--content", str(paths["content.jsonl"])]
        require(native["command"] == expected_native, "route capture command roles differ")
        require(diff["command"][1:] == [doc["pins"]["comparator"]["path"], "diff",
                str(oracle), str(paths["native.bin"])], "route diff is not strict whole-stream comparison")
        strict_marker(Path(diff["log"]).read_text(), final)
        terminal(Path(native["log"]).read_text(), final)
        endpoint = runner.main_endpoint(paths["native.bin"], final)
        require(endpoint == native["proof"]["endpoint"] == runner.main_endpoint(oracle, final),
                "route endpoint changed")
        validation = runner.content_samples(paths["content.jsonl"], final)
        require(validation == native["proof"]["content_samples"], "route content cadence changed")
        observed = gaps.observations(gaps.samples(paths["content.jsonl"]))
        episodes = gaps.actions.terminal_episodes(gaps.samples(paths["content.jsonl"]))
        captures.append(dict(name="route:" + name, profile="reference", final_frame=final,
            inputs=row["inputs"], executable=doc["pins"]["exe"], main=native["proof"]["artifacts"]["native.bin"],
            content=native["proof"]["artifacts"]["content.jsonl"], oracle=row["oracle"],
            observations=observed, terminal_episodes=episodes,
            source_bound_sampled_terminal_progressions=sum(e["source_bound_sampled_terminal_progression"] for e in episodes),
            gate_proof=False))
    return captures


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--reference-report", type=Path,
                    default=ROOT / "build/widescreen-uncapped/reference-renewal-v2/report.json")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--prepare-only", action="store_true")
    a = ap.parse_args(argv)
    output, exe, reference = a.out.resolve(), a.exe.resolve(), a.reference_report.resolve()
    require(not output.exists(), "preserve prior route evidence; choose a fresh output")
    output.mkdir(parents=True)
    report = runner.Report(output, dict(schema="stv.c3.route_replay.v2", result="PREPARING",
        gate_proof=False, clearance_proof=False, profile="reference", max_parallel_children=1,
        owner_pid=os.getpid(), started_at_utc=runner.now(), steps=[], cases=[]))
    try:
        verified_at, schedules, pins = prepare(exe, reference)
        report.update(report.data, verified_at=verified_at, head_at_launch=runner.git(ROOT, "rev-parse", "HEAD"),
                      pins=pins, schedules_receipt=schedules["receipt"])
        for schedule in schedules["schedules"]:
            name, final = schedule["name"], schedule["final_frame"]
            directory = output / name
            directory.mkdir()
            command = [exe, Path(pins["rom"]["path"]), directory / "native.bin", str(final),
                "--native-hcount", "--inputs", Path(schedule["inputs"]["path"]),
                "--snd", directory / "native.snd", "--coverage", directory / "native.cov",
                "--content", directory / "content.jsonl"]
            report.data["cases"].append(dict(name=name, final_frame=final, state="PREPARED",
                native_timeout_seconds=CASES[name][1], diff_timeout_seconds=90,
                inputs=schedule["inputs"], oracle=schedule["oracle"], command=list(map(str, command))))
        report.save()
        if a.prepare_only:
            report.update(report.data, result="PREPARED_NOT_RUN")
            return 0
        report.update(report.data, result="RUNNING")
        for row in report.data["cases"]:
            name, final, directory = row["name"], row["final_frame"], output / row["name"]
            report.update(row, state="RUNNING", started_at_utc=runner.now())
            def verify(text, directory=directory, final=final):
                return dict(terminal=terminal(text, final),
                    endpoint=runner.main_endpoint(directory / "native.bin", final),
                    sound_endpoint=runner.sound_endpoint(directory / "native.snd", final),
                    content_samples=runner.content_samples(directory / "content.jsonl", final),
                    artifacts={leaf: runner.pin(directory / leaf) for leaf in
                        ("native.bin", "native.snd", "native.cov", "content.jsonl")},
                    sound_oracle_proof=False)
            bounded(report, "native-" + name, row["command"], directory, row["native_timeout_seconds"], verify)
            bounded(report, "diff-" + name, [sys.executable, pins["comparator"]["path"], "diff",
                row["oracle"]["path"], directory / "native.bin"], directory, 90,
                lambda text, final=final: strict_marker(text, final))
            report.update(row, state="PASS", completed_at_utc=runner.now())
        runner.check_d10_sources(ROOT, verified_at)
        for pin in pins.values():
            gaps.checked(pin)
        for row in report.data["cases"]:
            gaps.checked(row["inputs"])
            gaps.checked(row["oracle"])
        report.update(report.data, result=RESULT)
        return 3
    except BaseException as error:
        report.cancel_children(str(error) or type(error).__name__)
        report.update(report.data, result="FAIL", error=str(error) or type(error).__name__)
        raise
    finally:
        report.update(report.data, finished_at_utc=runner.now())
        print(json.dumps(dict(result=report.data["result"], report=str(report.path),
                              owned_live_pids=report.data["owned_live_pids"])))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (runner.ProofError, OSError, ValueError, KeyError, TypeError) as error:
        print("C3_ROUTE_REPLAY_FAILED: " + str(error), file=sys.stderr)
        sys.exit(2)
