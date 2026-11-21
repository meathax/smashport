"""Summarize current protected entry controls; never promote observations to fidelity proof."""
import argparse
import csv
import hashlib
import json
import math
from collections import deque
from pathlib import Path
from summarize_stage_audit import ROOMS, verify_report


def collect(rows, mode):
    previous = None
    entry = None
    tail = deque(maxlen=10)
    results = []
    final_frame = -1
    for row in rows:
        frame = int(row['frame'])
        state = int(row['state'])
        room = (int(row['circuit']), int(row['wave']))
        final_frame = frame
        if previous is not None and int(previous['state']) == 24 and state == 16:
            entry = (frame, room, int(previous['from_door']))
            tail.clear()
        if entry is not None:
            start, target, door = entry
            if state != 16 or room != target:
                entry = None
                tail.clear()
            elif start + 110 <= frame <= start + 119:
                tail.append(row)
                if frame == start + 119:
                    players = []
                    for p in (1, 2):
                        keys = [f'p{p}_{k}' for k in ('x', 'y', 'vx', 'vy')]
                        values = [float(row[k]) for k in keys]
                        valid = all(math.isfinite(v) for v in values)
                        stationary = [int(t['frame']) for t in tail] == list(range(start+110,start+120)) and all(
                            int(t[f'p{p}_control']) == 0 and
                            float(t[f'p{p}_vx']) == 0 and float(t[f'p{p}_vy']) == 0 and
                            t[f'p{p}_x'] == row[f'p{p}_x'] and t[f'p{p}_y'] == row[f'p{p}_y']
                            for t in tail)
                        players.append(dict(x=values[0] if valid else None,
                                            y=values[1] if valid else None,
                                            valid=valid, stationary=stationary,
                                            visible=valid and 15 < values[0] < (532 if mode == 'wide' else 395) and 15 < values[1] < 245))
                    results.append(dict(mode=mode, frame=frame, circuit=room[0], wave=room[1], door=door, players=players))
                    entry = None
        previous = row
    return results, final_frame


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    verify_report(args.report, report)
    if report.get('kind') != 'probe':
        raise ValueError('Neutral-window analysis requires a protected probe report')
    observations, files = [], []
    for name, job in report['jobs'].items():
        if '-2p-' not in name:
            raise ValueError('This observer requires two-player jobs')
        path = args.report.parent / name / 'entrances.csv'
        with path.open(newline='') as stream:
            rows, final_frame = collect(csv.DictReader(stream), name.split('-')[0])
        if final_frame != job['frames']:
            raise ValueError(f'Incomplete CSV endpoint: {path}')
        for row in rows:
            row['run'] = name
        observations.extend(rows)
        files.append(dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    covered = {mode: {(r['circuit'], r['wave']) for r in observations if r['mode'] == mode and all(p['valid'] and p['stationary'] and p['visible'] for p in r['players'])} for mode in ('classic', 'wide')}
    result = dict(gate_proof=False, report=str(args.report),
                  report_sha256=hashlib.sha256(args.report.read_bytes()).hexdigest(),
                  executable_sha256=report['exe_sha256'], files=files, observations=observations,
                  missing={m: sorted(ROOMS - covered[m]) for m in covered},
                  findings=[r for r in observations if (r['circuit'],r['wave']) in ROOMS and not all(p['valid'] and p['stationary'] and p['visible'] for p in r['players'])],
                  limits=['Protected neutral windows are not survival or balance proof.',
                          'Visibility is not exact intended stage placement.',
                          'Raw player structures are not independently proven active-player ownership.',
                          'Coverage is by destination, not every alternative exit.'])
    args.out.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print(json.dumps(dict(observations=len(observations), missing=result['missing'], findings=len(result['findings']))))

if __name__ == '__main__':
    main()
