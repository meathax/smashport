"""Reject ambiguous projectile/owner/object attribution and incomplete streams."""
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/scenarios'))
import sample_projectile_owners as a

T,L,P=[0x01086790+n*0x5a0 for n in range(3)]
BODY,BULLET=[0x010003a0+n*0x1f0 for n in range(2)]
def put(b,address,value,width=32):struct.pack_into({8:'<B',16:'<H',32:'<I'}[width],b,(address-a.owner.BASE)//8,value)
def fixture(player=1,oid=0x4201):
    b=bytearray(a.owner.RAM_SIZE);put(b,a.owner.BASE,T)
    for node,next_,pid in [(T,L,246+player),(L,P,246+player),(P,0,286)]:put(b,node,next_);put(b,node+0x20,pid,16)
    put(b,a.owner.REGISTRY+(player-1)*32,T);put(b,T+0x1d8,player,8);put(b,T+0x2c0,L);put(b,L+0x160,T)
    for node in (T,L):put(b,node+0x100,BODY)
    put(b,0x010865a0,16,16);put(b,0x01000040,BODY);put(b,BODY,BULLET)
    for obj in (BODY,BULLET):put(b,obj+0xb0,0x8002,16)
    put(b,BODY+0x150,0x4100+player,16)
    put(b,BULLET+0x160,P);put(b,BULLET+0x150,oid,16);put(b,P+0xc0,BULLET)
    put(b,P+0x138,player,8);put(b,P+0x100,3,16)
    return b

class ProjectileOwners(unittest.TestCase):
    def test_all_visible_families_both_owners(self):
        for player in (1,2):
            for oid,family in a.FAMILIES.items():
                rows,_=a.snapshot(fixture(player,oid));self.assertEqual(len(rows),1)
                self.assertEqual((rows[0]['player'],rows[0]['family'],rows[0]['process']), (player,family,P))

    def test_invalid_links_and_unclassified_states_excluded(self):
        for address,value,width in [(P+0x20,247,16),(P+0xc0,BODY,32),(P+0x138,2,8),(P+0x100,0,16),
                (BULLET+0x160,T,32),(BULLET+0xb0,2,16),(BULLET+0x150,0,16),
                (T+0x1d8,2,8),(L+0x160,L,32),(BODY,BODY,32),(BODY+0x150,0,16),(P,T,32),(a.owner.REGISTRY,0,32)]:
            b=fixture();put(b,address,value,width)
            with self.subTest(address=hex(address)):self.assertEqual(a.snapshot(b)[0],[])

    def test_runs_are_not_allocations_and_gaps_split_them(self):
        valid=fixture();neutral=fixture(oid=0)
        with patch.object(a,'images',return_value=iter([(0,valid),(1,valid),(2,neutral),(3,valid)])):
            result=a.scan(None,3)
        g=result['groups'][0];self.assertEqual((g['object_frame_observations'],g['observation_runs'],g['distinct_slot_pairs']),(3,2,1))

    def test_missing_initial_ram_and_endpoint_rejected(self):
        with patch.object(a.r.lsfile,'frames',return_value=iter([(0,[(9,0,b'\0'*8)])])):
            with self.assertRaises(a.r.ProofError):list(a.images(None,0))
        with patch.object(a.r.lsfile,'frames',return_value=iter([])):
            with self.assertRaises(a.r.ProofError):list(a.images(None,1))

if __name__=='__main__':unittest.main()
