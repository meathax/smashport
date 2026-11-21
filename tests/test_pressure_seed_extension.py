import copy
from pathlib import Path
import sys
import unittest
import tempfile
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from extend_pressure_seeds import matrix, append_run, verify_binary_copy, checked_analysis_pin
from rebaseline import ProofError, pin


def report():
    jobs = [(f'{mode}-2p-seed3', mode, 2, 3, 100) for mode in ('reference','enhanced')]
    return dict(schema='stv.pressure_capture.v1', result='COMPLETE_OBSERVATIONS_GATE_OPEN',
        owned_live_pids=[], verified_at='runtime', jobs=jobs,
        steps=[dict(name=j[0],state='PASS',exit_code=0) for j in jobs])


class SeedExtensionTests(unittest.TestCase):
    def test_prior_parser_recovery_is_exact_and_cannot_cover_other_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'pressure.py';backup=Path(tmp)/'prior.py';other=Path(tmp)/'runtime.cpp'
            source.write_bytes(b'old parser');backup.write_bytes(source.read_bytes());old=pin(source)
            source.write_bytes(b'new parser');other.write_bytes(b'runtime');other_pin=pin(other);other.write_bytes(b'changed')
            with patch('extend_pressure_seeds.pressure.__file__',str(source)):
                self.assertEqual(checked_analysis_pin(old,backup),backup)
                with self.assertRaises(ProofError):checked_analysis_pin(other_pin,backup)
                backup.write_bytes(b'wrong parser')
                with self.assertRaises(ProofError):checked_analysis_pin(old,backup)

    def test_frozen_copy_requires_exact_bytes_and_unchanged_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'stv_l8_lockstep.exe';frozen=Path(tmp)/'frozen.exe'
            source.write_bytes(b'recorder');frozen.write_bytes(source.read_bytes())
            reference={'pins':{str(source):pin(source)}}
            verify_binary_copy(frozen,reference)
            frozen.write_bytes(b'changed!')
            with self.assertRaises(ProofError):verify_binary_copy(frozen,reference)
            frozen.write_bytes(b'recorder');source.write_bytes(b'changed!')
            with self.assertRaises(ProofError):verify_binary_copy(frozen,reference)

    def test_matched_seed_pair(self):
        self.assertEqual(len(matrix(report(), 'runtime')), 2)

    def test_incomplete_or_mismatched_batches_rejected(self):
        for key, value in [('result','RUNNING'), ('owned_live_pids',[42]), ('verified_at','other')]:
            bad=report();bad[key]=value
            with self.subTest(key=key), self.assertRaises(ProofError):matrix(bad,'runtime')
        for field,value in [(2,1),(3,4),(4,101)]:
            bad=report();job=list(bad['jobs'][1]);job[field]=value;bad['jobs'][1]=job
            with self.subTest(field=field), self.assertRaises(ProofError):matrix(bad,'runtime')
        bad=report();bad['steps'][1]['state']='FAIL'
        with self.assertRaises(ProofError):matrix(bad,'runtime')

    def test_duplicate_or_unpaired_seed_rejected(self):
        bad=report();bad['jobs'].append(bad['jobs'][0]);bad['steps'].append(bad['steps'][0])
        with self.assertRaises(ProofError):matrix(bad,'runtime')
        bad=report();bad['jobs'].pop();bad['steps'].pop()
        with self.assertRaises(ProofError):matrix(bad,'runtime')

    def test_no_duplicate_count_and_preserve_uncertainty(self):
        group={k:[] for k in ('observations','partial_visits','boundary_life_decreases')}
        run=dict(observations=[dict(frames=1000)],partial_visits=[dict(frames=900)],
                 boundary_life_decreases=[dict(life_decrease=2)])
        original=copy.deepcopy(run);append_run(group,'seed3',run)
        self.assertEqual(run,original)
        self.assertEqual(group['boundary_life_decreases'][0]['life_decrease'],2)
        before=copy.deepcopy(group)
        with self.assertRaises(ProofError):append_run(group,'seed3',run)
        self.assertEqual(group,before)


if __name__=='__main__':unittest.main()
