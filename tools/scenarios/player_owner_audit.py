#!/usr/bin/env python3
"""Bind sampled weapon selectors to live retail player owners using full RAM.

This development tool accepts an owned, coupled capture and the current full
reference receipt. It compares complete bounded main/sound windows against the
pinned MAME streams. A bounded window never renews a full-game fidelity gate.
No runtime program, recorder, source input or stored oracle is modified.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import struct
import sys

import rebaseline as runner
import c3_action_terminal_audit as actions

ROOT = Path(__file__).resolve().parents[2]
BASE = 0x01000000
RAM_SIZE = 0x20000
REGISTRY = 0x010DE2B0
FIELDS = {'pid': (0x20, 16), 'wake': (0xE0, 32), 'saved_sp': (0x40, 32),
          'pdata198_raw': (0x198, 16), 'saved_a8': (0xC0, 32),
          'saved_a9': (0xA0, 32), 'saved_a10': (0x80, 32), 'saved_a11': (0x60, 32)}
MODES = ('normal', 'spray', 'pod', 'grenade', 'nonstop')


def read_bits(image, address, width):
    runner.require(type(address) is int and type(width) is int and 0 < width <= 32,
                   'invalid CPU RAM field')
    bit = address - BASE
    runner.require(0 <= bit and bit + width <= len(image) * 8, 'CPU RAM field outside image')
    offset, shift = divmod(bit, 8)
    extent = (shift + width + 7) // 8
    return (int.from_bytes(image[offset:offset + extent], 'little') >> shift) & ((1 << width) - 1)


def process_list(image):
    """Match the passive observer, including its named incomplete boundary."""
    node, rows, seen = read_bits(image, BASE, 32), [], set()
    while node:
        if node % 16 or node in seen or not BASE <= node <= BASE + len(image) * 8 - 0x1A8:
            rows.append(dict(boundary_address=node, reason='invalid_or_cyclic_ram_node'))
            return rows, False
        seen.add(node)
        rows.append(dict(address=node, **{k: read_bits(image, node + off, width)
                                         for k, (off, width) in FIELDS.items()}))
        node = read_bits(image, node, 32)
    return rows, True


def owners(image, sample):
    """Attribute only live registry owners with reciprocal torso/leg links."""
    rows, complete = process_list(image)
    runner.require(rows == sample['processes'] and complete == sample['process_list_complete'],
                   'full RAM and same-frame process sidecar disagree')
    for key, address, width in [('gamstate', 0x010865A0, 16), ('circuit', 0x010EB610, 16),
                                ('wave', 0x010EB630, 16), ('p1_lives', 0x01086360, 16),
                                ('p2_lives', 0x01086520, 16)]:
        runner.require(read_bits(image, address, width) == sample[key], 'full RAM and sidecar globals disagree')
    bound, excluded = [], []
    if sample['gamstate'] != 16:
        return bound, [dict(reason='outside_in_game_state')]
    if not complete:
        return bound, [dict(reason='incomplete_active_process_list')]
    active = {row['address']: row for row in rows}
    for player in (1, 2):
        torso = read_bits(image, REGISTRY + (player - 1) * 32, 32)
        reason = None
        if not torso:
            reason = 'empty_player_registry'
        elif torso not in active:
            reason = 'registry_owner_not_active'
        elif active[torso]['pid'] != 246 + player:
            reason = 'registry_owner_pid_differs'
        elif torso + 0x2E0 > BASE + len(image) * 8:
            reason = 'torso_link_outside_ram'
        elif read_bits(image, torso + 0x1D8, 8) != player:
            reason = 'registry_owner_player_number_differs'
        else:
            legs = read_bits(image, torso + 0x2C0, 32)
            if legs == torso or legs not in active:
                reason = 'leg_owner_not_distinct_and_active'
            elif active[legs]['pid'] != 246 + player:
                reason = 'leg_owner_pid_differs'
            elif read_bits(image, legs + 0x160, 32) != torso:
                reason = 'leg_torso_backlink_differs'
            elif read_bits(image, torso + 0x100, 32) != read_bits(image, legs + 0x100, 32):
                reason = 'leg_torso_object_link_differs'
            else:
                selector = read_bits(image, torso + 0x198, 16)
                bound.append(dict(player=player, torso=torso, legs=legs, selector=selector,
                                  weapon=MODES[selector] if selector < len(MODES) else None,
                                  wake=active[torso]['wake'], frame=sample['frame'],
                                  circuit=sample['circuit'], wave=sample['wave']))
        if reason:
            excluded.append(dict(player=player, reason=reason))
    return bound, excluded


def chunk_hash(payload):
    runner.require(len(payload) == 256, 'full region chunk must contain 256 bytes')
    digest = 2166136261
    # Recorder uses multiply-then-XOR on 64 little-endian words, not byte FNV.
    for word in struct.unpack('<64I', payload):
        digest = ((digest * 16777619) & 0xFFFFFFFF) ^ word
    return digest


def write_record(stream, kind, frame, payload):
    stream.write(struct.pack('<BIH', kind, frame, len(payload)))
    stream.write(payload)


def convert_and_observe(full, content, compact, final):
    """Reconstruct full RAM only; reject compact hashes and stale sidecars."""
    runner.require(type(final) is int and final >= 0, 'invalid intended frame')
    image, initialized, current = bytearray(RAM_SIZE), set(), 0
    totals, exclusions, modes, unknown = collections.Counter(), {}, {}, {}
    with Path(content).open(encoding='utf-8') as sidecar, Path(compact).open('xb') as output:
        header = json.loads(sidecar.readline())
        runner.require(header == dict(schema='stv.content.samples.v1', cadence=60,
                                      phase='before_main_region_dump', process_pdata198='raw_unbound_field'),
                       'unknown content phase/cadence')
        output.write(runner.lsfile.MAGIC)
        for frame, records in runner.lsfile.frames(full, keep_pc=True):
            runner.require(frame == current and current <= final, 'full stream endpoint/frame sequence differs')
            summary, seen = False, set()
            for kind, record_frame, payload in records:
                runner.require(record_frame == frame, 'full record disagrees with frame end')
                if kind in (1, 2, 3):
                    runner.require(not summary and frame % 60 == 0 and len(payload) == 258,
                                   'full region length/order/cadence differs; compact hashes cannot recover RAM')
                    index, = struct.unpack_from('<H', payload)
                    runner.require(index < runner.lsfile.SIZES[kind] // 256 and (kind, index) not in seen,
                                   'full region index invalid or duplicated')
                    seen.add((kind, index))
                    if kind == 1:
                        image[index * 256:(index + 1) * 256] = payload[2:]
                        initialized.add(index)
                    payload = struct.pack('<HI', index, chunk_hash(payload[2:]))
                elif kind == 9:
                    runner.require(not summary and len(payload) == 8, 'missing/duplicate/malformed full summary')
                    summary = True
                else:
                    raise runner.ProofError('unexpected full summary-stream record')
                write_record(output, kind, frame, payload)
            runner.require(summary, 'full frame lacks one summary')
            write_record(output, 8, frame, b'')
            if frame % 60 == 0:
                runner.require(len(initialized) == RAM_SIZE // 256, 'initial full RAM dump incomplete')
                sample = json.loads(sidecar.readline())
                runner.require(sample['frame'] == frame, 'sidecar sample frame differs')
                bound, excluded = owners(image, sample)
                totals['samples'] += 1
                totals['bound_player_samples'] += len(bound)
                totals['in_game_samples'] += sample['gamstate'] == 16
                totals['partial_process_samples'] += not sample['process_list_complete']
                bound_addresses = {row['torso'] for row in bound}
                totals['unattributed_player_pid_samples'] += sum(
                    row.get('pid') in (247, 248) and row['address'] not in bound_addresses
                    for row in sample['processes'] if 'pid' in row)
                for row in bound:
                    key = (row['player'], row['selector'])
                    table = modes if row['weapon'] is not None else unknown
                    if key not in table:
                        table[key] = dict(row, last_frame=frame, sample_count=0)
                    table[key]['last_frame'] = frame
                    table[key]['sample_count'] += 1
                for row in excluded:
                    key = (row.get('player'), row['reason'])
                    if key not in exclusions:
                        exclusions[key] = dict(row, first_frame=frame, count=0)
                    exclusions[key]['count'] += 1
            current += 1
        runner.require(current == final + 1 and not sidecar.read().strip(), 'full stream/sidecar endpoint incomplete or trailing')
    return dict(totals=dict(totals), bound_weapon_selectors=list(modes.values()),
                unknown_owned_selectors=list(unknown.values()), exclusions=list(exclusions.values()),
                endpoint=runner.main_endpoint(compact, final))


def main_window(source, output, final):
    with Path(output).open('xb') as stream:
        stream.write(runner.lsfile.MAGIC)
        ended = False
        for kind, frame, payload in runner.lsfile.records(source):
            runner.require(frame <= final, 'oracle lacks requested main endpoint')
            write_record(stream, kind, frame, payload)
            if kind == 8 and frame == final:
                ended = True
                break
        runner.require(ended, 'oracle main window incomplete')
    return runner.main_endpoint(output, final)


def sound_window(source, output, final):
    with Path(source).open('rb') as original, Path(output).open('xb') as stream:
        magic = original.read(8)
        runner.require(magic == b'STVSND01', 'bad sound oracle magic')
        stream.write(magic)
        ended = False
        while head := original.read(1):
            kind = head[0]
            runner.require(kind in runner.sndfile.SIZES, 'unknown sound oracle record')
            payload = original.read(runner.sndfile.SIZES[kind])
            runner.require(len(payload) == runner.sndfile.SIZES[kind], 'truncated sound oracle payload')
            frame, = struct.unpack_from('<I', payload)
            runner.require(frame <= final, 'oracle lacks requested sound endpoint')
            stream.write(head + payload)
            if kind == 9 and frame == final:
                ended = True
                break
        runner.require(ended, 'oracle sound window incomplete')
    return runner.sound_endpoint(output, final)


def weapon_catalog(binding):
    """Use currently checked CREATE/pickup sites, not an absent old inventory."""
    rows=[]
    for name,selector,constant,write,line,pid,target,call,entry in actions.WEAPONS:
        runner.require(selector==len(rows) and name==MODES[selector], 'Weapon selector order changed')
        sites=[pid,target,call,entry]+([constant[1],write[1]] if write else [])
        runner.require(all(pc in binding['retail'] and binding['retail'][pc]['pc']==pc for pc in sites),
                       'Weapon catalog lacks verified retail sites')
        rows.append(dict(id='weapon.mode.'+name,WPN_TYP_value=selector,
            mapping_status='CURRENT_CREATE_AND_PICKUP_CONTRACT_VERIFIED',projectile_code_entries=[entry],
            bound_sites=sites,player_event_proof=False,lifetime_proof=False))
    runner.require(len(rows)==len(MODES),'Incomplete weapon catalog')
    return dict(schema='stv.current_weapon_catalog.v1',weapons=rows,contract=binding,
                builder=runner.pin(actions.__file__),global_content_completeness='OPEN')


def contract(baseline):
    """Recover narrow typed field relations from the pinned emitted program."""
    blocks, pins = {}, {}
    for path in sorted((runner.generated_root() / 'l8').glob('l8_chunk_*.cpp')):
        expected = baseline['pins'].get(str(path.resolve()))
        runner.require(expected == runner.pin(path), 'pinned emitted program changed')
        pins[str(path)] = expected
        for block in re.split(r'(?=^L_[0-9a-f]{8}:)', path.read_text(encoding='utf-8'), flags=re.M)[1:]:
            pc = block[2:10]
            runner.require(pc not in blocks, 'duplicate emitted instruction')
            blocks[pc] = block.splitlines()
    # Source anchors identify the routine/index, while actual emitted operands
    # establish the retail addresses. Added retail instructions remain explicit.
    checks = {
        'ffe787a0': ('[WAVES.ASM:642]', 'rf(m,0x010de2b0u,1)'),
        'ffe1a6b0': ('[ROBO.ASM:1574]', 'wb(m,c.a[13]+0x000001d8u,c.a[8]);'),
        'ffe1a6d0': ('[ROBO.ASM:1575]', 'c.a[8]=r;'),
        'ffe1a6e0': ('[ROBO.ASM:1576]', 'u32 k=5, r=c.a[8]'),
        'ffe1a7a0': ('ADDI {L1},A8', 'u32 a=0x010de2b0u, b=c.a[8]'),
        'ffe1a7d0': ('MOVE A13,*A8,1', 'wf(m,c.a[8],c.a[13],1);'),
        'ffe1aa40': ('MOVI {L1},A7', 'c.a[7]=0xffe1e620u;'),
        'ffe1ad00': ('MOVI {L1},A7', 'c.a[7]=0xffe1e620u;'),
        'ffe1ae40': ('MOVE A0,*A13({W1}),0', 'wf(m,c.a[13]+0x00000198u,c.a[0],0);'),
        'ffe1e620': ('MOVE A8,*A13({W1}),1', 'wf(m,c.a[13]+0x00000100u,c.a[8],1);'),
        'ffe1e640': ('MOVE A10,*A13({W1}),1', 'wf(m,c.a[13]+0x00000160u,c.a[10],1);'),
        'ffe1e680': ('MOVE A13,*A10({W1}),1', 'wf(m,c.a[10]+0x000002c0u,c.a[13],1);'),
        'ffe337c0': ('[SHOTS.ASM:954]', 'rf(m,c.a[13]+0x00000198u,0)'),
        'ffe31540': ('[SHOTS.ASM:752]', 'wf(m,c.a[0]+0x00000198u,c.a[1],0);'),
    }
    matched = []
    for pc, (annotation, operand) in checks.items():
        block = blocks.get(pc, [])
        runner.require(block and annotation in block[0] and operand in '\n'.join(block[1:]),
                       'retail player owner instruction contract differs at ' + pc)
        matched.append(dict(pc=pc, header=block[0], matched_native_operand=operand))
    fields = {'MPROC.EQU': {27: 'PDATA .SET 100H'},
              'ROBO.EQU': {75: 'TSO_PTR .EQU PDATA', 82: 'WPN_TYP .EQU PDATA+152', 86: 'PNUM .EQU PDATA+216',
                          99: 'LEG_PRC .EQU PDATA+448', 107: 'TSO_PRC .EQU PDATA+96',
                          159: 'PLY1PID .EQU 247', 160: 'PLY2PID .EQU 248'},
              'ROBO.ASM': {123: '.BSS PLYRPRCS,64', 1582: 'ADDI PLYRPRCS,A8',
                           1583: 'MOVE A13,*A8,L', 2067: 'MOVE A10,*A13(TSO_PRC),L',
                           2069: 'MOVE A13,*A10(LEG_PRC),L'},
              'WAVES.ASM': {642: 'MOVE @PLYRPRCS,A7,L'}}
    sources = {}
    for name, lines in fields.items():
        path = ROOT / 'original/archive/smashtv-main' / name
        source = path.read_text(encoding='utf-8').splitlines()
        for line, expected in lines.items():
            actual = ' '.join(source[line - 1].split(';')[0].split())
            runner.require(actual == expected, f'player source field contract differs at {name}:{line}')
        sources[name] = runner.pin(path)
    inventory_path = ROOT / 'evidence/c3-content-tables-preparation-2026-10-02.json'
    inventory_evidence = json.loads(inventory_path.read_text(encoding='utf-8'))
    historical=inventory_evidence['inventories']['weapons-pickups-prizes']
    old_path=Path(historical['path'])
    if not old_path.is_absolute():old_path=ROOT/old_path
    history_state='MISSING_PINNED_ARTIFACT'
    if old_path.exists():
        checked_pin(historical)
        history_state='EXACT_PIN_MATCH'
    catalog=weapon_catalog(actions.contracts())
    runner.require(all(pc in blocks for w in catalog['weapons'] for pc in w['projectile_code_entries']),
                   'Current projectile entry absent from pinned owner program')
    return dict(registry_bit_address=f'{REGISTRY:08x}', registry_stride_bits=32,
                torso_entry='ffe1a6b0', legs_entry='ffe1e620', weapon_field_bits='198',
                matched_instructions=matched, emitted_pins=pins, source_pins=sources,
                weapon_catalog=catalog, historical_weapon_inventory=dict(state=history_state,expected=historical),
                inventory_evidence=runner.pin(inventory_path),
                retail_differences='Extra torso initialization and additional same-PID children; no whole-body source match claimed.',
                scope='Registry owner and reciprocal live links at each sample, not arbitrary return/callback lifetime closure.')


def checked_pin(value):
    path = Path(value['path'])
    path = (path if path.is_absolute() else ROOT / path).resolve()
    runner.require(runner.pin(path) == dict(value, path=str(path)), 'artifact changed: ' + str(path))
    return path


def capture_command(command, baseline):
    runner.require(type(command) is list and len(command) >= 4 and all(type(v) is str for v in command),
                   'malformed coupled capture command')
    options = ('--inputs', '--snd', '--coverage', '--content')
    runner.require(all(command.count(option) == 1 and command.index(option) + 1 < len(command) for option in options),
                   'coupled capture command requires --inputs, --snd, --coverage and --content')
    runner.require(command[3].isdigit(), 'capture last frame must be a nonnegative integer')
    final = int(command[3])
    expected = list(baseline)
    expected[2:4] = command[2:4]
    for option in ('--snd', '--coverage', '--content'):
        expected[expected.index(option) + 1] = command[command.index(option) + 1]
    runner.require(command == expected and final <= int(baseline[3]), 'capture command/input/profile differs')
    return final


def audit(capture_path, baseline_path, directory):
    capture_path, baseline_path = Path(capture_path).resolve(), Path(baseline_path).resolve()
    capture, baseline = (json.loads(path.read_text(encoding='utf-8')) for path in (capture_path, baseline_path))
    runner.require(capture['result'] == 'CAPTURES_COMPLETE_IDENTITY_PENDING' and not capture['owned_live_pids'],
                   'full-RAM capture must be terminal and successful')
    runner.check_d10_sources(ROOT, baseline['verified_at'])
    runner.require(capture['native_protected_source'] == baseline['verified_at'], 'capture source differs from full baseline')
    runner.require(baseline['result'] == 'PROOFS_COMPLETE_TARGET_CLOSURE_OPEN' and not baseline['owned_live_pids'],
                   'current complete coupled baseline required')
    runner.require(all(s['state'] == 'PASS' and s['exit_code'] == 0 for s in baseline['steps']), 'baseline has failed/incomplete steps')
    for pin in capture['pins'].values():
        checked_pin(pin)
    data = dict(schema='stv.player.owner.audit.v1', verified_at=runner.git(ROOT, 'rev-parse', 'HEAD'),
                native_protected_source=baseline['verified_at'], result='RUNNING', steps=[],
                working_tree=runner.git(ROOT, 'status', '--short'),
                pins=dict(capture=runner.pin(capture_path), baseline=runner.pin(baseline_path),
                          audit_tool=runner.pin(__file__), capture_driver=capture['capture_driver']),
                contract=contract(baseline), scenarios=[], C3='OPEN', gate_proof=False)
    report = runner.Report(directory, data)
    try:
        checked_pin(capture['capture_driver'])
        runner.require(len(capture['steps']) == 2 and {s['name'] for s in capture['steps']} == {'1p-native-fullram', '2p-native-fullram'},
                       'capture scenarios differ')
        for step in capture['steps']:
            runner.require(step['state'] == 'PASS' and step['exit_code'] == 0, 'incomplete full-RAM child')
            checked_pin(dict(path=step['log'], bytes=Path(step['log']).stat().st_size, sha256=step['log_sha256']))
            name = step['name'].split('-')[0]
            native = next(s for s in baseline['steps'] if s['name'] == name + '-native')
            command = step['command']
            final = capture_command(command, native['command'])
            runner.require(step['environment'] == {'STV_LS_SUMMARY': '1', 'STV_LS_DUMP_EVERY': '60'}, 'capture full-RAM environment differs')
            for path in (command[0], command[1], command[command.index('--inputs') + 1]):
                runner.require(baseline['pins'][str(Path(path).resolve())] == runner.pin(path), 'baseline exe/ROM/input changed')
            artifacts = {Path(key).name: checked_pin(value) for key, value in step['proof']['artifacts'].items()}
            case = directory / name
            case.mkdir()
            row = convert_and_observe(artifacts['native-full.bin'], artifacts['content.jsonl'], case / 'compact.bin', final)
            runner.require(runner.content_samples(artifacts['content.jsonl'], final) == step['proof']['content_samples'], 'sidecar validation changed')
            main = next(s for s in baseline['steps'] if s['name'] == name + '-diff')
            sound = next(s for s in baseline['steps'] if s['name'] == name + '-sound-diff')
            for proof in (main, sound):
                oracle = Path(proof['command'][3]).resolve()
                runner.require(baseline['pins'][str(oracle)] == runner.pin(oracle), 'complete parent oracle changed')
                data['pins'][name + '-' + proof['name'] + '-parent'] = runner.pin(oracle)
            main_window(main['command'][3], case / 'mame-window.bin', final)
            sound_window(sound['command'][3], case / 'mame-window.snd', final)
            report.run(name + '-main-diff', [sys.executable, ROOT / 'tools/lockstep/lsfile.py', 'diff', case / 'mame-window.bin', case / 'compact.bin'], case)
            report.run(name + '-sound-diff', [sys.executable, ROOT / 'tools/lockstep/sndfile.py', 'diff', case / 'mame-window.snd', artifacts['native.snd']], case)
            row.update(scenario=name, intended_last_frame=final,
                       sound_endpoint=runner.sound_endpoint(artifacts['native.snd'], final),
                       raw_artifacts=step['proof']['artifacts'],
                       validated_artifacts={p.name: runner.pin(p) for p in case.iterdir() if p.is_file()})
            data['scenarios'].append(row)
            report.save()
        data.update(result='BOUND_SAMPLED_WEAPON_SELECTORS_MAIN_SOUND_MATCH', finished_at_utc=runner.now(),
                    limitations=['Each strict comparison covers the entire named 0..N bounded window; it does not renew or replace the full parent game proof.',
                                 'Registry membership and reciprocal links are established only at the sampled frame; no continuous process lifetime proof.',
                                 'An equipped selector does not establish firing, pickup collision, projectile timing, expired duration or room clearance.',
                                 'Other same-player-PID processes and incomplete/stale registry samples do not contribute weapon attribution.',
                                 'C2 legal target closure and complete C3 content coverage remain open.'])
        report.save()
        return data
    except BaseException as error:
        report.cancel_children(str(error))
        data.update(result='FAILED_OR_INTERRUPTED', error=str(error), finished_at_utc=runner.now())
        report.save()
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture-report', required=True)
    parser.add_argument('--baseline-report', default='evidence/c2-content-rebaseline-2026-10-02.json')
    parser.add_argument('--out-directory', required=True)
    args = parser.parse_args(argv)
    try:
        directory = Path(args.out_directory).resolve()
        directory.mkdir(exist_ok=False)
        result = audit(args.capture_report, args.baseline_report, directory)
        print(json.dumps(dict(result=result['result'], C3='OPEN', scenarios=[dict(scenario=s['scenario'], totals=s['totals'],
                         weapons=[(w['player'], w['weapon'], w['frame']) for w in s['bound_weapon_selectors']]) for s in result['scenarios']])))
        return 0
    except (OSError, ValueError, KeyError, TypeError, IndexError, SystemExit) as error:
        print('PLAYER_OWNER_AUDIT_FAILED: ' + str(error))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
