from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from classify_life_events import bound_decrement


class LifeEventTests(unittest.TestCase):
    def event(self):return dict(kind='life_write',player=1,address=0x01086360,a1_raw=0x01086220,
        pc_raw=0xffe1c6c0,a2_raw=1,process_player_raw=1,old=2,merged=1,data=1,mask=65535)

    def test_correct_owner_and_next_pc_required(self):
        self.assertTrue(bound_decrement(self.event()))
        e=self.event();e.update(player=2,address=0x01086520,a1_raw=0x010863e0,process_player_raw=2)
        self.assertTrue(bound_decrement(e))
        e=self.event();e['pc_raw']=0xffe1c6a0;self.assertFalse(bound_decrement(e))

    def test_rejects_wrong_owner_partial_write_and_invalid_arithmetic(self):
        for key,value in [('address',0),('a1_raw',0),('process_player_raw',2),('mask',255),
            ('old',3),('merged',0),('a2_raw',2),('data',65536)]:
            e=self.event();e[key]=value
            with self.subTest(key=key),self.assertRaises(Exception):bound_decrement(e)


if __name__=='__main__':unittest.main()
