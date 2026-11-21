#!/usr/bin/env python3
"""Attribute sampled retail selectors/OID values, preserving C3 coverage gaps.

Consumes a complete C2 report and the hash-pinned static content inventories.
This does not prove room clearance, exact spawn timing, routes, collision paths,
or that a matching OID literal was produced by a particular instruction.
"""
import argparse
import collections
import json
from pathlib import Path
import re

import rebaseline as runner

ROOT = Path(__file__).resolve().parents[2]


def verify_pin(recorded):
    runner.require(type(recorded) is dict and set(recorded) == {'path', 'bytes', 'sha256'}, 'artifact pin malformed')
    runner.require(type(recorded['path']) is str and type(recorded['bytes']) is int and recorded['bytes'] >= 0
                   and type(recorded['sha256']) is str and re.fullmatch(r'[0-9a-f]{64}',recorded['sha256']),
                   'artifact pin values malformed')
    actual = runner.pin(ROOT / recorded['path'])
    runner.require(actual['bytes'] == recorded['bytes'] and actual['sha256'] == recorded['sha256'],
                   f"artifact changed: {recorded['path']}")
    return Path(actual['path'])


def table_oid_values(row, enemies):
    """Exact, source-matched ROM records only; no runtime type-mask guessing."""
    components = {t['id']:t for t in enemies.get('boss_component_tables', [])}
    models = {t['id']:t for t in enemies.get('boss_model_pointer_tables', [])}
    records = {t['id']:t for t in enemies.get('boss_model_record_catalog', [])}
    values = collections.defaultdict(list)
    for ref in row.get('additional_ROM_record_refs', []):
        if ref in components:
            table = components[ref]
            producer = table['producer']
            runner.require(table['entire_linked_source_table_bytes_match'] is True
                           and producer['source_opcode_verified'] is True
                           and producer['typed_instruction']['mnem'] == 'MOVI'
                           and producer['typed_instruction']['operands'][0]['kind'] == 'IMM'
                           and producer['typed_instruction']['operands'][0]['value'] == int(table['retail_bit_address'],16)
                           and table['record_count'] == len(table['parts']), 'ROM component table owner differs')
            for part in table['parts']:
                value = int(part['OID_u16'],16)
                if part['source_OID_symbols'] == ['CLSNEUT']:
                    runner.require(part['exact_three_source_scalars_match'] is True and value == 0,
                                   'neutral component record contract differs')
                    continue
                runner.require(part['exact_three_source_scalars_match'] is True
                               and row['source_symbol'] in part['source_OID_symbols']
                               and 0 <= value <= 65535
                               and value & 0x1f00 == int(row['source_type_u16'],16), 'ROM component OID/source differs')
                values[value].append(part['id'])
        elif ref in models:
            table = models[ref]
            runner.require(table['producer']['literal_offset_transfer_matches'] is True,
                           'ROM model table owner differs')
            for record in table['part_refs']:
                runner.require(record in records, 'ROM model record reference missing')
                part = records[record]
                runner.require(part['OID_word_matches_linked_source'] is True, 'ROM model OID/source differs')
                value = int(part['OID_low16_hex'],16)
                if part['role'] == 'neutral_decorative_or_support_component':
                    # These source-matched neutral/support records do not acquire
                    # an enemy type merely because a boss creates them.
                    runner.require(value == 4, 'neutral boss record contract differs')
                    continue
                runner.require(part['role'] == 'collidable_mutoid_component' and 0 <= value <= 65535
                               and value & 0x1f00 == int(row['source_type_u16'],16), 'ROM boss OID/type differs')
                values[value].append(record)
        else:
            raise runner.ProofError('ROM OID table reference missing')
    return {value:sorted(set(refs)) for value,refs in values.items()}


