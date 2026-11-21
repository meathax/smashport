import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/scenarios'))
from route_write_events import classify
from rebaseline import ProofError


class RouteWriteTests(unittest.TestCase):
    def setUp(self):
        self.inventory = dict(transition_binding=dict(source_statements_verified=True,
            nonrelocated_bytes_identical=True, instructions=[dict(pc=p) for p in
                ('ffec9540','ffec9560','ffec9570','ffec95a0')]), physical_records=[
            dict(address='ffeccee0', table_users=[0], physical_wave=1,
                 options=[dict(slot=0, destination_field=2)])])
        self.rows = [dict(schema='stv.mame.route_writes.v1', phase='write_tap_before_store',
            pc_semantics='raw_cpu_state_not_instruction_start'),
            dict(kind='wave_write', frame=4736, address=0x010eb630, data=2, mask=65535,
                 old_wave=1, merged_wave=2, pc_raw=0xffec95a0, a0_raw=2,
                 a1_raw=0xffeccee0+192, circuit=0, gamstate=16, p1_lives=1, p2_lives=0),
            dict(kind='end', frame=10000, events=1)]

    def run_rows(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'events.jsonl'
            p.write_text('\n'.join(json.dumps(r) for r in rows), encoding='utf-8')
            return classify(p,self.inventory,10000)

    def test_bound_write_is_not_clearance(self):
        result=self.run_rows(self.rows)
        self.assertEqual(len(result['exit_writes']),1)
        self.assertFalse(result['clearance_proved'])

    def test_wrong_owner_value_mask_or_endpoint_rejected(self):
        for key,value in [('a1_raw',0),('old_wave',2),('circuit',1),('mask',255),
                          ('a0_raw',3),('merged_wave',3),('frame',10001)]:
            rows=copy.deepcopy(self.rows);rows[1][key]=value
            with self.subTest(key=key),self.assertRaises(ProofError):self.run_rows(rows)
        rows=copy.deepcopy(self.rows);rows[-1]['events']=2
        with self.assertRaises(ProofError):self.run_rows(rows)

    def test_zero_lives_are_raw_fields_not_player_presence(self):
        rows=copy.deepcopy(self.rows)
        rows[1]['p1_lives']=rows[1]['p2_lives']=0
        result=self.run_rows(rows)
        self.assertEqual(len(result['exit_writes']),1)
        self.assertFalse(result['clearance_proved'])

    def test_boot_and_other_writers_excluded(self):
        for key,value in [('pc_raw',0),('gamstate',0)]:
            rows=copy.deepcopy(self.rows);rows[1][key]=value
            result=self.run_rows(rows)
            self.assertEqual(len(result['excluded_writes']),1)
            self.assertEqual(result['exit_writes'],[])


if __name__=='__main__':unittest.main()
