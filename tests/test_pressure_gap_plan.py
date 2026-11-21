from pathlib import Path
import copy
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from prepare_pressure_gaps import stable_handoffs,verify_paths,STRATEGIES,strategy_set,expected_case_keys,case_key
from rebaseline import ProofError
from capture_pressure_gaps import validate_case


class PressureGapTests(unittest.TestCase):
    def test_dual_mode_matrix_and_modulo_exit_choice(self):
        selected=strategy_set('dual-mode')
        self.assertEqual(len(expected_case_keys('dual-mode')),14)
        self.assertEqual(len(expected_case_keys('original')),16)
        keys=expected_case_keys('dual-mode')
        for seed in (0,5):
            self.assertIn(case_key(dict(profile='enhanced',players=1,circuit=2,
                                       source_wave=1,bot_seed=seed)),keys)
        self.assertEqual(len({(c,t) for c,_,_,ts in selected[1] for t in ts}),17)
        inventory=dict(physical_records=[
            dict(table_users=[2],physical_wave=1,options=[dict(slot=i,destination_field=t)
                 for i,t in enumerate((2,8,13))]),
            dict(table_users=[2],physical_wave=13,options=[dict(slot=0,destination_field=14)])])
        verify_paths(inventory,{1:[(2,1,5,[13,14])]})
        for seed,targets in [(4,[13,14]),(5,[13,15])]:
            with self.assertRaises(ProofError):verify_paths(inventory,{1:[(2,1,seed,targets)]})

    def test_boss_geometry_includes_missing_coop_room14(self):
        selected=strategy_set('boss-geometry')
        case=dict(name='reference-2p-c2-from13-seed0',profile='reference',players=2,circuit=1,
                  source_wave=13,bot_seed=0,targets=[14,15,17,18,16],bot_after=100,final_frame=60100)
        validate_case(case,'boss-geometry')
        with self.assertRaises(ProofError):validate_case(case,'rescue-fixed')
        self.assertEqual({(c,t) for c,_,_,targets in selected[2] for t in targets},
                         {(0,5),(1,2),(1,3),(2,2),(2,3),(1,14),(1,15),(1,17),(1,18),(1,16)})

    def test_rescue_fixed_strategy_remains_source_bound(self):
        selected=strategy_set('rescue-fixed')
        case=dict(name='reference-2p-c2-from14-seed0',profile='reference',players=2,circuit=1,
                  source_wave=14,bot_seed=0,targets=[15,17,18,16],bot_after=100,final_frame=60100)
        validate_case(case,'rescue-fixed')
        with self.assertRaises(ProofError):validate_case(case)
        with self.assertRaises(ProofError):strategy_set('unknown')
        records=[]
        for rows in selected.values():
            for c,start,slot,targets in rows:
                for target in targets:
                    records.append(dict(table_users=[c],physical_wave=start,
                        options=[dict(slot=slot,destination_field=target)]));start=target
        verify_paths(dict(physical_records=records),selected)
        records=[r for r in records if not(r['table_users']==[1] and r['physical_wave']==18)]
        with self.assertRaises(ProofError):verify_paths(dict(physical_records=records),selected)

    def test_capture_arguments_are_bounded_and_reviewed(self):
        case=dict(name='reference-1p-c1-from4-seed0',profile='reference',players=1,circuit=0,
                  source_wave=4,bot_seed=0,targets=[5],bot_after=100,final_frame=60100)
        validate_case(case)
        for key,value in [('name','../escape'),('profile','other'),('players',True),
                          ('bot_after',False),('final_frame',60101),('bot_seed',2),('targets',[6])]:
            changed=copy.deepcopy(case);changed[key]=value
            with self.subTest(key=key),self.assertRaises(ProofError):validate_case(changed)

    def test_stable_live_pair_not_gap_or_transition(self):
        def row(f,w=4,state=16):return dict(frame=f,circuit=0,wave=w,gamstate=state)
        self.assertEqual(stable_handoffs([row(0),row(60),row(120)],{(0,4)}),{(0,4):60})
        for samples in ([row(0),row(120)],[row(0,state=24),row(60)],[row(0,w=3),row(60)]):
            self.assertEqual(stable_handoffs(samples,{(0,4)}),{})

    def test_every_proposed_edge_required(self):
        records=[]
        for strategies in STRATEGIES.values():
            for c,start,slot,targets in strategies:
                for target in targets:
                    records.append(dict(table_users=[c],physical_wave=start,
                                        options=[dict(slot=slot,destination_field=target)]));start=target
        verify_paths(dict(physical_records=records))
        records=[r for r in records if not(r['table_users']==[2] and r['physical_wave']==20)]
        with self.assertRaises(ProofError):verify_paths(dict(physical_records=records))


if __name__=='__main__':unittest.main()
