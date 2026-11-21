#!/usr/bin/env python3
"""Bind complete native C3 observations to narrow source/retail contracts.

Terminal selectors alone are never accepted as terminal progression. This audit
requires an ordered, sampled controller sequence and the following circuit.
Weapon coverage proves executed code sites, not per-player events or lifetime.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import sys

import rebaseline as runner

ROOT = Path(__file__).resolve().parents[2]
require = runner.require
TERMINALS = {0: 11, 1: 19, 2: 23}
PHASES = {0xffe7db20: "entry", 0xffe7dfc0: "rackup_wait",
          0xffe7e560: "exit_fade", 0xffe7e5e0: "exit_fade"}
WEAPONS = (
    ("normal", 0, None, None, 957, "ffe337f0", "ffe33810", "ffe33840", "ffe371d0"),
    ("spray", 1, (795, "ffe31ac0"), (797, "ffe31b00"), 980, "ffe33a70", "ffe33a90", "ffe33ac0", "ffe36d80"),
    ("pod", 2, (821, "ffe31e40"), (823, "ffe31e80"), 1023, "ffe34030", "ffe34050", "ffe34080", "ffe36370"),
    ("grenade", 3, (776, "ffe31880"), (778, "ffe318c0"), 1053, "ffe342a0", "ffe342c0", "ffe342f0", "ffe34e20"),
    ("nonstop", 4, (750, "ffe31500"), (752, "ffe31540"), 1066, "ffe343d0", "ffe343f0", "ffe34420", "ffe34ae0"),
)


def load_blocks():
    blocks, pins, coverage_indices = {}, [], set()
    for path in sorted((runner.generated_root() / "l8").glob("l8_chunk_*.cpp")):
        pins.append(runner.pin(path))
        for text in re.split(r"(?=^L_[0-9a-f]{8}:)", path.read_text(encoding="utf-8"), flags=re.M)[1:]:
            pc = text[2:10]
            require(pc not in blocks, "duplicate retail PC")
            match = re.search(r"\bl8_cov\[([0-9]+)\] = 1;", text)
            require(match is not None, f"{pc}: missing coverage index")
            index = int(match[1])
            require(index not in coverage_indices, "duplicate coverage index")
            coverage_indices.add(index)
            blocks[pc] = {"text": text, "index": index, "path": str(path)}
    require(blocks and coverage_indices == set(range(len(blocks))), "coverage index domain is not contiguous")
    return blocks, pins


def source_line(file, number, expected):
    path = ROOT / "original/archive/smashtv-main" / file
    lines = path.read_text(encoding="latin-1").splitlines()
    require(1 <= number <= len(lines), f"{file}:{number}: source line absent")
    actual = " ".join(lines[number - 1].split(";", 1)[0].split())
    require(actual == expected, f"{file}:{number}: source contract differs: {actual}")
    return {"file": str(path), "line": number, "text": actual}


def bind(blocks, pc, header, *operands):
    require(pc in blocks, f"retail contract PC absent: {pc}")
    block = blocks[pc]
    require(header in block["text"].splitlines()[0]
            and all(operand in block["text"] for operand in operands), f"{pc}: retail operand contract differs")
    return {"pc": pc, "coverage_index": block["index"], "file": block["path"],
            "header": block["text"].splitlines()[0], "verified_operands": list(operands)}


def contracts():
    blocks, pins = load_blocks()
    source = []
    for file, number, expected in (
        ("MPROC.EQU", 19, "PROCID .SET 20H"), ("MPROC.EQU", 26, "PWAKE .SET 0E0H"),
        ("MPROC.EQU", 27, "PDATA .SET 100H"), ("ROBO.EQU", 82, "WPN_TYP .EQU PDATA+152"),
        ("ROBO.EQU", 86, "PNUM .EQU PDATA+216"), ("ROBO.EQU", 197, "BULPID .EQU 286"),
        ("ROBO.EQU", 423, "ININTRO .EQU 18H"),
        ("WAVES.ASM", 684, "MOVI ININTRO,A0"), ("WAVES.ASM", 685, "MOVE A0,@GAMSTATE"),
        ("WAVES.ASM", 704, "CMPI 11,A0"), ("WAVES.ASM", 712, "CMPI 19,A0"),
        ("WAVES.ASM", 718, "CMPI 22,A0"), ("WAVES.ASM", 1369, "CTP SLEEP 1"),
        ("WAVES.ASM", 1370, "MOVB @CANT2,A0"), ("WAVES.ASM", 1371, "JRNZ CTP"),
        ("WAVES.ASM", 1464, "MOVE @CIRCUIT,A0"), ("WAVES.ASM", 1465, "INC A0"),
        ("WAVES.ASM", 1466, "MOVE A0,@CIRCUIT"), ("WAVES.ASM", 1467, "CMPI 3,A0"),
        ("WAVES.ASM", 1470, "CLR A0"), ("WAVES.ASM", 1471, "MOVE A0,@CIRCUIT"),
        ("SHOTS.ASM", 953, "MOVB *A13(PNUM),A11"),
        ("SHOTS.ASM", 954, "MOVE *A13(WPN_TYP),A0,W"),
        ("SHOTS.ASM", 1383, "MOVB A11,*A13(MYPLYR)"),
    ):
        source.append(source_line(file, number, expected))
    checks = (
        ("ffe0a490", "MOVE *SP+,A7,1", "u32 v=rf(m,c.sp,1)", "c.a[7]=v;"),
        ("ffe0a4b0", "[MPROC.ASM:79]", "u32(s32(s16(256)))"),
        ("ffe0a4d0", "[MPROC.ASM:80]", "c.a[1]-=0x20u; wlong(m,c.a[1],c.a[7]);"),
        ("ffe78c70", "[WAVES.ASM:684]", "c.a[0]=0x00000018u;"),
        ("ffe78c90", "[WAVES.ASM:685]", "wf(m,0x010865a0u,c.a[0],0);"),
        ("ffe78e40", "[WAVES.ASM:704]", "u32(s32(s16(11)))"),
        ("ffe78f10", "[WAVES.ASM:712]", "u32(s32(s16(19)))"),
        ("ffe78fb0", "[WAVES.ASM:718]", "u32(s32(s16(23)))"),
        ("ffe78ec0", "[WAVES.ASM:708]", "return 0xffe7d720u;"),
        ("ffe78f90", "[WAVES.ASM:715]", "return 0xffe7d720u;"),
        ("ffe79030", "[WAVES.ASM:721]", "return 0xffe7d720u;"),
        ("ffe7d720", "MOVK 1h,A0", "c.a[0]=1u;"),
        ("ffe7d730", "MOVB A0,@{L1}", "wb(m,0x010eb608u,c.a[0]);"),
        ("ffe7daf0", "CALLA {L1}", "push(m,c,0xffe7db20u);", "return 0xffe0a490u;"),
        ("ffe7df70", "MOVI {W1},A0", "c.a[0]=0x00000001u;"),
        ("ffe7df90", "CALLA {L1}", "push(m,c,0xffe7dfc0u);", "return 0xffe0a490u;"),
        ("ffe7dfc0", "MOVB @{L1},A0", "rb(m,0x010eb608u)"),
        ("ffe7dff0", "JRNE {REL}", "if (cond(c,11))", "goto L_ffe7df70;"),
        ("ffe7e530", "CALLA {L1}", "push(m,c,0xffe7e560u);", "return 0xffe0a490u;"),
        ("ffe7e5b0", "CALLA {L1}", "push(m,c,0xffe7e5e0u);", "return 0xffe0a490u;"),
        ("ffe7e8a0", "MOVE @{L1},A0,0", "rf(m,0x010eb610u,0)"),
        ("ffe7e8d0", "INC A0", "u32 a=1u, b=c.a[0], r=a+b;"),
        ("ffe7e8e0", "MOVE A0,@{L1},0", "wf(m,0x010eb610u,c.a[0],0);"),
        ("ffe7e910", "CMPI {~W1},A0", "u32(s32(s16(3)))"),
        ("ffe7e940", "CLR A0", "c.a[0]=0;"),
        ("ffe7e950", "MOVE A0,@{L1},0", "wf(m,0x010eb610u,c.a[0],0);"),
        ("ffe337a0", "[SHOTS.ASM:953]", "rb(m,c.a[13]+0x000001d8u)", "c.a[11]=v;"),
        ("ffe337c0", "[SHOTS.ASM:954]", "rf(m,c.a[13]+0x00000198u,0)"),
        ("ffe35e90", "[SHOTS.ASM:1383]", "wb(m,c.a[13]+0x00000138u,c.a[11]);"),
    )
    retail = {pc: bind(blocks, pc, header, *operands) for pc, header, *operands in checks}
    for name, selector, constant, write, line, pid, target, call, entry in WEAPONS:
        source.append(source_line("SHOTS.ASM", line, f"CREATE BULPID,WPN{selector + 1}"))
        retail[pid] = bind(blocks, pid, f"[SHOTS.ASM:{line}>CREATE:301]", "c.a[1]=0x0000011eu;")
        retail[target] = bind(blocks, target, f"[SHOTS.ASM:{line}>CREATE:302]", f"c.a[7]=0x{entry}u;")
        retail[call] = bind(blocks, call, f"[SHOTS.ASM:{line}>CREATE:303]", "return 0xffe0aba0u;")
        require(entry in blocks, f"{name}: projectile entry absent")
        retail[entry] = {"pc": entry, "coverage_index": blocks[entry]["index"],
                         "file": blocks[entry]["path"], "header": blocks[entry]["text"].splitlines()[0]}
        if write:
            source.append(source_line("SHOTS.ASM", constant[0], f"MOVK {selector},A1"))
            source.append(source_line("SHOTS.ASM", write[0], "MOVE A1,*A0(WPN_TYP),W"))
            retail[constant[1]] = bind(blocks, constant[1], f"[SHOTS.ASM:{constant[0]}]", f"c.a[1]={selector}u;")
            retail[write[1]] = bind(blocks, write[1], f"[SHOTS.ASM:{write[0]}]", "wf(m,c.a[0]+0x00000198u,c.a[1],0);")
    for file in sorted({row["file"] for row in source}):
        pins.append(runner.pin(file))
    pins.append(runner.pin(ROOT / "tools/lockstep/content_observer.hpp"))
    return {"source": source, "retail": retail, "pins": pins, "coverage_size": len(blocks),
            "retail_source_difference": {"source_circuit_2_terminal": 22, "retail_circuit_2_terminal": 23,
                "reason": "The emitted retail compare operand is authoritative; archived source is a different revision."},
            "terminal_selector": TERMINALS, "wake_pc_phases": {f"{pc:08x}": value for pc, value in PHASES.items()}}


def coverage_actions(data, contract):
    require(len(data) == contract["coverage_size"] and all(value in (0, 1) for value in data),
            "coverage bitmap size/value differs from pinned retail instruction domain")
    def hit(pc):
        return bool(data[contract["retail"][pc]["coverage_index"]])
    weapons = []
    for name, selector, constant, write, line, pid, target, call, entry in WEAPONS:
        sites = {"create_pid_literal": hit(pid), "create_entry_literal": hit(target),
                 "create_call": hit(call), "projectile_entry": hit(entry)}
        if write:
            sites.update(pickup_selector_literal=hit(constant[1]), pickup_weapon_write=hit(write[1]))
        weapons.append({"weapon": name, "selector": selector, "sites_executed": sites,
                        "create_call_pc": call, "projectile_entry_pc": entry,
                        "pickup_write_pc": write[1] if write else None,
                        "player_event_proof": False, "lifetime_proof": False})
    return {"weapons": weapons, "firing_player_number_read": hit("ffe337a0"),
            "firing_weapon_selector_read": hit("ffe337c0"), "projectile_player_number_write": hit("ffe35e90"),
            "terminal_branch_sites": {str(c): hit(pc) for c, pc in ((0, "ffe78ec0"), (1, "ffe78f90"), (2, "ffe79030"))},
            "scope": "Each true value proves that instruction executed at least once in this capture. "
                     "Bitmap co-occurrence does not bind separate instructions to one event, player, or lifetime."}


def terminal_episodes(samples):
    episodes, active = [], None
    previous_frame = -60

    def finish(following):
        nonlocal active
        if active is None:
            return
        active["following_sample"] = ({k: following[k] for k in ("frame", "gamstate", "circuit", "wave")}
                                      if following is not None else None)
        controllers = []
        for address, phases in active.pop("_controllers").items():
            entry, waits, exits = (phases.get(p, []) for p in ("entry", "rackup_wait", "exit_fade"))
            ordered = any(e < waits[0] <= waits[-1] < x for e in entry for x in exits) if len(waits) >= 2 else False
            controllers.append({"address": address, "pid": 0, "phase_frames": dict(phases), "ordered_sampled_sequence": ordered})
        active["controllers"] = controllers
        advance = following is not None and following["circuit"] == (active["circuit"] + 1) % 3
        active["next_circuit_observed"] = advance
        active["source_bound_sampled_terminal_progression"] = advance and any(c["ordered_sampled_sequence"] for c in controllers)
        active["clearance_proof"] = False
        active["continuous_process_lifetime_proof"] = False
        episodes.append(active)
        active = None

    for sample in samples:
        require(sample["frame"] == previous_frame + 60, "noncontiguous terminal input cadence")
        previous_frame = sample["frame"]
        c, w, g = sample["circuit"], sample["wave"], sample["gamstate"]
        terminal = g == 24 and c in TERMINALS and w == TERMINALS[c]
        if active is not None and (not terminal or c != active["circuit"]):
            finish(sample)
        if not terminal:
            continue
        if active is None:
            active = {"circuit": c, "wave": w, "gamstate": 24, "first_frame": sample["frame"],
                      "last_frame": sample["frame"], "samples": 0, "incomplete_process_samples": 0,
                      "ambiguous_controller_samples": 0, "_controllers": {}}
        active["last_frame"] = sample["frame"]
        active["samples"] += 1
        if not sample["process_list_complete"]:
            active["incomplete_process_samples"] += 1
            continue
        matches = [p for p in sample["processes"] if p.get("pid") == 0 and p.get("wake") in PHASES]
        if len(matches) > 1:
            active["ambiguous_controller_samples"] += 1
            continue
        for process in matches:
            phases = active["_controllers"].setdefault(process["address"], collections.defaultdict(list))
            phases[PHASES[process["wake"]]].append(sample["frame"])
    finish(None)
    return episodes


def checked_path(path):
    value = Path(path).resolve()
    require(value.is_relative_to(ROOT) and value.is_file(), f"artifact outside project/missing: {value}")
    return value


def pin_matches(path, expected):
    actual = runner.pin(path)
    require(actual["bytes"] == expected["bytes"] and actual["sha256"] == expected["sha256"],
            f"capture artifact changed: {path}")
    return actual


def validate_capture_status(status):
    require(status.get("task") == "capture" and status.get("status") == "PASS"
            and status.get("exit_code") == 0 and status.get("live_child") is False
            and isinstance(status.get("finished_utc"), str), "baseline capture is not terminal PASS")


def captures(status_paths, recovery_paths):
    result = []
    for status_path in status_paths:
        status_path = checked_path(status_path)
        status = json.loads(status_path.read_text(encoding="utf-8"))
        validate_capture_status(status)
        log = checked_path(status["log"])
        require(runner.sha(log) == status["log_sha256"], "baseline capture log changed")
        command = status["command"]
        require(command[4] == "--native-hcount", "capture is not native-hcount")
        main = checked_path(command[2])
        paths = {"native.bin": main}
        for flag, name in (("--snd", "native.snd"), ("--coverage", "native.cov"), ("--content", "content.jsonl")):
            require(command.count(flag) == 1, f"capture missing/ambiguous {flag}")
            paths[name] = checked_path(command[command.index(flag) + 1])
        require(all(p.parent == main.parent for p in paths.values()), "capture artifact directories differ")
        result.append({"name": status_path.parent.name, "final": int(command[3]), "paths": paths,
                       "capture_receipt": runner.pin(status_path), "executable": runner.pin(checked_path(command[0])),
                       "artifacts": {name: runner.pin(path) for name, path in paths.items()}})
    for report_path in recovery_paths:
        report_path = checked_path(report_path)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        require(report.get("schema") == "stv.c3.input_recovery.v1"
                and report.get("result") == "PASS_EXACT_INPUT_RECOVERY"
                and report.get("owned_live_pids") == [], "recovery is not terminal PASS")
        for step in report["steps"]:
            require(step["state"] == "PASS" and step["exit_code"] == 0, "recovery step incomplete")
            artifacts = step["proof"]["artifacts"]
            paths = {name: checked_path(artifacts[name]["path"]) for name in ("native.bin", "native.snd", "native.cov", "content.jsonl")}
            result.append({"name": step["name"], "final": step["proof"]["endpoint"]["last_frame"], "paths": paths,
                           "capture_receipt": runner.pin(report_path),
                           "executable": runner.pin(checked_path(step["command"][0])),
                           "artifacts": {name: pin_matches(path, artifacts[name]) for name, path in paths.items()}})
    require(result and len({c["name"] for c in result}) == len(result), "empty/duplicate capture names")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-status", action="append", type=Path, default=[])
    parser.add_argument("--recovery-report", action="append", type=Path, default=[])
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    output = args.output.resolve()
    require(not output.exists(), f"refusing to overwrite: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {"schema": "stv.c3.action_terminal_audit.v1", "result": "RUNNING", "started_at_utc": runner.now(),
              "gate_proof": False, "fresh_oracle_proof": False, "clearance_proof": False,
              "player_event_lifetime_proof": False, "tool": runner.pin(__file__), "captures": [],
              "limits": ["60-frame sampled process wake-PC sequences do not prove continuous process lifetime.",
                         "Terminal progression does not prove all rooms or all routes cleared.",
                         "Weapon instruction coverage does not prove pickup/firing ownership, collisions, or lifetime.",
                         "These native artifacts are not a new MAME or PCB oracle."]}
    runner.atomic_json(output, report)
    try:
        contract = contracts()
        report["contract"] = contract
        for capture in captures(args.capture_status, args.recovery_report):
            paths = capture.pop("paths")
            final = capture["final"]
            capture["endpoint"] = runner.main_endpoint(paths["native.bin"], final)
            capture["sound_endpoint"] = runner.sound_endpoint(paths["native.snd"], final)
            capture["content_endpoint"] = runner.content_samples(paths["content.jsonl"], final)
            capture["action_sites"] = coverage_actions(paths["native.cov"].read_bytes(), contract)
            with paths["content.jsonl"].open(encoding="utf-8") as source:
                next(source)
                capture["terminal_episodes"] = terminal_episodes(json.loads(line) for line in source)
            report["captures"].append(capture)
            runner.atomic_json(output, report)
        proved = sorted({e["circuit"] for c in report["captures"] for e in c["terminal_episodes"]
                         if e["source_bound_sampled_terminal_progression"]})
        report["terminal_circuits_with_sampled_progression"] = proved
        report["result"] = "PASS_NATIVE_ACTION_AND_TERMINAL_OBSERVATIONS_C3_OPEN"
        return 0
    except BaseException as error:
        report.update(result="FAIL", error=str(error) or type(error).__name__)
        raise
    finally:
        report["finished_at_utc"] = runner.now()
        runner.atomic_json(output, report)
        print(json.dumps({"result": report["result"], "report": str(output),
                          "terminal_circuits": report.get("terminal_circuits_with_sampled_progression", []),
                          "gate_proof": False}))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (runner.ProofError, OSError, ValueError, KeyError, TypeError) as error:
        print("C3 action/terminal audit failed: " + str(error), file=sys.stderr)
        sys.exit(2)
