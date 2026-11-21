#!/usr/bin/env python3
"""Adversarial checks for the conservative C2 contracts, not game-fidelity proof."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "controlflow_audit", ROOT / "tools/gsp2cpp/analysis/controlflow_audit.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
from decode import Insn, Opnd


def ins(pc, words, mnemonic, *ops):
    return Insn(pc, 0, words, mnemonic, [Opnd(*x) for x in ops])


def budget():
    site = 0xffe46cb0
    return [
        ins(site - 0x80, 3, "ANDI", ("IMM", None, 3), ("R", "A3", None)),
        ins(site - 0x50, 1, "SLL", ("K", None, 5), ("R", "A3", None)),
        ins(site - 0x40, 3, "ADDI", ("IMM", None, site + 16), ("R", "A3", None)),
        ins(site - 0x10, 1, "MOVE", ("IND", "A3", None),
            ("R", "A3", None), ("K", None, 1)),
        ins(site, 1, "JUMP", ("R", "A3", None)),
    ]


def table_rom(address, targets):
    raw = bytearray(0x40000)
    struct.pack_into("<" + "I" * len(targets), raw,
                     (address - audit.compare.BASE) // 8, *targets)
    return bytes(raw)


def collision():
    patterns = [
        (0xffe55a00, 2, "MOVB", ("DISP", "A1", 0x158), ("R", "A3", None)),
        (0xffe55a20, 2, "MOVB", ("DISP", "A2", 0x158), ("R", "A4", None)),
        (0xffe55a40, 1, "SLL", ("K", None, 27), ("R", "A3", None)),
        (0xffe55a50, 1, "SRL", ("K", None, 17), ("R", "A3", None)),
        (0xffe55a60, 1, "SLL", ("K", None, 27), ("R", "A4", None)),
        (0xffe55a70, 1, "SRL", ("K", None, 22), ("R", "A4", None)),
        (0xffe55a80, 1, "ADD", ("R", "A4", None), ("R", "A3", None)),
        (0xffe55a90, 3, "ADDI", ("IMM", None, 0xffe55b50), ("R", "A3", None)),
        (0xffe55ac0, 3, "CMPI", ("IMM", None, 0xffe5c750), ("R", "A3", None)),
        (0xffe55af0, 1, "JRC", ("REL", None, 0xffe55b30)),
        (0xffe55b00, 3, "MOVI", ("IMM", None, 0xffe55b50), ("R", "A3", None)),
        (0xffe55b30, 1, "MOVE", ("IND", "A3", None), ("R", "A3", None), ("K", None, 1)),
        (0xffe55b40, 1, "JUMP", ("R", "A3", None)),
    ]
    return [ins(*p) for p in patterns]


def circle():
    patterns = [
        (0xfff4ff40, 3, "MOVI", ("IMM", None, 0xfff501f0), ("R", "A7", None)),
        (0xfff4ff70, 1, "BTST", ("K", None, 16), ("R", "A6", None)),
        (0xfff4ff80, 1, "JREQ", ("REL", None, 0xfff4ffc0)),
        (0xfff4ff90, 3, "MOVI", ("IMM", None, 0xfff50370), ("R", "A7", None)),
        (0xfff4ffc0, 2, "MOVI", ("IMM", None, 0x1000), ("R", "B3", None)),
        (0xfff4ffe0, 2, "MOVI", ("IMM", None, 0x13), ("R", "B10", None)),
        (0xfff50000, 3, "MOVE", ("R", "B10", None), ("ABS", None, 0xc0000140), ("K", None, 0)),
        (0xfff50030, 1, "MOVE", ("R", "A6", None), ("R", "B9", None)),
        (0xfff50040, 3, "MOVI", ("IMM", None, 0x10000), ("R", "B7", None)),
        (0xfff50070, 1, "CLR", ("R", "A2", None)),
        (0xfff50080, 1, "MOVE", ("R", "A0", None), ("R", "A3", None)),
        (0xfff50090, 1, "MOVE", ("R", "A0", None), ("R", "A4", None)),
        (0xfff500a0, 1, "SLL", ("K", None, 1), ("R", "A4", None)),
        (0xfff500b0, 1, "SUBK", ("K", None, 3), ("R", "A4", None)),
        (0xfff500c0, 1, "NEG", ("R", "A4", None)),
        (0xfff500d0, 1, "CMP", ("R", "A3", None), ("R", "A2", None)),
        (0xfff500e0, 1, "JRGT", ("REL", None, 0xfff501e0)),
        (0xfff500f0, 1, "MOVE", ("R", "A7", None), ("R", "B0", None)),
        (0xfff50100, 1, "EXGPC", ("R", "B0", None)),
    ]
    return {p[0]: ins(*p) for p in patterns}


class IndexTests(unittest.TestCase):
    header = "l8_address,instruction,source,chunk,status\n"

    def test_unquoted_instruction_operands_do_not_shift_source_columns(self):
        text = self.header + "ffe00000,MMFM A12,A0,A3,A7,UTIL.ASM:2>RETP:3,l8c_ffe00000,translated\n"
        row = audit.parse_index(text)[0]
        self.assertEqual(row["instruction"], "MMFM A12,A0,A3,A7")
        self.assertEqual(row["source"], "UTIL.ASM:2>RETP:3")
        self.assertEqual(row["chunk"], "l8c_ffe00000")

    def test_empty_source_annotation_is_retained(self):
        row = audit.parse_index(self.header + "ffe00000,JUMP A3,,l8c_ffe00000,translated\n")[0]
        self.assertEqual(row["source"], "")

    def test_empty_index_rejected(self):
        for text in ("", self.header):
            with self.subTest(text=text), self.assertRaises(audit.ContractError):
                audit.parse_index(text)

    def test_unrecognized_header_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.parse_index("address,instruction,source,chunk,status\n")

    def test_truncated_columns_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.parse_index(self.header + "ffe00000,JUMP A3,,translated\n")

    def test_invalid_address_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.parse_index(self.header + "not_address,JUMP A3,,l8c_0,translated\n")


class TableContractTests(unittest.TestCase):
    targets = [0xffe46d40, 0xffe46db0, 0xffe46e10, 0xffe46ef0]

    def setUp(self):
        self.seq = budget()
        self.rom = table_rom(0xffe46cc0, self.targets)
        self.compiled = set(self.targets)

    def test_masked_dispatch_bounds_are_conditionally_four_slots(self):
        result = audit.masked_table_domain(self.seq, self.rom, self.compiled)
        self.assertEqual(result["entries"], 4)
        self.assertEqual(result["distinct_targets"], 4)
        self.assertNotIn("global_target_closure", result)

    def test_mask_widening_rejected(self):
        self.seq[0].ops[0].value = 7
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom, self.compiled)

    def test_wrong_pointer_stride_rejected(self):
        self.seq[1].ops[0].value = 4
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom, self.compiled)

    def test_wrong_field_selector_rejected(self):
        self.seq[3].ops[2].value = 0
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom, self.compiled)

    def test_guard_sequence_gap_rejected(self):
        self.seq[1].pc += 16
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom, self.compiled)

    def test_intervening_restore_does_not_keep_register_fact(self):
        self.seq.insert(3, ins(self.seq[3].pc, 2, "MMFM",
                              ("R", "A12", None), ("LIST", None, 8)))
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom, self.compiled)

    def test_uncompiled_table_destination_rejected(self):
        self.compiled.remove(self.targets[-1])
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom, self.compiled)

    def test_zero_table_slot_is_not_silently_skipped(self):
        self.rom = table_rom(0xffe46cc0, [*self.targets[:3], 0])
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom, self.compiled)

    def test_changed_table_hash_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom, self.compiled, "0" * 64)

    def test_truncated_rom_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.masked_table_domain(self.seq, self.rom[:8], self.compiled)

    def test_unaligned_table_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.checked_table(self.rom, 0xffe46cc1, 4, self.compiled)

    def test_collision_type_pairs_and_clamp_cover_864_slots(self):
        rom = table_rom(0xffe55b50, [self.targets[0]] * 864)
        result = audit.collision_table_domain(collision(), rom, self.compiled)
        self.assertEqual(result["input_field_pairs_checked"], 1024)
        self.assertEqual(result["entries"], 864)
        self.assertEqual(result["out_of_range_index_rule"], "reset_to_first_slot")

    def test_collision_signed_or_inverted_branch_rejected(self):
        seq = collision()
        seq[9].mnem = "JRNC"
        with self.assertRaises(audit.ContractError):
            audit.collision_table_domain(seq, self.rom, self.compiled)

    def test_collision_end_sentinel_change_rejected(self):
        seq = collision()
        seq[8].ops[0].value += 32
        with self.assertRaises(audit.ContractError):
            audit.collision_table_domain(seq, self.rom, self.compiled)


class LiteralAndJoinTests(unittest.TestCase):
    def setUp(self):
        self.producer = ins(0x1000, 3, "MOVI", ("IMM", None, 0x2000), ("R", "A7", None))
        self.transfer = ins(0x1030, 1, "EXGPC", ("R", "A7", None))
        self.compiled = {0x2000, 0x1040}

    def test_literal_pair_does_not_claim_arbitrary_entry_safety(self):
        result = audit.literal_pair(self.producer, self.transfer, self.compiled)
        self.assertEqual(result["target"], "00002000")
        self.assertFalse(result["alternate_entry_excluded"])

    def test_nonadjacent_literal_does_not_survive_unknown_instruction(self):
        self.transfer.pc += 16
        self.assertIsNone(audit.literal_pair(self.producer, self.transfer, self.compiled))

    def test_different_register_not_bound(self):
        self.producer.ops[1].reg = "A8"
        self.assertIsNone(audit.literal_pair(self.producer, self.transfer, self.compiled))

    def test_literal_target_missing_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.literal_pair(self.producer, self.transfer, {0x1040})

    def test_literal_return_continuation_missing_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.literal_pair(self.producer, self.transfer, {0x2000})

    def test_circle_retains_both_branch_choices(self):
        result = audit.circle_domain(circle(), {0xfff501f0, 0xfff50370, 0xfff50110})
        self.assertEqual(result["targets"], ["fff501f0", "fff50370"])
        self.assertEqual(result["callback_preservation_and_global_entry_proof"], "OPEN")

    def test_circle_callback_missing_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.circle_domain(circle(), {0xfff50370, 0xfff50110})

    def test_circle_new_restore_between_producer_and_use_rejected(self):
        seq = circle()
        seq[0xfff50080].mnem = "MMFM"
        with self.assertRaises(audit.ContractError):
            audit.circle_domain(seq, {0xfff501f0, 0xfff50370, 0xfff50110})

    def test_circle_a7_clobber_rejected(self):
        seq = circle()
        seq[0xfff50080].ops[-1].reg = "A7"
        with self.assertRaises(audit.ContractError):
            audit.circle_domain(seq, {0xfff501f0, 0xfff50370, 0xfff50110})

    def test_circle_wrong_join_rejected(self):
        seq = circle()
        seq[0xfff4ff80].ops[0].value = 0xfff4ff90
        with self.assertRaises(audit.ContractError):
            audit.circle_domain(seq, {0xfff501f0, 0xfff50370, 0xfff50110})


class CoverageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.rows = [
            {"address": "ffe00000", "source": "MENU.ASM:1",
             "instruction": "JUMP A0", "chunk": "x", "status": "translated"},
            {"address": "ffe00010", "source": "WAVES.ASM:2",
             "instruction": "JUMP A0", "chunk": "x", "status": "translated"},
            {"address": "ffe00020", "source": "",
             "instruction": "JUMP A0", "chunk": "x", "status": "translated"},
        ]

    def file(self, name, data):
        path = self.path / name
        path.write_bytes(data)
        return path

    def test_missing_stream_is_not_reported_as_zero_coverage(self):
        report = audit.merge_coverage([], self.rows)
        self.assertEqual(report["state"], "NOT_COLLECTED")
        self.assertNotIn("hits", report)

    def test_exact_union_preserves_proof_boundaries(self):
        a = self.file("a.cov", b"\x01\x00\x00")
        b = self.file("b.cov", b"\x00\x01\x00")
        report = audit.merge_coverage([a, b], self.rows)
        self.assertEqual((report["hits"], report["unhit"]), (2, 1))
        self.assertFalse(report["gate_proof"])
        self.assertFalse(report["scenario_terminal_and_oracle_identity_verified"])
        self.assertFalse(report["unhit_instructions"][0]["dead_or_unreachable_proven"])

    def test_truncation_rejected_even_if_another_stream_covers_everything(self):
        full = self.file("full.cov", b"\x01\x01\x01")
        short = self.file("short.cov", b"\x01\x01")
        with self.assertRaises(audit.ContractError):
            audit.merge_coverage([full, short], self.rows)

    def test_overlong_coverage_rejected(self):
        path = self.file("long.cov", b"\x00\x00\x00\x00")
        with self.assertRaises(audit.ContractError):
            audit.merge_coverage([path], self.rows)

    def test_nonboolean_coverage_rejected(self):
        path = self.file("bad.cov", b"\x01\x00\xff")
        with self.assertRaises(audit.ContractError):
            audit.merge_coverage([path], self.rows)

    def test_source_groups_do_not_declare_dead_or_complete_content(self):
        path = self.file("none.cov", b"\x00\x00\x00")
        report = audit.merge_coverage([path], self.rows)
        self.assertEqual(report["unhit_by_candidate_role"], {
            "operator_diagnostic_candidate": 1, "gameplay_candidate": 1, "unbound": 1})
        self.assertTrue(all(not x["dead_or_unreachable_proven"]
                            for x in report["unhit_instructions"]))


if __name__ == "__main__":
    unittest.main()
