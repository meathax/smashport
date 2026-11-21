import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/scenarios'))
from summarize_entry_controls import collect
from protected_route_edges import selector_changes

def rows():
    for f in range(121):
        row = dict(frame=str(f), state='24' if f==0 else '16', circuit='0', wave='2', from_door='2')
        for p in (1,2):
            row.update({f'p{p}_x':'270', f'p{p}_y':'130', f'p{p}_vx':'0', f'p{p}_vy':'0', f'p{p}_control':'0'})
        yield row

class Controls(unittest.TestCase):
    def test_route_requires_selector_publication_and_matching_arrival(self):
        def observe(data):
            return list(selector_changes(dict(state=s, circuit=c, wave=w, from_door=d, frame=f)
                                         for f,(s,c,w,d) in enumerate(data)))
        ordinary=[(16,1,2,2),(16,1,9,3),(24,1,9,3),(16,1,9,3)]
        tally=[(16,1,7,2),(16,1,19,2),(24,1,19,2),(24,2,19,0)]
        self.assertEqual(observe(ordinary), [(3,(1,2,9,3),'settled room candidate')])
        self.assertEqual(observe(tally), [(3,(1,7,19,2),'circuit advance')])
        for bad in (ordinary[1:],ordinary[:2]+ordinary[3:],
                    ordinary[:2]+[(24,1,9,4)]+ordinary[3:],
                    ordinary[:2]+[(0,1,9,3)]+ordinary[2:],
                    tally[:-1]+[(24,0,19,0)],tally[:-1]+[(24,2,19,2)]):
            with self.subTest(bad=bad):
                self.assertFalse(observe(bad))
    def test_stable_complete_window(self):
        result,last=collect(rows(),'wide')
        self.assertEqual(last,120)
        self.assertEqual(len(result),1)
        self.assertTrue(all(p['visible'] and p['stationary'] for p in result[0]['players']))
    def test_movement_is_not_a_stopped_entry(self):
        data=list(rows());data[115]['p1_vx']='1.375'
        result,_=collect(data,'wide')
        self.assertFalse(result[0]['players'][0]['stationary'])
    def test_requires_intro_and_full_window(self):
        self.assertFalse(collect(list(rows())[1:],'wide')[0])
        self.assertFalse(collect(list(rows())[:119],'wide')[0])
    def test_duplicate_frame_cannot_replace_missing_frame(self):
        data=list(rows());data[115]['frame']='114'
        result,_=collect(data,'wide')
        self.assertFalse(result[0]['players'][0]['stationary'])
    def test_held_control_is_not_neutral(self):
        data=list(rows());data[115]['p2_control']='8'
        result,_=collect(data,'wide')
        self.assertFalse(result[0]['players'][1]['stationary'])
    def test_absent_player_does_not_pass(self):
        data=list(rows());data[-1]['p2_x']='nan'
        result,_=collect(data,'wide')
        self.assertFalse(result[0]['players'][1]['valid'])
        self.assertIsNone(result[0]['players'][1]['x'])

if __name__ == '__main__': unittest.main()
