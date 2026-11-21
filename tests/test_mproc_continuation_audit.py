#!/usr/bin/env python3
"""Negative controls for bounded MPROC contracts; not a gameplay proof."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "mproc_continuation_audit",
    ROOT / "tools/scenarios/mproc_continuation_audit.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
from decode import Insn, Opnd

R, K, I, M = audit.R, audit.K, audit.I, audit.M


def ins(pc, words, mnemonic, *operands):
    return Insn(pc, 0, words, mnemonic, [Opnd(*x) for x in operands])


def chain(patterns, start=0x1000):
    result = []
    for words, mnemonic, *ops in patterns:
        result.append(ins(start, words, mnemonic, *ops))
        start += 16 * words
    return result


def process_call(register=False):
    last = (1, "JUMP", R("A3")) if register else (3, "JA", I(0x2000))
    seq = chain([(1, "GETPC", R("A7")),
                 (2, "ADDI", I(0x40 if register else 0x60), R("A7")),
                 (1, "MOVE", R("A7"), M("DEC", "A12"), K(1)), last])
    end = seq[-1].pc + seq[-1].words * 16
    return seq, {0x2000, end}


def wake_path(middle=()):
    seq = chain([(3, "MOVI", I(0x2000), R("A7")), *middle,
                 (3, "CALLA", I(audit.ENTRIES["PUTA7"]))], start=0x3000)
    return seq, {0x2000, seq[-1].pc + 48, 0x4000}


def helper(body=()):
    return chain([*body, (1, "RETS")])


class LaserDirectionTests(unittest.TestCase):
    def fixture(self):
        return chain([(1, "CLR", R("A0")),
                      (1, "JRHI", M("REL", None, 0x1050)),
                      (1, "JRNN", M("REL", None, 0x1080)),
                      (1, "MOVK", K(1), R("A0")),
                      (1, "JR", M("REL", None, 0x1080)),
                      (1, "MOVK", K(2), R("A0")),
                      (1, "JRN", M("REL", None, 0x1080)),
                      (1, "MOVK", K(3), R("A0")), (1, "RETS")])

    def test_all_branch_outcomes_bound_direction(self):
        self.assertEqual(audit.direction_domain(self.fixture()), [0, 1, 2, 3])

    def test_unknown_value_cycle_call_and_memory_write_rejected(self):
        for index, mnemonic, ops in [
            (0, "MOVE", [R("A1"), R("A0")]),
            (7, "MOVK", [K(4), R("A0")]),
            (4, "JR", [M("REL", None, 0x1000)]),
            (0, "CALL", [R("A1")]),
            (3, "MOVE", [R("A1"), M("IND", "A0"), K(1)]),
        ]:
            with self.subTest(index=index, mnemonic=mnemonic):
                seq = self.fixture()
                seq[index] = ins(seq[index].pc, 1, mnemonic, *ops)
                with self.assertRaises(audit.ContractError): audit.direction_domain(seq)


class ForkDomainTests(unittest.TestCase):
    def fixture(self):
        seq = chain([(2, "MMTM", R("SP"), M("LIST", None, 0x4000)),
                     (2, "MOVE", M("DISP", "A13", 0x20), R("A1"), K(0)),
                     (3, "CALLA", I(audit.ENTRIES["GETPRC"])),
                     (2, "MMFM", R("SP"), M("LIST", None, 2)), (1, "RETS")], start=0xffea5830)
        instructions = {x.pc: x for x in seq}
        compiled = {0x2000}
        for pc in [0xffeac390, 0xffeadb00, 0xffeadea0, 0xffeb6360, 0xffebea70]:
            instructions[pc-48] = ins(pc-48, 3, "MOVI", I(0x2000), R("A7"))
            instructions[pc] = ins(pc, 3, "CALLA", I(0xffea5830))
            compiled.add(pc+48)
        return instructions, compiled

    def test_direct_domain_keeps_global_obligations_open(self):
        instructions, compiled = self.fixture()
        result = audit.fork_writer_domain(instructions, compiled)
        self.assertEqual(result["wake_entries"], ["00002000"])
        self.assertEqual(len(result["direct_paths"]), 5)
        self.assertEqual(result["indirect_callers_alternate_entries_and_owner_lifetime"], "OPEN")

    def test_changed_caller_or_wrapper_rejected(self):
        for fault in range(5):
            with self.subTest(fault=fault):
                instructions, compiled = self.fixture()
                if fault == 0: instructions[0xffeac360] = ins(0xffeac360, 3, "MOVI", I(0x2000), R("A6"))
                if fault == 1: instructions[0xffea5830] = ins(0xffea5830, 2, "MMTM", R("SP"), M("LIST", None, 0x100))
                if fault == 2: del instructions[0xffeac390]
                if fault == 3: compiled.remove(0x2000)
                if fault == 4: instructions[0x5000] = ins(0x5000, 3, "CALLA", I(0xffea5830))
                with self.assertRaises(audit.ContractError):
                    audit.fork_writer_domain(instructions, compiled)


class LayoutTests(unittest.TestCase):
    def test_save_restore_use_opposite_mask_orders_and_same_slots(self):
        save, bottom = audit.mask_slots("MMTM", 0x1f8, 0x100)
        restore, top = audit.mask_slots("MMFM", 0x1f80, 0x40)
        self.assertEqual(save, restore)
        self.assertEqual(save["A7"], 0xe0)
        self.assertEqual(save["A12"], 0x40)
        self.assertEqual((bottom, top), (0x40, 0x100))
        self.assertNotEqual(audit.mask_slots("MMFM", 0x1f8, 0x40)[0], save)

    def test_empty_mask_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.mask_slots("MMFM", 0, 0x40)

    def test_equates_verified_including_process_stack_limit(self):
        text = "\n".join(f"{k} .SET {v}" for k, v in audit.LAYOUT.items())
        self.assertEqual(audit.source_layout(text), audit.LAYOUT)
        with self.assertRaises(audit.ContractError):
            audit.source_layout(text.replace("PRCSIZ .SET 1440", "PRCSIZ .SET 1472"))

    def test_duplicate_equate_rejected(self):
        text = "\n".join(f"{k} .SET {v}" for k, v in audit.LAYOUT.items())
        with self.assertRaises(audit.ContractError):
            audit.source_layout(text + "\nPWAKE .SET 224")

    def test_missing_instruction_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.window({}, 0x1000, 0x1010)

    def test_partial_instruction_window_rejected(self):
        item = ins(0x1000, 2, "MMFM", R("A12"), M("LIST", None, 0x80))
        with self.assertRaises(audit.ContractError):
            audit.window({item.pc: item}, 0x1000, 0x1010)


class ProcessContinuationTests(unittest.TestCase):
    def test_direct_call_continuation_matches_instruction_end(self):
        seq, compiled = process_call()
        result = audit.process_call(seq, compiled)
        self.assertEqual(result["continuation"], "00001070")
        self.assertEqual(result["destination"], "00002000")
        self.assertEqual(result["owner_stack_and_alternate_entry_proof"], "OPEN")

    def test_register_call_retains_unknown_target_domain(self):
        seq, compiled = process_call(True)
        result = audit.process_call(seq, compiled)
        self.assertEqual(result["continuation"], "00001050")
        self.assertEqual(result["destination"]["target_domain"], "OPEN")

    def test_off_by_instruction_continuation_rejected(self):
        seq, compiled = process_call()
        seq[1].ops[0].value += 16
        with self.assertRaises(audit.ContractError):
            audit.process_call(seq, compiled)

    def test_wrong_process_stack_rejected(self):
        seq, compiled = process_call()
        seq[2].ops[1].reg = "SP"
        with self.assertRaises(audit.ContractError):
            audit.process_call(seq, compiled)

    def test_wrong_store_field_rejected(self):
        seq, compiled = process_call()
        seq[2].ops[2].value = 0
        with self.assertRaises(audit.ContractError):
            audit.process_call(seq, compiled)

    def test_gap_rejected(self):
        seq, compiled = process_call()
        seq[2].pc += 16
        with self.assertRaises(audit.ContractError):
            audit.process_call(seq, compiled)

    def test_uncompiled_callee_rejected(self):
        seq, compiled = process_call()
        compiled.remove(0x2000)
        with self.assertRaises(audit.ContractError):
            audit.process_call(seq, compiled)

    def test_uncompiled_continuation_rejected(self):
        seq, compiled = process_call()
        compiled.remove(0x1070)
        with self.assertRaises(audit.ContractError):
            audit.process_call(seq, compiled)

    def test_register_target_destroyed_by_getpc_rejected(self):
        seq, compiled = process_call(True)
        seq[-1].ops[0].reg = "A7"
        with self.assertRaises(audit.ContractError):
            audit.process_call(seq, compiled)

    def test_restore_return_pair_is_only_local_contract(self):
        seq = chain([(2, "MMFM", R("A12"), M("LIST", None, 0x80)),
                     (1, "EXGPC", R("A7"))])
        self.assertEqual(audit.process_return(*seq)["owner_and_matching_push_proof"],
                         "OPEN")

    def test_return_extra_restore_register_rejected(self):
        seq = chain([(2, "MMFM", R("A12"), M("LIST", None, 0x180)),
                     (1, "EXGPC", R("A7"))])
        with self.assertRaises(audit.ContractError):
            audit.process_return(*seq)

    def test_nonadjacent_return_rejected(self):
        seq = chain([(2, "MMFM", R("A12"), M("LIST", None, 0x80)),
                     (1, "EXGPC", R("A7"))])
        seq[1].pc += 16
        with self.assertRaises(audit.ContractError):
            audit.process_return(*seq)


class WakeProducerTests(unittest.TestCase):
    def test_adjacent_literal_does_not_prove_process_owner(self):
        seq, compiled = wake_path()
        result = audit.literal_wake_path(seq, compiled, {})
        self.assertEqual(result["wake_entry"], "00002000")
        self.assertEqual(
            result["process_owner_alias_lifetime_and_alternate_entry_proof"], "OPEN")

    def test_verified_helper_and_nonclobbering_operation(self):
        seq, compiled = wake_path([(3, "CALLA", I(0xffe0b330)),
                                   (1, "CLR", R("A1"))])
        result = audit.literal_wake_path(seq, compiled, {0xffe0b330: {}})
        self.assertEqual(result["helper_calls"], ["ffe0b330"])

    def test_unknown_helper_cannot_preserve_literal_fact(self):
        seq, compiled = wake_path([(3, "CALLA", I(0xffe0b330))])
        with self.assertRaises(audit.ContractError):
            audit.literal_wake_path(seq, compiled, {})

    def test_implicit_restore_cannot_preserve_literal_fact(self):
        seq, compiled = wake_path([(2, "MMFM", R("SP"), M("LIST", None, 0x80))])
        with self.assertRaises(audit.ContractError):
            audit.literal_wake_path(seq, compiled, {})

    def test_direct_a7_clobber_rejected(self):
        seq, compiled = wake_path([(1, "CLR", R("A7"))])
        with self.assertRaises(audit.ContractError):
            audit.literal_wake_path(seq, compiled, {})

    def test_unknown_memory_write_rejected(self):
        seq, compiled = wake_path([(1, "MOVE", R("A0"), M("IND", "A1"), K(1))])
        with self.assertRaises(audit.ContractError):
            audit.literal_wake_path(seq, compiled, {})

    def test_wrong_writer_rejected(self):
        seq, compiled = wake_path()
        seq[-1].ops[0].value = 0x9000
        with self.assertRaises(audit.ContractError):
            audit.literal_wake_path(seq, compiled, {})

    def test_uncompiled_literal_rejected(self):
        seq, compiled = wake_path()
        compiled.remove(0x2000)
        with self.assertRaises(audit.ContractError):
            audit.literal_wake_path(seq, compiled, {})

    def test_exit_branch_is_conditional_path_not_reachability(self):
        seq, compiled = wake_path([(1, "JREQ", M("REL", None, 0x4000))])
        result = audit.literal_wake_path(seq, compiled, {})
        self.assertEqual(result["conditional_exits"], ["00004000"])

    def test_branch_back_into_producer_path_rejected(self):
        seq, compiled = wake_path([(1, "JREQ", M("REL", None, 0x3000))])
        compiled.add(0x3000)
        with self.assertRaises(audit.ContractError):
            audit.literal_wake_path(seq, compiled, {})


class HelperTests(unittest.TestCase):
    def test_checked_read_helper_preserves_only_register(self):
        seq = helper([(2, "MOVE", M("DISP", "A0", 0xc0), R("A8"), K(1))])
        result = audit.verify_preserves_a7(seq)
        self.assertTrue(result["a7_preserved_on_normal_return"])
        self.assertEqual(result["stack_memory_integrity_and_termination"], "OPEN")

    def test_implicit_a7_restore_rejected(self):
        seq = helper([(2, "MMFM", R("SP"), M("LIST", None, 0x80))])
        with self.assertRaises(audit.ContractError):
            audit.verify_preserves_a7(seq)

    def test_a6_multiply_cannot_hide_implicit_a7_write(self):
        seq = helper([(1, "MPYU", R("A0"), R("A6"))])
        with self.assertRaises(audit.ContractError):
            audit.verify_preserves_a7(seq)

    def test_unchecked_nested_call_rejected(self):
        seq = helper([(3, "CALLA", I(0x2000))])
        with self.assertRaises(audit.ContractError):
            audit.verify_preserves_a7(seq)

    def test_external_branch_rejected(self):
        seq = helper([(1, "JREQ", M("REL", None, 0x2000))])
        with self.assertRaises(audit.ContractError):
            audit.verify_preserves_a7(seq)

    def test_restoring_stack_pointer_rejected(self):
        seq = helper([(2, "MMFM", R("SP"), M("LIST", None, 0x8000))])
        with self.assertRaises(audit.ContractError):
            audit.verify_preserves_a7(seq)


class BindingAndEvidenceTests(unittest.TestCase):
    def fixture(self):
        item = ins(audit.BASE, 1, "RETS")
        return [item], bytes(2), bytes(2), {
            audit.BASE: [[f"{audit.BASE:08x}", "0000", "MPROC.ASM", 1, "RETS"]]
        }, ["  RETS ; normal return"]

    def test_exact_binding_records_source_and_bytes(self):
        result = audit.bind_exact_core(*self.fixture())
        self.assertTrue(result["source_statements_verified"])
        self.assertTrue(result["p1_retail_bytes_identical"])

    def test_changed_retail_bytes_rejected(self):
        args = list(self.fixture())
        args[1] = b"\x01\x00"
        with self.assertRaises(audit.ContractError):
            audit.bind_exact_core(*args)

    def test_changed_p1_and_retail_bytes_cannot_reuse_stale_decoding(self):
        args = list(self.fixture())
        args[1] = args[2] = b"\x01\x00"
        with self.assertRaises(audit.ContractError):
            audit.bind_exact_core(*args)

    def test_same_opcode_different_source_statement_rejected(self):
        args = list(self.fixture())
        args[-1] = ["EXGPC A7"]
        with self.assertRaises(audit.ContractError):
            audit.bind_exact_core(*args)

    def test_ambiguous_p1_source_row_rejected(self):
        args = list(self.fixture())
        args[3][audit.BASE] *= 2
        with self.assertRaises(audit.ContractError):
            audit.bind_exact_core(*args)

    def test_other_source_module_rejected(self):
        args = list(self.fixture())
        args[3][audit.BASE][0][2] = "OTHER.ASM"
        with self.assertRaises(audit.ContractError):
            audit.bind_exact_core(*args)

    def test_truncated_image_rejected(self):
        args = list(self.fixture())
        args[2] = b""
        with self.assertRaises(audit.ContractError):
            audit.bind_exact_core(*args)

    def test_source_census_excludes_commented_calls_and_is_only_a_hint(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "SAMPLE.ASM"
            path.write_text("; CALLA XFERPROC\n* CALLA PUTA7\n"
                            "CALLA XFERPROC ; update\nCALLA PUTA7\n")
            result = audit.source_writer_census([path], [{"source": "SAMPLE.ASM:3"}])
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0]["retail_annotation_rows"], 1)
        self.assertEqual(result[1]["retail_annotation_rows"], 0)
        self.assertFalse(result[0]["annotation_is_source_binding"])

    def test_original_dos_comment_bytes_are_read_without_rewriting_source(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "DOS.ASM"
            original = b";\xc9\x85 DOS comment\nCALLA PUTA7\n"
            path.write_bytes(original)
            result = audit.source_writer_census([path], [])
            self.assertEqual(len(result), 1)
            self.assertEqual(result[0]["source"], "DOS.ASM:2")
            self.assertEqual(path.read_bytes(), original)

    def test_existing_and_external_output_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "build").mkdir()
            existing = root / "build" / "kept.json"
            existing.write_text("keep")
            with self.assertRaises(audit.ContractError):
                audit.validate_output(root, existing)
            with self.assertRaises(audit.ContractError):
                audit.validate_output(root, root / "source.json")
            self.assertEqual(existing.read_text(), "keep")
            self.assertEqual(audit.validate_output(root, root / "build/new.json"),
                             (root / "build/new.json").resolve())


def menu_domain():
    seq = chain([(3, "MOVI", I(0x2000), R("A7")),
                 (1, "JR", M("REL", None, 0x1070)),
                 (3, "MOVI", I(0x3000), R("A7")),
                 (2, "MOVI", I(0xac1), R("A1")),
                 (3, "CALLA", I(audit.ENTRIES["GETPRC"]))])
    return seq, {0x2000, 0x3000, 0x10c0}


def switch_domain():
    base = audit.BASE + 0x400
    seq = chain([
        (1, "SLL", K(27), R("A0")), (1, "SRL", K(27), R("A0")),
        (1, "MOVE", R("A0"), R("A2")), (1, "ADD", R("A0"), R("A2")),
        (1, "ADD", R("A0"), R("A2")), (1, "SLL", K(4), R("A2")),
        (3, "ADDI", I(base), R("A2")),
        (1, "MOVE", M("INC", "A2"), R("A1"), K(0)),
        (1, "MOVE", M("INC", "A2"), R("A7"), K(1)),
        (1, "JREQ", M("REL", None, 0xffeaf4f0)),
        (3, "CALLA", I(audit.ENTRIES["GETPRC"]))], start=0xffeaf5e0)
    raw = bytearray(512)
    offset = (base - audit.BASE) // 8
    for n in range(32):
        raw[offset+n*6:offset+n*6+2] = (0x606).to_bytes(2, "little")
        target = 0x2000 if n % 2 == 0 else 0
        raw[offset+n*6+2:offset+n*6+6] = target.to_bytes(4, "little")
    return seq, bytes(raw), {0x2000, 0xffeaf4f0, 0xffeaf6d0}


class BoundedWorkerTests(unittest.TestCase):
    def test_menu_has_two_compiled_workers_and_keeps_alternate_entries_open(self):
        seq, compiled = menu_domain()
        result = audit.menu_worker_domain(seq, compiled)
        self.assertEqual(result["wake_entries"], ["00002000", "00003000"])
        self.assertEqual(result["process_owner_and_alternate_entry_proof"], "OPEN")

    def test_menu_branch_cannot_overwrite_first_worker(self):
        seq, compiled = menu_domain()
        seq[1].ops[0].value = seq[2].pc
        with self.assertRaises(audit.ContractError):
            audit.menu_worker_domain(seq, compiled)

    def test_menu_missing_second_worker_rejected(self):
        seq, compiled = menu_domain()
        compiled.remove(0x3000)
        with self.assertRaises(audit.ContractError):
            audit.menu_worker_domain(seq, compiled)

    def test_switch_masks_all_32_indices_and_skips_null_records(self):
        result = audit.switch_worker_domain(*switch_domain())
        self.assertEqual(result["index_domain"], [0, 31])
        self.assertEqual(len(result["records"]), 32)
        self.assertEqual(result["record_stride_bits"], 48)
        self.assertEqual(result["wake_entries"], ["00002000"])
        self.assertTrue(result["null_records_skip_writer"])
        self.assertEqual(result["process_owner_and_alternate_entry_proof"], "OPEN")

    def test_switch_widened_index_rejected(self):
        seq, rom, compiled = switch_domain()
        seq[0].ops[0].value = seq[1].ops[0].value = 26
        with self.assertRaises(audit.ContractError):
            audit.switch_worker_domain(seq, rom, compiled)

    def test_switch_wrong_stride_rejected(self):
        seq, rom, compiled = switch_domain()
        seq[5].ops[0].value = 5
        with self.assertRaises(audit.ContractError):
            audit.switch_worker_domain(seq, rom, compiled)

    def test_switch_wrong_pid_field_rejected(self):
        seq, rom, compiled = switch_domain()
        seq[7].ops[-1].value = 1
        with self.assertRaises(audit.ContractError):
            audit.switch_worker_domain(seq, rom, compiled)

    def test_switch_wrong_pc_field_rejected(self):
        seq, rom, compiled = switch_domain()
        seq[8].ops[-1].value = 0
        with self.assertRaises(audit.ContractError):
            audit.switch_worker_domain(seq, rom, compiled)

    def test_switch_inverted_null_guard_rejected(self):
        seq, rom, compiled = switch_domain()
        seq[9].mnem = "JRNE"
        with self.assertRaises(audit.ContractError):
            audit.switch_worker_domain(seq, rom, compiled)

    def test_switch_noncompiled_nonzero_target_rejected(self):
        seq, rom, compiled = switch_domain()
        compiled.remove(0x2000)
        with self.assertRaises(audit.ContractError):
            audit.switch_worker_domain(seq, rom, compiled)

    def test_switch_truncated_records_rejected(self):
        seq, rom, compiled = switch_domain()
        with self.assertRaises(audit.ContractError):
            audit.switch_worker_domain(seq, rom[:200], compiled)

    def test_switch_changed_pinned_table_rejected(self):
        with self.assertRaises(audit.ContractError):
            audit.switch_worker_domain(*switch_domain(), expected_hash="0" * 64)


class RelocatedSourceBindingTests(unittest.TestCase):
    def fixture(self):
        seq = [ins(audit.BASE, 3, "MOVI", I(2), R("A7"))]
        rom, p1 = b"\x00\x00\x02\x00\x00\x00", b"\x00\x00\x01\x00\x00\x00"
        mask = b"\x00\x00\x01\x01\x01\x01"
        rows = [[f"{audit.BASE:08x}", "0000", "AUDIT.ASM", 1, "MOVI TARGET,A7"]]
        return [seq, rom, p1, mask, rows, "AUDIT.ASM", [1], ["MOVI TARGET,A7"]]

    def test_relocation_is_explicit_and_never_global_entry_proof(self):
        result = audit.bind_relocated_source(*self.fixture())
        self.assertEqual(result["instructions"][0]["changed_relocated_byte_offsets"], [2])
        self.assertFalse(result["relocations_or_annotations_prove_global_entry_set"])

    def test_unmasked_operand_change_rejected(self):
        args = self.fixture()
        args[3] = bytes(6)
        with self.assertRaises(audit.ContractError):
            audit.bind_relocated_source(*args)

    def test_mask_cannot_hide_changed_opcode(self):
        args = self.fixture()
        args[1] = b"\x01\x00\x02\x00\x00\x00"
        args[3] = bytes([1] * 6)
        with self.assertRaises(audit.ContractError):
            audit.bind_relocated_source(*args)

    def test_relocated_source_statement_mismatch_rejected(self):
        args = self.fixture()
        args[-1] = ["MOVI OTHER,A7"]
        with self.assertRaises(audit.ContractError):
            audit.bind_relocated_source(*args)

    def test_truncated_relocation_mask_rejected(self):
        args = self.fixture()
        args[3] = bytes(1)
        with self.assertRaises(audit.ContractError):
            audit.bind_relocated_source(*args)


if __name__ == "__main__":
    unittest.main()
