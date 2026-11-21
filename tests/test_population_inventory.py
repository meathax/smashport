from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
from reconstruct_population import source_table,retail_table


class PopulationInventoryTests(unittest.TestCase):
    def test_source_comments_hex_and_terminators_preserved(self):
        source='WAVE_INFO:\n; row\n .WORD HULK,100,0,6,4,30 ; old\n .WORD 0\n .WORD MUTD,0,0,07FFFH,1,1\n .WORD 0\n .IF DEBUG\n .WORD HULK,5,6,1,0,1\n'
        words,locations,waves=source_table(source,'WAVE_INFO')
        retail=retail_table(words,0xffe137f0)
        self.assertEqual(len(waves),2);self.assertEqual(retail[1][0]['fields']['spawn_budget_raw'],0)
        self.assertEqual(retail[1][0]['fields']['release_interval_ticks'],32767)
        self.assertEqual(locations[:6],[3]*6)
        self.assertEqual(retail[1][0]['address'],f'{0xffe137f0+112:08x}')

    def test_rejects_unknown_type_malformed_or_unterminated_rows(self):
        for row in ['0','HULK,1,2','UNKNOWN,1,2,3,4,5','HULK,70000,0,1,1,1','HULK,1,0,1,1,1']:
            with self.subTest(row=row),self.assertRaises(Exception):source_table('X:\n .WORD '+row+'\n','X')
        for words in [[0],[17,1,0,1,1,1,0],[6,1],[6,1,0,1,1,1]]:
            with self.subTest(words=words),self.assertRaises(Exception):retail_table(words,0xffe00000)


if __name__=='__main__':unittest.main()
