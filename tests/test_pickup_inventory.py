from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import reconstruct_pickups as audit
import reconstruct_pickup_pools as pools


class PickupInventoryTests(unittest.TestCase):
    def test_pool_aliases_and_short_source_extent_remain_explicit(self):
        text='ICN1:\nICN2:\n .BYTE 0,BLDX,BLDX\nICN3:\n .BYTE 0\n .EVEN\n'
        values=pools.source_pools(text,{'BLDX':1})
        self.assertEqual(values,{'ICN1':[0,1,1],'ICN2':[0,1,1],'ICN3':[0]})
        sampled=pools.sampled_slots([0]+[1]*20,1)
        self.assertEqual(sampled['index_domain'],[1,20])
        self.assertEqual(sampled['selector_counts'],{1:20})
        self.assertEqual(len(sampled['beyond_source_extent']),20)
        self.assertEqual(pools.sampled_slots([0]+[1]*20,24)['beyond_source_extent'],[])
        for raw,extent in [([0]*20,21),([0]*20+[12],21),([0]*21,0)]:
            with self.assertRaises(audit.a.ContractError):pools.sampled_slots(raw,extent)
        with self.assertRaises(audit.a.ContractError):pools.source_pools(text.replace('BLDX','BAD'),{})

    def test_descriptor_bounds_and_fields(self):
        raw=struct.pack('<IIIHHHII',0,0,audit.a.BASE+32,90,0x8002,0xc08,0,0)
        value=audit.record(raw,audit.a.BASE);source={'z':90}
        audit.validate_record(value,source,audit.a.BASE+32,0xc08)
        for field in ('x','y','z','oid','x_velocity','y_velocity','image'):
            bad=dict(value);bad[field]+=1
            with self.subTest(field=field),self.assertRaises(audit.a.ContractError):
                audit.validate_record(bad,source,audit.a.BASE+32,0xc08)
        for address in (audit.a.BASE-16,audit.a.BASE+1,audit.a.BASE+16):
            with self.assertRaises(audit.a.ContractError):audit.record(raw,address)

    def test_source_selector_and_descriptor_reject_ambiguity(self):
        text='ICINIT .LONG 0,A,B,C,D,E\n .LONG F,G,H,I,J,K\n'
        self.assertEqual(len(audit.source_selectors(text)),12)
        for bad in (text.replace(',K',',A'),text.replace('0,A','1,A'),text.replace(',K','')):
            with self.assertRaises(audit.a.ContractError):audit.source_selectors(bad)
        desc='KEYI:\n .LONG 0,0,KEY\n .WORD 90,DMAWNZ,CLSNEUT|TYPICON|SUBKEY\n .LONG 0,0\n'
        self.assertEqual(audit.source_descriptor(desc,'KEYI')['image_symbol'],'KEY')
        for bad in (desc+desc,desc.replace('0,0,KEY','1,0,KEY')):
            with self.assertRaises(audit.a.ContractError):audit.source_descriptor(bad,'KEYI')


if __name__=='__main__':unittest.main()
