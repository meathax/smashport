import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/scenarios"))
from summarize_stage_audit import verify_report

class Integrity(unittest.TestCase):
    def test_valid_and_tampered_reports(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            job = root / "classic-2p-seed0"
            job.mkdir()
            log = job / "run.log"
            log.write_bytes(b"original observation")
            entry = dict(state="PASS", exit_code=0, terminal=True,
                         log_sha256=hashlib.sha256(log.read_bytes()).hexdigest())
            report = dict(result="COMPLETE", live_pids=[], jobs={job.name: entry})
            verify_report(root / "report.json", report)
            for key, bad in (("state", "FAIL"), ("exit_code", 1), ("terminal", False)):
                old = entry[key]
                entry[key] = bad
                with self.assertRaises(ValueError): verify_report(root / "report.json", report)
                entry[key] = old
            for key, name in (("csv_sha256", "entrances.csv"), ("coverage_sha256", "native.cov")):
                artifact = job / name
                artifact.write_bytes(b"recorded artifact")
                entry[key] = hashlib.sha256(artifact.read_bytes()).hexdigest()
                verify_report(root / "report.json", report)
                artifact.write_bytes(b"modified artifact")
                with self.assertRaises(ValueError): verify_report(root / "report.json", report)
                artifact.write_bytes(b"recorded artifact")
            log.write_bytes(b"changed observation")
            with self.assertRaises(ValueError): verify_report(root / "report.json", report)
            report["jobs"] = {}
            with self.assertRaises(ValueError): verify_report(root / "report.json", report)

if __name__ == "__main__": unittest.main()
