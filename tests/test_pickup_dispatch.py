from copy import deepcopy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import pickup_dispatch_contract as audit
import shoe_effect_contract as shoes
import sample_shoe_owners as sampled
import audit_shoe_events as event_audit
import struct
import tempfile


class PickupDispatchTests(unittest.TestCase):
    def test_shoe_store_requires_correct_writer_icon_and_live_owner(self):
        torso=0x01086790;legs=torso+0x5a0
        event=dict(kind='shoe_assignment',pc_raw=0xffe30380,icon_active=True,
            object_list_complete=True,process_list_complete=True,live_owner_links=True,
            player=1,process=legs,torso=torso,address=legs+0x180,a0_raw=legs,
            legs_backlink=torso,torso_pid=247,legs_pid=247,player_number=1,
            torso_object=0x010003a0,legs_object=0x010003a0,icon=0x01000590,
            circuit=0,wave=2,frame=5400,icon_oid=0x0c09,mask=0xffff,merged_timer=300,data=300,old_timer=0)
        self.assertEqual(event_audit.validate_event(event)['new_timer'],300)
        for key,value in [('pc_raw',0xffe1fe00),('icon_active',False),('live_owner_links','true'),
                ('player',True),('player_number',0),('legs_backlink',0),('icon_oid',0x0709),('data',500),('mask',255)]:
            bad=dict(event);bad[key]=value
            with self.subTest(key=key),self.assertRaises(event_audit.r.ProofError):event_audit.validate_event(bad)

    def test_sampled_shoes_require_live_reciprocal_owner(self):
        image=bytearray(sampled.owner.RAM_SIZE)
        def write(address,value,width=32):
            offset=(address-sampled.owner.BASE)//8
            image[offset:offset+width//8]=value.to_bytes(width//8,'little')
        torso,legs=0x01087000,0x01088000
        write(sampled.owner.BASE,torso);write(torso,legs)
        write(torso+0x20,247,16);write(legs+0x20,247,16)
        write(0x010865a0,16,16);write(sampled.owner.REGISTRY,torso)
        write(torso+0x1d8,1,8);write(torso+0x2c0,legs);write(legs+0x160,torso)
        write(torso+0x100,0x01001000);write(legs+0x100,0x01001000)
        write(legs+0x180,299,16)
        bound,_=sampled.owners(image)
        self.assertEqual([(r['player'],r['timer']) for r in bound],[(1,299)])
        baseline=bytes(image)
        for address,value,width in [(legs+0x160,0,32),(torso+0x1d8,2,8),
                (legs+0x20,248,16),(sampled.owner.REGISTRY,0,32),
                (legs,torso,32),(0x010865a0,0,16)]:
            image[:]=baseline;write(address,value,width)
            with self.subTest(address=address):self.assertEqual(sampled.owners(image)[0],[])
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'ram.bin'
            data=bytearray(sampled.r.lsfile.MAGIC)
            for index in range(len(baseline)//256):
                payload=struct.pack('<H',index)+baseline[index*256:(index+1)*256]
                data+=struct.pack('<BIH',1,0,len(payload))+payload
            data+=struct.pack('<BIH',9,0,8)+bytes(8)+struct.pack('<BIH',8,0,0)
            path.write_bytes(data)
            result=sampled.scan(path,0)
            self.assertEqual(result['positive_owner_samples'][0]['timer'],299)
            with self.assertRaises(sampled.r.ProofError):sampled.scan(path,1)
            path.write_bytes(sampled.r.lsfile.MAGIC+struct.pack('<BIH',1,0,6)+bytes(6)
                             +struct.pack('<BIH',8,0,0))
            with self.assertRaises(sampled.r.ProofError):sampled.scan(path,0)

    def test_shoe_owner_duration_and_expiry_contract(self):
        _,_,_,_,ins,_=audit.a.load_current(audit.ROOT)
        for start,end,validate,mutations in [
            (0xffe30230,0xffe30380,shoes.validate_assignment,
             [(1,0,0x140),(2,0,0x2e0),(3,0,299),(5,0,1),(7,0,499),(9,1,0x190)]),
            (0xffe30380,0xffe30450,shoes.validate_cleanup,
             [(1,0,0xffe034b0),(2,0,0xffe63430),(3,0,0xffe60cf0),(4,1,7)]),
            (0xffe1fda0,0xffe20000,shoes.validate_movement,
             [(0,0,0x190),(1,0,0xffe1fe00),(4,0,0xffe1f8a0),(19,0,0x120)])]:
            seq=audit.a.window(ins,start,end);validate(seq)
            for index,operand,value in mutations:
                changed=deepcopy(seq);changed[index].ops[operand].value=value
                with self.subTest(start=start,index=index),self.assertRaises(audit.a.ContractError):validate(changed)

    def test_dispatch_rejects_changed_fields_mask_ring_branch_table_and_restore(self):
        _,_,_,_,ins,_=audit.a.load_current(audit.ROOT)
        seq=audit.a.window(ins,0xffe5de80,0xffe5e040)
        audit.validate(seq)
        for index,operand,value in [(1,0,0x140),(2,0,7),(4,0,0xffe5dff0),
                (5,0,0x140),(6,0,15),(7,0,4),(8,0,0xffe5e060),(11,1,2),(14,1,2)]:
            changed=deepcopy(seq);changed[index].ops[operand].value=value
            with self.subTest(index=index),self.assertRaises(audit.a.ContractError):
                audit.validate(changed)
        with self.assertRaises(audit.a.ContractError):audit.validate(seq[:-1])


if __name__=='__main__':unittest.main()
