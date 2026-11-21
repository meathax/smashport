"""Coverage-gap and provenance regressions for sampled content attribution."""
import contextlib
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/scenarios'))
import content_audit as audit


def inventories():
    waves = dict(waves=[dict(id='room.1',circuit=0,wave=1,role='wave_room',name=dict(text='ROOM'),source_position_anchor='WAVES.ASM:1'),
                       dict(id='room.2',circuit=0,wave=2,role='wave_room',name=None)])
    enemies = dict(object_type_contracts=[dict(id='type.robot',source_symbol='TYPRBTA',source_definition_citation='ROBO.EQU:232',retail_producer_refs=['producer.1']),
                                         dict(id='type.unknown',source_symbol='TYPFACE',source_definition_citation='ROBO.EQU:254',retail_producer_refs=[])],
                   object_OID_literal_producers=[dict(id='producer.1',type_ref='type.robot',mapping_status='TYPED_RETAIL_PRODUCER_SOURCE_OPCODE_VERIFIED',
                                                     expected_u16='8300',retail_low16_matches_source_equates=True,
                                                     typed_instruction=dict(mnem='MOVI',operands=[dict(kind='IMM',value=0x8300)]))])
    return waves,enemies


def sample(frame,gs=16,wave=1,objects=None,complete=True):
    return dict(frame=frame,gamstate=gs,circuit=0,wave=wave,world_x=-370,world_y=-172,
                objects=objects or [],object_list_complete=complete,processes=[],process_list_complete=True)


class ContentAuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.waves,self.enemies = inventories()

    def observe(self,rows):
        items,waves,oids = audit.catalog(self.waves,self.enemies)
        p=self.dir/'content.jsonl'
        p.write_text(json.dumps(dict(schema='header'))+'\n'+'\n'.join(json.dumps(r) for r in rows)+'\n')
        summary=audit.observe(p,'fixture',items,waves,oids)
        return items,summary

    def test_revisits_remain_separate_sample_spans(self):
        items,_=self.observe([sample(0),sample(60),sample(120,wave=2),sample(180)])
        observation=items['room.1']['observations'][0]
        self.assertEqual(observation['sampled_presence_spans'],[dict(first_frame=0,last_frame=60,samples=2),dict(first_frame=180,last_frame=180,samples=1)])
        self.assertEqual(observation['sampled_presence_count'],3)

    def test_attract_state_does_not_count_gameplay_room_or_actor(self):
        items,summary=self.observe([sample(0,gs=0,objects=[dict(oid=0x8300)])])
        self.assertTrue(all(not item['observations'] for item in items.values()))
        self.assertEqual(summary['totals'].get('in_game_samples',0),0)

    def test_partial_object_list_cannot_establish_oid_presence(self):
        items,summary=self.observe([sample(0,objects=[dict(oid=0x8300),dict(boundary_address=3)],complete=False)])
        self.assertFalse(items['type.robot']['observations'])
        self.assertTrue(items['room.1']['observations'])
        self.assertEqual(summary['totals']['in_game_partial_object_samples_excluded'],1)

    def test_partial_process_list_remains_explicit(self):
        row=sample(0);row['process_list_complete']=False
        _,summary=self.observe([row])
        self.assertEqual(summary['totals']['partial_process_samples'],1)

    def test_exact_oid_match_is_not_type_mask_guess(self):
        items,_=self.observe([sample(0,objects=[dict(oid=0x8301)])])
        self.assertFalse(items['type.robot']['observations'])

    def test_multiple_objects_count_presence_once_per_sample(self):
        items,_=self.observe([sample(0,objects=[dict(oid=0x8300),dict(oid=0x8300)])])
        self.assertEqual(items['type.robot']['observations'][0]['sampled_presence_count'],1)

    def test_signed_world_coordinates_remain_signed(self):
        items,_=self.observe([sample(0)])
        self.assertEqual(items['room.1']['observations'][0]['context_at_first']['world_x'],-370)

    def test_unknown_selector_is_recorded_not_invented(self):
        items,summary=self.observe([sample(0,wave=99)])
        self.assertFalse(items['room.1']['observations'])
        self.assertEqual(summary['unknown_selectors'][0]['selector'],[0,99])

    def test_unmapped_source_producer_does_not_bind_oid(self):
        self.enemies['object_OID_literal_producers'][0]['mapping_status']='OPEN'
        items,_=self.observe([sample(0,objects=[dict(oid=0x8300)])])
        self.assertFalse(items['type.robot']['observations'])

    def test_no_typed_producer_is_a_coverage_gap_not_unreachable(self):
        items,_=self.observe([sample(0,objects=[dict(oid=0x9900)])])
        self.assertFalse(items['type.unknown']['observations'])
        self.assertNotIn('unreachable',items['type.unknown'])
        self.assertEqual(items['type.unknown']['binding'],'NO_VERIFIED_RETAIL_LITERAL_PRODUCER')

    def test_wrong_literal_rejects_catalog(self):
        self.enemies['object_OID_literal_producers'][0]['typed_instruction']['operands'][0]['value']=0x8400
        with self.assertRaises(audit.runner.ProofError):audit.catalog(self.waves,self.enemies)

    def test_wrong_type_reference_rejects_catalog(self):
        self.enemies['object_OID_literal_producers'][0]['type_ref']='other'
        with self.assertRaises(audit.runner.ProofError):audit.catalog(self.waves,self.enemies)

    def test_duplicate_wave_rejects_catalog(self):
        self.waves['waves'].append(copy.deepcopy(self.waves['waves'][0]))
        with self.assertRaises(audit.runner.ProofError):audit.catalog(self.waves,self.enemies)

    def test_boolean_selector_rejects_catalog(self):
        self.waves['waves'][0]['circuit']=False
        with self.assertRaises(audit.runner.ProofError):audit.catalog(self.waves,self.enemies)

    def test_missing_producer_reference_rejects_catalog(self):
        self.enemies['object_OID_literal_producers']=[]
        with self.assertRaises(audit.runner.ProofError):audit.catalog(self.waves,self.enemies)

    def test_changed_artifact_rejects_pin(self):
        p=self.dir/'data';p.write_bytes(b'before')
        pin=audit.runner.pin(p);p.write_bytes(b'after')
        with self.assertRaises(audit.runner.ProofError):audit.verify_pin(pin)

    def test_missing_pin_hash_rejects_pin(self):
        with self.assertRaises(audit.runner.ProofError):audit.verify_pin(dict(path='missing',bytes=0))

    def test_boolean_pin_size_rejected(self):
        with self.assertRaises(audit.runner.ProofError):audit.verify_pin(dict(path='missing',bytes=True,sha256='0'*64))

    def test_running_or_failed_report_cannot_make_content_evidence(self):
        p=self.dir/'report.json'
        for state in ['RUNNING','FAILED']:
            with self.subTest(state=state):
                p.write_text(json.dumps(dict(result=state)))
                with self.assertRaises(audit.runner.ProofError):audit.audit(p,self.dir/'unused')

    def test_live_pid_rejects_otherwise_terminal_report(self):
        p=self.dir/'report.json';p.write_text(json.dumps(dict(result='PROOFS_COMPLETE_TARGET_CLOSURE_OPEN',owned_live_pids=[1])))
        with self.assertRaises(audit.runner.ProofError):audit.audit(p,self.dir/'unused')

    def test_cli_preserves_existing_artifact(self):
        p=self.dir/'audit.json';p.write_bytes(b'preserved')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(audit.main(['--report','missing','--out',str(p)]),2)
        self.assertEqual(p.read_bytes(),b'preserved')

    def test_running_variant_cannot_add_room_observations(self):
        p=self.dir/'variant.json';p.write_text(json.dumps(dict(result='RUNNING')))
        with self.assertRaises(audit.runner.ProofError):audit.audit_variant(p,{}, {}, {})

    def test_live_variant_cannot_add_room_observations(self):
        p=self.dir/'variant.json';p.write_text(json.dumps(dict(result='PASS_REFERENCE_MATCH_CONTENT_GATE_OPEN',owned_live_pids=[1])))
        with self.assertRaises(audit.runner.ProofError):audit.audit_variant(p,{}, {}, {})

    def test_failed_variant_cannot_add_room_observations(self):
        p=self.dir/'variant.json';p.write_text(json.dumps(dict(result='FAILED',owned_live_pids=[])))
        with self.assertRaises(audit.runner.ProofError):audit.audit_variant(p,{}, {}, {})

    def test_cli_returns_open_gate_even_for_successful_measurements(self):
        p=self.dir/'audit.json'
        with patch.object(audit,'audit',return_value=dict(result='OBSERVED',summary={})),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(audit.main(['--report','unused','--out',str(p)]),3)
        self.assertTrue(p.exists())


