from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import sample_process_ownership as audit


class OwnershipTests(unittest.TestCase):
    def test_pool_stride_stack_and_wake_boundaries(self):
        base=0x01086790;stride=0x5a0;end=base+240*stride;wake=0xffe00000
        def inspect(address,sp,target=wake):return audit.inspect_node(dict(address=address,saved_sp=sp,wake=target),base,end,stride,{wake})
        self.assertEqual(inspect(base,base+stride),[])
        self.assertEqual(inspect(base,base+0x2a0),[])
        for ptr in (base-16,base+16,end):self.assertEqual(inspect(ptr,ptr+stride),['outside_process_pool'])
        for sp in (base+0x290,base+stride+16,base+stride-1):self.assertIn('saved_stack_outside_source_reservation',inspect(base,sp))
        self.assertEqual(inspect(base,base+stride,0),['wake_not_compiled'])


if __name__=='__main__':unittest.main()
