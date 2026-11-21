"""Exercise the actual PowerShell pacing classifier without launching game/load."""
import copy
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
POWERSHELL = shutil.which("powershell.exe") or shutil.which("pwsh")
FRAMES = 3300
VALID = {
    "frames": FRAMES, "seconds": FRAMES / 54.7068, "timed_out": False,
    "audio_initial_device": True, "audio_device": True, "audio_rate": 48000,
    "samples": 2895532, "audio_errors": 0, "audio_error_stage": "",
    "audio_error_code": "00000000", "audio_underruns": 0,
    "audio_min_queued_ms": 35.0, "sound_fault": "",
}

@unittest.skipUnless(POWERSHELL, "PowerShell is required for the actual gate")
class PacingGate(unittest.TestCase):
    def test_existing_output_is_preserved_without_launch(self):
        with tempfile.TemporaryDirectory(prefix="smash-pacing-preserve-") as directory:
            evidence = pathlib.Path(directory) / "host.json"
            evidence.write_bytes(b"prior evidence")
            run = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", str(ROOT / "tools/pacing_stress.ps1"), "-OutputDirectory", directory,
                "-Exe", "must-not-launch.exe", "-Burners", "0"],
                cwd=ROOT, capture_output=True, text=True, timeout=20)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("existing evidence is preserved", run.stderr)
            self.assertEqual(evidence.read_bytes(), b"prior evidence")

    def validate(self, changes=None, remove=()):
        report = copy.deepcopy(VALID)
        report.update(changes or {})
        for key in remove:
            del report[key]
        with tempfile.TemporaryDirectory(prefix="smash-pacing-classifier-") as directory:
            path = pathlib.Path(directory) / "host.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            run = subprocess.run(
                [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                 str(ROOT / "tools/pacing_stress.ps1"), "-Frames", str(FRAMES),
                 "-ValidateReport", str(path)],
                cwd=ROOT, capture_output=True, text=True, timeout=20)
        self.assertIn(run.returncode, (0, 1), run.stdout + run.stderr)
        try:
            result = json.loads(run.stdout)
        except json.JSONDecodeError:
            self.fail("Classifier failed before producing a verdict: " + run.stdout + run.stderr)
        self.assertEqual(run.returncode == 0, result["result"] == "PASS")
        return result

    def test_valid_audio_and_timing(self):
        r = self.validate()
        self.assertEqual(r["result"], "PASS")
        self.assertTrue(r["timing_pass"])
        self.assertTrue(r["audio_required_pass"])

    def test_no_endpoint_and_zero_underruns_is_blocked(self):
        r = self.validate({"audio_initial_device": False, "audio_device": False,
                           "audio_rate": 0, "samples": 0, "audio_min_queued_ms": 0})
        self.assertEqual(r["result"], "BLOCKED_AUDIO_UNAVAILABLE")
        self.assertTrue(r["timing_pass"])
        self.assertFalse(r["audio_required_pass"])

    def test_endpoint_initialization_error_is_retained(self):
        r = self.validate({"audio_initial_device": False, "audio_device": False,
                           "audio_errors": 1, "audio_error_stage": "GetDefaultAudioEndpoint",
                           "audio_error_code": "80070490", "audio_min_queued_ms": 0})
        self.assertEqual(r["result"], "BLOCKED_AUDIO_UNAVAILABLE")
        self.assertEqual(r["audio_errors"], 1)
        self.assertEqual(r["audio_error_stage"], "GetDefaultAudioEndpoint")

    def test_device_loss_fails(self):
        self.assertEqual(self.validate({"audio_device": False})["result"], "FAIL")

    def test_missing_flags_fail(self):
        self.assertEqual(self.validate(remove=("audio_initial_device", "audio_device"))["result"], "FAIL")

    def test_string_flags_fail(self):
        self.assertEqual(self.validate({"audio_device": "true"})["result"], "FAIL")

    def test_incomplete_frames_fail(self):
        self.assertEqual(self.validate({"frames": FRAMES - 1})["result"], "FAIL")

    def test_timing_failure_fails(self):
        self.assertEqual(self.validate({"seconds": VALID["seconds"] * 1.02})["result"], "FAIL")

    def test_underrun_fails(self):
        self.assertEqual(self.validate({"audio_underruns": 1})["result"], "FAIL")

    def test_audio_error_fails(self):
        self.assertEqual(self.validate({"audio_errors": 1, "audio_error_stage": "GetBuffer",
                                        "audio_error_code": "88890004"})["result"], "FAIL")

    def test_zero_queue_minimum_fails(self):
        self.assertEqual(self.validate({"audio_min_queued_ms": 0})["result"], "FAIL")

    def test_sound_fault_fails_even_without_endpoint(self):
        self.assertEqual(self.validate({"audio_initial_device": False, "audio_device": False,
                                        "sound_fault": "named sound failure"})["result"], "FAIL")

    def test_non_numeric_samples_fail(self):
        self.assertEqual(self.validate({"samples": "2895532"})["result"], "FAIL")

if __name__ == "__main__":
    unittest.main(verbosity=2)
