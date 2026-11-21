"""Regression controls for source-bound retail route reconstruction."""
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/scenarios'))
import reconstruct_routes as routes
from controlflow_audit import ContractError


class RouteInventoryTests(unittest.TestCase):
    def test_source_extents_and_fields(self):
        text=(ROOT/'original/archive/smashtv-main/ENDWAVE.ASM').read_text(encoding='utf-8')
        tables=routes.source_tables(text)
        self.assertEqual([len(t)//12 for t in tables],[10,18,21])
        self.assertEqual([v['value'] for v in tables[0][:12]],
                         [0x006e0145,0,0,0x0091018b,0,0,2,0,0,2,0,0])
        for bad in [text.replace('CIRCB1:','MISSING:'),text+'\nCIRCB1:\n',
                    text.replace('[110,325],0,0','[110,325],0',1),
                    text.replace('[110,325],0,0','[110,325],0,0,0',1)]:
            with self.subTest(text=bad[:15]),self.assertRaises(ContractError):routes.source_tables(bad)

    def test_packed_coordinates_and_route_fields(self):
        values=[0x006e0145,0,0,0x0091018b,0,0,2,0,0,2,0,0]
        self.assertEqual(routes.decode_record(values),[dict(slot=0,rectangle_xyxy=[325,110,395,145],destination_field=2,from_door_field=2)])
        signed=values.copy();signed[0]=0xfff0fff0;signed[3]=0xffffffff
        self.assertEqual(routes.decode_record(signed)[0]['rectangle_xyxy'],[-16,-16,-1,-1])
        for index,value in [(0,0),(3,1),(6,0),(6,25),(9,5),(0,True),(1,1)]:
            bad=values.copy();bad[index]=value
            with self.subTest(index=index,value=value),self.assertRaises(ContractError):routes.decode_record(bad)
        for bad in [[],values[:-1],[0]*12]:
            with self.assertRaises(ContractError):routes.decode_record(bad)


if __name__=='__main__':unittest.main()
