"""Collision pairing, source owner and malformed-boundary regressions."""
import copy
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools/scenarios'))
import audit_collision_events as a
def fixture(player=1,side=1):
    e=dict(kind='collision_dispatch',phase='before',pair=1,frame=3,pc_raw=0xffe557c0,sp=0x01090000,
           object1=0x010003a0,object2=0x01000590,object_list_complete=True,process_list_complete=True,
           active1=True,active2=True,flags1=0x8402,flags2=0x8002,callback1=0xffe322d0,callback2=0,
           owner1=player==1,owner2=player==2)
    for n in (1,2):e.update({f'oid{n}':0x4201 if n==side else 0x8600,f'process{n}':0x01086790,
                            f'process_active{n}':True,f'pid{n}':286 if n==side else 254,f'myplayer{n}':player})
    after=dict(e,phase='after',pc_raw=0xffe55890,frame=4);after['active'+str(side)]=False
    return [dict(schema='stv.mame.collision_dispatch.v1',scope='projectile_enemy_pairs'),e,after,
            dict(kind='end',frame=5,events=2,pairs=1,overlaps=0,unpaired=0)]
class CollisionEvents(unittest.TestCase):
    def check(self,rows):return a.validate(rows,5,{0xffe322d0})
    def test_both_object_orders_and_players(self):
        for player in (1,2):
            for side in (1,2):
                r=self.check(fixture(player,side));self.assertEqual(r['bound_pairs'],1)
                self.assertEqual((r['groups'][0]['player'],r['groups'][0]['projectile_after']),(player,'absent_after'))
    def test_unbound_owner_preserved(self):
        for key,value in [('owner1',False),('pid1',247),('process_active1',False),('myplayer1',0)]:
            rows=fixture();rows[1][key]=value;r=self.check(rows)
            self.assertEqual((r['bound_pairs'],r['unbound_pairs']),(0,1))
    def test_bad_pairs_boundaries_and_targets_rejected(self):
        for index,key,value in [(1,'pair',2),(2,'pair',2),(1,'pc_raw',0),(2,'sp',0x01090010),
                (1,'active1',False),(1,'object_list_complete',False),(1,'callback1',0x12345678),
                (1,'oid2',0),(2,'frame',2),(3,'overlaps',1),(3,'unpaired',1),(3,'events',4)]:
            rows=fixture();rows[index][key]=value
            with self.subTest(key=key):
                with self.assertRaises(a.r.ProofError):self.check(rows)
    def test_unbound_reasons_are_explicit_and_can_overlap(self):
        cases=[('process_list_complete',False,'incomplete_process_list'),
               ('process_active1',False,'inactive_process'),('pid1',247,'unexpected_process_id'),
               ('process1',0,'invalid_process_slot'),('myplayer1',9,'invalid_player_field'),
               ('owner1',False,'missing_live_player_owner')]
        for key,value,reason in cases:
            rows=fixture();rows[1][key]=value
            with self.subTest(reason=reason):
                self.assertEqual(self.check(rows)['unbound_reasons'],{reason:1})
        rows=fixture();rows[1].update(myplayer1=46,process_active1=False)
        result=self.check(rows)
        self.assertEqual(result['unbound_pairs'],1)
        self.assertEqual(result['unbound_reasons'],{'inactive_process':1,'invalid_player_field':1})
if __name__=='__main__':unittest.main()
