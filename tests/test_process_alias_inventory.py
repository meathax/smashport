from pathlib import Path
import sys
import unittest
import copy
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import process_alias_inventory as audit
from decode import Insn,Opnd


class AliasInventoryTests(unittest.TestCase):
    def test_player_object_links_cannot_be_replaced_with_process_fields(self):
        _,_,_,_,instructions,_=audit.a.load_current(audit.ROOT)
        audit.player_leg_writer(instructions,0xffe1ca60,0)
        audit.player_leg_writer(instructions,0xffe1cc50,1)
        audit.rescue_object_stores(instructions)
        for pc,index,operand in [(0xffe1c9f0,0,Opnd('ABS',None,0x01086790)),
            (0xffe1ca20,0,Opnd('DISP','A0',0x80)),(0xffe7be80,0,Opnd('DISP','A1',0x80)),
            (0xffe7bfc0,1,Opnd('DISP','A13',0x80))]:
            changed=copy.deepcopy(instructions);changed[pc].ops[index]=operand
            with self.assertRaises(audit.a.ContractError):
                if pc<0xffe70000:audit.player_leg_writer(changed,0xffe1ca60,0)
                else:audit.rescue_object_stores(changed)

    def test_timer_owner_requires_unchanged_allocation_result(self):
        _,_,compiled,_,instructions,_=audit.a.load_current(audit.ROOT)
        self.assertEqual(audit.initials_writer(instructions,compiled)['initial_a10'],30)
        for pc,operand,value in [(0xffea6cc0,0,Opnd('IMM',None,0)),
            (0xffea6cf0,1,Opnd('R','A0',None)),
            (0xffea6d40,1,Opnd('DISP','A13',0x80))]:
            changed=copy.deepcopy(instructions);changed[pc].ops[operand]=value
            with self.subTest(pc=hex(pc)),self.assertRaises(audit.a.ContractError):
                audit.initials_writer(changed,compiled)

    def test_invalid_pool_intervals_and_overlap_rejected(self):
        pool=audit.pool_range(0x01000000,2,0x5a0)
        audit.disjoint(pool,audit.pool_range(0x01002000,2,0x1f0))
        for args in [(0x01000001,2,0x5a0),(0x01000000,0,0x5a0),
                     (0x010ffff0,2,0x5a0),(0x01000000,2,0x5a1),
                     (0x01000000,2,0x80),(0x00fffff0,1,0x5a0)]:
            with self.subTest(args=args),self.assertRaises(audit.a.ContractError):audit.pool_range(*args)
        with self.assertRaises(audit.a.ContractError):
            audit.disjoint(pool,audit.pool_range(0x01000100,2,0x1f0))

    def test_read_and_other_displacement_are_not_saved_field_writes(self):
        def move(pc,src,dst):return Insn(pc,0,2,'MOVE',[Opnd(*src),Opnd(*dst),Opnd(*audit.a.K(1))])
        write=move(0x1000,audit.a.R('A1'),audit.a.M('DISP','A8',0x80))
        read=move(0x1020,audit.a.M('DISP','A8',0x80),audit.a.R('A1'))
        other=move(0x1040,audit.a.R('A1'),audit.a.M('DISP','A8',0x130))
        generic=move(0x1060,audit.a.R('A1'),audit.a.M('IND','A8'))
        self.assertEqual(audit.offset_writes({i.pc:i for i in [write,read,other,generic]}),[write])


if __name__=='__main__':unittest.main()
