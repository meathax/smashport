#!/usr/bin/env python3
"""Reject empty, shortened and malformed op-fuzz proofs before they can PASS."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build/msvc/stv_op_fuzz.exe"
ROM = ROOT / "original/user-supplied-roms/smashtv.zip"
HEADER = "IO " + " ".join(["0000"] * 32) + "\n"
VECTOR = " ".join(["00000000"] * 33) + "\n"
RESULT = " ".join(["00000000"] * 34) + "\n"


class InputValidation(unittest.TestCase):
    def run_case(self, vectors, results):
        with tempfile.TemporaryDirectory(prefix="stv-op-fuzz-") as directory:
            folder = Path(directory)
            v, r = folder / "vectors.txt", folder / "results.txt"
            v.write_text(vectors, encoding="ascii")
            r.write_text(results, encoding="ascii")
            return subprocess.run([str(EXE), str(ROM), str(v), str(r)],
                                  capture_output=True, text=True, timeout=30)

    def rejected(self, vectors, results, reason):
        run = self.run_case(vectors, results)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn(reason, run.stderr)
        self.assertNotIn("PASS", run.stdout)

    def test_empty_pair(self):
        self.rejected("", HEADER, "empty comparison")

    def test_short_oracle(self):
        self.rejected(VECTOR, HEADER, "length mismatch")

    def test_short_vectors(self):
        self.rejected("", HEADER + RESULT, "length mismatch")

    def test_blank_rows_are_not_skipped(self):
        self.rejected("\n", HEADER + "\n", "malformed")

    def test_short_io_header(self):
        self.rejected(VECTOR, "IO 0000\n" + RESULT, "32 hexadecimal registers")

    def test_wide_io_register(self):
        self.rejected(VECTOR, HEADER.replace("0000", "10000", 1) + RESULT,
                      "exceeds 16 bits")

    def test_partly_hexadecimal_token(self):
        self.rejected(VECTOR.replace("00000000", "00000000junk", 1),
                      HEADER + RESULT, "malformed")

    def test_overflowing_token(self):
        self.rejected(VECTOR.replace("00000000", "100000000", 1),
                      HEADER + RESULT, "malformed")

    def test_short_result_row(self):
        self.rejected(VECTOR, HEADER + "00000000\n", "malformed")

    def test_valid_rows_reach_comparison(self):
        # Deliberately wrong expected state: valid inputs must report mismatch,
        # rather than being skipped or rejected as malformed proof.
        run = self.run_case(VECTOR, HEADER + RESULT)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertIn("FAIL: 1 vectors", run.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=EXE)
    parser.add_argument("--rom", type=Path, default=ROM)
    args, unittest_args = parser.parse_known_args()
    EXE, ROM = args.exe.resolve(), args.rom.resolve()
    unittest.main(argv=[__file__] + unittest_args)
