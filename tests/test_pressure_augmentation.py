from pathlib import Path
import sys
import unittest
from unittest.mock import patch
import copy
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from augment_pressure import target_visits, retry_visits


class PressureAugmentationTests(unittest.TestCase):
    def test_retry_provenance_and_raw_visit_validation(self):
        case=dict(profile='enhanced',players=1,circuit=2,targets=[3],bot_after=100,final_frame=200)
        row=dict(frame=150,circuit=2,wave=3,frames=1100,player=1)
        report=dict(schema='stv.pressure_target_retry.v1',result='COMPLETE_RETRY_OBSERVATIONS_GATE_OPEN',
            owned_live_pids=[],verified_at='source',pins=['parent'],case=case,
            steps=[dict(state='PASS',exit_code=0,log_sha256='hash',proof=dict(
                artifacts={'retry.log':'log-pin'},target_visits=[row]))])
        run=dict(terminal=dict(frames=200),observations=[dict(row,frame=99),row],
                 partial_visits=[],boundary_life_decreases=[])
        with patch('augment_pressure.gaps.checked',return_value=Path('log')), \
             patch('augment_pressure.runner.sha',return_value='hash'), \
             patch('augment_pressure.runner.pin',return_value='log-pin'), \
             patch('augment_pressure.pressure.read_run',return_value=run):
            case,selected,pin=retry_visits(report,'source','parent')
            self.assertEqual([r['frame'] for r in selected['observations']],[150])
            self.assertEqual(case['name'],'retry-hash')
            variants=[]
            for key,value in [('owned_live_pids',[12]),('verified_at','other'),('pins',[]),('result','RUNNING')]:
                bad=copy.deepcopy(report);bad[key]=value;variants.append(bad)
            bad=copy.deepcopy(report);bad['steps'][0]['proof']['target_visits']=[];variants.append(bad)
            bad=copy.deepcopy(report);bad['case']['final_frame']=201;variants.append(bad)
            bad=copy.deepcopy(report);bad['steps'][0]['log_sha256']='wrong';variants.append(bad)
            for bad in variants:
                with self.subTest(bad=bad),self.assertRaises(Exception):retry_visits(bad,'source','parent')

    def test_prefix_other_rooms_and_circuits_excluded(self):
        case=dict(name='new',bot_after=100,circuit=1,targets=[2,3])
        rows=[dict(frame=f,circuit=c,wave=w) for f,c,w in [(99,1,2),(100,1,2),(101,0,2),(102,1,4),(103,1,2),(104,1,3)]]
        run=dict(observations=rows,partial_visits=rows,boundary_life_decreases=[])
        result=target_visits(run,case)
        for key in ('observations','partial_visits'):
            self.assertEqual([r['frame'] for r in result[key]],[103,104])
            self.assertTrue(all(r['source_run']=='new' for r in result[key]))
        self.assertNotIn('source_run',rows[-1])

    def test_only_new_target_boundary_drops_retained(self):
        case=dict(name='new',bot_after=100,circuit=1,targets=[2])
        def row(frame,fc,fw,tc,tw):return dict(frame=frame,from_circuit=fc,from_wave=fw,to_circuit=tc,to_wave=tw)
        run=dict(observations=[],partial_visits=[],boundary_life_decreases=[row(100,1,2,1,3),row(101,0,2,0,3),
            row(102,1,3,1,4),row(103,1,1,1,2),row(104,1,2,1,3)])
        self.assertEqual([r['frame'] for r in target_visits(run,case)['boundary_life_decreases']],[103,104])


if __name__=='__main__':unittest.main()
