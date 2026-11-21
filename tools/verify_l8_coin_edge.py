#!/usr/bin/env python3
"""Verify a controlled coin-switch pulse in the development L8 reference."""
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
    starts = [fields(line) for line in lines if line.startswith('L8_COIN_INIT ')]
    events = [fields(line) for line in lines if line.startswith('L8_COIN_EVENT ')]
    reads = [fields(line) for line in lines if line.startswith('L8_COIN_READ ')]
    if len(starts) != 1 or len(events) != 2 or len(reads) != 50:
        raise ValueError('Incomplete controlled coin pulse')
    if [(row['action'], int(row['frame'])) for row in events] != [('press', 1048), ('release', 1052)]:
        raise ValueError('Coin override events differ')
    if any(int(row['sample']) != i for i, row in enumerate(reads)):
        raise ValueError('Input read numbering has a gap')
    press, release = (float(row['emutime']) for row in events)
    if not press < release:
        raise ValueError('Input pulse interval is invalid')
    for row in reads:
        now = float(row['emutime'])
        expected = 0xfffe if press < now < release else 0xffff
        if int(row['data'], 16) != expected or int(row['mask'], 16) != 0xffff:
            raise ValueError(f'Coin port read {row["sample"]} differs from the controlled pulse')
    scans = [row for row in reads if row['pc'] == 'ffe0d6c0']
    if len(scans) != 20:
        raise ValueError('Second widget read count differs')
    # The bounded trace begins during an already-idle attract loop.
    older = previous = 0xffffffff
    activations = []
    for row in scans:
        current = (int(row['data'], 16) << 16) | 0xffff
        edge = older & previous & ~current & 0xffffffff
        if edge & (1 << 16):
            activations.append((int(row['frame']), float(row['emutime'])))
        older, previous = previous, current
    if len(activations) != 1 or activations[0][0] != 1048:
        raise ValueError('Source 110 formula did not identify one first coin edge')
    first_active = next(row for row in reads if int(row['data'], 16) == 0xfffe)
    first_released = next(row for row in reads if float(row['emutime']) > release)
    return {
        'result': 'PASS',
        'scope': 'Controlled active-low L8 coin-port pulse and source 110 edge inference; not credit/audit behavior',
        'log_sha256': hashlib.sha256(raw).hexdigest(),
        'script_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
        'reads': len(reads),
        'press_emutime': press,
        'release_emutime': release,
        'first_active_read_delay_us': (float(first_active['emutime']) - press) * 1_000_000,
        'first_released_read_delay_us': (float(first_released['emutime']) - release) * 1_000_000,
        'source_110_inferred_activation_frame': activations[0][0],
        'coin_port_mask': '0x0001',
        'limitation': 'An input-port read and inferred switch edge do not prove original audit/credit transaction or game start.',
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--script', type=Path, default=Path('tools/mame_l8_coin_edge.lua'))
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error('Report exists; refusing to overwrite it')
    report = verify(args.log, args.script)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_bytes((json.dumps(report, indent=2) + '\n').encode('utf-8'))
    print('PASS: L8 active-low coin reads and one inferred source 110 edge')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
