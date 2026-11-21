#!/usr/bin/env python3
"""Focused failure-mode checks for the C2 coverage and sound validators."""
import array
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import wave


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools" / "lockstep"
RATE = 48000
# Ten video frames per command; two commands need 17548 complete samples.
END = 20 * 506 * 289 * RATE // 8000000


class C2Validators(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)

    def run_tool(self, name, *args):
        return subprocess.run(
            [sys.executable, str(TOOLS / name), *map(str, args)],
            cwd=ROOT, capture_output=True, text=True, timeout=30,
        )

    def coverage(self, *data):
        index = self.path / "index.csv"
        index.write_text(
            "address,instruction,source,chunk,status\n"
            "00000000,MOVE A0,A1,MAIN:1,0,emitted\n"
            "00000010,JUMP A2,MAIN:2,0,emitted\n"
            "00000020,EXGPC A7,UTIL:1,0,emitted\n",
            encoding="utf-8",
        )
        covs = []
        for i, value in enumerate(data):
            cov = self.path / f"run{i}.cov"
            cov.write_bytes(value)
            covs.append(cov)
        return self.run_tool("coverage.py", index, *covs)

    def wav(self, name, frames, rate=RATE, audible=False):
        path = self.path / name
        pcm = array.array("h", (
            int(8192 * math.sin(2 * math.pi * 440 * i / rate)) if audible else 0
            for i in range(frames)
        ))
        if sys.byteorder == "big":
            pcm.byteswap()
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(rate)
            output.writeframes(pcm.tobytes())
        return path

    def goldens(self, ref, native, *args):
        return self.run_tool(
            "snd_goldens.py", ref, native,
            "--start", 0, "--period", 10, "--from", 0, "--to", 1, *args,
        )

    def test_exact_coverage_runs_merge(self):
        result = self.coverage(b"\x01\x00\x00", b"\x00\x01\x01")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("executed 3/3 instructions (100.0%) over 2 run(s)", result.stdout)

    def test_truncated_coverage_rejected_even_with_complete_other_run(self):
        result = self.coverage(b"\x01\x01\x01", b"\x01\x01")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("expected 3", result.stderr)
        self.assertIn("got 2", result.stderr)
        self.assertNotIn("executed", result.stdout)

    def test_overlong_coverage_rejected(self):
        result = self.coverage(b"\x01\x01\x01\x00")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("expected 3", result.stderr)
        self.assertIn("got 4", result.stderr)

    def test_exact_size_non_boolean_coverage_rejected(self):
        result = self.coverage(b"\x01\x01\x01", b"\xff\xff\xff")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("invalid coverage byte 255", result.stderr)
        self.assertIn("expected 0 or 1", result.stderr)
        self.assertNotIn("executed", result.stdout)

    def test_complete_silent_windows_allow_different_tails(self):
        result = self.goldens(self.wav("ref.wav", END + 480), self.wav("nat.wav", END + 960))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {
            "commands": 2, "audible": 0, "silent": 2, "failed": [],
        })

    def test_complete_audible_windows_preserve_results(self):
        report = self.path / "report.json"
        result = self.goldens(
            self.wav("ref.wav", END, audible=True),
            self.wav("nat.wav", END, audible=True), "--json", report,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(report.read_text())
        self.assertEqual(data["summary"], {
            "commands": 2, "audible": 2, "silent": 0, "failed": [],
        })
        self.assertEqual([row["cmd"] for row in data["rows"]], [0, 1])
        self.assertTrue(all(row["status"] == "pass" for row in data["rows"]))
        self.assertTrue(all(row["level_diff_db"] == 0 for row in data["rows"]))

    def test_every_command_requires_complete_window_on_both_sides(self):
        for short_side in ("reference", "native"):
            with self.subTest(short_side=short_side):
                report = self.path / "report.json"
                ref = self.wav("ref.wav", END - 1 if short_side == "reference" else END)
                nat = self.wav("nat.wav", END - 1 if short_side == "native" else END)
                result = self.goldens(ref, nat, "--json", report)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                data = json.loads(report.read_text())
                self.assertEqual(data["summary"]["commands"], 2)
                self.assertEqual(data["summary"]["failed"], [1])
                self.assertEqual(data["rows"][1]["status"], "missing")

    def test_wrong_rate_rejected_on_both_sides(self):
        for wrong_side in ("reference", "native", "both"):
            with self.subTest(wrong_side=wrong_side):
                ref = self.wav("ref.wav", END, 44100 if wrong_side in ("reference", "both") else RATE)
                nat = self.wav("nat.wav", END, 44100 if wrong_side in ("native", "both") else RATE)
                result = self.goldens(ref, nat)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("48000", result.stderr)

    def test_truncated_declared_pcm_payload_rejected_on_both_sides(self):
        for short_side in ("reference", "native"):
            with self.subTest(short_side=short_side):
                ref, nat = self.wav("ref.wav", END + 480), self.wav("nat.wav", END + 480)
                short = ref if short_side == "reference" else nat
                # Leave every command window present, but truncate the declared tail.
                short.write_bytes(short.read_bytes()[:-8])
                result = self.goldens(ref, nat)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("truncated PCM", result.stderr)

    def test_empty_command_range_rejected(self):
        result = self.goldens(self.wav("ref.wav", END), self.wav("nat.wav", END), "--from", 2)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("command range", result.stderr)


if __name__ == "__main__":
    unittest.main()
