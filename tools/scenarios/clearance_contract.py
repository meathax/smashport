"""Bind the retail enemy-presence exit guard; this is not a room-clear certificate."""
import argparse
import json
from pathlib import Path
import mproc_continuation_audit as a

ROOT = Path(__file__).resolve().parents[2]


def validate_lookup(seq):
    patterns = [
        ('MMTM', a.R('SP'), a.M('LIST', None, 0x3000)),
        ('MOVI', a.I(0x01000040), a.R('A2')),
        ('SEXT', a.R('A0'), a.K(0)),
        ('AND', a.R('A1'), a.R('A0')),
        ('MOVE', a.M('IND', 'A2'), a.R('A2'), a.K(1)),
        ('JREQ', a.M('REL', None, 0xffe03740)),
        ('MOVE', a.M('DISP', 'A2', 0x150), a.R('A3'), a.K(0)),
        ('AND', a.R('A1'), a.R('A3')),
        ('CMP', a.R('A0'), a.R('A3')),
        ('JRNE', a.M('REL', None, 0xffe036d0)),
        ('MOVE', a.R('A2'), a.R('A0')),
        ('MMFM', a.R('SP'), a.M('LIST', None, 12)),
        ('RETS',),
    ]
    a.require(len(seq) == len(patterns), 'Object lookup extent changed')
    a.contiguous(seq)
    for ins, pattern in zip(seq, patterns):
        a.expect(ins, *pattern)


def validate(seq):
    patterns = [
        ('MOVI', a.I(0x8000), a.R('A0')),
        ('MOVI', a.I(0xc000), a.R('A1')),
        ('CALLA', a.I(0xffe03660)),
        ('JREQ', a.M('REL', None, 0xffec73a0)),
        ('MOVE', a.M('ABS', None, 0x010eb610), a.R('A0'), a.K(0)),
        ('CMPI', a.I(2), a.R('A0')),
        ('JRNE', a.M('REL', None, 0xffec7340)),
        ('MOVE', a.M('ABS', None, 0x010eb630), a.R('A0'), a.K(0)),
        ('CMPI', a.I(7), a.R('A0')),
        ('JREQ', a.M('REL', None, 0xffec73a0)),
    ]
    a.require(len(seq) == len(patterns), 'Enemy exit guard extent changed')
    a.contiguous(seq)
    for ins, pattern in zip(seq, patterns):
        a.expect(ins, *pattern)


def build():
    _, _, _, rom, ins, pins = a.load_current(ROOT)
    a.require(a.sha(rom) == a.RETAIL_SHA, 'Retail image changed')
    seq = a.window(ins, 0xffec71e0, 0xffec7340)
    validate(seq)
    src = ROOT/'original/archive/smashtv-main/ENDWAVE.ASM'
    pp = ROOT/'build/p1/robo.bin'
    mp = ROOT/'build/p1/robo.bin.relmask'
    lp = ROOT/'build/p1/listing.json'
    binding = a.bind_relocated_source(seq, rom, pp.read_bytes(), mp.read_bytes(),
        json.loads(lp.read_text())['instructions'], 'ENDWAVE.ASM',
        [652, 653, 655, 656, 657, 658, 659, 660, 661, 662], src.read_text().splitlines())
    lookup_source = ROOT/'original/archive/smashtv-main/NDSP1.ASM'
    lookup = a.window(ins, 0xffe03660, 0xffe03780)
    validate_lookup(lookup)
    lookup_binding = a.bind_relocated_source(lookup, rom, pp.read_bytes(), mp.read_bytes(),
        json.loads(lp.read_text())['instructions'], 'NDSP1.ASM',
        [1170, 1171, 1172, 1173, 1175, 1176, 1177, 1178, 1179, 1180, 1182, 1183, 1184],
        lookup_source.read_text().splitlines())
    pins += [a.pin(p) for p in [src, lookup_source, pp, mp, lp, Path(__file__),
        ROOT/'tools/scenarios/mproc_continuation_audit.py']]
    return dict(schema='stv.clearance_guard.v1', result='ENEMY_EXIT_GUARD_BOUND_CLEARANCE_OPEN',
        gate_proof=False, pins=pins, binding=binding, lookup_binding=lookup_binding,
        lookup=dict(head='01000040', next_offset_bits=0, oid_offset_bits=0x150,
            match='(FW0/FE0 field-read(OID) & A1) == (signed-FW0(input A0) & A1); with FW0=16 and mask=0xc000, FE0 cannot change the match',
            result='A0 is first matching object pointer, or zero at list end; A2/A3 restored'),
        predicate='EXISTOBJ(class=0x8000, mask=0xc000) returns zero OR (raw circuit == 2 AND raw wave == 7)',
        continue_pc='ffec73a0', wait_pc='ffec7340',
        limits=[
            'Complete helper body is source-bound; termination requires an acyclic valid object list and intact stack. Live membership/lifetime and translated flag behavior remain separate obligations.',
            'Raw selectors are source field values; do not silently convert them to displayed room numbering.',
            'DEXIT creation and spawn-budget exhaustion are not room-clear events.',
            'Passing this guard precedes neutral-object cleanup, door checks and player exit; it is not a completed transition or ending.',
            'No recording is certified as a room clear by this static contract.'])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    out = args.out.resolve()
    a.require(out.is_relative_to(ROOT/'build') and not out.exists(), 'Use fresh build output')
    result = build()
    out.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(result['result'])
