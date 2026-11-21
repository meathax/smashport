#!/usr/bin/env python3
"""Summarize current C3 observations and explicit C4 gaps without inventing clears.

Consumes the existing complete reference proof, source contracts, and pinned
historical receipts. Does not run the game or modify any runtime/source data.
"""
import argparse
import collections
import json
from pathlib import Path
import sys

import c3_action_terminal_audit as actions
import rebaseline as runner

ROOT = Path(__file__).resolve().parents[2]
require = runner.require


def resolve_pin(pin):
    path = Path(pin["path"])
    return path if path.is_absolute() else ROOT / path


def checked(pin):
    path = resolve_pin(pin)
    actual = runner.pin(path)
    require(actual["bytes"] == pin["bytes"] and actual["sha256"] == pin["sha256"],
            f"pinned artifact changed: {path}")
    return path


def inventory_state(pin):
    """Missing historical data is an explicit gap, never an empty inventory."""
    path = resolve_pin(pin)
    if not path.is_file():
        return dict(state="MISSING_PINNED_ARTIFACT", expected=pin, gate_proof=False)
    actual = runner.pin(path)
    same = actual["bytes"] == pin["bytes"] and actual["sha256"] == pin["sha256"]
    return dict(state="EXACT_PIN_MATCH" if same else "PIN_MISMATCH",
                expected=pin, actual=actual, gate_proof=False)


def complete_steps(report):
    require(report.get("result") == "PROOFS_COMPLETE_TARGET_CLOSURE_OPEN"
            and report.get("owned_live_pids") == [], "reference proof is not complete/terminal")
    steps = {s["name"]: s for s in report["steps"]}
    require(len(steps) == len(report["steps"])
            and all(s.get("state") == "PASS" and s.get("exit_code") == 0 for s in steps.values()),
            "reference proof contains duplicate/incomplete/failed steps")
    for name, _, _, _ in runner.CASES:
        require(name + "-native" in steps and name + "-diff" in steps,
                f"reference proof missing case: {name}")
    return steps


def samples(path):
    with Path(path).open(encoding="utf-8") as source:
        header = json.loads(next(source))
        require(header.get("schema") == "stv.content.samples.v1" and header.get("cadence") == 60,
                "unexpected content schema/cadence")
        for line in source:
            yield json.loads(line)


def observations(stream):
    """Track sampled presence, not entry/exit events or room-clear events."""
    rows, counts, candidates = {}, collections.Counter(), []
    previous, active, last_frame = None, None, -60
    for s in stream:
        require(type(s["frame"]) is int and s["frame"] == last_frame + 60,
                "noncontiguous content samples")
        last_frame = s["frame"]
        counts["samples"] += 1
        counts["partial_object_samples"] += not s["object_list_complete"]
        counts["partial_process_samples"] += not s["process_list_complete"]
        key = (s["circuit"], s["wave"]) if s["gamstate"] == 0x10 else None
        if key is not None:
            row = rows.setdefault(key, dict(selector=list(key), first_frame=s["frame"],
                last_frame=s["frame"], samples=0, sampled_spans=0,
                partial_object_samples=0, partial_process_samples=0,
                clearance_state="UNVERIFIED"))
            row["last_frame"] = s["frame"]
            row["samples"] += 1
            row["partial_object_samples"] += not s["object_list_complete"]
            row["partial_process_samples"] += not s["process_list_complete"]
            row["sampled_spans"] += key != active
            counts["in_game_samples"] += 1
            if (previous is not None and previous["gamstate"] == 0x10
                    and previous["circuit"] == s["circuit"]
                    and s["world_x"] < previous["world_x"]):
                candidates.append(dict(delta_x=s["world_x"]-previous["world_x"], before={k: previous[k] for k in
                    ("frame", "circuit", "wave", "world_x", "world_y")},
                    after={k: s[k] for k in ("frame", "circuit", "wave", "world_x", "world_y")},
                    state="CAMERA_DECREASE_CANDIDATE_NOT_VISUAL_OR_ROUTE_PROOF"))
        if active is not None and key != active:
            counts["sampled_span_ends_on_state_or_selector_change"] += 1
        active, previous = key, s
    require(last_frame >= 0, "empty content sample stream")
    return dict(totals=dict(counts), selectors=[rows[k] for k in sorted(rows)],
                capture_ended_during_selector=list(active) if active else None,
                leftward_candidates=sorted(candidates, key=lambda r: r["delta_x"])[:24],
                sampled_camera_decreases=len(candidates),
                largest_sampled_camera_drop=max((-r["delta_x"] for r in candidates), default=0),
                cleared_room_count=0,
                clearance_state="UNVERIFIED",
                scope="INGAME selector presence at 60-frame boundaries; span changes do not prove a room clear.")


