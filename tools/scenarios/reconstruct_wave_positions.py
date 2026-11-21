"""Reconstruct source-bound wave camera tables without inventing route/clear proofs."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess

from mproc_continuation_audit import (BASE, RETAIL_SHA, R, K, I, M, bind_relocated_source,
                                    expect, load_current, pin, require, sha, window)

ROOT = Path(__file__).resolve().parents[2]


def coordinate(text):
    terms = text.strip().split('+')
    require(all(re.fullmatch(r'(?:>[0-9A-Fa-f]+|[0-9]+)', t.strip()) for t in terms), 'Unsupported coordinate expression')
    value = sum(int(t.strip()[1:], 16) if t.strip().startswith('>') else int(t.strip()) for t in terms)
    require(0 <= value <= 0xffff, 'Coordinate exceeds its word')
    return value


def source_tables(text):
    lines = text.splitlines()
    result = []
    for label in ('CRCT1YX', 'CRCT2YX', 'CRCT3YX'):
        starts = [i for i, line in enumerate(lines) if line.strip() == label + ':']
        require(len(starts) == 1, 'Missing/duplicate source table label')
        rows = []
        for i in range(starts[0]+1, len(lines)):
            line = lines[i].split(';', 1)[0].strip()
            if not line: continue
            match = re.fullmatch(r'\.LONG\s+(.+)', line, re.I)
            if not match: break
            tokens = re.findall(r'\[[^\]]*\]|[^,]+', match[1])
            require(','.join(t.strip() for t in tokens).replace(' ', '') == match[1].replace(' ', ''), 'Malformed coordinate list')
            for token in tokens:
                token = token.strip()
                if token == '0': value = 0
                else:
                    pair = re.fullmatch(r'\[([^,]+),([^,]+)\]', token)
                    require(pair is not None, 'Unsupported packed coordinate')
                    value = (coordinate(pair[1]) << 16) | coordinate(pair[2])
                rows.append(dict(value=value, source=f'original/archive/smashtv-main/WAVES.ASM:{i+1}'))
        require(rows, 'Empty source table')
        result.append(rows)
    require([len(r) for r in result] == [12, 20, 23], 'Supplied source table extents differ')
    return result


def read_words(image, address, count):
    offset = (address - BASE) // 8
    require(type(count) is int and count > 0 and address % 16 == 0 and offset >= 0 and offset + count*4 <= len(image), 'Table outside image or unaligned')
    return list(struct.unpack_from('<'+'I'*count, image, offset))


def build():
    source = ROOT / 'original/archive/smashtv-main/WAVES.ASM'
    source_text = source.read_text(encoding='utf-8')
    tables = source_tables(source_text)
    _, rows, compiled, rom, instructions, generated = load_current(ROOT)
    require(sha(rom) == RETAIL_SHA, 'Retail identity changed')
    p1_path = ROOT / 'build/p1/robo.bin'
    p1 = p1_path.read_bytes()
    mask_path = ROOT / 'build/p1/robo.bin.relmask'
    listing_path = ROOT / 'build/p1/listing.json'
    map_path = ROOT / 'build/p1/robo.map.json'
    listing = json.loads(listing_path.read_text(encoding='utf-8'))['instructions']
    globals_ = json.loads(map_path.read_text(encoding='utf-8'))['globals']
    seq = window(instructions, 0xffe78cc0, 0xffe78dd0)
    require(len(seq) == 9, 'Camera consumer extent changed')
    patterns = [('MOVE', M('ABS', None, 0x010eb610), R('A1'), K(0)),
                ('SLL', K(5), R('A1')), ('ADDI', I(0xffe8c7f0), R('A1')),
                ('MOVE', M('IND', 'A1'), R('A1'), K(1)),
                ('MOVE', M('ABS', None, 0x010eb630), R('A0'), K(0)),
                ('SLL', K(5), R('A0')), ('ADD', R('A1'), R('A0')),
                ('MOVE', M('IND', 'A0'), R('A0'), K(1)),
                ('MOVE', R('A0'), M('ABS', None, 0x010ee080), K(1))]
    for item, pattern in zip(seq, patterns): expect(item, *pattern)
    binding = bind_relocated_source(seq, rom, p1, mask_path.read_bytes(), listing, 'WAVES.ASM',
                                   [686, 687, 688, 689, 691, 692, 693, 694, 695], source_text.splitlines())
    original_roots = read_words(p1, int(globals_['CRCTTAB'], 16), 3)
    retail_roots = read_words(rom, 0xffe8c7f0, 3)
    require(retail_roots == [0xffe8c850, 0xffe8c9d0, 0xffe8cc50], 'Retail root pointers changed')
    require(min(p for p in compiled if p >= retail_roots[-1]) == 0xffe8cf50, 'Following code/data boundary changed')
    require(next(r['source'] for r in rows if r['address'] == 'ffe8cf50') == 'WAVES.ASM:3352', 'Following source function changed')
    extents = [(end-start)//32 for start,end in zip(retail_roots, retail_roots[1:]+[0xffe8cf50])]
    require(extents == [12, 20, 24], 'Retail table extents changed')
    recovered = []
    differences = []
    for circuit, (src, origin, retail, count) in enumerate(zip(tables, original_roots, retail_roots, extents)):
        expected = [r['value'] for r in src]
        require(read_words(p1, origin, len(src)) == expected, 'Source expressions do not match linked source bytes')
        actual = read_words(rom, retail, count)
        for wave, value in enumerate(actual):
            # Retail inserted the outer Pleasure Dome position at C3 slot22.
            source_index = wave if circuit != 2 or wave < 22 else (22 if wave == 23 else None)
            matched = source_index is not None and expected[source_index] == value
            require(matched or (circuit, wave, value) == (2, 22, 0x03ea0ce9), 'Unreviewed source/retail table difference')
            row = dict(circuit=circuit, wave=wave, retail_address=f'{retail+wave*32:08x}',
                       packed_yx=f'{value:08x}', x=value & 0xffff, y=value >> 16,
                       source_index=source_index, source_bytes_match=matched,
                       source=src[source_index]['source'] if matched else None,
                       source_status='SOURCE_COORDINATE_BYTES_MATCH' if matched else 'RETAIL_ONLY_INSERTION')
            recovered.append(row)
            if source_index != wave or not matched: differences.append(row)
    paths = [source, ROOT / 'original/user-supplied-roms/smashtv.zip', p1_path, mask_path, listing_path, map_path, Path(__file__).resolve(),
             ROOT / 'tools/scenarios/mproc_continuation_audit.py', ROOT / 'tools/gsp2cpp/analysis/controlflow_audit.py']
    return dict(schema='stv.wave_positions.v1', result='PASS_RECONSTRUCTED_POSITION_TABLES_C3_OPEN', gate_proof=False,
                verified_at=subprocess.check_output(['git','rev-parse','HEAD'], cwd=ROOT, text=True).strip(),
                inputs=[pin(p) for p in paths]+generated, consumer_binding=binding, source_counts=[12,20,23],
                retail_counts=extents, positions=recovered, retail_differences=differences,
                limits=['New reconstruction; not a replacement claiming the missing historical inventory hash.',
                        'Names, exits/routes, enemies, items, operator actions and clearances are not established here.',
                        'Consumer contract requires valid circuit/wave indexes and 16/32-bit fields; lifetime/global index proof remains open.'])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    require(out.is_relative_to(ROOT/'build') and not out.exists(), 'Use a fresh output below build/')
    report = build()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(result=report['result'], positions=len(report['positions']), differences=len(report['retail_differences']), out=str(out))))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
