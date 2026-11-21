from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import reconstruct_enemies as audit


class EnemyInventoryTests(unittest.TestCase):
    def test_null_dispatch_is_only_safe_when_corresponding_count_is_zero(self):
        rooms=[dict(circuit=0,wave=1,rows=[dict(values=[6,100,0,6,4,30])])]
        initial=[0]*16;spawn=[0]*16;initial[5]=0xffe00000;spawn[5]=0xffe00010
        self.assertEqual(len(audit.population_targets(rooms,initial,spawn)),2)
        for a,b in [([0]*16,spawn),(initial,[0]*16),(initial[:-1],spawn)]:
            with self.assertRaises(audit.a.ContractError):audit.population_targets(rooms,a,b)
        rooms[0]['rows'][0]['values'][1]=0;rooms[0]['rows'][0]['values'][4]=0
        self.assertEqual(audit.population_targets(rooms,[0]*16,[0]*16),[])
        for kind in (0,17):
            rooms[0]['rows'][0]['values'][0]=kind
            with self.assertRaises(audit.a.ContractError):audit.population_targets(rooms,initial,spawn)
        for values in ([],[6],[6,1,0,0,0,0,0]):
            rooms[0]['rows'][0]['values']=values
            with self.assertRaises(audit.a.ContractError):audit.population_targets(rooms,initial,spawn)

    def test_dispatch_table_extent_and_null_slot(self):
        text='TYP_TBL:\n .LONG '+','.join(['0']+['NO']*15)+'\nNO RETS\n'
        self.assertEqual(len(audit.table_labels(text,'TYP_TBL')),16)
        for bad in (text.replace('0,NO','NO,NO'),text.replace('0,NO','0')):
            with self.assertRaises(audit.a.ContractError):audit.table_labels(bad,'TYP_TBL')


if __name__=='__main__':unittest.main()
