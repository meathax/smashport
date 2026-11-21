#!/usr/bin/env python3
"""Guard the gap manifest against stale pins and overstated clearance."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/scenarios"))
import coverage_gap_manifest as gaps


def sample(frame, wave=1, circuit=0, state=0x10, x=472, y=338,
           objects_complete=True, processes_complete=True):
    return dict(frame=frame, wave=wave, circuit=circuit, gamstate=state,
                world_x=x, world_y=y, object_list_complete=objects_complete,
                process_list_complete=processes_complete)


def report():
    steps = []
    for name, _, _, _ in gaps.runner.CASES:
        steps.extend(dict(name=name + suffix, state="PASS", exit_code=0)
                     for suffix in ("-native", "-diff"))
    return dict(result="PROOFS_COMPLETE_TARGET_CLOSURE_OPEN",
                owned_live_pids=[], steps=steps)


class PresenceTests(unittest.TestCase):
    def test_selector_change_is_not_a_room_clear(self):
        result = gaps.observations([sample(0), sample(60), sample(120, wave=2)])
        self.assertEqual(result["cleared_room_count"], 0)
        self.assertTrue(all(row["clearance_state"] == "UNVERIFIED" for row in result["selectors"]))
        self.assertEqual(result["capture_ended_during_selector"], [0, 2])

    def test_revisit_is_a_new_sampled_span(self):
        result = gaps.observations([sample(0), sample(60, state=0x18), sample(120)])
        self.assertEqual(result["selectors"][0]["sampled_spans"], 2)
        self.assertEqual(result["selectors"][0]["samples"], 2)
        self.assertEqual(result["cleared_room_count"], 0)

    def test_partial_lists_remain_explicit(self):
        result = gaps.observations([sample(0, objects_complete=False),
                                    sample(60, processes_complete=False)])
        self.assertEqual(result["totals"]["partial_object_samples"], 1)
        self.assertEqual(result["totals"]["partial_process_samples"], 1)
        self.assertEqual(result["selectors"][0]["partial_object_samples"], 1)

    def test_non_game_state_does_not_claim_a_visit(self):
        result = gaps.observations([sample(0, state=0x18), sample(60, state=0x50)])
        self.assertEqual(result["selectors"], [])
        self.assertIsNone(result["capture_ended_during_selector"])

    def test_camera_decrease_is_only_a_candidate(self):
        result = gaps.observations([sample(0, x=600), sample(60, x=550)])
        self.assertEqual(len(result["leftward_candidates"]), 1)
        self.assertIn("NOT_VISUAL_OR_ROUTE_PROOF", result["leftward_candidates"][0]["state"])
        self.assertEqual(result["cleared_room_count"], 0)

    def test_new_circuit_camera_reset_is_not_leftward_candidate(self):
        result = gaps.observations([sample(0, circuit=2, x=4000),
                                    sample(60, circuit=0, x=472)])
        self.assertEqual(result["leftward_candidates"], [])

    def test_gap_or_empty_stream_is_rejected(self):
        for data in ([], [sample(60)], [sample(0), sample(120)]):
            with self.subTest(data=data), self.assertRaises(gaps.runner.ProofError):
                gaps.observations(data)


class ProvenanceTests(unittest.TestCase):
    def test_missing_inventory_is_a_gap_not_an_empty_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            pin = dict(path=str(Path(directory) / "absent.json"), bytes=3, sha256="a" * 64)
            result = gaps.inventory_state(pin)
            self.assertEqual(result["state"], "MISSING_PINNED_ARTIFACT")
            self.assertEqual(result["expected"], pin)
            self.assertFalse(result["gate_proof"])

    def test_changed_pin_rejected_and_inventory_mismatch_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "artifact.json"
            path.write_bytes(b"original")
            pin = gaps.runner.pin(path)
            self.assertEqual(gaps.checked(pin), path)
            path.write_bytes(b"modified")
            with self.assertRaises(gaps.runner.ProofError):
                gaps.checked(pin)
            self.assertEqual(gaps.inventory_state(pin)["state"], "PIN_MISMATCH")

    def test_complete_steps_require_terminal_all_case_proof(self):
        original = report()
        self.assertEqual(len(gaps.complete_steps(original)), 2 * len(gaps.runner.CASES))
        variants = []
        x = copy.deepcopy(original); x["owned_live_pids"] = [123]; variants.append(x)
        x = copy.deepcopy(original); x["steps"].pop(); variants.append(x)
        x = copy.deepcopy(original); x["steps"].append(copy.deepcopy(x["steps"][0])); variants.append(x)
        x = copy.deepcopy(original); x["steps"][0]["state"] = "RUNNING"; variants.append(x)
        x = copy.deepcopy(original); x["steps"][0]["exit_code"] = 1; variants.append(x)
        for x in variants:
            with self.subTest(report=x), self.assertRaises(gaps.runner.ProofError):
                gaps.complete_steps(x)

    def test_header_schema_and_cadence_are_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "content.jsonl"
            for header in (dict(schema="untrusted", cadence=60),
                           dict(schema="stv.content.samples.v1", cadence=30)):
                path.write_text(json.dumps(header) + "\n" + json.dumps(sample(0)) + "\n")
                with self.assertRaises(gaps.runner.ProofError):
                    list(gaps.samples(path))



class RouteControllerTests(unittest.TestCase):
    def test_native_terminal_rejects_unconsumed_input_and_partial_endpoint(self):
        import replay_c3_routes as replay
        text = "native: frames=55 cycles=100 pc=ffe00000 faults=256 hcount_left=0 input_left=0 input_mismatch=0 big_field=0\n"
        self.assertEqual(replay.terminal(text, 55)["observed_fault_count"], 256)
        for bad in (text.replace("input_left=0", "input_left=1"), text + text, text.replace("frames=55", "frames=54")):
            with self.subTest(bad=bad), self.assertRaises(gaps.runner.ProofError):
                replay.terminal(bad, 55)

    def test_prefix_comparison_marker_is_rejected(self):
        import replay_c3_routes as replay
        self.assertEqual(replay.strict_marker("NO_DIVERGENCE over 56 complete frames (both streams ended)", 55)["frames"], 56)
        with self.assertRaises(gaps.runner.ProofError):
            replay.strict_marker("NO_DIVERGENCE over 56 frames (allowed prefix)", 55)

    def test_incomplete_and_duplicate_route_receipts_are_rejected(self):
        import replay_c3_routes as replay
        doc = dict(schema="stv.c3.route_replay.v1", result=replay.RESULT, owned_live_pids=[],
            pins=dict(exe=dict(sha256=replay.V8_SHA)),
            cases=[dict(name=name, final_frame=values[0], state="PASS") for name,values in replay.CASES.items()],
            steps=[dict(name=prefix+name, state="PASS", exit_code=0)
                   for name in replay.CASES for prefix in ("native-", "diff-")])
        self.assertEqual(len(replay.validate_completed(doc)), 6)
        for key in ("cases", "steps"):
            changed = copy.deepcopy(doc); changed[key].append(changed[key][0])
            with self.assertRaises(gaps.runner.ProofError):
                replay.validate_completed(changed)
        changed = copy.deepcopy(doc); changed["owned_live_pids"]=[123]
        with self.assertRaises(gaps.runner.ProofError):
            replay.validate_completed(changed)

    def test_wrong_native_executable_rejected_before_preparation(self):
        import replay_c3_routes as replay
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "wrong.exe"; exe.write_bytes(b"wrong")
            with self.assertRaises(gaps.runner.ProofError):
                replay.reference_executable(exe, {"pins": {}})
            proof = {"pins": {str(exe.resolve()): gaps.runner.pin(exe)}}
            self.assertEqual(replay.reference_executable(exe, proof), gaps.runner.pin(exe))
            exe.write_bytes(b"changed")
            with self.assertRaises(gaps.runner.ProofError):
                replay.reference_executable(exe, proof)

    def test_timeout_reaps_the_owned_dummy_child(self):
        import replay_c3_routes as replay
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = gaps.runner.Report(root, dict(result="TEST_ONLY", steps=[]))
            with self.assertRaises(gaps.runner.Cancelled):
                replay.bounded(report, "deadline-probe",
                    [sys.executable, "-c", "import time; time.sleep(30)"], root, 1)
            self.assertEqual(report.data["owned_live_pids"], [])
            self.assertTrue(report.cancelled.is_set())
            self.assertEqual(report.data["steps"][0]["state"], "CANCELLED")


if __name__ == "__main__":
    unittest.main()
