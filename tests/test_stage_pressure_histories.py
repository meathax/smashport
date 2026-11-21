from pathlib import Path
import hashlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/scenarios'))
from summarize_stage_audit import distinct_pool, input_prefixes, verify_report
from stage_integrity import read_integrity, censor_pressure
from test_sampled_damage_tags import SampledDamageTests


def capture(player=1, executable='same', deaths=2):
    row = dict(player=player, frame=20, circuit=0, wave=5, frames=10,
               deaths=deaths, avg=5.0, peak=8, line=3)
    return dict(observations=[row], partial_visits=[dict(frame=25)],
                boundary_life_decreases=[], executable=executable,
                input_prefixes={20: 'input-history'})


class InputHistoryTests(unittest.TestCase):
    def test_prefix_includes_every_event_at_the_endpoint(self):
        records = [b'1 :IN0 1 1\n', b'20 :IN0 1 0\n',
                   b'20 :IN1 4 1\n', b'21 :IN1 4 0\n']
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'inputs.txt'
            path.write_bytes(b''.join(records))
            values = input_prefixes(path, [20, 0, 20, 21])
        self.assertEqual(values[0], hashlib.sha256(b'').hexdigest())
        self.assertEqual(values[20], hashlib.sha256(b''.join(records[:3])).hexdigest())
        self.assertEqual(values[21], hashlib.sha256(b''.join(records)).hexdigest())

    def test_duplicate_visits_keep_other_players_builds_and_partial_visits(self):
        original = capture()
        result = distinct_pool([('first', original), ('repeat', capture()),
                                ('player-two', capture(player=2)),
                                ('other-build', capture(executable='other'))])
        self.assertEqual([r['source_run'] for r in result['observations']],
                         ['first', 'player-two', 'other-build'])
        self.assertEqual(result['duplicate_observations'][0]['duplicate_of'], 'first')
        self.assertEqual(len(result['partial_visits']), 4)
        self.assertNotIn('source_run', original['observations'][0])

    def test_conflicting_observations_fail(self):
        with self.assertRaisesRegex(ValueError, 'different pressure results'):
            distinct_pool([('first', capture()), ('conflict', capture(deaths=3))])

    def test_unsorted_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'inputs.txt'
            path.write_bytes(b'20 :IN0 1 1\n1 :IN0 1 0\n')
            with self.assertRaisesRegex(ValueError, 'Out-of-order'):
                input_prefixes(path, [30])


class IntegrityTests(unittest.TestCase):
    def fixture(self, folder, mode='classic', first=0, samples=None):
        directory = Path(folder) / (mode+'-1p-seed0')
        directory.mkdir()
        sidecar = directory/'list-integrity.csv'
        sidecar.write_bytes(f'{first},1000040,39d0000\n'.encode() if first else b'')
        count = samples if samples is not None else (first or 2600)-2500+1
        text = f'LIST_INTEGRITY samples={count} first_invalid_frame={first}\n'
        log = directory/'run.log';log.write_bytes(text.encode())
        health = read_integrity(directory,text,2600)
        job = dict(state='PASS',exit_code=0,terminal=True,frames=2600,
                   log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(),list_integrity=health)
        report = dict(result='COMPLETE',live_pids=[],require_integrity=True,
                      jobs={directory.name:job})
        return directory,report,job

    def test_clean_complete_health_and_legacy_are_accepted(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,report,_ = self.fixture(folder)
            verify_report(Path(folder)/'report.json',report)
            self.assertIsNone(read_integrity(directory,(directory/'run.log').read_text(),2600)['first_invalid_frame'])
            report.pop('require_integrity')
            verify_report(Path(folder)/'report.json',report)

    def test_changed_integrity_sidecar_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,report,_ = self.fixture(folder,first=2550)
            (directory/'list-integrity.csv').write_bytes(b'2551,1000040,39d0000\n')
            with self.assertRaises(ValueError):verify_report(Path(folder)/'report.json',report)

    def test_false_clean_receipt_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            _,report,job = self.fixture(folder,first=2550)
            job['list_integrity']['first_invalid_frame']=None
            with self.assertRaisesRegex(ValueError,'receipt differs'):
                verify_report(Path(folder)/'report.json',report)

    def test_widescreen_failure_cannot_be_admitted_as_success(self):
        with tempfile.TemporaryDirectory() as folder:
            _,report,_ = self.fixture(folder,mode='wide',first=2550)
            with self.assertRaisesRegex(ValueError,'Invalid widescreen'):
                verify_report(Path(folder)/'report.json',report)

    def test_short_sampling_cannot_claim_clean_endpoint(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError,'Incomplete'):
                self.fixture(folder,samples=100)

    def test_corrupted_visits_and_crossing_partials_do_not_enter_ratios(self):
        original = capture()
        original['observations'].append(dict(original['observations'][0],frame=30,deaths=100))
        original['boundary_life_decreases']=[dict(frame=31,life_decrease=9)]
        clean,excluded = censor_pressure(original,25)
        self.assertEqual(sum(r['deaths'] for r in clean['observations']),2)
        self.assertEqual(clean['partial_visits'],[])
        self.assertEqual(clean['boundary_life_decreases'],[])
        self.assertEqual(sorted((r['frame'],r['kind']) for r in excluded),
                         [(25,'partial_visits'),(30,'observations'),(31,'boundary_life_decreases')])
        self.assertEqual(len(original['observations']),2)
        self.assertEqual(censor_pressure(original,None),(original,[]))


if __name__ == '__main__':
    unittest.main()
