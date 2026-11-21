from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from capture_opening_variants import VARIANTS,varied_prefix
from recover_c3_variants import events,prefix_bytes
from summarize_opening_variants import player_positions


class OpeningVariantTests(unittest.TestCase):
    def test_position_signature_excludes_enemy_pickup_and_other_player_side_types(self):
        sample=dict(objects=[dict(oid=oid,x=x,y=3) for oid,x in [(0x8101,1),(0x4c01,2),(0x4102,5),(0x4101,4),(0x4701,6)]])
        self.assertEqual(player_positions(sample),[(0x4101,4,3),(0x4102,5,3)])

    def test_preserves_startup_and_coin_start_overwrites_only_sticks(self):
        raw=b'0 :IN1 1 1\n80 :IN0 1 1\n100 :IN0 1 0\n110 :IN1 4 1\n120 :IN0 8 1\n159 :IN1 4 0\n160 :IN0 2 1\n'
        results=[varied_prefix(raw,100,160,v) for v in VARIANTS]
        self.assertEqual(len(set(results)),5)
        self.assertEqual(results[0],prefix_bytes(raw,160))
        for name,data in zip(VARIANTS,results):
            rows=events(data)
            self.assertEqual([r[:4] for r in rows if r[0]<100],[r[:4] for r in events(raw) if r[0]<100])
            self.assertEqual([r[:4] for r in rows if r[1]==':IN1'],[r[:4] for r in events(raw) if r[1]==':IN1' and r[0]<160])
            self.assertTrue(all(r[0]<160 for r in rows))
            if name!='control':
                buttons=[r for r in rows if r[0]>=100 and r[1]==':IN0']
                self.assertEqual(len(buttons),16)
                mask=sum(r[2] for r in buttons if r[3])
                p1,p2=VARIANTS[name];self.assertEqual(mask,p1|(p2<<8))
                for shift in (0,4,8,12):self.assertIn((mask>>shift)&15,(1,2,4,8))

    def test_rejects_invalid_window_or_variant(self):
        for start,end,name in [(0,60,'control'),(100,159,'control'),(100,160,'unknown')]:
            with self.subTest(start=start,end=end,name=name),self.assertRaises(Exception):varied_prefix(b'0 :IN1 1 1\n',start,end,name)


if __name__=='__main__':unittest.main()
