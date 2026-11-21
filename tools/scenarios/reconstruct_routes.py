"""Reconstruct retail exit records, preserving source differences and reachability gaps."""
import argparse
import json
from pathlib import Path
import re
import subprocess

from reconstruct_wave_positions import coordinate, read_words
from mproc_continuation_audit import (R, K, I, M, RETAIL_SHA, sha, bind_relocated_source, expect,
                                    load_current, pin, require, window)

ROOT = Path(__file__).resolve().parents[2]


def source_tables(text):
    lines = text.splitlines()
    tables = []
    for label, count in [('CIRCB1', 10), ('CIRCB2', 18), ('CIRCB3', 21)]:
        starts = [i for i, s in enumerate(lines) if s.strip() == label + ':']
        require(len(starts) == 1, 'Missing/duplicate route table label')
        values = []
        for n in range(starts[0] + 1, len(lines)):
            line = lines[n].split(';', 1)[0].strip()
            if not line: continue
            match = re.fullmatch(r'\.LONG\s+(.+)', line, re.I)
            if not match: break
            tokens = re.findall(r'\[[^\]]*\]|[^,]+', match[1])
            require(len(tokens) == 3 and ','.join(t.strip() for t in tokens).replace(' ', '') == match[1].replace(' ', ''),
                    'Route row must contain exactly three values')
            for token in tokens:
                token = token.strip()
                pair = re.fullmatch(r'\[([^,]+),([^,]+)\]', token)
                value = (coordinate(pair[1]) << 16) | coordinate(pair[2]) if pair else coordinate(token)
                values.append(dict(value=value, source=f'original/archive/smashtv-main/ENDWAVE.ASM:{n+1}'))
        require(len(values) == count * 12, 'Source route table extent differs')
        tables.append(values)
    return tables


def decode_record(values):
    require(len(values) == 12 and all(type(v) is int and 0 <= v <= 0xffffffff for v in values), 'Malformed route record')
    options = []
    for slot in range(3):
        ul, lr, destination, door = [values[slot + row*3] for row in range(4)]
        if not any((ul, lr, destination, door)): continue
        require(all((ul, lr, destination, door)), 'Partially empty route option')
        signed = lambda v: v-65536 if v & 0x8000 else v
        box = [signed(ul & 65535), signed(ul >> 16), signed(lr & 65535), signed(lr >> 16)]
        require(box[0] <= box[2] and box[1] <= box[3], 'Reversed route rectangle')
        require(1 <= destination <= 24 and 1 <= door <= 4, 'Unreviewed destination/door value')
        options.append(dict(slot=slot, rectangle_xyxy=box, destination_field=destination, from_door_field=door))
    require(options, 'Empty physical route record')
    return options


