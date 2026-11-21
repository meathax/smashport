from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from snapshot_cmos import read_cmos


class SnapshotTests(unittest.TestCase):
    def test_scoped_layout_and_malformed_headers(self):
        data=b'STVMS003'+bytes(4)+b'\1'+bytes(136)
        for count,width in [(0x10000,2),(0x2000,2),(0x1000,4),(0x4000,2)]:
            data+=struct.pack('<Q',count)+bytes(count*width)
        self.assertEqual(len(read_cmos(data)),16384)
        modified=bytearray(data);modified[-2:]=b'\x34\x12'
        self.assertEqual(read_cmos(modified)[-1],0x1234)
        for bad in [data[:-1],data[:149],b'BADMAGIC'+data[8:],data[:12]+b'\2'+data[13:],data[:149]+bytes(8)+data[157:]]:
            with self.assertRaises(ValueError):read_cmos(bad)


if __name__=='__main__':unittest.main()
