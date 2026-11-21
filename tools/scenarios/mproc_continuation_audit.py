#!/usr/bin/env python3
"""Source-bound MPROC layout and typed continuation census for retail L8.

This is a bounded source/ROM audit, not an exhaustive indirect-target proof.
It checks MPROC's save/restore contract against exact P1 bytes/source rows,
enumerates accepted retail producer/return sites, and reports open ownership,
alias, alternate-entry and stack-lifetime obligations. It never changes runtime,
translators, generated code, original inputs or existing evidence.

Exit 3: local contracts verified, global closure remains OPEN.
Exit 1: invalid input/contract. Output must be a new file below build/.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/gsp2cpp/analysis"))
from controlflow_audit import (ContractError, contiguous, expect, load_current,
                              pin, require, sha, shape)

BASE = 0xffe00000
RETAIL_SHA = "28f150ed8fd38f0eab76494ac443eae7ce84c61091d7334dfc7a2fa11a297641"
ENTRIES = {"GETPRC": 0xffe0aba0, "XFERPROC": 0xffe0ac70, "PUTA7": 0xffe0b4b0}
LAYOUT = {"PLINK": 0, "PROCID": 0x20, "PTIME": 0x30, "PSPTR": 0x40,
          "PA11": 0x60, "PA10": 0x80, "PA9": 0xa0, "PA8": 0xc0,
          "PWAKE": 0xe0, "PDATA": 0x100, "PSDATA": 0x2a0,
          "PRCSIZ": 0x5a0, "NPROC": 240}
CORE_WINDOWS = (
    ("PRCSLP", 0xffe0a490, 0xffe0a500),
    ("PRCD2", 0xffe0a580, 0xffe0a5e0),
    ("GETPRC_allocate", 0xffe0aba0, 0xffe0ac70),
    ("XFERPROC_guard", 0xffe0ac70, 0xffe0acd0),
    ("initialize_header", 0xffe0ad90, 0xffe0ae70),
    ("EXISTP", 0xffe0b100, 0xffe0b240),
    ("GETA9", 0xffe0b2a0, 0xffe0b2d0),
    ("GETA8", 0xffe0b330, 0xffe0b360),
    ("PUTA7", 0xffe0b4b0, 0xffe0b4e0),
)
# Reviewed literal-to-wake paths, including two retail paths with no annotation.
WAKE_PATHS = {
    0xffe1da60: 0xffe1d9b0, 0xffe3f3e0: 0xffe3f350,
    0xffec2570: 0xffec24c0, 0xffec74d0: 0xffec74a0,
    0xffed3850: 0xffed3820, 0xffed3990: 0xffed3960,
    0xffed4fe0: 0xffed4fb0, 0xfff17e70: 0xfff17e40,
    0xfff2b9e0: 0xfff2b9a0, 0xfff30f80: 0xfff30ef0,
    0xfff45040: 0xfff45000,
}
GETPRC_LITERAL_PATHS = {0xffeaf450: 0xffeaf410}
ADDITIONAL_SOURCE_WINDOWS = (
    ("hidden_process", "AUDIT.ASM", [234, 235, 236], 0xffeaf410, 0xffeaf480),
    ("switch_process", "AUDIT.ASM", list(range(253, 264)), 0xffeaf5e0, 0xffeaf6d0),
    ("menu_workers", "TEST.ASM", [785, 786, 789, 791, 792], 0xffebb2b0, 0xffebb370),
)
SWITCH_TABLE_SHA = "046aed668f82ce1811a662bb00944abc4e9800c96756b86a8fe0b6d6e06e4c18"
R = lambda name: ("R", name, None)
K = lambda value: ("K", None, value)
I = lambda value: ("IMM", None, value)
M = lambda kind, reg, value=None: (kind, reg, value)


def window(instructions, start, end):
    """End-exclusive, with no omitted instruction/gap accepted."""
    require(start < end, "empty/reversed window")
    result, pc = [], start
    while pc < end:
        require(pc in instructions, f"missing instruction at {pc:08x}")
        ins = instructions[pc]
        require(ins.words > 0, "zero-length instruction")
        result.append(ins)
        pc += ins.words * 16
    require(pc == end, "window ends inside an instruction")
    contiguous(result)
    return result


def normalized(text):
    return re.sub(r"\s+", "", text.split(";", 1)[0]).upper()


def source_layout(text):
    found = {}
    for line in text.splitlines():
        match = re.match(r"\s*(\w+)\s+\.SET\s+([0-9A-F]+H?|>[0-9A-F]+)\b",
                         line.upper())
        if not match or match[1] not in LAYOUT:
            continue
        token = match[2]
        value = (int(token[1:], 16) if token.startswith(">") else
                 int(token[:-1], 16) if token.endswith("H") else int(token))
        require(match[1] not in found, "duplicate process layout equate")
        found[match[1]] = value
    require(found == LAYOUT, f"MPROC source layout changed: {found!r}")
    return found


def mask_slots(mnemonic, mask, pointer):
    """TMS34010 MMTM/MMFM bit ordering, in bit-addressed process storage."""
    require(mnemonic in {"MMTM", "MMFM"}, "invalid multi-register operation")
    require(0 < mask <= 0xffff, "empty/invalid register mask")
    slots = {}
    order = range(16) if mnemonic == "MMTM" else range(15, -1, -1)
    for reg in order:
        bit = (0x8000 >> reg) if mnemonic == "MMTM" else (1 << reg)
        if mask & bit:
            if mnemonic == "MMTM":
                pointer -= 32
            slots[f"A{reg}" if reg < 15 else "SP"] = pointer
            if mnemonic == "MMFM":
                pointer += 32
    return slots, pointer


def validate_layout(instructions, layout):
    require(layout == LAYOUT, "unexpected process structure")
    patterns = {
        0xffe0a490: ("MOVE", M("INC", "SP"), R("A7"), K(1)),
        0xffe0a4a0: ("MOVE", R("A13"), R("A1")),
        0xffe0a4b0: ("ADDI", I(layout["PDATA"]), R("A1")),
        0xffe0a4d0: ("MMTM", R("A1"), M("LIST", None, 0x01f8)),
        0xffe0a4f0: ("MOVE", R("A0"), M("DEC", "A1"), K(0)),
        0xffe0a580: ("MOVE", R("A13"), R("A1")),
        0xffe0a590: ("ADDI", I(layout["PSPTR"]), R("A1")),
        0xffe0a5b0: ("MMFM", R("A1"), M("LIST", None, 0x1f80)),
        0xffe0a5d0: ("JUMP", R("A7")),
        0xffe0aba0: ("MMTM", R("SP"), M("LIST", None, 0x2008)),
        0xffe0abc0: ("MOVE", M("ABS", None, 0x01000020), R("A0"), K(1)),
        0xffe0abf0: ("JREQ", M("REL", None, 0xffe0ae70)),
        0xffe0ac00: ("MOVE", M("IND", "A0"), R("A2"), K(1)),
        0xffe0ac10: ("MOVE", R("A2"), M("ABS", None, 0x01000020), K(1)),
        0xffe0ac40: ("MOVE", M("IND", "A13"), M("IND", "A0"), K(1)),
        0xffe0ac50: ("MOVE", R("A0"), M("IND", "A13"), K(1)),
        0xffe0ac60: ("JR", M("REL", None, 0xffe0ac90)),
        0xffe0ac70: ("MMTM", R("SP"), M("LIST", None, 0x2008)),
        0xffe0ac90: ("CMPI", I(BASE), R("A7")),
        0xffe0acc0: ("JRNC", M("REL", None, 0xffe0ad90)),
        0xffe0ad90: ("MOVE", R("A0"), R("A2")),
        0xffe0ada0: ("ADDI", I(layout["PDATA"]), R("A2")),
        0xffe0adc0: ("MOVE", R("A0"), R("A12")),
        0xffe0add0: ("ADDI", I(layout["PRCSIZ"]), R("A12")),
        0xffe0adf0: ("MMTM", R("A2"), M("LIST", None, 0x01f8)),
        0xffe0ae10: ("MOVK", K(1), R("A12")),
        0xffe0ae20: ("MOVE", R("A12"), M("DEC", "A2"), K(0)),
        0xffe0ae30: ("MOVE", R("A1"), M("DEC", "A2"), K(0)),
        0xffe0ae40: ("MMFM", R("SP"), M("LIST", None, 0x1004)),
        0xffe0ae60: ("RETS",),
        0xffe0b2a0: ("MOVE", M("DISP", "A0", layout["PA9"]), R("A9"), K(1)),
        0xffe0b2c0: ("RETS",),
        0xffe0b330: ("MOVE", M("DISP", "A0", layout["PA8"]), R("A8"), K(1)),
        0xffe0b350: ("RETS",),
        0xffe0b4b0: ("MOVE", R("A7"), M("DISP", "A0", layout["PWAKE"]), K(1)),
        0xffe0b4d0: ("RETS",),
    }
    for pc, pattern in patterns.items():
        require(pc in instructions, f"missing MPROC contract instruction {pc:08x}")
        expect(instructions[pc], *pattern)
    saved, after_save = mask_slots("MMTM", 0x01f8, layout["PDATA"])
    restored, after_restore = mask_slots("MMFM", 0x1f80, layout["PSPTR"])
    expected = {"A7": layout["PWAKE"], "A8": layout["PA8"],
                "A9": layout["PA9"], "A10": layout["PA10"],
                "A11": layout["PA11"], "A12": layout["PSPTR"]}
    require(saved == restored == expected, "saved/restored register slots disagree")
    require(after_save - 16 == layout["PTIME"], "sleep write misses PTIME")
    require(after_save - 32 == layout["PROCID"], "ID write misses PROCID")
    require(after_restore == layout["PDATA"], "restore ends at wrong address")
    require(max(saved.values()) + 32 == layout["PDATA"],
            "saved header overlaps process data")
    return {"register_slots_bits": saved, "sleep_field_bits": [48, 63],
            "id_field_bits": [32, 47], "initialized_header_bits": [32, 255],
            "process_data_cleared_by_GETPRC": False,
            "stack_top_bits": layout["PRCSIZ"],
            "source_stack_reservation_bits": [layout["PSDATA"], layout["PRCSIZ"]],
            "runtime_stack_bounds_proven": False,
            "wake_ROM_lower_bound_is_compiled_entry_check": False,
            "preconditions": ["field0 is 16 bits", "field1 is 32 bits",
                              "valid disjoint process object and hardware stack",
                              "normal entry and normal helper return"]}


def bind_exact_core(seq, rom, p1, listing, source_lines):
    """Bind an exact retail window to P1 rows and actual archived source text."""
    contiguous(seq)
    start, end = seq[0].pc, seq[-1].pc + seq[-1].words * 16
    lo, hi = (start - BASE) // 8, (end - BASE) // 8
    require(0 <= lo < hi <= len(rom) and hi <= len(p1), "binding outside image")
    require(rom[lo:hi] == p1[lo:hi], "MPROC P1/retail bytes differ")
    origins = []
    for ins in seq:
        records = listing.get(ins.pc, [])
        require(len(records) == 1, f"{ins.pc:08x}: absent/ambiguous P1 source row")
        record = records[0]
        require(record[2] == "MPROC.ASM", "MPROC binding has wrong source module")
        line = record[3]
        require(1 <= line <= len(source_lines), "P1 source line outside file")
        require(normalized(record[4]) == normalized(source_lines[line - 1]),
                f"MPROC.ASM:{line}: P1/source statement differs")
        offset = (ins.pc - BASE) // 8
        require(int.from_bytes(rom[offset:offset + 2], "little") == ins.op,
                "decoded opcode differs from actual retail bytes")
        require(int(record[1], 16) == ins.op, "P1 opcode differs from retail")
        origins.append({"pc": f"{ins.pc:08x}", "source": f"MPROC.ASM:{line}",
                        "typed": shape(ins)})
    return {"start": f"{start:08x}", "end_exclusive": f"{end:08x}",
            "bytes_sha256": sha(rom[lo:hi]), "p1_retail_bytes_identical": True,
            "source_statements_verified": True, "instructions": origins}


def bind_relocated_source(seq, rom, p1, relmask, records, module, lines, source):
    """Match exact source rows and all nonrelocated P1 bytes to retail."""
    contiguous(seq)
    require(len(seq) == len(lines), "source/window instruction count differs")
    require(len(relmask) == len(p1), "P1 relocation mask has wrong length")
    offsets, origins = set(), []
    for ins, line in zip(seq, lines):
        candidates = [r for r in records if r[2] == module and r[3] == line]
        require(len(candidates) == 1, "missing/ambiguous relocated source row")
        record = candidates[0]
        require(1 <= line <= len(source), "relocated source line outside file")
        require(normalized(record[4]) == normalized(source[line - 1]),
                f"{module}:{line}: listing/source statement differs")
        p1pc = int(record[0], 16)
        offsets.add(ins.pc - p1pc)
        lo, dst, size = (p1pc - BASE) // 8, (ins.pc - BASE) // 8, ins.words * 2
        require(0 <= lo and lo + size <= len(p1) and
                0 <= dst and dst + size <= len(rom), "source binding outside image")
        left, right, mask = p1[lo:lo + size], rom[dst:dst + size], relmask[lo:lo + size]
        require(int.from_bytes(left[:2], "little") == ins.op ==
                int.from_bytes(right[:2], "little") == int(record[1], 16),
                "source/retail opcode differs")
        require(all(a == b or m for a, b, m in zip(left, right, mask)),
                "nonrelocated P1/source bytes differ from retail")
        origins.append({"pc": f"{ins.pc:08x}", "p1_pc": f"{p1pc:08x}",
                        "source": f"{module}:{line}", "typed": shape(ins),
                        "p1_bytes_sha256": sha(left), "retail_bytes_sha256": sha(right),
                        "relocation_mask_sha256": sha(mask),
                        "changed_relocated_byte_offsets": [
                            n for n, (a, b) in enumerate(zip(left, right)) if a != b]})
    require(len(offsets) == 1, "source window has inconsistent relocation delta")
    return {"source_statements_verified": True, "nonrelocated_bytes_identical": True,
            "source_to_retail_delta_bits": offsets.pop(), "instructions": origins,
            "relocations_or_annotations_prove_global_entry_set": False}


def verify_preserves_a7(seq):
    """Register-preservation lemma only; no memory/stack/termination claim."""
    contiguous(seq)
    pcs = {i.pc for i in seq}
    expect(seq[-1], "RETS")
    allowed = {"MMTM", "MMFM", "SEXT", "AND", "MOVI", "MOVE", "JREQ", "CMP", "JRNE"}
    for ins in seq[:-1]:
        require(ins.mnem in allowed, "helper has an unreviewed operation")
        if ins.mnem in {"MMTM", "MMFM"}:
            require(ins.ops[0].reg == "SP", "helper changes a non-SP register stack")
            slots, _ = mask_slots(ins.mnem, ins.ops[1].value, 0)
            require("A7" not in slots and "SP" not in slots,
                    "helper implicitly saves/restores A7 or SP")
        else:
            require(not any(o.reg in {"A7", "SP"} for o in ins.ops),
                    "helper touches A7 or SP")
        require(ins.mnem not in {"CALL", "CALLA", "CALLR", "EXGPC", "JUMP",
                                 "JA", "RETI", "POPST", "PUTST"},
                "helper has unchecked call/indirect/interrupt exit")
        for op in ins.ops:
            if op.kind == "REL":
                require(op.value in pcs, "helper branch leaves checked body")
    return {"a7_preserved_on_normal_return": True,
            "stack_memory_integrity_and_termination": "OPEN"}


def process_call(seq, compiled):
    """The JSRP/JSRPR continuation arithmetic, not the caller/owner proof."""
    require(len(seq) == 4, "process call requires four instructions")
    contiguous(seq)
    expect(seq[0], "GETPC", R("A7"))
    require(seq[1].mnem == "ADDI" and len(seq[1].ops) == 2 and
            seq[1].ops[0].kind == "IMM" and seq[1].ops[1].reg == "A7",
            "process call lacks A7 continuation adjustment")
    expect(seq[2], "MOVE", R("A7"), M("DEC", "A12"), K(1))
    continuation = seq[0].pc + seq[0].words * 16 + seq[1].ops[0].value
    end = seq[-1].pc + seq[-1].words * 16
    require(continuation == end, "process continuation is not after the transfer")
    require(continuation in compiled, "process continuation is not compiled")
    transfer = seq[-1]
    require(transfer.mnem in {"JA", "JUMP"}, "unexpected process call transfer")
    require(len(transfer.ops) == 1, "invalid process call operands")
    target = transfer.ops[0]
    if transfer.mnem == "JA":
        require(target.kind == "IMM" and target.value in compiled,
                "process callee is not a compiled entry")
        destination = f"{target.value:08x}"
    else:
        require(target.kind == "R" and target.reg != "A7",
                "register call clobbers its own target with GETPC")
        destination = {"register": target.reg, "target_domain": "OPEN"}
    return {"producer": f"{seq[0].pc:08x}", "transfer": f"{transfer.pc:08x}",
            "continuation": f"{continuation:08x}", "destination": destination,
            "push_size_bits": 32, "field1_32bit_required": True,
            "owner_stack_and_alternate_entry_proof": "OPEN"}


def process_return(previous, transfer):
    require(previous.pc + previous.words * 16 == transfer.pc,
            "return restore is not adjacent")
    expect(previous, "MMFM", R("A12"), M("LIST", None, 0x80))
    expect(transfer, "EXGPC", R("A7"))
    return {"restore": f"{previous.pc:08x}", "transfer": f"{transfer.pc:08x}",
            "pop_size_bits": 32, "owner_and_matching_push_proof": "OPEN"}


def literal_wake_path(seq, compiled, helpers):
    """Normal path from a literal A7 producer to a known MPROC writer."""
    require(len(seq) >= 2, "empty literal-to-wake path")
    contiguous(seq)
    first, call = seq[0], seq[-1]
    require(first.mnem == "MOVI" and len(first.ops) == 2 and
            first.ops[0].kind == "IMM" and first.ops[1].reg == "A7",
            "wake path lacks literal A7 producer")
    target = first.ops[0].value
    require(target in compiled, "wake producer targets a noncompiled entry")
    require(call.mnem in {"CALLA", "CALLR"} and len(call.ops) == 1 and
            call.ops[0].value in ENTRIES.values(), "wake path ends at wrong writer")
    continuation = call.pc + call.words * 16
    require(continuation in compiled, "wake writer return is not compiled")
    conditional_exits, called = [], []
    for ins in seq[1:-1]:
        if ins.mnem in {"CALLA", "CALLR"}:
            require(len(ins.ops) == 1 and ins.ops[0].value in helpers,
                    "wake path has unchecked helper call")
            called.append(f"{ins.ops[0].value:08x}")
            continue
        if ins.mnem == "JREQ":
            require(len(ins.ops) == 1 and ins.ops[0].kind == "REL" and
                    ins.ops[0].value >= continuation and
                    ins.ops[0].value in compiled,
                    "wake path branch reenters or escapes to uncompiled code")
            conditional_exits.append(f"{ins.ops[0].value:08x}")
            continue
        require(ins.mnem in {"CLR", "NOT", "MOVI", "MOVE"},
                "unreviewed operation in literal-to-wake path")
        require(not any(o.reg in {"A7", "SP"} for o in ins.ops),
                "wake path touches A7 or the hardware stack")
        require(ins.ops[-1].kind == "R" or
                (ins.mnem == "MOVE" and ins.ops[-1].kind == "K" and
                 len(ins.ops) >= 2 and ins.ops[-2].kind == "R"),
                "wake path has an unchecked memory write")
    return {"producer": f"{first.pc:08x}", "writer_call": f"{call.pc:08x}",
            "wake_entry": f"{target:08x}", "compiled": True,
            "helper_calls": called, "conditional_exits": conditional_exits,
            "proof_scope": "normal path beginning at the literal producer",
            "preconditions": ["normal helper returns", "valid hardware stack",
                              "field0 is 16 bits and field1 is 32 bits"],
            "process_owner_alias_lifetime_and_alternate_entry_proof": "OPEN"}


def direction_domain(seq):
    """Overapproximate both arms of the acyclic GETLZDIR register-only CFG."""
    contiguous(seq)
    by_pc = {i.pc: i for i in seq}
    pending = [(seq[0].pc, None)]
    seen, returned = set(), set()
    allowed = {"MOVE", "SUB", "ABS", "CLR", "CMP", "JRHI", "JRNN", "MOVK", "JR", "JRN", "RETS"}
    while pending:
        pc, value = pending.pop()
        if (pc, value) in seen: continue
        seen.add((pc, value))
        require(pc in by_pc, "direction path escapes checked body")
        item = by_pc[pc]
        require(item.mnem in allowed, "unreviewed direction operation")
        if item.mnem == "RETS":
            expect(item, "RETS")
            require(value in {0, 1, 2, 3}, "direction return is not bounded")
            returned.add(value)
            continue
        if item.mnem.startswith("JR"):
            require(len(item.ops) == 1 and item.ops[0].kind == "REL" and
                    item.ops[0].value in by_pc and item.ops[0].value > pc,
                    "direction branch is not internal/acyclic")
            pending.append((item.ops[0].value, value))
            if item.mnem == "JR": continue
        else:
            destination = item.ops[-2] if item.mnem == "MOVE" and item.ops[-1].kind == "K" else item.ops[-1]
            require(destination.kind == "R", "direction body has a memory destination")
            if item.mnem != "CMP" and destination.reg == "A0":
                if item.mnem == "CLR": value = 0
                elif item.mnem == "MOVK":
                    require(item.ops[0].kind == "K", "direction constant is not immediate")
                    value = item.ops[0].value
                elif item.mnem == "MOVE": value = None
                else: raise ContractError("unchecked direction accumulator operation")
        pending.append((pc + item.words * 16, value))
    require(returned == {0, 1, 2, 3}, "direction return census changed")
    return sorted(returned)


def laser_table_precondition(instructions, rom, compiled):
    directions = direction_domain(window(instructions, 0xfff16420, 0xfff165c0))
    seq = window(instructions, 0xfff15d40, 0xfff15df0)
    require(len(seq) == 8 and seq[2].mnem == "MOVI" and seq[2].ops[0].kind == "IMM",
            "laser table setup extent changed")
    base = seq[2].ops[0].value
    for item, pattern in zip(seq, (
        ("CALLR", M("REL", None, 0xfff16420)), ("SLL", K(5), R("A0")),
        ("MOVI", I(base), R("A10")),
        ("ADD", R("A0"), R("A10")), ("ADD", R("A0"), R("A10")), ("ADD", R("A0"), R("A10")),
        ("MOVE", M("INC", "A10"), R("A0"), K(1)),
        ("MOVE", M("INC", "A10"), R("A9"), K(1)),
    )): expect(item, *pattern)
    offset = (base - BASE) // 8
    require(base % 32 == 0 and offset >= 0 and offset + 48 <= len(rom), "laser table lies outside ROM")
    raw = rom[offset:offset + 48]
    targets = [int.from_bytes(raw[i*12+8:i*12+12], "little") for i in directions]
    require(set(targets) <= compiled, "laser table contains a noncompiled child entry")
    return {"directions": directions, "table_address": f"{base:08x}", "table_sha256": sha(raw),
            "a10_before_animation": [f"{base+i*96+64:08x}" for i in directions],
            "conditional_child_entries": [f"{p:08x}" for p in targets],
            "proof_scope": "normal GETLZDIR return and typed table setup, before FRANIM/PSTOP",
            "preconditions": ["field1 is 32 bits", "normal hardware-stack return", "read-only pinned ROM"],
            "a10_preservation_through_animation_sleep_stop_and_owner_lifetime": "OPEN"}


def fork_writer_domain(instructions, compiled):
    """Conditional normal-entry domain through the five direct P_FORK callers."""
    entry = 0xffea5830
    seq = window(instructions, entry, 0xffea58d0)
    require(len(seq) == 5, "P_FORK wrapper extent changed")
    for item, pattern in zip(seq, (
        ("MMTM", R("SP"), M("LIST", None, 0x4000)),
        ("MOVE", M("DISP", "A13", 0x20), R("A1"), K(0)),
        ("CALLA", I(ENTRIES["GETPRC"])),
        ("MMFM", R("SP"), M("LIST", None, 2)),
        ("RETS",),
    )):
        expect(item, *pattern)
    callers = sorted(p for p, ins in instructions.items()
                     if ins.mnem in {"CALLA", "CALLR"} and len(ins.ops) == 1
                     and ins.ops[0].value == entry)
    require(callers == [0xffeac390, 0xffeadb00, 0xffeadea0, 0xffeb6360, 0xffebea70],
            "P_FORK direct caller census changed")
    paths, targets = [], set()
    for pc in callers:
        producer = instructions.get(pc - 48)
        require(producer is not None and producer.words == 3 and
                producer.mnem == "MOVI" and len(producer.ops) == 2 and
                producer.ops[0].kind == "IMM" and producer.ops[1].reg == "A7",
                "P_FORK caller lacks adjacent literal A7 producer")
        target = producer.ops[0].value
        require(target in compiled, "P_FORK child entry is not compiled")
        require(pc + instructions[pc].words * 16 in compiled, "P_FORK return is not compiled")
        targets.add(target)
        paths.append({"producer": f"{producer.pc:08x}", "call": f"{pc:08x}",
                      "wake_entry": f"{target:08x}"})
    return {"wake_entries": [f"{p:08x}" for p in sorted(targets)], "direct_paths": paths,
            "proof_scope": "normal entry at each checked literal through P_FORK to GETPRC",
            "preconditions": ["valid hardware stack and normal call returns", "field0 is 16 bits",
                              "interrupts preserve A7 and the live caller context"],
            "indirect_callers_alternate_entries_and_owner_lifetime": "OPEN"}


def menu_worker_domain(seq, compiled):
    require(len(seq) == 5, "menu worker domain requires both literal branches")
    contiguous(seq)
    for producer in (seq[0], seq[2]):
        require(producer.mnem == "MOVI" and len(producer.ops) == 2 and
                producer.ops[0].kind == "IMM" and producer.ops[1].reg == "A7",
                "menu worker lacks literal producer")
        require(producer.ops[0].value in compiled, "menu worker is not compiled")
    expect(seq[1], "JR", M("REL", None, seq[3].pc))
    expect(seq[3], "MOVI", I(0xac1), R("A1"))
    expect(seq[4], "CALLA", I(ENTRIES["GETPRC"]))
    require(seq[4].pc + seq[4].words * 16 in compiled, "menu continuation not compiled")
    return {"wake_entries": [f"{seq[n].ops[0].value:08x}" for n in (0, 2)],
            "normal_entries": [f"{seq[n].pc:08x}" for n in (0, 2)],
            "proof_scope": "normal B_MENU or GET_MENU entry through the checked join",
            "process_owner_and_alternate_entry_proof": "OPEN"}


def switch_worker_domain(seq, rom, compiled, expected_hash=None):
    require(len(seq) == 11, "switch domain requires complete mask-to-writer path")
    contiguous(seq)
    require(seq[6].mnem == "ADDI" and seq[6].ops[0].kind == "IMM",
            "switch table address is not literal")
    base = seq[6].ops[0].value
    patterns = (
        ("SLL", K(27), R("A0")), ("SRL", K(27), R("A0")),
        ("MOVE", R("A0"), R("A2")), ("ADD", R("A0"), R("A2")),
        ("ADD", R("A0"), R("A2")), ("SLL", K(4), R("A2")),
        ("ADDI", I(base), R("A2")),
        ("MOVE", M("INC", "A2"), R("A1"), K(0)),
        ("MOVE", M("INC", "A2"), R("A7"), K(1)),
        ("JREQ", M("REL", None, 0xffeaf4f0)),
        ("CALLA", I(ENTRIES["GETPRC"])),
    )
    for item, pattern in zip(seq, patterns):
        expect(item, *pattern)
    require(seq[10].pc + seq[10].words * 16 in compiled and 0xffeaf4f0 in compiled,
            "switch exit/continuation is not compiled")
    require(base % 16 == 0 and BASE <= base and base + 32 * 48 <= (1 << 32),
            "switch table is unaligned or can wrap")
    offset = (base - BASE) // 8
    require(offset + 32 * 6 <= len(rom), "switch records outside ROM")
    raw = rom[offset:offset + 32 * 6]
    if expected_hash is not None:
        require(sha(raw) == expected_hash, "switch table bytes changed")
    records = [{"slot": n, "process_id": int.from_bytes(raw[n*6:n*6+2], "little"),
                "target": int.from_bytes(raw[n*6+2:n*6+6], "little")}
               for n in range(32)]
    targets = {r["target"] for r in records if r["target"]}
    require(targets <= compiled, "switch record has noncompiled nonzero target")
    for record in records:
        record["target"] = f"{record['target']:08x}"
    return {"wake_entries": [f"{p:08x}" for p in sorted(targets)],
            "record_count": 32, "record_stride_bits": 48, "index_domain": [0, 31],
            "table_address": f"{base:08x}", "table_sha256": sha(raw),
            "records": records, "null_records_skip_writer": True,
            "proof_scope": "checked unsigned five-bit mask through the table load",
            "preconditions": ["field0 is 16 bits", "field1 is 32 bits",
                              "read-only pinned ROM table", "entry through the mask"],
            "process_owner_and_alternate_entry_proof": "OPEN"}


def source_writer_census(paths, rows):
    """Source calls are hints to reconcile; they do not bound retail callers."""
    annotations = Counter(r["source"] for r in rows if r["source"])
    found = []
    for path in sorted(paths):
        # DOS archive comments include non-UTF-8 bytes. Latin-1 preserves every
        # byte while the instructions searched here are strictly ASCII.
        for number, text in enumerate(path.read_text(encoding="latin-1").split("\n"), 1):
            code = text.split(";", 1)[0].strip()
            if code.startswith("*"):
                continue
            match = re.search(r"\bCALL[AR]\s+(XFERPROC|PUTA7|PUTWAKE)\b", code, re.I)
            if match:
                origin = f"{path.name}:{number}"
                found.append({"source": origin, "writer": match[1].upper(),
                              "retail_annotation_rows": annotations[origin],
                              "annotation_is_source_binding": False})
    return found


def validate_output(root, path):
    result = path.resolve()
    require(result.is_relative_to((root / "build").resolve()),
            "output must be below this project's build directory")
    require(not result.exists(), "refusing to overwrite existing evidence")
    return result


def analyze(root, checkpoint):
    checkpoint_data = json.loads(checkpoint.read_text(encoding="utf-8"))
    require(checkpoint_data.get("result") == "PASS_CHECKPOINT",
            "checkpoint is not PASS_CHECKPOINT")
    source_root = root / "original/archive/smashtv-main"
    sources = sorted(source_root.glob("*.ASM"))
    extra = [checkpoint, source_root / "MPROC.EQU",
             root / "build/p1/listing.json", root / "build/p1/robo.bin",
             root / "build/p1/robo.bin.relmask", root / "build/p1/robo.map.json",
             root / "original/user-supplied-roms/smashtv.zip",
             Path(__file__), root / "tests/test_mproc_continuation_audit.py"]
    extra += [root / "tools/gsp2cpp" / name for name in
              ("decode.py", "isa_table.json", "codemap.py", "compare.py", "emit.py")]
    extra += [root / "tools/gsp2cpp/analysis/controlflow_audit.py"]
    extra += [root / "tools/scenarios/laser_animation_contract.py",
              root / "tests/test_laser_animation_contract.py"]
    inputs = [pin(p) for p in sorted(set(sources + extra))]
    code_map, rows, compiled, rom, instructions, generated_pins = load_current(root)
    inputs += generated_pins
    require(sha(rom) == RETAIL_SHA, "retail L8 program identity changed")
    require(len(code_map["indirect_sites"]) == 147, "global obligation inventory changed")
    layout = source_layout((source_root / "MPROC.EQU").read_text(encoding="utf-8"))
    contract = validate_layout(instructions, layout)
    p1 = (root / "build/p1/robo.bin").read_bytes()
    p1_listing = json.loads((root / "build/p1/listing.json").read_text())["instructions"]
    listing = {}
    for row in p1_listing:
        listing.setdefault(int(row[0], 16), []).append(row)
    source_lines = (source_root / "MPROC.ASM").read_text(encoding="utf-8").splitlines()
    bindings, helper_contracts = [], {}
    for name, start, end in CORE_WINDOWS:
        seq = window(instructions, start, end)
        binding = bind_exact_core(seq, rom, p1, listing, source_lines)
        binding["name"] = name
        bindings.append(binding)
        if name in {"EXISTP", "GETA8", "GETA9"}:
            helper_contracts[start] = verify_preserves_a7(seq)
    relmask = (root / "build/p1/robo.bin.relmask").read_bytes()
    additional_bindings = []
    for name, module, lines, start, end in ADDITIONAL_SOURCE_WINDOWS:
        seq = window(instructions, start, end)
        source = (source_root / module).read_text(encoding="latin-1").split("\n")
        binding = bind_relocated_source(seq, rom, p1, relmask, p1_listing,
                                        module, lines, source)
        binding["name"] = name
        additional_bindings.append(binding)
    pc_order = sorted(instructions)
    previous = {pc: instructions[pc_order[n - 1]] if n else None
                for n, pc in enumerate(pc_order)}
    source_hints = {int(r["address"], 16): r["source"] for r in rows}
    returns, calls, writers = [], [], []
    for pc in pc_order:
        ins = instructions[pc]
        if shape(ins) == ("EXGPC", (R("A7"),)):
            require(previous[pc] is not None, "return lacks preceding instruction")
            result = process_return(previous[pc], ins)
            result["source_annotation_hint"] = source_hints[pc]
            returns.append(result)
        if shape(ins) == ("GETPC", (R("A7"),)):
            seq, next_pc = [], pc
            for _ in range(4):
                require(next_pc in instructions, "process call sequence is incomplete")
                item = instructions[next_pc]
                seq.append(item)
                next_pc += item.words * 16
            result = process_call(seq, compiled)
            result["source_annotation_hint"] = source_hints[pc]
            calls.append(result)
        if (ins.mnem in {"CALLA", "CALLR"} and len(ins.ops) == 1 and
                ins.ops[0].value in ENTRIES.values()):
            name = next(k for k, v in ENTRIES.items() if v == ins.ops[0].value)
            result = {"address": f"{pc:08x}", "writer": name,
                      "source_annotation_hint": source_hints[pc],
                      "source_binding": "UNPROVEN", "global_target_closure": "OPEN"}
            if name != "GETPRC":
                require(pc in WAKE_PATHS, "new wake-writer path requires review")
                seq = window(instructions, WAKE_PATHS[pc], pc + ins.words * 16)
                result["local_producer"] = literal_wake_path(seq, compiled, helper_contracts)
            elif pc in GETPRC_LITERAL_PATHS:
                seq = window(instructions, GETPRC_LITERAL_PATHS[pc], pc + ins.words * 16)
                result["local_producer"] = literal_wake_path(seq, compiled, helper_contracts)
            elif pc == 0xffeaf6a0:
                result["local_producer"] = switch_worker_domain(
                    window(instructions, 0xffeaf5e0, 0xffeaf6d0), rom, compiled,
                    SWITCH_TABLE_SHA)
            elif pc == 0xffebb340:
                result["local_producer"] = menu_worker_domain(
                    window(instructions, 0xffebb2b0, 0xffebb370), compiled)
            elif (previous[pc] is not None and previous[pc].mnem == "MOVI" and
                  len(previous[pc].ops) == 2 and previous[pc].ops[1].reg == "A7"):
                result["local_producer"] = literal_wake_path(
                    [previous[pc], ins], compiled, helper_contracts)
            elif pc == 0xffea5870:
                result["local_producer"] = fork_writer_domain(instructions, compiled)
            elif pc == 0xfff15f20:
                # Same explicit normal-entry/stack/owner premises as the local
                # contracts; this does not discharge any global obligation.
                from laser_animation_contract import writer_domain
                result["local_producer"] = writer_domain(instructions, rom, compiled)
            else:
                result["local_producer"] = {"status": "OPEN",
                    "missing_invariant": "unreviewed producer path"}
            writers.append(result)
    rewrites = {int(w["address"], 16) for w in writers if w["writer"] != "GETPRC"}
    require(rewrites == set(WAKE_PATHS), "reviewed wake-writer inventory differs")
    source_calls = source_writer_census(sources, rows)
    require(all(pin(Path(p["path"])) == p for p in inputs), "input changed during audit")
    literal_count = sum("wake_entry" in w["local_producer"] for w in writers)
    bounded_count = sum("wake_entries" in w["local_producer"] for w in writers)
    return {
        "schema": "mproc-continuation-audit/v1",
        "result": "LOCAL_MPROC_CONTRACTS_VERIFIED_GLOBAL_CLOSURE_OPEN",
        "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
        "gate_proof": False,
        "base_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "checkpoint_source_commit": checkpoint_data["source_commit"],
        "command": [sys.executable, *sys.argv],
        "scope": "accepted typed retail instructions and exact MPROC P1/source windows",
        "main_program_sha256": sha(rom), "inputs": inputs,
        "inputs_unchanged_after_audit": True,
        "summary": {"accepted_instructions": len(instructions),
                    "source_bound_core_windows": len(bindings),
                    "source_bound_additional_windows": len(additional_bindings),
                    "typed_process_call_producers": len(calls),
                    "typed_process_return_consumers": len(returns),
                    "direct_writer_calls": dict(Counter(w["writer"] for w in writers)),
                    "local_literal_writer_paths": literal_count,
                    "local_bounded_writer_domains": bounded_count,
                    "writer_producers_open": len(writers) - literal_count - bounded_count,
                    "global_indirect_sites_open": len(code_map["indirect_sites"]),
                    "global_sites_closed": 0, "instructions_proved_dead": 0},
        "layout": contract, "source_retail_bindings": bindings,
        "additional_source_retail_bindings": additional_bindings,
        "local_contract_preconditions": [
            "execution begins at the checked normal producer entry",
            "field0 is 16 bits and field1 is 32 bits",
            "normal helper returns and valid hardware/process stacks",
            "interrupts preserve live registers and saved stack contents"],
        "a7_preserving_helpers": {f"{p:08x}": v for p, v in helper_contracts.items()},
        "process_call_producers": calls, "process_return_consumers": returns,
        "writer_call_census": writers, "archived_source_writer_calls": source_calls,
        "global_obligations": [{"address": p, "target_closure": "OPEN"}
                               for p in code_map["indirect_sites"]],
        "remaining_obligations": [
            "All computed and alternate entries; direct-call census is not exhaustive control flow.",
            "Valid ACTIVE/FREE lists, allocation, kill/reuse and correct A13/A0 owner identity.",
            "Object OPLINK aliases and writes through copied or computed process pointers.",
            "Matching process-stack pushes/pops, bounds, and saved continuation lifetime.",
            "Field widths plus IRQ memory, nesting and hardware-stack integrity.",
            "Global P_FORK caller domain, LZRORB A10 lifetime and source-unbound retail paths.",
            "Weak-seed/data classification, unhit code reachability and all other C2 domains."],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--checkpoint", required=True, type=Path)
    args = parser.parse_args()
    try:
        output = validate_output(ROOT, args.out)
        report = analyze(ROOT, args.checkpoint.resolve())
    except (ContractError, OSError, ValueError, KeyError) as error:
        print(json.dumps({"result": "INVALID_MPROC_CONTRACT", "error": str(error)}))
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"result": report["result"], "output": str(output),
                      "summary": report["summary"]}, indent=2))
    return 3


if __name__ == "__main__":
    raise SystemExit(main())
