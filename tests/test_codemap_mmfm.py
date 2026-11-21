"""MMFM code-map regression: real generator, typed ISA fixtures, optional retail inputs.

Standalone: python -B tests/test_codemap_mmfm.py --out-dir build/mmfm-tests
Add --with-retail for the user's immutable L8 ROM/P1/symbol prerequisites.
--generator selects an isolated candidate without copying its constant tracker.
All code-map and fixture output is confined to a fresh directory beneath build/.
No C++ emission files, native execution, compiler or MAME is invoked.
"""
from pathlib import Path
import argparse
import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / "tools/gsp2cpp/codemap.py").is_file())
GENERATOR = ROOT / "tools/gsp2cpp/codemap.py"
OUTPUT = None
WITH_RETAIL = False
sys.path.insert(0, str(ROOT / "tools/gsp2cpp"))
from decode import Decoder
from emit import Emitter

BASE = 0xFFE00000
TARGET = BASE + 0x18000
RETURNS = ("ffe6f500", "ffe7b910", "ffe9b3e0",
           "ffeb63b0", "ffebb560", "fff3a690")
# name, pointer, encoded file, mask, tracked transfer register, unknown?,
# memory operation, control operation. Expected results are explicit.
CASES = (
    ("a_low_mask", "A12", "A", 0x0001, "A0", True, "MMFM", "EXGPC"),
    ("a_a7_mask", "A12", "A", 0x0080, "A7", True, "MMFM", "EXGPC"),
    ("a_high_mask", "A12", "A", 0x4000, "A14", True, "MMFM", "EXGPC"),
    ("b_low_mask", "B12", "B", 0x0001, "B0", True, "MMFM", "EXGPC"),
    ("b_b7_mask", "B12", "B", 0x0080, "B7", True, "MMFM", "EXGPC"),
    ("b_high_mask", "B12", "B", 0x4000, "B14", True, "MMFM", "EXGPC"),
    ("sp_pointer_selects_a", "SP", "A", 0x0080, "A7", True, "MMFM", "EXGPC"),
    ("sp_pointer_selects_b", "SP", "B", 0x0080, "B7", True, "MMFM", "EXGPC"),
    ("shared_sp_loaded_by_a", "A12", "A", 0x8000, "SP", True, "MMFM", "JUMP"),
    ("shared_sp_loaded_by_b", "B12", "B", 0x8000, "SP", True, "MMFM", "JUMP"),
    ("a_pointer_advances", "A12", "A", 0x0001, "A12", True, "MMFM", "EXGPC"),
    ("b_pointer_advances", "B12", "B", 0x0001, "B12", True, "MMFM", "EXGPC"),
    ("sp_pointer_advances", "SP", "B", 0x0080, "SP", True, "MMFM", "JUMP"),
    ("pointer_also_restored", "A12", "A", 0x1000, "A12", True, "MMFM", "EXGPC"),
    ("a_sparse_restored", "A12", "A", 0x0101, "A8", True, "MMFM", "EXGPC"),
    ("b_full_mask", "B12", "B", 0xFFFF, "B7", True, "MMFM", "EXGPC"),
    ("a_unlisted_preserved", "A12", "A", 0x0080, "A8", False, "MMFM", "EXGPC"),
    ("b_unlisted_preserved", "B12", "B", 0x0080, "B8", False, "MMFM", "EXGPC"),
    ("other_b_file_preserved", "A12", "A", 0x0080, "B7", False, "MMFM", "EXGPC"),
    ("other_a_file_preserved", "B12", "B", 0x0080, "A7", False, "MMFM", "EXGPC"),
    ("sp_a_keeps_b_constant", "SP", "A", 0x0080, "B7", False, "MMFM", "EXGPC"),
    ("sp_b_keeps_a_constant", "SP", "B", 0x0080, "A7", False, "MMFM", "EXGPC"),
    ("low_mask_keeps_a14", "A12", "A", 0x0001, "A14", False, "MMFM", "EXGPC"),
    ("zero_mask_keeps_a7", "A12", "A", 0x0000, "A7", False, "MMFM", "EXGPC"),
    ("sparse_mask_keeps_a7", "A12", "A", 0x0101, "A7", False, "MMFM", "EXGPC"),
    ("mmtm_store_keeps_a7", "A12", "A", 0x0100, "A7", False, "MMTM", "EXGPC"),
    ("mmtm_store_keeps_sp", "A12", "A", 0x0001, "SP", False, "MMTM", "JUMP"),
    ("mmfm_before_call", "A12", "A", 0x0080, "A7", True, "MMFM", "CALL"),
    ("mmfm_before_jump", "B12", "B", 0x0080, "B7", True, "MMFM", "JUMP"),
)


