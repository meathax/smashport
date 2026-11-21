from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from summarize_pressure import pool
import pressure
from rebaseline import ProofError


def run(frames):
    return dict(observations=[dict(player=1,circuit=0,wave=1,frames=f,deaths=1,avg=2,peak=3,line=i)
                             for i,f in enumerate(frames)],partial_visits=[],boundary_life_decreases=[])


class PoolTests(unittest.TestCase):
    def test_visit_weighting_and_provenance(self):
        original=run([1000,1000,1000]);pooled=pool([('one',original),('two',run([5000]))])
        rows,_=pressure.aggregate(pooled,1000)
        self.assertEqual(rows[1,0,1]['frames_per_visit'],2000)
        self.assertEqual([r['source_run'] for r in pooled['observations']],['one']*3+['two'])
        self.assertNotIn('source_run',original['observations'][0])

    def test_empty_duplicate_and_partial_runs(self):
        for runs in ([],[('same',run([1000])),('same',run([2000]))]):
            with self.assertRaises(ProofError):pool(runs)
        r=run([1000]);r['partial_visits']=[dict(frames=999)];r['boundary_life_decreases']=[dict(life_decrease=1)]
        pooled=pool([('a',r)])
        self.assertEqual(len(pooled['observations']),1)
        self.assertEqual(pooled['partial_visits'][0]['source_run'],'a')
        self.assertEqual(pooled['boundary_life_decreases'][0]['life_decrease'],1)


if __name__=='__main__':unittest.main()