def catalog(waves, enemies):
    items, wave_keys, oid_keys = {}, {}, collections.defaultdict(list)
    for row in waves['waves']:
        key = (row['circuit'], row['wave'])
        runner.require(all(type(v) is int and 0 <= v <= 65535 for v in key) and key not in wave_keys,
                       'duplicate/invalid wave selector')
        item = row['id']
        runner.require(item not in items, 'duplicate item ID')
        wave_keys[key] = item
        items[item] = dict(id=item, kind='wave_selector', role=row['role'],
                           name=(row.get('name') or {}).get('text'), selector=list(key),
                           source_anchor=row.get('source_position_anchor'), observations=[])
    producers = {p['id']:p for p in enemies['object_OID_literal_producers']}
    runner.require(len(producers) == len(enemies['object_OID_literal_producers']), 'duplicate OID producer')
    for row in enemies['object_type_contracts']:
        item, values = row['id'], {}
        runner.require(item not in items, 'duplicate item ID')
        for ref in row['retail_producer_refs']:
            runner.require(ref in producers, 'OID producer reference missing')
            p = producers[ref]
            if p['mapping_status'] != 'TYPED_RETAIL_PRODUCER_SOURCE_OPCODE_VERIFIED':
                continue
            expected = int(p['expected_u16'], 16)
            instruction = p['typed_instruction']
            runner.require(p['type_ref'] == item and p['retail_low16_matches_source_equates'] is True
                           and instruction['mnem'] == 'MOVI'
                           and instruction['operands'][0]['kind'] == 'IMM'
                           and instruction['operands'][0]['value'] & 65535 == expected,
                           'typed retail OID literal disagrees with source contract')
            values.setdefault(expected, []).append(ref)
        rom_values = table_oid_values(row,enemies)
        for value in set(values) | set(rom_values):
            oid_keys[value].append(item)
        items[item] = dict(id=item, kind='typed_oid_value', source_symbol=row['source_symbol'],
                           source_definition=row['source_definition_citation'],
                           literal_producer_refs={str(k):v for k,v in values.items()},
                           ROM_record_refs={str(k):v for k,v in rom_values.items()},
                           binding='VERIFIED_RETAIL_OID_VALUES' if values or rom_values else 'NO_VERIFIED_RETAIL_LITERAL_PRODUCER', observations=[])
    return items, wave_keys, oid_keys


def observe(path, scenario, items, wave_keys, oid_keys):
    """Summarize presence only. Never infer absence from a partial object list."""
    found, unknown = {}, {}
    totals = collections.Counter()
    def hit(item, sample, value=None):
        key = (item, value)
        row = found.setdefault(key, dict(scenario=scenario, first_frame=sample['frame'], last_frame=sample['frame'],
                                        sampled_presence_count=0, sampled_presence_spans=[], context_at_first=dict(circuit=sample['circuit'],wave=sample['wave'],
                                                                                     world_x=sample['world_x'],world_y=sample['world_y'])))
        spans = row['sampled_presence_spans']
        if spans and spans[-1]['last_frame'] + 60 == sample['frame']:
            spans[-1]['last_frame'] = sample['frame']
            spans[-1]['samples'] += 1
        else:
            spans.append(dict(first_frame=sample['frame'],last_frame=sample['frame'],samples=1))
        row['last_frame'] = sample['frame']
        row['sampled_presence_count'] += 1
        if value is not None:
            row['oid_u16'] = value
    with Path(path).open(encoding='utf-8') as stream:
        next(stream)  # Complete schema/cadence/node checks run before this pass.
        for line in stream:
            s = json.loads(line)
            totals['samples'] += 1
            totals['partial_object_samples'] += not s['object_list_complete']
            totals['partial_process_samples'] += not s['process_list_complete']
            if s['gamstate'] != 0x10:
                continue
            totals['in_game_samples'] += 1
            key = (s['circuit'], s['wave'])
            if key in wave_keys:
                hit(wave_keys[key], s)
            else:
                row = unknown.setdefault(key, dict(selector=list(key), first_frame=s['frame'], last_frame=s['frame'], samples=0))
                row['samples'] += 1
                row['last_frame'] = s['frame']
            if not s['object_list_complete']:
                totals['in_game_partial_object_samples_excluded'] += 1
                continue
            # A type may have multiple source literal producers. Matching a
            # value does not identify which producer/collision/variant executed.
            for value in {node['oid'] for node in s['objects']}:
                for item in oid_keys.get(value, ()):
                    hit(item, s, value)
    for (item, value), row in sorted(found.items(), key=lambda pair: (pair[0][0], pair[0][1] or -1)):
        items[item]['observations'].append(row)
    return dict(scenario=scenario, totals=dict(totals), unknown_selectors=list(unknown.values()))


