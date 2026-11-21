from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import reconstruct_operator as audit


class OperatorInventoryTests(unittest.TestCase):
    def test_jump_wrapper_rejects_wrong_opcode_short_or_unaligned_data(self):
        import struct
        data=struct.pack('<HI',0xc080,0xfff48260)
        self.assertEqual(audit.absolute_jump(data,audit.a.BASE),0xfff48260)
        for b,p in [(b'\0\0'+data[2:],audit.a.BASE),(data[:-1],audit.a.BASE),(data,audit.a.BASE+8)]:
            with self.assertRaises(audit.a.ContractError):audit.absolute_jump(b,p)

    def test_named_entry_requires_nearby_unique_linked_instruction(self):
        row=['ffe00000','0000','TEST.ASM',2,'RETS']
        self.assertEqual(audit.label_entry('GO:\n RETS','GO',[row],'TEST.ASM'),row)
        for source,rows in [('OTHER:\n RETS',[row]),('GO:\n RETS\nGO:',[row]),('GO:\n RETS',[row,row]),('GO:\n RETS',[])]:
            with self.assertRaises(audit.a.ContractError):audit.label_entry(source,'GO',rows,'TEST.ASM')

    def test_text_preserves_empty_and_spacing_and_rejects_bad_pointers(self):
        self.assertEqual(audit.terminated_text(b'\0A  B\0',audit.a.BASE),'')
        self.assertEqual(audit.terminated_text(b'\0A  B\0',audit.a.BASE+8),'A  B')
        for data,ptr in [(b'A'*256,audit.a.BASE),(b'A\0',audit.a.BASE-8),(b'A\0',audit.a.BASE+1)]:
            with self.assertRaises(audit.a.ContractError):audit.terminated_text(data,ptr)

    def test_header_entries_and_termination(self):
        text='MEN_MAIN\n .LONG RD7FONT\n .LONG STRCNRM\n .LONG TITLE\n'+''.join(' .WORD -4\n' for _ in range(10))+' .LONG TEXT,ROUTINE,0\n .LONG 0\n'
        self.assertEqual(audit.descriptors(text)[0]['entries'],[['TEXT','ROUTINE','0']])
        for bad in [text.replace(' .LONG 0\n',''),text.replace('TEXT,ROUTINE,0','TEXT,ROUTINE'),text.replace('RD7FONT','OTHER'),text+text]:
            with self.assertRaises(audit.a.ContractError):audit.descriptors(bad)

    def test_current_source_menu_count(self):
        menus=[]
        for name in ('MENU.ASM','ADJUST.ASM'):
            menus+=audit.descriptors((audit.ROOT/'original/archive/smashtv-main'/name).read_text(encoding='latin-1'))
        self.assertEqual(len(menus),11)
        self.assertEqual(len(next(m for m in menus if m['label']=='MEN_CUST')['entries']),15)


if __name__=='__main__':unittest.main()
