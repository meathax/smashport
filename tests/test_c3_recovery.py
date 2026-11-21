"""C3 recovery rejects ambiguous recipes and preserves exact historical bytes."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/scenarios"))
import recover_c3_variants as recover


class C3RecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.receipts = {name: json.loads((ROOT / path).read_text(encoding="utf-8"))
                        for name, path in recover.CASES}

    def test_real_prefixes_match_all_historical_pins(self):
        data = recover.BASE.read_bytes()
        expected = {
            "bonus": (55000, 15000, 0, 140735, "268d5fa9b6a78e2cb1d71eeca532ed8d633301e87cb08292a5aa36bbbfb3cdfe"),
            "lower": (110000, 43000, 2, 358670, "63aa88a73a86fec9be0368c4f243a9dcc78f7d25f90e49c37a3e9cd0b11c4903"),
            "scorpion": (160000, 102000, 0, 891344, "bbeb4bdfd3a1d77de64001320a5b1b50fd7b0150535fda030cf2c66e71d026bf"),
        }
        for name, receipt in self.receipts.items():
            with self.subTest(name=name):
                spec = recover.recipe(receipt)
                prefix = recover.prefix_bytes(data, spec["bot_after"])
                final, after, seed, size, sha = expected[name]
                self.assertEqual((spec["final"], spec["bot_after"], spec["bot_seed"]), (final, after, seed))
                self.assertEqual((len(prefix), hashlib.sha256(prefix).hexdigest()), (size, sha))
                recover.check_pin(prefix, spec["prefix"], name)

    def test_preserves_same_frame_order_and_excludes_handoff(self):
        data = b"0 :IN1 1 1\r\n0 :IN1 2 1\r\n10 :IN0 10 0\r\n"
        self.assertEqual(recover.prefix_bytes(data, 10), b"0 :IN1 1 1\n0 :IN1 2 1\n")

    def test_schedule_rejects_malformed_or_ambiguous_events(self):
        bad = (b"1 :IN0 1 1\n0 :IN0 2 1\n", b"0 :IN0 0 1\n", b"0 :IN0 10000 1\n",
               b"0 :DSW 1 1\n", b"0 :IN0 1 2\n", b"0 :IN0 1 1 extra\n", b"", b"\xff\n")
        for data in bad:
            with self.subTest(data=data):
                with self.assertRaises(recover.runner.ProofError):
                    recover.events(data)
        with self.assertRaises(recover.runner.ProofError):
            recover.events(b"2 :IN0 1 1\n", final=1)

    def test_command_is_data_not_arbitrary_execution(self):
        original = self.receipts["lower"]
        mutations = [
            lambda r: r["steps"][0]["command"].__setitem__(4, "--load-state"),
            lambda r: r["steps"][0]["command"].append("--unexpected"),
            lambda r: r["steps"][0]["command"].__setitem__(11, "-1"),
            lambda r: r["steps"][0]["command"].__setitem__(13, "seed"),
            lambda r: r["steps"][0]["command"].__setitem__(0, "powershell.exe"),
            lambda r: r["steps"][0]["command"].__setitem__(2, "some-other.bin"),
            lambda r: r["steps"][0]["environment"].__setitem__("STV_BOT_LEGACY", "1"),
            lambda r: r["steps"][0].__setitem__("state", "RUNNING"),
            lambda r: r["steps"][0]["proof"]["endpoint"].__setitem__("last_frame", 100),
            lambda r: r.__setitem__("gate_proof", True),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                receipt = copy.deepcopy(original)
                mutate(receipt)
                with self.assertRaises(recover.runner.ProofError):
                    recover.recipe(receipt)

    def test_pin_requires_both_size_and_digest(self):
        data = b"a\n"
        good = {"bytes": 2, "sha256": hashlib.sha256(data).hexdigest()}
        recover.check_pin(data, good, "fixture")
        for key, value in (("bytes", 3), ("sha256", "0" * 64)):
            bad = dict(good, **{key: value})
            with self.assertRaises(recover.runner.ProofError):
                recover.check_pin(data, bad, "fixture")

    def test_duplicate_pin_is_rejected(self):
        receipt = copy.deepcopy(self.receipts["bonus"])
        pin = copy.deepcopy(recover.receipt_pin(receipt, "prefix.inputs"))
        receipt["pins"]["duplicate"] = pin
        with self.assertRaises(recover.runner.ProofError):
            recover.receipt_pin(receipt, "prefix.inputs")

    def test_reproduction_environment_discards_external_bot_overrides(self):
        from unittest.mock import patch
        with patch.dict("os.environ", {"STV_BOT_LEGACY": "1", "STV_BOT_DEBUG": "1", "STV_LS_DUMP_EVERY": "1"}):
            env = recover.runner.child_environment()
        self.assertEqual({key: val for key, val in env.items() if key.startswith("STV_")}, recover.ENV)


if __name__ == "__main__":
    unittest.main()