def audit_variant(report_path, items, wave_keys, oid_keys):
    """Accept input-only variants only after complete native/MAME comparison."""
    path=Path(report_path).resolve()
    report=json.loads(path.read_text(encoding='utf-8'))
    runner.require(report.get('result')=='PASS_REFERENCE_MATCH_CONTENT_GATE_OPEN' and not report.get('owned_live_pids'),
                   'variant needs terminal complete reference comparison')
    runner.check_d10_sources(ROOT,report['verified_at'])
    steps={s['name']:s for s in report['steps']}
    runner.require(set(steps)=={'native-variant','mame-variant','main-diff'} and len(report['steps'])==3
                   and all(s['state']=='PASS' and s['exit_code']==0 for s in steps.values()), 'variant steps incomplete')
    for p in report['pins'].values():
        verify_pin(p)
    native,mame,diff=(steps[k] for k in ('native-variant','mame-variant','main-diff'))
    for s in steps.values():
        runner.require(runner.sha(s['log'])==s['log_sha256'],'variant log changed')
    proof=native['proof']
    final=proof['endpoint']['last_frame']
    runner.require(type(final) is int and final>=0, 'variant final frame invalid')
    artifacts={name:verify_pin(p) for name,p in proof['artifacts'].items()}
    oracle=verify_pin(mame['proof']['stream'])
    runner.require(runner.main_endpoint(artifacts['native.bin'],final)==proof['endpoint']
                   and runner.main_endpoint(oracle,final)==mame['proof']['endpoint'], 'variant endpoints differ')
    samples=runner.content_samples(artifacts['content.jsonl'],final)
    runner.require(samples==proof['samples'], 'variant content cadence changed')
    runner.require(mame['environment']['STV_LS_INPUTS']==str(artifacts['recorded.inputs'])
                   and mame['environment']['STV_LS_FRAMES']==str(final), 'variant oracle inputs/endpoint differ')
    runner.require(diff['command'][3:]==[str(oracle),str(artifacts['native.bin'])]
                   and f'NO_DIVERGENCE over {final+1} complete frames (both streams ended)' in Path(diff['log']).read_text(),
                   'variant comparison is not strict complete identity')
    scenario='variant:'+path.parent.name
    return observe(artifacts['content.jsonl'],scenario,items,wave_keys,oid_keys), dict(report=runner.pin(path),
        native_artifacts=proof['artifacts'],oracle=mame['proof']['stream'],validation=samples,
        scope='Complete main stream identity for recorded legal inputs; no additional strict sound-access proof claimed.')