def build():
    source = ROOT / 'original/archive/smashtv-main/ENDWAVE.ASM'
    text = source.read_text(encoding='utf-8')
    tables = source_tables(text)
    _, rows, compiled, rom, instructions, generated = load_current(ROOT)
    require(sha(rom) == RETAIL_SHA, 'Retail identity changed')
    p1_path = ROOT / 'build/p1/robo.bin'
    mask_path = ROOT / 'build/p1/robo.bin.relmask'
    listing_path = ROOT / 'build/p1/listing.json'
    seq = window(instructions, 0xffec8150, 0xffec8200)
    require(len(seq) == 5, 'Route consumer extent changed')
    patterns = [('MOVE', M('ABS', None, 0x010eb610), R('A0'), K(0)),
                ('SLL', K(5), R('A0')), ('ADDI', I(0xffecce80), R('A0')),
                ('MOVE', M('IND', 'A0'), R('A0'), K(1)),
                ('MOVE', M('ABS', None, 0x010eb630), R('A1'), K(0))]
    for item, pattern in zip(seq, patterns): expect(item, *pattern)
    binding = bind_relocated_source(seq, rom, p1_path.read_bytes(), mask_path.read_bytes(),
              json.loads(listing_path.read_text())['instructions'], 'ENDWAVE.ASM', [786,787,788,789,790], text.splitlines())
    # ADDI long immediate follows its 16-bit opcode; its source opcode and
    # relocation-mask correspondence were checked by the binding above.
    p1 = p1_path.read_bytes()
    original_root = read_words(p1, int(binding['instructions'][2]['p1_pc'],16)+16, 1)[0]
    original_tables = read_words(p1, original_root, 3)
    for address, table in zip(original_tables, tables):
        require(read_words(p1,address,len(table)) == [s['value'] for s in table],
                'Source route expressions differ from linked-source bytes')
    expect(instructions[0xffec8220], 'ADDI', I(384), R('A0'))
    transition = window(instructions, 0xffec9540, 0xffec9600)
    require(len(transition) == 6, 'Route destination writer extent changed')
    writes = [('ADDI', I(96), R('A1')),
              ('MOVE', M('IND','A1'), R('A0'), K(0)),
              ('MOVE', R('A0'), M('ABS',None,0x010eb630), K(0)),
              ('ADDI', I(96), R('A1')),
              ('MOVE', M('IND','A1'), R('A0'), K(0)),
              ('MOVE', R('A0'), M('ABS',None,0x010ee988), K(0))]
    for item, pattern in zip(transition, writes): expect(item, *pattern)
    transition_binding = bind_relocated_source(transition,rom,p1,mask_path.read_bytes(),
        json.loads(listing_path.read_text())['instructions'],'ENDWAVE.ASM',list(range(1019,1025)),text.splitlines())
    roots = read_words(rom, 0xffecce80, 3)
    require(roots == [0xffeccee0, 0xffecdde0, 0xffecdde0], 'Retail root sharing changed')
    require(min(p for p in compiled if p >= roots[-1]) == 0xffecfee0, 'Following compiled boundary changed')
    records = []
    for root, count in [(roots[0],10), (roots[1],22)]:
        for wave in range(1, count+1):
            address = root + (wave-1)*384
            values = read_words(rom, address, 12)
            comparisons = []
            for circuit in range(3):
                if roots[circuit] != root or wave*12 > len(tables[circuit]): continue
                src = tables[circuit][(wave-1)*12:wave*12]
                comparisons.append(dict(circuit=circuit, source=[s['source'] for s in src],
                    exact_scalar_match=[s['value'] for s in src] == values,
                    differences=[dict(field=i, source=s['value'], retail=v) for i,(s,v) in enumerate(zip(src,values)) if s['value'] != v]))
            records.append(dict(address=f'{address:08x}', physical_wave=wave,
                table_users=[c for c in range(3) if roots[c] == root], words=values,
                options=decode_record(values), source_comparisons=comparisons))
    paths = [source,p1_path,mask_path,listing_path,Path(__file__).resolve(),
             ROOT/'tools/scenarios/reconstruct_wave_positions.py', ROOT/'tools/scenarios/mproc_continuation_audit.py',
             ROOT/'tools/gsp2cpp/analysis/controlflow_audit.py', ROOT/'original/user-supplied-roms/smashtv.zip']
    return dict(schema='stv.reconstructed_routes.v1', result='PASS_RETAIL_ROUTE_RECORDS_RECONSTRUCTED_C3_OPEN',
        gate_proof=False, verified_at=subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=ROOT).strip(),
        pins=[pin(p) for p in paths]+generated, consumer_binding=binding, transition_binding=transition_binding,
        table_roots=[f'{p:08x}' for p in roots],
        physical_records=records, source_record_counts=[10,18,21],
        linked_source_tables=[f'{a:08x}' for a in original_tables], source_table_bytes_verified=True,
        limits=['New reconstruction, not recovery of the missing historical inventory hash.',
                'Shared table ownership does not prove every physical row is reachable in both circuits.',
                'Route activation, transition semantics, player readiness and room clearance remain unproved.'])


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    out=args.out.resolve()
    require(out.is_relative_to(ROOT/'build') and not out.exists(), 'Use a fresh output under build/')
    report=build()
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(result=report['result'],records=len(report['physical_records']),out=str(out))))


if __name__=='__main__': main()