def current_capture(report, steps, name, final):
    native, diff = steps[name + "-native"], steps[name + "-diff"]
    command = native["command"]
    require("--enhanced" not in command and command.count("--inputs") == 1,
            f"{name}: not a reference replay with one input schedule")
    for step in (native, diff):
        require(runner.sha(step["log"]) == step["log_sha256"], f"{name}: changed proof log")
    require(len(diff["command"]) == 5 and diff["command"][2] == "diff",
            f"{name}: comparison is not strict whole stream")
    require(f"NO_DIVERGENCE over {final+1} complete frames (both streams ended)"
            in Path(diff["log"]).read_text(), f"{name}: complete comparison marker missing")
    p = native["proof"]
    stream, content = checked(p["stream"]), checked(p["content_stream"])
    require(runner.main_endpoint(stream, final) == p["endpoint"], f"{name}: native endpoint changed")
    oracle = Path(diff["command"][3])
    require(Path(diff["command"][4]).resolve() == stream.resolve(), f"{name}: diff stream differs")
    inputs = Path(command[command.index("--inputs") + 1])
    for path in (oracle, inputs, Path(command[0])):
        pin = report["pins"].get(str(path.resolve()))
        require(pin is not None and checked(pin).resolve() == path.resolve(), f"{name}: missing role pin")
    require(runner.main_endpoint(oracle, final) == p["endpoint"], f"{name}: oracle endpoint changed")
    require(runner.content_samples(content, final) == p["content_samples"], f"{name}: content validation changed")
    observed = observations(samples(content))
    episodes = actions.terminal_episodes(samples(content))
    return dict(name=name, profile="reference", final_frame=final,
                inputs=runner.pin(inputs), executable=runner.pin(command[0]),
                main=p["stream"], content=p["content_stream"],
                oracle=runner.pin(oracle), observations=observed, terminal_episodes=episodes,
                source_bound_sampled_terminal_progressions=sum(
                    e["source_bound_sampled_terminal_progression"] for e in episodes),
                gate_proof=False)


def legal_schedules(receipt_path):
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    require(receipt["result"] == "PASS_EXACT_HISTORICAL_MAIN_ORACLE_RESTORATION"
            and receipt["gate_proof"] is False, "historical oracle receipt differs")
    group = json.loads(checked(receipt["group"]).read_text(encoding="utf-8"))
    require(group["result"] == receipt["result"] and group["owned_owner_pids"] == [],
            "historical oracle restoration not terminal")
    rows = []
    for case in receipt["cases"]:
        inputs, oracle = checked(case["inputs"]), checked(case["stream"])
        require(runner.main_endpoint(oracle, case["final_frame"]) == case["endpoint"],
                "historical oracle endpoint changed")
        rows.append(dict(name=case["name"], final_frame=case["final_frame"],
            inputs=runner.pin(inputs), oracle=runner.pin(oracle),
            state="EXACT_RECOVERED_INPUT_AND_HISTORICAL_ORACLE",
            current_executable_replay="UNVERIFIED", gate_proof=False))
    return dict(receipt=runner.pin(receipt_path), schedules=rows)