def opcode(decoder, mnemonic, reg=None, file=None, words=None):
    found = []
    for op, (length, name, operands, text) in decoder.table.items():
        regs = [r for kind, r, value in operands if kind == "R"]
        if name != mnemonic or (words is not None and length != words):
            continue
        if reg is not None and regs != [reg]:
            continue
        if reg == "SP" and file is not None and bool(op & 0x10) != (file == "B"):
            continue
        found.append(op)
    if not found:
        raise AssertionError(f"ISA lacks {mnemonic} {reg} file={file}")
    return min(found)


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def invoke(generator, output, *arguments, fixture_rom=None, fixture_root=None):
    command = [str(GENERATOR), "--out", str(output), *arguments]
    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.object(sys, "argv", command))
        if fixture_rom is not None:
            stack.enter_context(mock.patch.object(generator.compare, "retail",
                                                  return_value=fixture_rom))
            stack.enter_context(mock.patch.object(generator, "ROOT", str(fixture_root)))
        capture = stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        generator.main()
        log = capture.getvalue()
    output.with_suffix(".log").write_text(log, encoding="utf-8", newline="\n")
    return json.loads(output.read_text(encoding="utf-8"))


class CodeMapMMFM(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global OUTPUT
        if OUTPUT is None:
            parent = ROOT / "build/codemap-mmfm-tests"
            parent.mkdir(parents=True, exist_ok=True)
            OUTPUT = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
        else:
            OUTPUT.mkdir(parents=True, exist_ok=False)
        if not OUTPUT.resolve().is_relative_to((ROOT / "build").resolve()):
            raise ValueError("All regression output must stay beneath build/")
        spec = importlib.util.spec_from_file_location("codemap_under_test", GENERATOR)
        cls.generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.generator)
        cls.decoder = Decoder()
        cls.fixture = OUTPUT / "fixture"
        # A complete virtual ROM span makes the real vector/weak-seed stages run.
        invalid = next(op for op in range(65536) if op not in cls.decoder.table)
        words = [invalid] * ((0x100000000 - BASE) // 16)
        listing = []
        cls.transfers = {}
        for n, case in enumerate(CASES):
            name, pointer, file, mask, target, unknown, memory, control = case
            start = n * 0x40
            program = [opcode(cls.decoder, "MOVI", target, words=3),
                       TARGET & 0xFFFF, TARGET >> 16]
            memory_at = BASE + (start + len(program)) * 16
            program += [opcode(cls.decoder, memory, pointer, file, 2), mask]
            transfer = BASE + (start + len(program)) * 16
            program += [opcode(cls.decoder, control, target)]
            if control == "CALL":
                program += [opcode(cls.decoder, "RETS")]
            words[start:start + len(program)] = program
            cls.transfers[name] = transfer
            listing.append([f"{BASE + start * 16:08x}", f"{program[0]:04x}",
                            "synthetic_mmfm.asm", n + 1, name])
        words[(TARGET - BASE) // 16] = opcode(cls.decoder, "RETS")
        # Invalid padding also prevents fictitious trap-vector roots.
        rom = b"".join(w.to_bytes(2, "little") for w in words)
        write_json(cls.fixture / "generated/gsp2cpp/symbols_l8.json",
                   {"symbols": {"FIXTURE_ENTRY": f"{BASE:08x}"}})
        write_json(cls.fixture / "listing.json",
                   {"instructions": listing, "code_pointers": []})
        write_json(cls.fixture / "map.json",
                   {"globals": {"FIXTURE_ENTRY": f"{BASE:08x}"},
                    "sections": {"fixture:.text": f"{BASE:08x}"}})
        cls.synthetic = invoke(cls.generator, OUTPUT / "synthetic-map.json",
                               "--listing", str(cls.fixture / "listing.json"),
                               "--map", str(cls.fixture / "map.json"),
                               fixture_rom=rom, fixture_root=cls.fixture)
        write_json(OUTPUT / "fixture-manifest.json", {
            "cases": [{"name": c[0], "pointer": c[1], "file": c[2], "mask": c[3],
                       "target_register": c[4], "expected_indirect": c[5],
                       "memory_operation": c[6], "control_operation": c[7],
                       "transfer_pc": f"{cls.transfers[c[0]]:08x}"} for c in CASES],
            "rom_bytes": len(rom), "scope": "synthetic input, actual generator/decoder",
        })

    def test_typed_list_and_emitter_a_b_sp_semantics(self):
        # Explicit expectations also guard LIST mask orientation and shared SP.
        examples = (
            ("A12", "A", 0xC081, ["c.sp", "c.a[14]", "c.a[7]", "c.a[0]"],
             "c.a[12]"),
            ("B12", "B", 0xC081, ["c.sp", "c.b[14]", "c.b[7]", "c.b[0]"],
             "c.b[12]"),
            ("SP", "A", 0x0080, ["c.a[7]"], "c.sp"),
            ("SP", "B", 0x0080, ["c.b[7]"], "c.sp"),
            ("A12", "A", 0x0000, [], "c.a[12]"),
            ("A12", "A", 0x1000, ["c.a[12]"], "c.a[12]"),
        )
        for pointer, file, mask, expected, pointer_cpp in examples:
            with self.subTest(pointer=pointer, file=file, mask=mask):
                w = [opcode(self.decoder, "MMFM", pointer, file, 2), mask]
                ins = self.decoder.decode(w, 0, BASE)
                self.assertEqual([(o.kind, o.reg, o.value) for o in ins.ops],
                                 [("R", pointer, None), ("LIST", None, mask)])
                lines, kind = Emitter(w, {}, {}).emit(ins, set())
                restores = [line for line in lines if "rlong(m," in line]
                self.assertEqual(len(restores), len(expected))
                for line, destination in zip(restores, expected):
                    self.assertIn(f"rlong(m,{pointer_cpp})", line)
                    self.assertIn(f"{destination}=v;", line)
                    self.assertIn(f"{pointer_cpp}+=0x20u;", line)
                self.assertIn(f"c.cyc += {3 + 4 * len(expected)};", lines)
                self.assertEqual(kind, "fall")

    def test_six_real_typed_mmfm_returns(self):
        if not WITH_RETAIL:
            self.skipTest("Use --with-retail; requires immutable private ROM/P1/symbols.")
        rom = self.generator.compare.retail("l8")
        words = [rom[i] | (rom[i + 1] << 8) for i in range(0, len(rom), 2)]
        for address in RETURNS:
            with self.subTest(return_pc=address):
                pc = int(address, 16)
                mmfm = self.decoder.decode(words, (pc - 32 - BASE) // 16, pc - 32)
                ret = self.decoder.decode(words, (pc - BASE) // 16, pc)
                self.assertEqual(mmfm.mnem, "MMFM")
                self.assertEqual([(o.kind, o.reg, o.value) for o in mmfm.ops],
                                 [("R", "A12", None), ("LIST", None, 0x0080)])
                self.assertEqual(mmfm.pc + mmfm.words * 16, pc)
                self.assertEqual(ret.mnem, "EXGPC")
                self.assertEqual([(o.kind, o.reg) for o in ret.ops], [("R", "A7")])

    def test_six_real_returns_are_unknown_in_actual_generator(self):
        if not WITH_RETAIL:
            self.skipTest("Use --with-retail; requires immutable private ROM/P1/symbols.")
        required = ("original/user-supplied-roms/smashtv.zip", "build/p1/listing.json",
                    "build/p1/robo.map.json", "generated/gsp2cpp/symbols_l8.json")
        self.assertFalse([p for p in required if not (ROOT / p).is_file()],
                         "Missing retail prerequisites must fail when requested")
        result = invoke(self.generator, OUTPUT / "retail-map.json",
                        "--listing", str(ROOT / "build/p1/listing.json"),
                        "--map", str(ROOT / "build/p1/robo.map.json"))
        missing = sorted(set(RETURNS) - set(result["indirect_sites"]))
        self.assertEqual(missing, [], f"MMFM-restored A7 returns hidden as constants: {missing}")


def install_case(case):
    def test(self):
        name, pointer, file, mask, target, unknown, memory, control = case
        pc = f"{self.transfers[name]:08x}"
        self.assertIn(pc, self.synthetic["instructions"], "Fixture transfer must decode")
        self.assertEqual(pc in self.synthetic["indirect_sites"], unknown,
                         f"{name}: {memory} {pointer},mask={mask:04x}; {control} {target}")
    test.__name__ = f"test_{case[0]}"
    setattr(CodeMapMMFM, test.__name__, test)


for case in CASES:
    install_case(case)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", type=Path, default=GENERATOR)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--with-retail", action="store_true")
    args, remaining = parser.parse_known_args()
    GENERATOR = args.generator.resolve()
    OUTPUT = args.out_dir.resolve() if args.out_dir else None
    WITH_RETAIL = args.with_retail
    if OUTPUT is not None and not OUTPUT.is_relative_to((ROOT / "build").resolve()):
        parser.error("--out-dir must be beneath this repository's build/")
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