def audit(report_path, inventory_evidence, variants=()):
    report_path = Path(report_path).resolve()
    report = json.loads(report_path.read_text(encoding='utf-8'))
    runner.require(report.get('result') == 'PROOFS_COMPLETE_TARGET_CLOSURE_OPEN' and not report.get('owned_live_pids'),
                   'content audit needs terminal complete reference measurements')
    runner.check_d10_sources(ROOT, report['verified_at'])
    steps = {s['name']:s for s in report['steps']}
    runner.require(len(steps) == len(report['steps']) and all(s['state'] == 'PASS' for s in steps.values()),
                   'missing/duplicate/failed reference measurement')
    evidence_path = Path(inventory_evidence).resolve()
    evidence = json.loads(evidence_path.read_text(encoding='utf-8'))
    inventory_pins = {name:evidence['inventories'][name] for name in ('waves-and-routes','enemies-and-bosses')}
    data = {name:json.loads(verify_pin(p).read_text(encoding='utf-8')) for name,p in inventory_pins.items()}
    items, wave_keys, oid_keys = catalog(data['waves-and-routes'],data['enemies-and-bosses'])
    scenario_rows, pins = [], dict(report=runner.pin(report_path), inventory_evidence=runner.pin(evidence_path),
                                  audit_tool=runner.pin(__file__), observer=runner.pin(ROOT/'tools/lockstep/content_observer.hpp'))
    # Static inventory snapshots precede metadata-only changes. Revalidate
    # their original source/ROM pins; never silently rebase their generated map.
    originals = {}
    for inventory in data.values():
        entries = inventory['input_pins']
        entries = [dict(path=k,**v) for k,v in entries.items()] if isinstance(entries,dict) else entries
        for p in entries:
            if p['path'].replace('\\','/').startswith('original/'):
                originals[p['path']] = runner.pin(verify_pin(p))
    pins['original_inventory_inputs'] = originals
    for name, final, _, _ in runner.CASES:
        runner.require(name+'-native' in steps and name+'-diff' in steps, f'{name}: missing reference proof')
        native, diff = steps[name+'-native'], steps[name+'-diff']
        runner.require(native.get('exit_code') == 0 and diff.get('exit_code') == 0, f'{name}: failed reference proof')
        native_log, diff_log = Path(native['log']), Path(diff['log'])
        runner.require(runner.sha(native_log) == native['log_sha256'] and runner.sha(diff_log) == diff['log_sha256'],
                       f'{name}: measurement log changed')
        runner.require(f'NO_DIVERGENCE over {final+1} complete frames (both streams ended)' in diff_log.read_text(),
                       f'{name}: incomplete/partial reference comparison')
        proof = native['proof']
        stream = verify_pin(proof['stream'])
        content = verify_pin(proof['content_stream'])
        runner.require(runner.main_endpoint(stream, final) == proof['endpoint'], f'{name}: main endpoint changed')
        samples = runner.content_samples(content, final)
        runner.require(samples == proof['content_samples'], f'{name}: content validation changed')
        # Pin the oracle and input used for the recorded main comparison.
        oracle = Path(diff['command'][3])
        inputs = Path(native['command'][native['command'].index('--inputs')+1])
        for p in (oracle,inputs,Path(native['command'][0])):
            expected = report['pins'].get(str(p.resolve()))
            runner.require(expected is not None and runner.pin(p) == expected, f'{name}: reference/input/exe changed')
        scenario_rows.append(observe(content, name, items, wave_keys, oid_keys))
        pins[name] = dict(content=proof['content_stream'], main=proof['stream'], oracle=runner.pin(oracle),
                          inputs=runner.pin(inputs), native_log=runner.pin(native_log), diff_log=runner.pin(diff_log),
                          validation=samples)
    runner.require(len({str(Path(p).resolve()) for p in variants})==len(variants), 'duplicate variant report')
    pins['variants']={}
    for path in variants:
        observations,proof=audit_variant(path,items,wave_keys,oid_keys)
        scenario_rows.append(observations)
        pins['variants'][str(Path(path).resolve())]=proof
    rows = list(items.values())
    for row in rows:
        row['state'] = ('OBSERVED_SELECTOR' if row['kind']=='wave_selector' else 'OBSERVED_TYPED_OID_VALUE') if row['observations'] else 'UNOBSERVED'
        if row.get('binding') == 'NO_VERIFIED_RETAIL_LITERAL_PRODUCER':
            row['state'] = 'UNBOUND_RETAIL_LITERAL'
        row['gate_proof'] = False
    return dict(schema='stv.content.audit.v1', verified_at=report['verified_at'], result='SAMPLED_OBSERVATIONS_WITH_COVERAGE_GAPS',
                C3='OPEN', gate_proof=False, sample_cadence=60, pins=pins, static_inventory_pins=inventory_pins,
                summary={kind:dict(total=sum(x['kind']==kind for x in rows),observed=sum(x['kind']==kind and bool(x['observations']) for x in rows),
                                   unbound=sum(x['kind']==kind and x.get('binding')=='NO_VERIFIED_RETAIL_LITERAL_PRODUCER' for x in rows))
                         for kind in ('wave_selector','typed_oid_value')}, scenarios=scenario_rows, items=rows,
                limitations=['Selector presence does not prove room clearance, route choice or normal progression.',
                             'Matching a validated instruction-literal or ROM-record OID value does not prove a producer, every enemy variant or collision path executed.',
                             'Only complete object lists contribute OID observations; incomplete POST lists remain counted.',
                             'Weapon process fields remain unbound and are not interpreted.',
                             'Other enemies, pickups, prizes, bosses, lifecycle and operator items still need owner/frame attribution.',
                             'Unobserved items are coverage gaps, never proved dead or unreachable.',
                             'Main oracle agreement uses RAM hashes at sampled frames; this is not physical PCB identity.'])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--report',required=True)
    ap.add_argument('--inventory-evidence',default='evidence/c3-content-tables-preparation-2026-10-02.json')
    ap.add_argument('--out',required=True)
    ap.add_argument('--variant-report',action='append',default=[],help='add a complete pinned input-only native/MAME variant report')
    a = ap.parse_args(argv)
    try:
        output = Path(a.out)
        runner.require(not output.exists(), 'preserve prior content audit; choose a fresh output')
        result = audit(a.report,a.inventory_evidence,a.variant_report)
        runner.atomic_json(output,result)
        print(json.dumps(dict(result=result['result'],summary=result['summary'],C3='OPEN')))
        return 3
    except (OSError,ValueError,KeyError,TypeError,IndexError) as error:
        print(f'CONTENT_AUDIT_FAILED: {error}')
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
