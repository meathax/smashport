#!/usr/bin/env python3
"""Verify one executed L8 DRINF index-4 read and its following dereference."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r'([a-z_]+)=([^\s]+)', line))


def verify(log: Path, script: Path, image: Path) -> dict:
    raw = log.read_bytes()
    lines = raw.decode('utf-8', errors='replace').splitlines()
    if any('[LUA ERROR]' in line for line in lines):
        raise ValueError('MAME reported a Lua probe error')
    starts = [fields(line) for line in lines if line.startswith('L8_DRINF_DEREF ')]
    index = [fields(line) for line in lines if line.startswith('L8_INDEX4 ')]
    derefs = [fields(line) for line in lines if line.startswith('L8_DEREF ')]
    if len(starts) != 1 or len(index) != 10 or len(derefs) != 1:
        raise ValueError('Incomplete executed DRINF witness')
    if any(int(row['sample']) != i for i, row in enumerate(index)):
        raise ValueError('Index-read sequence has a gap')
    if any(int(row['sample']) != i for i, row in enumerate(derefs)):
        raise ValueError('Dereference sequence has a gap')
    # Earlier reads from a different PC are ROM verification/instruction fetch;
    # the paired runtime data fetch occurs at the same PC immediately before
    # the byte dereference in the tank path.
    data_reads = [row for row in index if row['pc'] == 'ffe40e40']
    if len(data_reads) != 2:
        raise ValueError('Tank-path index-4 data fetch missing')
    low, high = data_reads
    if ((int(low['address'], 16), int(high['address'], 16)) !=
            (0xffe3b270, 0xffe3b280) or
            (int(low['data'], 16), int(high['data'], 16)) != (0x5610, 0x1891) or
            any(int(row['mask'], 16) != 0xffff for row in data_reads)):
        raise ValueError('Tank-path index-4 pointer bytes differ')
    pointer = int(low['data'], 16) | (int(high['data'], 16) << 16)
    deref = derefs[0]
    if (pointer != 0x18915610 or int(deref['address'], 16) != pointer or
            int(deref['data'], 16) != 0 or int(deref['mask'], 16) != 0xffff or
            deref['pc'] != 'ffe40e50' or deref['frame'] != low['frame']):
        raise ValueError('Executed target read differs')
    interval_us = (float(deref['emutime']) - float(low['emutime'])) * 1_000_000
    if not 0 < interval_us < 2:
        raise ValueError('Pointer and target reads are not adjacent in time')
    if not image.is_file() or image.stat().st_size < 1024:
        raise ValueError('Reference first-room screenshot missing')
    return {
        'result': 'PASS',
        'scope': 'One executed MAME 0.289 L8 tank-path index-4 pointer and target read; not PCB or all-room proof',
        'log_sha256': hashlib.sha256(raw).hexdigest(),
        'script_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
        'first_room_image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
        'frame': int(low['frame']),
        'emutime_seconds': float(low['emutime']),
        'table_index_4_bit_address': '0xffe3b270',
        'table_index_4_pointer': f'0x{pointer:08x}',
        'pointer_read_cpu_pc': '0xffe40e40',
        'target_read_bit_address': f'0x{int(deref["address"], 16):08x}',
        'target_read_data_16': f'0x{int(deref["data"], 16):04x}',
        'target_read_cpu_pc': '0xffe40e50',
        'pointer_to_target_read_us': interval_us,
        'limitation': 'MAME returned zero on this observed read; production tank actor and original PCB bus equivalence remain unimplemented/unverified.',
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--script', type=Path, default=Path('tools/mame_l8_drinf_deref.lua'))
    parser.add_argument('--image', type=Path, default=Path('evidence/l8-reference-first-room-frame1700-2026-09-22.png'))
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error('Report exists; refusing to overwrite it')
    report = verify(args.log, args.script, args.image)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_bytes((json.dumps(report, indent=2) + '\n').encode('utf-8'))
    print('PASS: executed L8 tank index-4 pointer and following zero-valued target read')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