class TableTests(unittest.TestCase):
    def setUp(self):
        self.row=dict(id='face',source_symbol='TYPFACE',source_type_u16='1900',additional_ROM_record_refs=['table.face'])
        self.table=dict(id='table.face',retail_bit_address='fff2a890',entire_linked_source_table_bytes_match=True,record_count=1,
                        producer=dict(source_opcode_verified=True,typed_instruction=dict(mnem='MOVI',operands=[dict(kind='IMM',value=0xfff2a890)])),
                        parts=[dict(id='face.part0',OID_u16='9900',source_OID_symbols=['CLSENMY','TYPFACE'],exact_three_source_scalars_match=True)])
        self.enemies=dict(boss_component_tables=[self.table])

    def test_component_records_bind_without_an_instruction_oid_literal(self):
        self.assertEqual(audit.table_oid_values(self.row,self.enemies),{0x9900:['face.part0']})

    def test_wrong_source_record_rejected(self):
        self.table['parts'][0]['exact_three_source_scalars_match']=False
        with self.assertRaises(audit.runner.ProofError):audit.table_oid_values(self.row,self.enemies)

    def test_wrong_table_pointer_rejected(self):
        self.table['producer']['typed_instruction']['operands'][0]['value']-=16
        with self.assertRaises(audit.runner.ProofError):audit.table_oid_values(self.row,self.enemies)

    def test_wrong_type_reference_rejected(self):
        self.table['parts'][0]['source_OID_symbols']=['CLSENMY','TYPMUT']
        with self.assertRaises(audit.runner.ProofError):audit.table_oid_values(self.row,self.enemies)

    def test_wrong_record_count_rejected(self):
        self.table['record_count']=2
        with self.assertRaises(audit.runner.ProofError):audit.table_oid_values(self.row,self.enemies)

    def test_missing_table_rejected(self):
        self.row['additional_ROM_record_refs']=['missing']
        with self.assertRaises(audit.runner.ProofError):audit.table_oid_values(self.row,self.enemies)

    def test_neutral_component_record_is_not_an_enemy_oid(self):
        self.table['parts'].append(dict(id='neutral',OID_u16='0000',source_OID_symbols=['CLSNEUT'],exact_three_source_scalars_match=True))
        self.table['record_count']=2
        self.assertEqual(audit.table_oid_values(self.row,self.enemies),{0x9900:['face.part0']})

    def test_neutral_model_parts_do_not_become_enemy_observations(self):
        row=dict(source_type_u16='1800',additional_ROM_record_refs=['model'])
        records=[dict(id='neutral',OID_low16_hex='0004',OID_word_matches_linked_source=True,role='neutral_decorative_or_support_component'),
                 dict(id='enemy',OID_low16_hex='9801',OID_word_matches_linked_source=True,role='collidable_mutoid_component')]
        enemies=dict(boss_model_pointer_tables=[dict(id='model',producer=dict(literal_offset_transfer_matches=True),part_refs=['neutral','enemy'])],boss_model_record_catalog=records)
        self.assertEqual(audit.table_oid_values(row,enemies),{0x9801:['enemy']})


if __name__ == '__main__':
    unittest.main()
