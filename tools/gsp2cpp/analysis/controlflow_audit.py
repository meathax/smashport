#!/usr/bin/env python3
"""Reproducible conservative C2 analysis; never edits generated game code.

Verifies the current ROM, instruction index, compiled labels and all register
transfers. Rechecks six bounded table-dispatch contracts and the CIRCLE
branch-selected callback from typed operands, not disassembler template text.
Local domains are conditional proofs, never global indirect-target closure.

Optional .cov inputs must contain one Boolean byte per exact index row. Their
presence is execution observation only; this tool does not certify oracle,
scenario completion, source lifetime, or dead-code proof. Output stays in build/.
Exit 3 means analysis succeeded but the C2/C3 gates remain open; 1 means invalid
inputs or a broken checked contract.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/gsp2cpp"))
import compare
from codemap import unresolved_transfers
from decode import Decoder

TRANSFER_FORMS = {"CALL", "JUMP", "EXGPC"}
MASK_RETURNS = {
    0xffe6f500, 0xffe7b910, 0xffe9b3e0,
    0xffeb63b0, 0xffebb560, 0xfff3a690,
}
WEAK_DATA = {
    0xffe62340, 0xffe62380, 0xffe62440, 0xffe62680,
    0xffe626c0, 0xffe62700, 0xffe6de40, 0xffe6e2d0,
    0xffe70b40, 0xfff90000, 0xfff9cac0, 0xfffb55a0,
}
BUDGET_CONTRACTS = (
    (0xffe46cb0, 2178, "120b8c4e536ab64422cf676372c7b23b3771a54bf6c4013c8d5dbabaa1d1a36f"),
    (0xffe48340, 2406, "7bbdd9a9e4432ca68695c5c043e93820553d1a93a25cd8cafb8825cddec9be00"),
    (0xffe494d0, 2581, "cb0070b6d20e48c2079bcbd4c7e8c21495941a90fcf5fdaf187f74cfc6faf41e"),
    (0xffe4a420, 2725, "e41a0402bf4b022dfe30242606987ea5e01236a08811320d84f44dc5e28f696b"),
    (0xffe4b490, 2884, "40a5b776665b8b39f30bd6ef3bfea2927d6f61753398a1f5961c4051a83a6926"),
)
COLLISION_TABLE_HASH = "3937999bef377e6c9edbbd48f85e1b5c4a516a90b3d7a66b58d23da6678b1967"
OPERATOR_MODULES = {"ADJUST.ASM", "AUDIT.ASM", "DIAG.ASM", "HSTD.ASM",
                    "MENU.ASM", "TEST.ASM", "TVMNTR.ASM"}
PLATFORM_MODULES = {"MAIN.ASM", "MPROC.ASM", "NDSP1.ASM", "PALL.ASM",
                    "PATCH.ASM", "UTIL.ASM", "TRIG.ASM", "SOUNDS.ASM"}


class ContractError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def pin(path):
    path = Path(path).resolve()
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": sha(path.read_bytes())}


def operand(o):
    return (o.kind, o.reg, o.value)


def shape(ins):
    return (ins.mnem, tuple(operand(o) for o in ins.ops))


def expect(ins, mnemonic, *operands):
    require(shape(ins) == (mnemonic, tuple(operands)),
            f"{ins.pc:08x}: unexpected typed operation {shape(ins)!r}")


def contiguous(seq):
    require(bool(seq), "empty instruction window")
    for left, right in zip(seq, seq[1:]):
        require(left.pc + left.words * 16 == right.pc,
                f"noncontiguous instruction window at {left.pc:08x}")


def checked_table(rom, address, count, compiled, expected_hash=None):
    require(count > 0, "empty target table")
    require(address % 16 == 0, "unaligned target table")
    offset = (address - compare.BASE) // 8
    require(address >= compare.BASE and offset + 4 * count <= len(rom),
            "target table outside ROM")
    raw = rom[offset:offset + 4 * count]
    if expected_hash is not None:
        require(sha(raw) == expected_hash, "target table bytes changed")
    targets = list(struct.unpack("<" + "I" * count, raw))
    missing = sorted({target for target in targets if target not in compiled})
    require(not missing, "table has noncompiled targets: " +
            ",".join(f"{x:08x}" for x in missing))
    return {"address": f"{address:08x}", "entries": count,
            "sha256": sha(raw),
            "targets": [f"{x:08x}" for x in targets],
            "distinct_targets": len(set(targets))}


def masked_table_domain(seq, rom, compiled, expected_hash=None):
    """Conditional on entry at the AND and 32-bit field 1."""
    require(len(seq) == 5, "budget dispatch needs five instructions")
    contiguous(seq)
    table = seq[-1].pc + 16
    expect(seq[0], "ANDI", ("IMM", None, 3), ("R", "A3", None))
    expect(seq[1], "SLL", ("K", None, 5), ("R", "A3", None))
    expect(seq[2], "ADDI", ("IMM", None, table), ("R", "A3", None))
    expect(seq[3], "MOVE", ("IND", "A3", None), ("R", "A3", None),
           ("K", None, 1))
    expect(seq[4], "JUMP", ("R", "A3", None))
    return checked_table(rom, table, 4, compiled, expected_hash)


def collision_table_domain(seq, rom, compiled, expected_hash=None):
    """Both unsigned five-bit fields are enumerated; out-of-table -> slot zero."""
    require(len(seq) == 13, "collision dispatch needs thirteen instructions")
    contiguous(seq)
    base, end = 0xffe55b50, 0xffe5c750
    patterns = (
        ("MOVB", ("DISP", "A1", 0x158), ("R", "A3", None)),
        ("MOVB", ("DISP", "A2", 0x158), ("R", "A4", None)),
        ("SLL", ("K", None, 27), ("R", "A3", None)),
        ("SRL", ("K", None, 17), ("R", "A3", None)),
        ("SLL", ("K", None, 27), ("R", "A4", None)),
        ("SRL", ("K", None, 22), ("R", "A4", None)),
        ("ADD", ("R", "A4", None), ("R", "A3", None)),
        ("ADDI", ("IMM", None, base), ("R", "A3", None)),
        ("CMPI", ("IMM", None, end), ("R", "A3", None)),
        ("JRC", ("REL", None, 0xffe55b30)),
        ("MOVI", ("IMM", None, base), ("R", "A3", None)),
        ("MOVE", ("IND", "A3", None), ("R", "A3", None), ("K", None, 1)),
        ("JUMP", ("R", "A3", None)),
    )
    for ins, pattern in zip(seq, patterns):
        expect(ins, *pattern)
    count = (end - base) // 32
    require(base + 1023 * 32 < (1 << 32), "table index can wrap CPU address")
    slots = set()
    for a in range(32):
        for b in range(32):
            pointer = base + (a << 10) + (b << 5)
            pointer = pointer if pointer < end else base
            slots.add((pointer - base) // 32)
    require(slots == set(range(count)), "collision bound does not cover declared table")
    result = checked_table(rom, base, count, compiled, expected_hash)
    result["input_field_pairs_checked"] = 32 * 32
    result["out_of_range_index_rule"] = "reset_to_first_slot"
    return result


def literal_pair(previous, transfer, compiled):
    """One exact producer path, without asserting alternate-entry exclusion."""
    if (previous is None or previous.pc + previous.words * 16 != transfer.pc or
            previous.mnem != "MOVI" or len(previous.ops) != 2 or
            previous.ops[0].kind != "IMM" or previous.ops[1].kind != "R" or
            previous.ops[1].reg != transfer.ops[0].reg):
        return None
    target = previous.ops[0].value & 0xffffffff
    continuation = transfer.pc + transfer.words * 16
    require(target in compiled, f"{transfer.pc:08x}: literal target is not compiled")
    if transfer.mnem in {"CALL", "EXGPC"}:
        require(continuation in compiled,
                f"{transfer.pc:08x}: return continuation is not compiled")
    return {"producer": f"{previous.pc:08x}", "target": f"{target:08x}",
            "continuation": f"{continuation:08x}",
            "proof_scope": "execution_through_adjacent_literal_producer",
            "alternate_entry_excluded": False}


def circle_domain(instructions, compiled):
    pcs = [p for p in sorted(instructions) if 0xfff4ff40 <= p <= 0xfff50100]
    seq = [instructions[p] for p in pcs]
    contiguous(seq)
    require(len(seq) >= 6, "CIRCLE window missing")
    expect(seq[0], "MOVI", ("IMM", None, 0xfff501f0), ("R", "A7", None))
    expect(seq[1], "BTST", ("K", None, 16), ("R", "A6", None))
    expect(seq[2], "JREQ", ("REL", None, 0xfff4ffc0))
    expect(seq[3], "MOVI", ("IMM", None, 0xfff50370), ("R", "A7", None))
    require(seq[4].pc == 0xfff4ffc0, "CIRCLE branch does not join after both producers")
    # Require the complete reviewed path; a new call/implicit restore must never
    # silently retain a stale A7 fact.
    expected_middle = [
        "MOVI", "MOVI", "MOVE", "MOVE", "MOVI", "CLR", "MOVE", "MOVE",
        "SLL", "SUBK", "NEG", "CMP", "JRGT",
    ]
    require([x.mnem for x in seq[4:-2]] == expected_middle,
            "CIRCLE producer-to-use instruction sequence changed")
    for ins in seq[4:-2]:
        require(not any(o.reg == "A7" for o in ins.ops),
                "CIRCLE A7 touched before callback copy")
    expect(seq[-2], "MOVE", ("R", "A7", None), ("R", "B0", None))
    expect(seq[-1], "EXGPC", ("R", "B0", None))
    targets = [0xfff501f0, 0xfff50370]
    require(all(p in compiled for p in targets + [0xfff50110]),
            "CIRCLE callback or continuation is not compiled")
    return {"targets": [f"{p:08x}" for p in targets],
            "continuation": "fff50110",
            "proof_scope": "first_callback_after_normal_CIRCLE_entry",
            "callback_preservation_and_global_entry_proof": "OPEN"}


def merge_coverage(paths, rows):
    if not paths:
        return {"state": "NOT_COLLECTED", "gate_proof": False}
    union = bytearray(len(rows))
    pins = []
    for path in paths:
        data = Path(path).read_bytes()
        require(len(data) == len(rows),
                f"{path}: expected {len(rows)} coverage bytes, got {len(data)}")
        bad = next((v for v in data if v not in (0, 1)), None)
        require(bad is None, f"{path}: nonboolean coverage byte {bad}")
        for i, value in enumerate(data):
            union[i] |= value
        pins.append(pin(path))
    missing = []
    for hit, row in zip(union, rows):
        if hit:
            continue
        module = row["source"].split(":", 1)[0] or "(unbound)"
        role = ("operator_diagnostic_candidate" if module in OPERATOR_MODULES else
                "platform_utility_candidate" if module in PLATFORM_MODULES else
                "unbound" if module == "(unbound)" else "gameplay_candidate")
        missing.append({**row, "candidate_role": role,
                        "dead_or_unreachable_proven": False})
    return {"state": "OBSERVATION_ONLY", "gate_proof": False,
            "scenario_terminal_and_oracle_identity_verified": False,
            "inputs": pins, "instructions": len(rows), "hits": sum(union),
            "unhit": len(missing),
            "unhit_by_module": dict(Counter(
                x["source"].split(":", 1)[0] or "(unbound)" for x in missing)),
            "unhit_by_candidate_role": dict(Counter(
                x["candidate_role"] for x in missing)),
            "unhit_instructions": missing}


def parse_index(text):
    # The retained emitter format leaves instruction commas unquoted. Parse its
    # fixed trailing columns first; DictReader silently misattributes operands.
    lines = text.splitlines()
    require(bool(lines) and lines[0] == "l8_address,instruction,source,chunk,status",
            "unexpected translation-index header")
    rows = []
    for number, line in enumerate(lines[1:], 2):
        tail = line.rsplit(",", 3)
        require(len(tail) == 4 and "," in tail[0], f"malformed index row {number}")
        address, instruction = tail[0].split(",", 1)
        require(bool(re.fullmatch(r"[0-9a-f]{8}", address)) and bool(instruction),
                f"invalid address/instruction in index row {number}")
        rows.append(dict(zip(("address", "instruction", "source", "chunk", "status"),
                             (address, instruction, *tail[1:]))))
    require(bool(rows), "empty translation index")
    return rows


def load_current(root):
    generated_root = Path(os.environ.get("STV_PLAYER_INPUTS", root / "generated/private")).resolve()
    map_path = generated_root / "l8_codemap.json"
    index_path = generated_root / "l8/l8_translation_index.csv"
    code_map = json.loads(map_path.read_text(encoding="utf-8"))
    rows = parse_index(index_path.read_text(encoding="utf-8"))
    addresses = [int(x["address"], 16) for x in rows]
    require(addresses == sorted(set(addresses)), "index duplicates or order changed")
    require(all(x["status"] == "translated" for x in rows), "blocked index entries")
    require(set(addresses) == {int(x, 16) for x in code_map["instructions"]},
            "index/map instruction sets disagree")
    require(not code_map["conflicts"], "code-map conflicts remain")
    compiled, labels, owners = set(), set(), {}
    generated = sorted((generated_root / "l8").glob("l8_chunk_*.cpp"))
    require(bool(generated), "no generated main chunks")
    for path in generated:
        owner = None
        for line in path.read_text(encoding="utf-8").splitlines():
            match = re.match(r"u32 (l8c_[0-9a-f]+)\(Machine&", line)
            if match:
                owner = match.group(1)
            match = re.match(r"\s*case 0x([0-9a-f]{8})u: goto L_([0-9a-f]{8});", line)
            if match:
                pc = int(match.group(1), 16)
                require(match.group(1) == match.group(2), "generated case/label mismatch")
                require(pc not in compiled and owner, "duplicate/unowned compiled entry")
                compiled.add(pc)
                owners[pc] = owner
            match = re.match(r"L_([0-9a-f]{8}):", line)
            if match:
                pc = int(match.group(1), 16)
                require(pc not in labels, "duplicate generated instruction label")
                labels.add(pc)
    require(compiled == labels == set(addresses), "compiled/index instruction sets disagree")
    require(all(owners[int(row["address"], 16)] == row["chunk"] for row in rows),
            "compiled/index chunk ownership differs")
    if os.environ.get("STV_ROM_ZIP"):
        compare.ZIP = os.environ["STV_ROM_ZIP"]
    rom = compare.retail("l8")
    words = list(struct.unpack("<" + "H" * (len(rom) // 2), rom))
    dec, instructions = Decoder(), {}
    for pc in addresses:
        require(pc % 16 == 0 and pc >= compare.BASE, "invalid instruction address")
        word_index = (pc - compare.BASE) // 16
        require(word_index < len(words), "instruction outside ROM")
        ins = dec.decode(words, word_index, pc)
        metadata = code_map["instructions"][f"{pc:08x}"]
        require(ins is not None and [ins.words, ins.op] == metadata[:2],
                f"{pc:08x}: metadata differs from actual ROM instruction")
        instructions[pc] = ins
    unknown = unresolved_transfers(instructions)
    require(unknown == [int(x, 16) for x in code_map["indirect_sites"]],
            "conservative transfer metadata is stale")
    pins = [pin(map_path), pin(index_path)] + [pin(p) for p in generated]
    return code_map, rows, compiled, rom, instructions, pins


def analyze(root, coverage):
    code_map, rows, compiled, rom, instructions, pins = load_current(root)
    ordered = sorted(instructions)
    previous = {pc: instructions[ordered[i - 1]] if i else None
                for i, pc in enumerate(ordered)}
    old_path = root / "evidence/c2-indirect-preparation-2026-10-01.json"
    old = json.loads(old_path.read_text(encoding="utf-8"))
    old_contracts = {int(x["address"], 16): x for x in old["sites"]}
    unknown = {int(x, 16) for x in code_map["indirect_sites"]}
    conditional = {}
    for site, source_line, digest in BUDGET_CONTRACTS:
        pos = ordered.index(site)
        seq = [instructions[x] for x in ordered[pos - 4:pos + 1]]
        for i, ins in enumerate(seq):
            annotation = code_map["instructions"][f"{ins.pc:08x}"][2:]
            require(annotation == [f"HULKS.ASM:{source_line + i}"],
                    "budget source annotation changed")
        conditional[site] = masked_table_domain(seq, rom, compiled, digest)
    collision_seq = [instructions[p] for p in ordered if 0xffe55a00 <= p <= 0xffe55b40]
    conditional[0xffe55b40] = collision_table_domain(
        collision_seq, rom, compiled, COLLISION_TABLE_HASH)
    circle = circle_domain(instructions, compiled)
    sites = []
    for pc, ins in sorted(instructions.items()):
        if ins.mnem not in TRANSFER_FORMS:
            continue
        pair = literal_pair(previous[pc], ins, compiled)
        if pc not in unknown:
            require(pair is not None, "resolved transfer lacks adjacent literal proof")
        historical = old_contracts.get(pc, {})
        classification = ("literal_call_producer_path" if pair else
                          "bounded_table_producer_path" if pc in conditional else
                          "branch_selected_callback" if pc == 0xfff50100 else
                          "MMFM_restored_process_return" if pc in MASK_RETURNS else
                          "weak_seed_data_candidate" if pc in WEAK_DATA else
                          historical.get("classification", "unclassified"))
        require(classification != "unclassified", f"{pc:08x}: unclassified transfer")
        row = {"address": f"{pc:08x}", "form": ins.mnem,
               "register": ins.ops[0].reg,
               "source": code_map["instructions"][f"{pc:08x}"][2:],
               "classification": classification,
               "map_unknown": pc in unknown, "compiled_entry_present": True,
               "global_target_closure": "OPEN"}
        if historical:
            row["historical_source_contract"] = {
                k: historical[k] for k in ("classification", "source", "contract")
                if k in historical
            }
            row["historical_contract_reachability_reproved"] = False
        if pair:
            row["local_literal_domain"] = pair
        if pc in conditional:
            row["local_table_domain"] = conditional[pc]
            row["local_preconditions"] = [
                "execution enters the checked guard sequence",
                "field 1 remains 32 bits at the table load",
                "table bytes equal the checked immutable reference",
                "no unproved alternate entry or interrupt/lifetime interference",
            ]
        if pc == 0xfff50100:
            row["local_callback_domain"] = circle
        sites.append(row)
    result = {
        "schema": "smash.c2.conservative-analysis.v1",
        "result": "STATIC_CONTRACTS_VERIFIED_GLOBAL_GATES_OPEN",
        "gate_proof": False,
        "base_commit": subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
        "scope": "typed_ROM_and_generated_entry_audit_no_runtime_changes",
        "inputs": pins + [pin(old_path), pin(__file__)],
        "main_program_sha256": sha(rom),
        "summary": {
            "instructions": len(rows), "register_transfers": len(sites),
            "conservative_unknown_sites": len(unknown),
            "adjacent_literal_producer_paths": sum("local_literal_domain" in x for x in sites),
            "verified_bounded_table_producer_paths": len(conditional),
            "conditional_circle_callback_targets": len(circle["targets"]),
            "classification_counts": dict(Counter(x["classification"] for x in sites)),
            "global_target_closure": "OPEN",
            "instructions_proved_dead": 0,
        },
        "transfers": sites,
        "coverage": merge_coverage(coverage, rows),
        "limitations": [
            "An exact compiled entry and observed execution are not global legal-target proof.",
            "Local bounds require their stated producer/field-width/entry preconditions.",
            "Return-stack aliases, process lifetime and callback preservation remain open.",
            "Weak-seeded data candidates are not declared globally unreachable.",
            "Coverage role labels prioritize review; they never establish source ownership or dead code.",
            "No MAME reference or game-completion gate is certified by this tool.",
        ],
    }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--coverage", type=Path, action="append", default=[])
    args = parser.parse_args()
    try:
        output = args.out.resolve()
        require(output.is_relative_to((ROOT / "build").resolve()),
                "analysis output must be inside build/")
        require(not output.exists(), "refusing to overwrite existing analysis evidence")
        result = analyze(ROOT, args.coverage)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"result": result["result"], **result["summary"],
                          "coverage_state": result["coverage"]["state"],
                          "output": str(output)}, indent=2))
        return 3
    except (ContractError, OSError, KeyError, ValueError) as exc:
        print(f"C2 analysis rejected: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
