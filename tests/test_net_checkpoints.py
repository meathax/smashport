"""Malformed/partial proof regressions for actual-peer periodic RAM validation."""
import copy
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from net_checkpoints import SCHEMA, compare_periodic


def fixture(frames=121):
    return {"resyncs": 0, "first_resync_base": None, "periodic_checkpoints": {
        "schema": SCHEMA, "enabled": True, "frames_requested": frames, "period": 30,
        "checkpoints": [[f, f"{7 + f * 17:08x}"] for f in range(0, frames, 30)],
        "resync_bases": []}}


class PeriodicProofTests(unittest.TestCase):
    def setUp(self):
        self.reference = fixture()
        self.host = copy.deepcopy(self.reference)
        self.join = copy.deepcopy(self.reference)

    def run_case(self, fault=0, frames=121):
        return compare_periodic(self.reference, self.host, self.join, frames, fault)

    def rejected(self, fragment, fault=0, frames=121):
        with self.assertRaisesRegex(ValueError, fragment):
            self.run_case(fault, frames)

    def inject(self, after_hash="deadbeef"):
        self.join["resyncs"] = 1
        self.join["first_resync_base"] = 91
        self.join["periodic_checkpoints"]["resync_bases"] = [91]
        self.join["periodic_checkpoints"]["checkpoints"][2][1] = after_hash
        self.join["fault_injection"] = {
            "schema": "smash.net.fault-injection.v1",
            "mode": "single_exchanged_hash_frame_xor_8000", "enabled": True,
            "requested_frame": 59, "effective_frame": 60, "hash_period": 30,
            "attempts": 1, "events": [{"frame": 60, "after_hash": after_hash}]}

    def test_complete_exact_periods(self):
        result = self.run_case()
        self.assertEqual((result["checkpoints_per_peer"], result["host_matching"], result["join_matching"]), (5, 5, 5))
        self.assertFalse(result["complete_C6_gate_proof"])

    def test_extent_without_terminal_checkpoint(self):
        self.reference = fixture(120)
        self.host = copy.deepcopy(self.reference)
        self.join = copy.deepcopy(self.reference)
        self.assertEqual(self.run_case(frames=120)["checkpoints_per_peer"], 4)

    def test_injection_is_retained_not_hidden(self):
        self.inject()
        result = self.run_case(fault=59)
        self.assertEqual(result["join_matching"], 4)
        self.assertEqual(result["injected_pre_resync_differences"][0]["frame"], 60)
        self.assertEqual(result["first_resync_base"], 91)

    def test_fault_without_periodic_mismatch_still_binds_observed_hash(self):
        self.inject(self.reference["periodic_checkpoints"]["checkpoints"][2][1])
        self.assertEqual(self.run_case(fault=59)["join_matching"], 5)

    def test_selected_checkpoint_must_match_final_observed_injection(self):
        self.inject()
        self.join["periodic_checkpoints"]["checkpoints"][2][1] = "01234567"
        self.rejected("final recorded injection hash", 59)

    def test_final_replay_hash_is_used_not_an_earlier_prediction(self):
        self.inject()
        observed = self.join["fault_injection"]
        observed["events"].append({"frame": 60, "after_hash": "01234567"})
        observed["attempts"] = 2
        self.rejected("final recorded injection hash", 59)
        self.join["periodic_checkpoints"]["checkpoints"][2][1] = "01234567"
        result = self.run_case(fault=59)
        self.assertEqual(result["selected_injection_checkpoint"],
                         {"frame": 60, "after_hash": "01234567"})

    def test_missing_or_partial_injection_observations_reject(self):
        self.inject()
        original = copy.deepcopy(self.join["fault_injection"])
        for observed in (None, {}, dict(original, events=[]),
                         dict(original, attempts=2), dict(original, attempts=True)):
            self.join["fault_injection"] = observed
            with self.subTest(observed=observed), self.assertRaises(ValueError):
                self.run_case(fault=59)

    def test_injection_contract_and_boundary_must_match(self):
        self.inject()
        original = copy.deepcopy(self.join["fault_injection"])
        for field, value in (("schema", "unknown"), ("mode", "or_8000"), ("enabled", 1),
                             ("requested_frame", 60), ("effective_frame", True), ("hash_period", 60)):
            self.join["fault_injection"] = dict(original, **{field: value})
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.run_case(fault=59)

    def test_malformed_final_injection_event_rejects(self):
        self.inject()
        for event in (None, {}, {"frame": 61, "after_hash": "deadbeef"},
                      {"frame": True, "after_hash": "deadbeef"},
                      {"frame": 60, "after_hash": "DEADBEEF"},
                      {"frame": 60, "after_hash": None}):
            self.join["fault_injection"]["events"] = [event]
            with self.subTest(event=event), self.assertRaises(ValueError):
                self.run_case(fault=59)

    def test_later_difference_is_waived_without_causal_attribution(self):
        self.inject()
        self.join["periodic_checkpoints"]["checkpoints"][3][1] = "01234567"
        result = self.run_case(fault=59)
        changes = result["injected_pre_resync_differences"]
        self.assertEqual([change["frame"] for change in changes], [60, 90])
        self.assertEqual(changes[0]["classification"], "observed_selected_injection_hash_difference")
        self.assertEqual(changes[1]["classification"], "waived_pre_resync_difference_unattributed")

    def test_host_mismatch_is_never_waived(self):
        self.inject()
        self.host["periodic_checkpoints"]["checkpoints"][2][1] = "deadbeef"
        self.rejected("host RAM differs", 59)

    def test_join_before_fault_is_not_waived(self):
        self.inject()
        self.join["periodic_checkpoints"]["checkpoints"][1][1] = "deadbeef"
        self.rejected("outside the injected interval", 59)

    def test_join_after_resync_is_not_waived(self):
        self.inject()
        self.join["periodic_checkpoints"]["checkpoints"][4][1] = "deadbeef"
        self.rejected("outside the injected interval", 59)

    def test_resync_boundary_is_exclusive(self):
        self.inject()
        self.join["periodic_checkpoints"]["resync_bases"] = [60]
        self.rejected("actual later resync boundary", 59)

    def test_missing_report(self):
        del self.host["periodic_checkpoints"]
        self.rejected("missing periodic checkpoint report")

    def test_missing_checkpoint(self):
        self.join["periodic_checkpoints"]["checkpoints"].pop()
        self.rejected("incomplete or extra")

    def test_extra_checkpoint(self):
        self.join["periodic_checkpoints"]["checkpoints"].append([150, "00000001"])
        self.rejected("incomplete or extra")

    def test_duplicate_checkpoint(self):
        self.join["periodic_checkpoints"]["checkpoints"][2][0] = 30
        self.rejected("missing, repeated or out of order")

    def test_wrong_hash(self):
        self.join["periodic_checkpoints"]["checkpoints"][2][1] = "deadbeef"
        self.rejected("outside the injected interval")

    def test_noncanonical_hash(self):
        self.join["periodic_checkpoints"]["checkpoints"][2][1] = "ABCDEF12"
        self.rejected("invalid checkpoint RAM hash")

    def test_wrong_period(self):
        self.join["periodic_checkpoints"]["period"] = 60
        self.rejected("period differs")

    def test_wrong_extent(self):
        self.join["periodic_checkpoints"]["frames_requested"] = 120
        self.rejected("extent differs")

    def test_disabled_audit(self):
        self.join["periodic_checkpoints"]["enabled"] = False
        self.rejected("audit was disabled")

    def test_integer_enabled_is_rejected(self):
        self.join["periodic_checkpoints"]["enabled"] = 1
        self.rejected("audit was disabled")

    def test_boolean_frame_is_rejected(self):
        self.join["periodic_checkpoints"]["checkpoints"][0][0] = False
        self.rejected("missing, repeated or out of order")

    def test_missing_resync_boundaries(self):
        del self.join["periodic_checkpoints"]["resync_bases"]
        self.rejected("missing resync boundary evidence")

    def test_unrequested_resync(self):
        self.inject()
        self.rejected("unexpected resync boundaries")

    def test_fault_without_resync(self):
        self.rejected("actual later resync boundary", 59)

    def test_resync_counter_disagrees(self):
        self.inject()
        self.join["resyncs"] = 2
        self.rejected("resync count", 59)

    def test_resync_observations_must_agree(self):
        self.inject()
        self.join["first_resync_base"] = 61
        self.rejected("first resync observation", 59)

    def test_out_of_run_resync(self):
        self.inject()
        self.join["periodic_checkpoints"]["resync_bases"] = [122]
        self.rejected("out-of-run resync boundary", 59)

    def test_duplicate_resync(self):
        self.inject()
        self.join["periodic_checkpoints"]["resync_bases"] = [91, 91]
        self.join["resyncs"] = 2
        self.rejected("repeated", 59)

    def test_host_cannot_claim_join_load(self):
        self.host["periodic_checkpoints"]["resync_bases"] = [91]
        self.rejected("authoritative host")

    def test_fault_boundary_outside_run(self):
        self.reference = fixture(100)
        self.host = copy.deepcopy(self.reference)
        self.join = copy.deepcopy(self.reference)
        self.rejected("exchanged hash falls outside", 91, 100)

    def test_negative_fault(self):
        self.rejected("fault must be disabled", -1)

    def test_boolean_fault(self):
        self.rejected("fault must be disabled", True)

    def test_zero_extent(self):
        self.rejected("fault must be disabled", frames=0)


class DriverCheckpointEnvironmentTests(unittest.TestCase):
    def test_parent_flag_cannot_enable_an_unrequested_capture(self):
        import os
        import tempfile
        from types import SimpleNamespace
        from unittest.mock import patch
        import net_exe_peer as peer
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            rom = path / "fixture.zip"
            rom.write_bytes(b"fixture bytes, never launched as a game")
            args = SimpleNamespace(rom=rom, profile="reference", inputs=path/"inputs",
                                   frames=121, port=27015, proxy_port=27016, fault=0,
                                   checkpoint_audit=False)
            with patch.dict(os.environ, {"STV_NET_CHECKPOINT_AUDIT": "1"}):
                child = peer.child_environment(path, "host", args)
                self.assertNotIn("STV_NET_CHECKPOINT_AUDIT", child)
                args.checkpoint_audit = True
                child = peer.child_environment(path, "join", args)
                self.assertEqual(child["STV_NET_CHECKPOINT_AUDIT"], "1")
                self.assertEqual(os.environ["STV_NET_CHECKPOINT_AUDIT"], "1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
