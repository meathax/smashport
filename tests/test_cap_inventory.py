from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from reconstruct_caps import expression,values,cap_byte


class CapInventoryTests(unittest.TestCase):
    def test_arithmetic_and_continued_tables(self):
        self.assertEqual(expression('10-2'),8);self.assertEqual(expression('-4'),-4)
        self.assertEqual(values('RMAX .BYTE 2,10-2 ; comment\n .BYTE 22-7\nNEXT .BYTE 9\n','RMAX','BYTE'),[2,8,15])
        self.assertEqual(values('GRUNTTBL\n .WORD -4,-3,0,6\nNEXT\n','GRUNTTBL','WORD'),[-4,-3,0,6])
        for bad in ('1*2','1/2','__import__(1)','1--2',''):
            with self.subTest(bad=bad),self.assertRaises(Exception):expression(bad)

    def test_status_adjustment_signed_modifier_and_byte_wrap(self):
        self.assertEqual(cap_byte(12,3,5,-4),8)
        self.assertEqual(cap_byte(12,1,5,-4),3)
        self.assertEqual(cap_byte(12,2,5,-4),3)
        self.assertEqual(cap_byte(8,1,5,-4),255)
        self.assertEqual(cap_byte(2,1,2,0),0)


if __name__=='__main__':unittest.main()
