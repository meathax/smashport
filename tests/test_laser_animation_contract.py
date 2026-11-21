from pathlib import Path
import sys
import unittest
import copy
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import laser_animation_contract as audit
from decode import Insn,Opnd


def ins(mnem,*ops):return Insn(0x1000,0,1,mnem,[Opnd(*o) for o in ops])


class LaserAnimationTests(unittest.TestCase):
    def test_retail_writer_bridge_rejects_changed_continuations_and_targets(self):
        a=audit.a
        _,_,compiled,rom,instructions,_=a.load_current(audit.ROOT)
        proof=audit.writer_domain(instructions,rom,compiled)
        self.assertTrue(proof['wake_entries'])
        self.assertEqual(proof['process_owner_alias_lifetime_and_alternate_entry_proof'],'OPEN')
        for pc,operand in [(0xfff15e20,0),(0xfff15e50,0),(0xfff15e80,0),
                           (0xfff15eb0,0),(0xfff15ef0,0),(0xfff15f20,0)]:
            changed=copy.deepcopy(instructions)
            old=changed[pc].ops[operand]
            changed[pc].ops[operand]=Opnd(old.kind,old.reg,old.value+16)
            with self.subTest(pc=hex(pc)),self.assertRaises(a.ContractError):
                audit.writer_domain(changed,rom,compiled)
        changed=copy.deepcopy(instructions)
        changed[0xfff15f10].ops[0]=Opnd('IND','A9',None)
        with self.assertRaises(a.ContractError):audit.writer_domain(changed,rom,compiled)

    def test_register_clobbers_and_unreviewed_calls_rejected(self):
        a=audit.a
        for instruction in [ins('CLR',a.R('A10')),
            ins('MMFM',a.R('SP'),a.M('LIST',None,1<<10)),
            ins('MPYS',a.R('A0'),a.R('A9')),
            ins('CALLA',a.I(0)),ins('RETI'),ins('JR',a.M('REL',None,0x2000))]:
            with self.subTest(op=instruction.mnem),self.assertRaises(a.ContractError):
                audit.preserve_a10([instruction])
        self.assertEqual(audit.preserve_a10([ins('CLR',a.R('A0'))])['a10_direct_or_implicit_clobbers'],0)

    def fixture(self):
        rom=bytearray(256)
        for i in range(4):rom[i*12+4:i*12+8]=(audit.a.BASE+1024).to_bytes(4,'little')
        for j in range(3):
            rom[128+j*6:132+j*6]=(audit.a.BASE+2048).to_bytes(4,'little')
            rom[132+j*6:134+j*6]=(5).to_bytes(2,'little')
        return rom

    def test_scripts_have_only_three_ordinary_frames(self):
        self.assertEqual(len(audit.scripts(self.fixture(),audit.a.BASE)),4)
        for offset,value in [(132,255),(128,0),(146,1)]:
            rom=self.fixture()
            if offset==128:rom[offset:offset+4]=bytes(4)
            else:rom[offset]=value
            with self.subTest(offset=offset),self.assertRaises(audit.a.ContractError):
                audit.scripts(rom,audit.a.BASE)


if __name__=='__main__':unittest.main()
