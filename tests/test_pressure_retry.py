from pathlib import Path
import sys
import unittest
import copy
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from retry_pressure_target import preferred_exit,preferred_route,bounded_endpoint
from retry_pressure_chain import parent_case
from rebaseline import ProofError


class RetryTests(unittest.TestCase):
    def test_retry_parent_requires_terminal_unique_success(self):
        case=dict(profile='enhanced',players=1)
        step=dict(name='retry',state='PASS',exit_code=0,proof=dict(missing_player_rooms=[[1,6]]))
        source=dict(schema='stv.pressure_target_retry.v1',result='COMPLETE_RETRY_OBSERVATIONS_GATE_OPEN',
                    owned_live_pids=[],case=case,steps=[step])
        self.assertEqual(parent_case(source,'retry'),(case,step,[[1,6]]))
        for key,value in [('owned_live_pids',[1]),('result','RUNNING'),('steps',[step,step])]:
            bad=copy.deepcopy(source);bad[key]=value
            with self.assertRaises(ProofError):parent_case(bad,'retry')
        with self.assertRaises(ProofError):parent_case(source,'other')
        bad=copy.deepcopy(source);bad['steps'][0]['exit_code']=1
        with self.assertRaises(ProofError):parent_case(bad,'retry')

    def test_retry_horizon_is_explicit_and_bounded(self):
        for budget in (60000,120000,180000):
            self.assertEqual(bounded_endpoint(100,budget),100+budget)
        for handoff,budget in [(0,60000),(True,60000),(100,60001),(1900000,180000),(100,True)]:
            with self.assertRaises(ProofError):bounded_endpoint(handoff,budget)

    def test_route_requires_each_consecutive_retail_edge(self):
        records=[dict(table_users=[1],physical_wave=w,options=[dict(destination_field=t,slot=0)])
                 for w,t in [(1,2),(2,3)]]
        self.assertEqual([r['destination_field'] for r in preferred_route(records,1,1,[2,3],3)],[2,3])
        for circuit,targets in [(0,[2,3]),(1,[3,2]),(1,[2,2]),(1,[])]:
            with self.assertRaises(ProofError):preferred_route(records,circuit,1,targets,3)
        with self.assertRaises(ProofError):preferred_route(records[:1],1,1,[2,3],3)

    def test_same_initial_exit_different_seed(self):
        options=[dict(destination_field=d,slot=i) for i,d in enumerate((3,9,14))]
        self.assertEqual(preferred_exit(options,3,0),preferred_exit(options,3,3))
        for target,seed in [(3,1),(3,2),(3,-1),(3,101),(3,True),(7,0)]:
            with self.assertRaises(ProofError):preferred_exit(options,target,seed)
        with self.assertRaises(ProofError):preferred_exit(options+[options[0]],3,0)


if __name__=='__main__':unittest.main()
