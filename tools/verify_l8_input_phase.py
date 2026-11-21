#!/usr/bin/env python3
"""Verify a bounded, executed MAME L8 widget/display-interrupt trace."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r'([a-z_]+)=([^\s]+)', line))


def verify(log: Path, script: Path) -> dict:
    raw = log.read_bytes()
    lines = raw.decode('utf-8', errors='replace').splitlines()
    if any('[LUA ERROR]' in line for line in lines):
        raise ValueError('MAME reported a Lua probe error')
    starts = [fields(line) for line in lines if line.startswith('L8_INPUT_INIT ')]
    inputs = [fields(line) for line in lines if line.startswith('L8_INPUT ')]
    interrupts = [fields(line) for line in lines if line.startswith('L8_INTERRUPT ')]
    if len(starts) != 1 or len(inputs) != 79 or len(interrupts) != 13:
        raise ValueError('Incomplete bounded L8 input/interrupt witness')
    start, end = float(starts[0]['start']), float(starts[0]['end'])
    if (start, end) != (19.2, 19.32):
        raise ValueError('Unexpected witness interval')
    if any(int(row['sample']) != i for i, row in enumerate(inputs)):
        raise ValueError('Input sample sequence has a gap')
    if any(int(row['sample']) != i for i, row in enumerate(interrupts)):
        raise ValueError('Interrupt sequence has a gap')
    values = [int(row['data'], 16) for row in interrupts]
    if values != [0x11a, 0x9a] * 6 + [0x11a]:
        raise ValueError('Observed display-interrupt phases differ')
    if any(int(row['frame']) != 1050 + i // 2 for i, row in enumerate(interrupts)):
        raise ValueError('Interrupt frame sequence differs')
    if any(int(row['port']) not in (0, 1, 3) or
           int(row['address'], 16) != 0x01c00000 + int(row['port']) * 0x10 or
           int(row['data'], 16) != 0xffff or
           int(row['mask'], 16) != 0xffff for row in inputs):
        raise ValueError('Idle active-low widget read differs')
    scan_delays_us = []
    for i, irq in enumerate(interrupts[:12]):
        phase_time = float(irq['emutime'])
        next_time = float(interrupts[i + 1]['emutime'])
        phase_inputs = [row for row in inputs
                        if phase_time <= float(row['emutime']) < next_time]
        scans = [row for row in phase_inputs
                 if row['pc'] in ('ffe0d660', 'ffe0d6c0')]
        expected = [(0, 'ffe0d660'), (1, 'ffe0d660'),
                    (0, 'ffe0d6c0'), (1, 'ffe0d6c0')]
        if [(int(row['port']), row['pc']) for row in scans] != expected:
            raise ValueError(f'Widget scan pair differs after interrupt {i}')
        delay = (float(scans[0]['emutime']) - phase_time) * 1_000_000
        if not 20 <= delay <= 55:
            raise ValueError(f'Widget scan phase {i} timing differs')
        scan_delays_us.append(delay)
        is_half = values[i] == 0x9a
        dip_reads = [row for row in phase_inputs
                     if int(row['port']) == 3 and row['pc'] == 'ffe0e770']
        if len(dip_reads) != int(is_half):
            raise ValueError(f'DIP read count differs after interrupt {i}')
        if is_half and not float(dip_reads[0]['emutime']) < float(scans[0]['emutime']):
            raise ValueError('Half-screen DIP read is out of order')
    return {
        'result': 'PASS',
        'scope': 'Executed idle L8 widget reads at both display phases; not input-change response or complete scheduler',
        'log_sha256': hashlib.sha256(raw).hexdigest(),
        'script_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
        'witness_seconds': [start, end],
        'interrupt_writes': len(interrupts),
        'input_reads': len(inputs),
        'complete_scans_verified': 12,
        'scan_count_per_complete_frame': 2,
        'widget_ports': {'movement': '0x01c00000', 'coin_start': '0x01c00010', 'dip': '0x01c00030'},
        'widget_scan_cpu_pcs': ['0xffe0d660', '0xffe0d6c0'],
        'scan_delay_us_range': [min(scan_delays_us), max(scan_delays_us)],
        'interrupt_values': values,
        'limitation': 'All widget samples were idle 0xffff; CPU PCs are original virtual addresses, not host pointers.',
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--script', type=Path, default=Path('tools/mame_l8_input_phase.lua'))
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error('Report exists; refusing to overwrite it')
    report = verify(args.log, args.script)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_bytes((json.dumps(report, indent=2) + '\n').encode('utf-8'))
    print('PASS: 12 L8 input scans across six complete frame pairs')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
