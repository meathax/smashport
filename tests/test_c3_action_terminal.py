"""Fail-closed boundaries for C3 source-bound sampled evidence."""
import copy
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/scenarios"))
import c3_action_terminal_audit as audit


def sample(frame, wake=None, circuit=1, wave=19, state=24, pid=0, address=0x01001000, complete=True):
    return {"frame": frame, "gamstate": state, "circuit": circuit, "wave": wave,
            "process_list_complete": complete,
            "processes": [{"address": address, "pid": pid, "wake": wake}] if wake is not None else []}


def sequence():
    return [sample(0, 0xffe7db20), sample(60, 0xffe7dfc0), sample(120, 0xffe7dfc0),
            sample(180, 0xffe7e560), sample(240, circuit=2, wave=1)]


class C3TerminalTests(unittest.TestCase):
    def test_ordered_source_bound_sequence_and_following_circuit(self):
        episode, = audit.terminal_episodes(sequence())
        self.assertTrue(episode["source_bound_sampled_terminal_progression"])
        self.assertEqual(episode["following_sample"]["circuit"], 2)
        self.assertFalse(episode["clearance_proof"])
        self.assertFalse(episode["continuous_process_lifetime_proof"])

    def test_selector_alone_never_proves_progression(self):
        rows = sequence()
        for row in rows:
            row["processes"] = []
        episode, = audit.terminal_episodes(rows)
        self.assertFalse(episode["source_bound_sampled_terminal_progression"])

    def test_wrong_state_is_not_terminal_phase(self):
        rows = sequence()
        for row in rows:
            row["gamstate"] = 16
        self.assertEqual(audit.terminal_episodes(rows), [])

    def test_wake_pc_requires_correct_controller_pid(self):
        rows = sequence()
        for row in rows:
            for process in row["processes"]:
                process["pid"] = 247
        self.assertFalse(audit.terminal_episodes(rows)[0]["source_bound_sampled_terminal_progression"])

    def test_missing_out_of_order_or_partial_phase_fails(self):
        cases = []
        rows = sequence()
        rows[0]["processes"] = []
        cases.append(rows)
        rows = sequence()
        rows[0]["processes"][0]["wake"], rows[3]["processes"][0]["wake"] = 0xffe7e560, 0xffe7db20
        cases.append(rows)
        rows = sequence()
        rows[1]["process_list_complete"] = False
        cases.append(rows)
        rows = sequence()
        rows[3]["processes"][0]["address"] += 0x1000
        cases.append(rows)
        rows = sequence()
        rows[3]["processes"].append(dict(rows[3]["processes"][0], address=0x01002000))
        cases.append(rows)
        for rows in cases:
            with self.subTest(rows=rows):
                self.assertFalse(audit.terminal_episodes(rows)[0]["source_bound_sampled_terminal_progression"])

    def test_missing_or_wrong_next_circuit_fails(self):
        for rows in (sequence()[:-1], sequence()[:-1] + [sample(240, circuit=0, wave=1)]):
            self.assertFalse(audit.terminal_episodes(rows)[0]["source_bound_sampled_terminal_progression"])

    def test_circuit_three_uses_retail_23_and_observes_reset(self):
        rows = sequence()
        for row in rows[:-1]:
            row.update(circuit=2, wave=23)
        rows[-1].update(circuit=0, wave=23)
        self.assertTrue(audit.terminal_episodes(rows)[0]["source_bound_sampled_terminal_progression"])
        for row in rows[:-1]:
            row["wave"] = 22
        self.assertEqual(audit.terminal_episodes(rows), [])

    def test_only_terminal_capture_status_is_accepted(self):
        good = {"task": "capture", "status": "PASS", "exit_code": 0, "live_child": False, "finished_utc": "recorded"}
        audit.validate_capture_status(good)
        for key, value in (("status", "RUNNING"), ("status", "FAIL"), ("exit_code", 1), ("live_child", True), ("finished_utc", None)):
            with self.subTest(key=key, value=value):
                with self.assertRaises(audit.runner.ProofError):
                    audit.validate_capture_status(dict(good, **{key: value}))

    def test_noncontiguous_samples_fail(self):
        rows = sequence()
        rows[2]["frame"] = 121
        with self.assertRaises(audit.runner.ProofError):
            audit.terminal_episodes(rows)


class C3ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = audit.contracts()

    def test_current_source_and_retail_contract(self):
        self.assertEqual(self.contract["coverage_size"], 33295)
        self.assertEqual(self.contract["retail_source_difference"]["source_circuit_2_terminal"], 22)
        self.assertEqual(self.contract["retail_source_difference"]["retail_circuit_2_terminal"], 23)

    def test_coverage_size_and_values_fail_closed(self):
        for data in (bytes(33294), bytes(33296), bytes(33294) + b"\x02"):
            with self.assertRaises(audit.runner.ProofError):
                audit.coverage_actions(data, self.contract)

    def test_execution_bits_do_not_become_player_or_lifetime_proof(self):
        report = audit.coverage_actions(bytes([1]) * 33295, self.contract)
        self.assertTrue(report["projectile_player_number_write"])
        self.assertEqual(len(report["weapons"]), 5)
        for weapon in report["weapons"]:
            self.assertTrue(all(weapon["sites_executed"].values()))
            self.assertFalse(weapon["player_event_proof"])
            self.assertFalse(weapon["lifetime_proof"])

    def test_individual_site_is_not_implied_by_other_sites(self):
        data = bytearray(33295)
        data[self.contract["retail"]["ffe33840"]["coverage_index"]] = 1
        report = audit.coverage_actions(data, self.contract)
        normal = report["weapons"][0]
        self.assertTrue(normal["sites_executed"]["create_call"])
        self.assertFalse(normal["sites_executed"]["projectile_entry"])
        self.assertFalse(report["projectile_player_number_write"])


if __name__ == "__main__":
    unittest.main()
