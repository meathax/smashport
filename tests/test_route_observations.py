from pathlib import Path
import sys
import unittest
import copy
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import route_observations as audit
from rebaseline import ProofError


def row(frame,wave,circuit=0,state=16):
    return dict(frame=frame,wave=wave,circuit=circuit,gamstate=state)


class RouteObservationTests(unittest.TestCase):
    def targeted_fixture(self):
        r=dict(result='PASS_SIX_COMPLETE_TARGETED_ROUTES_CLEARANCE_OPEN',owned_live_pids=[],
            verified_at='runtime',pins=[dict(path='exe',sha256='recorder'),dict(path='rom',sha256='romsha')],cases=[],steps=[])
        for i in range(6):
            name=str(i);art={n:dict(path=name+'/'+n) for n in ('native.bin','content.jsonl')}
            c=dict(name=name,final_frame=60,state='PASS',inputs=dict(path=name+'/inputs'),oracle=dict(path=name+'/oracle'))
            r['cases'].append(c)
            r['steps'].extend([
                dict(name='native-'+name,state='PASS',exit_code=0,
                    command=['exe','rom',art['native.bin']['path'],'60','--native-hcount','--inputs',c['inputs']['path'],'--content',art['content.jsonl']['path']],
                    proof=dict(endpoint=dict(first_frame=0,last_frame=60,frames=61),artifacts=art)),
                dict(name='diff-'+name,state='PASS',exit_code=0,
                    command=['python','lsfile','diff',c['oracle']['path'],art['native.bin']['path']],
                    proof=dict(frames=61,allow_prefix=False))])
        return r

    def test_targeted_requires_current_complete_bound_reference(self):
        fixture=self.targeted_fixture()
        with patch.object(audit.gaps,'checked',side_effect=lambda p:Path(p['path'])):
            self.assertEqual(len(audit.targeted_captures(fixture,'runtime','recorder','romsha')),6)
            mutations=[lambda r:r.update(owned_live_pids=[1]),lambda r:r.update(verified_at='old'),
                lambda r:r['steps'][1]['proof'].update(allow_prefix=True),
                lambda r:r['steps'][0]['proof']['endpoint'].update(last_frame=59),
                lambda r:r['steps'][0]['command'].append('--enhanced'),
                lambda r:r['steps'][1]['command'].__setitem__(-1,'other.bin'),
                lambda r:r['steps'][0]['command'].__setitem__(0,'other.exe'),
                lambda r:r['steps'][0]['command'].__setitem__(-1,'other-content.jsonl'),
                lambda r:r['steps'][0]['command'].__setitem__(1,'other-rom'),
                lambda r:r['cases'].pop(),lambda r:r['cases'][0].update(state='RUNNING')]
            for mutation in mutations:
                candidate=copy.deepcopy(fixture);mutation(candidate)
                with self.subTest(mutation=mutation),self.assertRaises(ProofError):
                    audit.targeted_captures(candidate,'runtime','recorder','romsha')
        with patch.object(audit.gaps,'checked',side_effect=ProofError('changed hash')):
            with self.assertRaises(ProofError):audit.targeted_captures(fixture,'runtime','recorder','romsha')

    def test_only_player_capture_names(self):
        self.assertEqual(audit.PLAYER_CAPTURES,{'1p','2p','route:bonus','route:lower','route:scorpion'})
        for name in ('attract','operator_walk','framebuffers','bonus'):
            self.assertNotIn(name,audit.PLAYER_CAPTURES)

    def test_adjacent_selector_change(self):
        self.assertEqual(list(audit.transitions([row(0,1),row(60,1),row(120,2)])),
                         [dict(circuit=0,source_wave=1,destination_wave=2,before_frame=60,after_frame=120)])

    def test_no_gap_boot_or_circuit_bridge(self):
        for samples in [[row(0,1),row(120,2)], [row(0,1),row(60,2,1)],
                        [row(0,1,state=0),row(60,2)], [row(0,1),row(60,2,state=0)],
                        [row(0,1),row(60,1)]]:
            with self.subTest(samples=samples):self.assertEqual(list(audit.transitions(samples)),[])

    def test_malformed_or_unordered_rejected(self):
        for samples in [[row(60,1),row(60,2)],[row(60,1),row(0,2)],[row(True,1)],[{}]]:
            with self.assertRaises(ProofError):list(audit.transitions(samples))

    def test_full_key_and_ambiguous_matches_retained(self):
        change=dict(circuit=0,source_wave=1,destination_wave=2)
        options=[dict(circuit=0,source_wave=1,destination_field=2),
                 dict(circuit=1,source_wave=1,destination_field=2),
                 dict(circuit=0,source_wave=2,destination_field=2),
                 dict(circuit=0,source_wave=1,destination_field=3)]
        self.assertEqual(audit.match_options(change,options),[0])
        self.assertEqual(audit.match_options(change,options+[options[0]]),[0,4])


if __name__=='__main__':unittest.main()