def build(report_path, route_report=None):
    report = json.loads(report_path.read_text(encoding="utf-8"))
    steps = complete_steps(report)
    runner.check_d10_sources(ROOT, report["verified_at"])
    # These already-reviewed contracts bind sampled controller phases to source
    # and current generated retail operands. They do not prove process lifetimes.
    contract = actions.contracts()
    require(actions.source_line("ROBO.EQU", 422, "INGAME .EQU 10H"), "INGAME contract changed")
    old_path = ROOT / "evidence/c3-content-observations-2026-10-02.json"
    old = json.loads(old_path.read_text(encoding="utf-8"))
    pressure_path = ROOT / "build/completion-20261108/c4-pressure/report.json"
    pressure = json.loads(pressure_path.read_text(encoding="utf-8"))
    require(checked(pressure["pins"]["inventory_source"]).resolve() == old_path.resolve(),
            "historical normal-room receipt changed")
    required_path = checked(pressure["pins"]["required_rooms"])
    required = {tuple(row) for row in json.loads(required_path.read_text())}
    normal = {tuple(row["selector"]): row for row in old["items"]
              if row["kind"] == "wave_selector" and row["role"] == "wave_room"}
    require(required == set(normal) and len(normal) == 48, "declared normal room obligations differ")
    for pin in old["pins"]["original_inventory_inputs"].values():
        checked(pin)
    inventory_path = ROOT / "evidence/c3-content-tables-preparation-2026-10-02.json"
    inventory = json.loads(inventory_path.read_text())
    historical_path = ROOT / "evidence/c4-pressure-renewal-2026-11-08.json"
    historical = json.loads(historical_path.read_text())
    captures = [current_capture(report, steps, name, final) for name, final, _, _ in runner.CASES]
    if route_report is not None:
        import replay_c3_routes
        captures.extend(replay_c3_routes.read_completed(route_report))
    schedules = legal_schedules(ROOT / "evidence/c3-historical-oracle-restoration-2026-11-08.json")
    if route_report is not None:
        schedules["current_replay_report"] = runner.pin(route_report)
        for row in schedules["schedules"]:
            row["current_executable_replay"] = "COMPLETE_STRICT_REFERENCE_MATCH"
    room_rows = []
    for key, row in sorted(normal.items()):
        seen = [dict(scenario=c["name"], **obs) for c in captures
                for obs in c["observations"]["selectors"] if tuple(obs["selector"]) == key]
        room_rows.append(dict(selector=list(key), name=row["name"], source_anchor=row["source_anchor"],
            obligation_basis="HISTORICAL_HASH_PINNED_DECLARATION_BACKING_STATIC_INVENTORY_MISSING",
            observed_in_current_reference=bool(seen), observations=seen,
            current_enhanced_state="UNVERIFIED_BY_THIS_MANIFEST", clearance_state="UNVERIFIED"))
    return dict(schema="stv.coverage.gaps.v1", result="CURRENT_REFERENCE_OBSERVATIONS_GAPS_OPEN",
        gate_proof=False, verified_at=report["verified_at"], created_at_utc=runner.now(),
        report=runner.pin(report_path), tool=runner.pin(__file__), reference_only=True,
        source_contract=contract, current_captures=captures, normal_room_obligations=room_rows,
        summary=dict(required_normal_selectors=48, current_reference_cases=len(captures),
            current_reference_observed=sum(r["observed_in_current_reference"] for r in room_rows),
            current_reference_unobserved=[r["selector"] for r in room_rows if not r["observed_in_current_reference"]],
            source_bound_sampled_terminal_progressions=sum(c["source_bound_sampled_terminal_progressions"] for c in captures),
            cleared_rooms_proved=0, enhanced_clearability="OPEN"),
        historical_static_inventories=dict(receipt=runner.pin(inventory_path),
            artifacts={name: inventory_state(pin) for name, pin in inventory["inventories"].items()}),
        legal_route_schedules=schedules,
        historical_pressure=dict(receipt=runner.pin(historical_path), source_state=historical["source_state"],
            applicable_to_current_geometry=False, paired_summary=historical["pooled_summary"],
            clearability_limits=historical["clearability_limits"], gate_proof=False),
        obligation_pins=dict(declaration=runner.pin(old_path), required_rooms=runner.pin(required_path),
            pressure_capture=runner.pin(pressure_path)),
        limitations=["Sampled selector presence and camera movement do not prove entry through a legal door or room clearance.",
            "Attract samples remain labelled by their scenario; they do not prove paid gameplay.",
            "Terminal controller sequences are sampled source-bound progressions, not continuous lifetimes or all-room clears.",
            "Missing exact historical inventories remain missing; declarations are not replacement static inventories.",
            "Pressure results belong to their older executable and cannot justify tuning the current enhanced geometry."])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--route-report", type=Path, help="completed current three-route replay receipt")
    args = ap.parse_args(argv)
    require(not args.out.exists(), "preserve prior manifest; choose a fresh output")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = build(args.report.resolve(), args.route_report.resolve() if args.route_report else None)
    except (runner.ProofError, OSError, ValueError, KeyError, TypeError) as error:
        runner.atomic_json(args.out, dict(schema="stv.coverage.gaps.v1", result="FAIL",
            gate_proof=False, error=str(error), created_at_utc=runner.now()))
        raise
    runner.atomic_json(args.out, result)
    print(json.dumps(dict(result=result["result"], summary=result["summary"], path=str(args.out))))
    return 3  # The work is useful evidence; the content gate deliberately stays open.


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (runner.ProofError, OSError, ValueError, KeyError, TypeError) as error:
        print("COVERAGE_GAP_MANIFEST_FAILED: " + str(error), file=sys.stderr)
        sys.exit(2)
