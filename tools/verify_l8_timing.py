#!/usr/bin/env python3
"""Check a development-only MAME L8 timing trace from mame_l8_timing.lua."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r'([a-z_]+)=([^\s]+)', line))


def verify(log: Path, script: Path) -> dict:
    lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
    if any('[LUA ERROR]' in line for line in lines):
        raise ValueError('MAME reported a Lua timing-probe error')
    starts = [fields(line) for line in lines if line.startswith('L8_INIT ')]
    frames = [fields(line) for line in lines if line.startswith('L8_FRAME ')]
    writes = [fields(line) for line in lines if line.startswith('L8_DPYINT ')]
    reads = [fields(line) for line in lines if line.startswith('L8_HCOUNT ')]
    if len(starts) != 1 or len(frames) != 12 or len(writes) != 24 or len(reads) != 24:
        raise ValueError('Incomplete L8 timing witness')
    start = starts[0]
    if (int(start['width']), int(start['height'])) != (410, 256):
        raise ValueError('L8 visible geometry differs')
    pixel_period, scan_period, frame_period = (
        float(start[key]) for key in ('pixel_period', 'scan_period', 'frame_period'))
    if (abs(pixel_period - 1 / 8_000_000) > 1e-12 or
            abs(scan_period - 506 / 8_000_000) > 1e-12 or
            abs(frame_period - 506 * 289 / 8_000_000) > 1e-12):
        raise ValueError('L8 pixel/scan/frame periods differ')
    for number, frame in enumerate(frames):
        if (int(frame['sample']) != number or int(frame['frame']) != number or
                abs(float(frame['emutime']) - (number + 1) * frame_period) > 1e-9):
            raise ValueError(f'Frame {number} has unexpected cadence')
    values = [int(row['data'], 16) for row in writes]
    if values[0] != 0x9a or values[1:10] != [0x9a, 0x11a] * 4 + [0x9a]:
        raise ValueError('DPYINT writes do not show source half/end alternation')
    if any(int(row['address'], 16) != 0xc00000a0 for row in writes):
        raise ValueError('Unexpected display-interrupt register address')
    hcounts = [int(row['data'], 16) for row in reads]
    if (any(int(row['address'], 16) != 0xc00001c0 for row in reads) or
            len(set(hcounts)) < 4):
        raise ValueError('HCOUNT reads absent or constant')
    return {
        'result': 'PASS',
        'scope': 'Executed MAME 0.289 L8 screen/DPYINT/HCOUNT witness; not a gameplay differential or full interrupt phase model',
        'log_sha256': hashlib.sha256(log.read_bytes()).hexdigest(),
        'script_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
        'visible_pixels': [410, 256],
        'pixel_clock_hz': 8_000_000,
        'horizontal_total': 506,
        'vertical_total': 289,
        'frame_period_seconds': frame_period,
        'refresh_hz': 1 / frame_period,
        'first_twelve_frames': [float(row['emutime']) for row in frames],
        'first_display_interrupt_writes': writes,
        'first_hcount_reads': reads,
        'initial_display_interrupt_write_before_post_seconds': float(writes[0]['emutime']),
        'display_interrupt_writes_resume_after_post_seconds': float(writes[1]['emutime']),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--script', type=Path, default=Path('tools/mame_l8_timing.lua'))
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error('Report exists; refusing to overwrite it')
    report = verify(args.log, args.script)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_bytes((json.dumps(report, indent=2) + '\n').encode('utf-8'))
    print('PASS: executed L8 frame cadence, 24 DPYINT writes and 24 variable HCOUNT reads')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
