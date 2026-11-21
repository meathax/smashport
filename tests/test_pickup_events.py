import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import audit_pickup_events as a

class PickupEvents(unittest.TestCase):
    def event(self):
        return dict(kind='pickup_handler',phase='owner_read',pc_raw=0xffe31ac0,subtype=2,frame=100,circuit=0,wave=1,
            icon=0x010003a0,icon_oid=0x0c02,player_object=0x01000590,player_oid=0x4101,
            player=1,torso=0x01086790,legs=0x01086d30,torso_pid=247,legs_pid=247,player_number=1,
            legs_backlink=0x01086790,torso_object=0x01000780,legs_object=0x01000780,
            icon_active=True,player_object_active=True,torso_active=True,legs_active=True,
            process_list_complete=True,object_list_complete=True)

    def test_all_boundaries(self):
        for pc,(_,slot) in a.SITES.items():
            for subtype in ((11,12,13) if slot is None else (slot,)):
                e=self.event();e.update(pc_raw=pc,subtype=subtype,icon_oid=0x0c00+subtype)
                self.assertEqual(a.validate_event(e)['subtype'],subtype)
        for phase,pc,slot in [('bomb_flag_store',0xffe304c0,10),('helper_delete_entry',0xffe03240,1)]:
            e=self.event();e.update(phase=phase,pc_raw=pc,subtype=slot,icon_oid=0x0c00+slot);a.validate_event(e)

    def test_reject_invalid_owner_and_handler(self):
        changes={'player':0,'pc_raw':0,'subtype':17,'phase':'unknown','icon_oid':0x0c03,'player_oid':0x4102,
            'torso':0x01086791,'legs':0x01086790,'legs_pid':248,'player_number':2,'legs_backlink':0,
            'torso_object':0,'legs_object':0x01000970,'icon':0x01000590,'frame':-1,'circuit':3}
        for key in ('icon_active','player_object_active','torso_active','legs_active','process_list_complete','object_list_complete'):
            changes[key]=False
        for key,value in changes.items():
            e=copy.deepcopy(self.event());e[key]=value
            with self.subTest(key=key),self.assertRaises(a.r.ProofError):a.validate_event(e)

if __name__=='__main__':unittest.main()
