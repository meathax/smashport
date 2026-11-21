import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from summarize_opening_pressure import job_matrix,opening_only,trajectory_key


class OpeningPressureTests(unittest.TestCase):
    def test_same_opening_prefix_not_mistaken_for_new_seed_trial(self):
        raw=b'0 :IN0 01 1\n10 :IN1 10 0\n30 :IN0 20 1\n'
        visit=dict(frame=20)
        self.assertEqual(trajectory_key(raw,visit),trajectory_key(raw.replace(b'30 :IN0 20 1',b'40 :IN1 30 0'),visit))
        self.assertEqual(trajectory_key(raw,visit),trajectory_key(raw.replace(b'01',b'0001'),visit))
        self.assertNotEqual(trajectory_key(raw,visit),trajectory_key(raw.replace(b'10 :IN1',b'11 :IN1'),visit))
        self.assertNotEqual(trajectory_key(raw,visit),trajectory_key(raw,dict(frame=21)))

    def test_fixed_matrix_rejects_missing_duplicate_failed_or_live_captures(self):
        jobs=[(f'{m}-{s}',m,s) for s in (3,4,5,6) for m in ('reference','enhanced')]
        report=dict(result='COMPLETE_OPENING_OBSERVATIONS_BALANCE_OPEN',owned_live_pids=[],jobs=jobs,
            experiment=dict(final_frame=30000),steps=[dict(name=j[0],state='PASS',exit_code=0) for j in jobs])
        self.assertEqual(len(job_matrix(report,True)[0]),8)
        cases=[]
        bad=copy.deepcopy(report);bad['jobs'].pop();cases.append(bad)
        bad=copy.deepcopy(report);bad['jobs'][-1]=bad['jobs'][0];cases.append(bad)
        bad=copy.deepcopy(report);bad['steps'][0]['exit_code']=1;cases.append(bad)
        bad=copy.deepcopy(report);bad['owned_live_pids']=[10];cases.append(bad)
        bad=copy.deepcopy(report);bad['result']='RUNNING';cases.append(bad)
        bad=copy.deepcopy(report);bad['steps'][-1]=bad['steps'][0];cases.append(bad)
        for bad in cases:
            with self.subTest(bad=bad),self.assertRaises(Exception):job_matrix(bad,True)

    def test_preserves_all_opening_revisits_and_related_boundary_drops(self):
        rows=[dict(circuit=c,wave=w,frame=f) for c,w,f in [(0,1,100),(0,2,200),(0,1,500),(1,1,600)]]
        boundaries=[dict(from_circuit=0,from_wave=1,to_circuit=0,to_wave=2),
            dict(from_circuit=2,from_wave=1,to_circuit=0,to_wave=1),
            dict(from_circuit=1,from_wave=1,to_circuit=1,to_wave=2)]
        run=dict(observations=rows,partial_visits=rows,boundary_life_decreases=boundaries)
        result=opening_only(run)
        self.assertEqual([r['frame'] for r in result['observations']],[100,500])
        self.assertEqual(len(result['partial_visits']),2)
        self.assertEqual(result['boundary_life_decreases'],boundaries[:2])
        self.assertEqual(len(run['observations']),4)


if __name__=='__main__':unittest.main()
