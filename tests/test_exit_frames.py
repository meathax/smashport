import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import audit_exit_frames as a


class ExitFramesTests(unittest.TestCase):
    def test_live_list_preserves_cycles_and_invalid_boundaries(self):
        image=bytearray(0x20000)
        def put(addr,value,fmt='<I'):struct.pack_into(fmt,image,(addr-0x01000000)//8,value)
        first=0x010003a0;second=first+0x1f0
        put(0x01000040,first);put(first,second);put(first+0x150,0x8001,'<H')
        put(second,0);put(second+0x150,0xc001,'<H')
        row=a.snapshot(image,10)
        self.assertEqual(row['gratson'],0)
        put(0x010ee130,1,'<H')
        self.assertEqual(a.snapshot(image,10)['gratson'],1)
        self.assertEqual((row['list_boundary'],len(row['objects']),row['enemy_class_objects']),('complete',2,1))
        put(second,first);row=a.snapshot(image,11)
        self.assertEqual((row['list_boundary'],row['boundary_pointer'],len(row['objects'])),('cycle',first,2))
        put(second,0x01033160);row=a.snapshot(image,12)
        self.assertEqual((row['list_boundary'],len(row['objects'])),('outside_pool',2))
        put(0x01000040,first+16)
        self.assertEqual(a.snapshot(image,13)['list_boundary'],'outside_pool')
        put(0x01000040,0)
        self.assertEqual(a.snapshot(image,14)['objects'],[])
        with self.assertRaises(a.r.ProofError):a.snapshot(image[:-1],0)


if __name__=='__main__':unittest.main()
