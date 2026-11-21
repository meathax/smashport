import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from verify_route_gap_oracles import matching_reuse_case
from rebaseline import ProofError


class OracleReuseTests(unittest.TestCase):
    def test_terminal_roles_must_match(self):
        case=dict(name='case',target=[1,2,9],final_frame=100,inputs=dict(sha256='inputs'),native=dict(sha256='native'))
        previous=dict(result='FAIL',owned_live_pids=[],cases=[copy.deepcopy(case)])
        self.assertEqual(matching_reuse_case(previous,case),case)
        for key,value in [('target',[1,2,14]),('final_frame',101),('inputs',{}),('native',{})]:
            changed=copy.deepcopy(case);changed[key]=value
            with self.subTest(key=key),self.assertRaises(ProofError):matching_reuse_case(previous,changed)
        for key,value in [('result','RUNNING'),('owned_live_pids',[123]),('cases',[case,case])]:
            changed=copy.deepcopy(previous);changed[key]=value
            with self.subTest(key=key),self.assertRaises(ProofError):matching_reuse_case(changed,case)


if __name__=='__main__':unittest.main()
