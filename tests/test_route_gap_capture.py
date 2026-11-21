import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from capture_route_gaps import validate_case
from rebaseline import ProofError

class RouteGapCaptureTests(unittest.TestCase):
    def test_accept_bounded_plan(self):
        validate_case(dict(name='c2-wave2-upper',bot_after=73080,bot_seed=1,final_frame=103080,two_players=False,target=[1,2,9]))

    def test_reject_unsafe_or_ambiguous_plan_values(self):
        good=dict(name='case',bot_after=60,bot_seed=1,final_frame=300,two_players=False,target=[1,2,9])
        for key,value in [('name','../escape'),('name','C:\\escape'),('bot_after',True),('bot_after',300),
                          ('final_frame',2000001),('bot_seed',-1),('two_players',1),('target',[3,2,9]),
                          ('target',[1,True,9]),('target',[1,0,9]),('target',[1,2,25]),('target',[1,2])]:
            bad=copy.deepcopy(good);bad[key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ProofError):validate_case(bad)

if __name__=='__main__':unittest.main()
