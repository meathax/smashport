from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from classify_damage_events import death_assignment,difficulty_assignment,merged_dead


class DamageEventTests(unittest.TestCase):
    def death(self):return dict(kind='dead_write',pc_raw=0xffe199b0,process=0x01087e10,address=0x01088020,
        data=0x86,mask=65535,old_dead=0,merged_dead=0x8600,a11_raw=0x8600,killer_oid_raw=0x8600,
        a0_raw=0x01087e10,a7_raw=0x01087e10,object_oplink_raw=0x01087e10,player_slot=1,process_player_raw=1,a10_raw=0x0101fb60)

    def test_unaligned_two_word_store_counted_once(self):
        e=self.death();self.assertTrue(death_assignment(e))
        first=dict(e,address=e['address']-16,data=0,merged_dead=0)
        self.assertEqual(merged_dead(first),0);self.assertFalse(death_assignment(first))
        for key,value in [('object_oplink_raw',0),('process_player_raw',2),('killer_oid_raw',0),('merged_dead',1),('address',0)]:
            bad=dict(e);bad[key]=value
            with self.subTest(key=key),self.assertRaises(Exception):death_assignment(bad)

    def test_zero_to_zero_is_executed_write_not_difficulty_change(self):
        e=dict(kind='difficulty_write',pc_raw=0xffe19690,a0_raw=0x010ddef0,address=0x010ddf10,
            entry_type_raw=6,a2_raw=6,a10_raw=0,old=0,merged=0,mask=65535,circuit=0,wave=1)
        self.assertTrue(difficulty_assignment(e));self.assertEqual(e['old'],e['merged'])
        for key,value in [('address',e['address']+16),('entry_type_raw',7),('wave',3),('mask',255),('merged',1)]:
            bad=dict(e);bad[key]=value
            with self.subTest(key=key),self.assertRaises(Exception):difficulty_assignment(bad)


if __name__=='__main__':unittest.main()
