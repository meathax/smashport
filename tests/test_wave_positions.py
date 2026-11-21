"""Negative controls for the reconstructed wave position inventory."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/scenarios'))
import reconstruct_wave_positions as inventory
from controlflow_audit import ContractError


class WavePositionTests(unittest.TestCase):
    def test_source_arithmetic_without_eval(self):
        self.assertEqual(inventory.coordinate('>EC2+>1D8'), 0x109a)
        self.assertEqual(inventory.coordinate('5'), 5)
        for bad in ['-1', '>10000', '1+', '1*2', '__import__("os")', '0x10']:
            with self.subTest(bad=bad), self.assertRaises(ContractError):
                inventory.coordinate(bad)

    def test_long_values_need_word_not_long_alignment(self):
        data = b'xx' + struct.pack('<II', 0x12345678, 0xabcdef01)
        self.assertEqual(inventory.read_words(data, inventory.BASE + 16, 2), [0x12345678, 0xabcdef01])
        for address, count in [(inventory.BASE+8, 1), (inventory.BASE-16, 1),
                               (inventory.BASE+16, 3), (inventory.BASE, -1)]:
            with self.subTest(address=address, count=count), self.assertRaises(ContractError):
                inventory.read_words(data, address, count)

    def test_preserved_source_extent_and_packed_order(self):
        source = (ROOT / 'original/archive/smashtv-main/WAVES.ASM').read_text(encoding='utf-8')
        tables = inventory.source_tables(source)
        self.assertEqual([len(t) for t in tables], [12, 20, 23])
        self.assertEqual(tables[0][1]['value'], 0x015201d8)
        self.assertEqual(tables[2][-1]['value'], 0x029e109a)
        for bad in [source.replace('CRCT1YX:', 'MISSING:'), source+'\nCRCT1YX:\n',
                    source.replace('[>152,>1D8]', '[>10000,>1D8]', 1),
                    source.replace('[>152,>1D8]', '[>152,>1D8],0', 1)]:
            with self.assertRaises(ContractError): inventory.source_tables(bad)


if __name__ == '__main__':
    unittest.main()
