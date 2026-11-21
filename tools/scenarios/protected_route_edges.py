"""Match actual selector publications to subsequent settled room arrivals."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import reconstruct_routes
from summarize_entry_controls import collect
from summarize_stage_audit import ROOMS, verify_report


def selector_changes(rows):
    previous = None
    pending = None
    for r in rows:
        state, circuit, stage, door, frame = (int(r[k]) for k in
            ('state', 'circuit', 'wave', 'from_door', 'frame'))
        if previous is not None:
            ps, pc, pw = previous
            if (pending and pending[0] in (0,1,2)
                    and pending[2] == (11,19,23)[pending[0]]
                    and ps == state == 24 and pc == pending[0] and pw == stage == pending[2]
                    and circuit == (pc+1)%3 and door == 0):
                yield frame, pending, 'circuit advance'
                pending = None
            if ps == state == 16 and pc == circuit and pw != stage:
                pending = (circuit, pw, stage, door)
            elif pending and (pending[0] != circuit or pending[2:] != (stage, door)
                              or state not in (16, 24)):
                pending = None
            if ps == 24 and state == 16 and pending:
                yield frame, pending, 'settled room candidate'
                pending = None
        previous = state, circuit, stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    verify_report(args.report, report)
    if report['kind'] != 'probe':
        raise ValueError('Protected entry probe required')
    inventory = reconstruct_routes.build()
    edges = [(c, row['physical_wave'], o['destination_field'], o['from_door_field'])
             for row in inventory['physical_records'] for c in row['table_users']
             if (c, row['physical_wave']) in ROOMS for o in row['options']]
    if len(set(edges)) != len(edges):
        raise ValueError('Ambiguous retail route keys')
    covered = {mode: set() for mode in ('classic', 'wide')}
    observations = []
    for name, job in report['jobs'].items():
        mode = name.split('-')[0]
        if '-2p-' not in name or not job.get('csv_sha256'):
            raise ValueError('Two-player probe with verified CSV required')
        path = args.report.parent / name / 'entrances.csv'
        with path.open(newline='') as stream:
            stops, final = collect(csv.DictReader(stream), mode)
        if final != job['frames']:
            raise ValueError('Incomplete probe')
        valid = {(r['frame']-119, r['circuit'], r['wave'], r['door']) for r in stops
                 if all(p['valid'] and p['visible'] and p['stationary'] for p in r['players'])}
        with path.open(newline='') as stream:
            for frame, edge, kind in selector_changes(csv.DictReader(stream)):
                c, source, target, door = edge
                if edge in edges and (kind == 'circuit advance' or (frame, c, target, door) in valid):
                    covered[mode].add(edge)
                    observations.append(dict(run=name, frame=frame, edge=edge, kind=kind))
    missing = {mode: sorted(set(edges)-found) for mode, found in covered.items()}
    result = dict(retail_options=len(edges), door_values=sorted({e[3] for e in edges}),
                  report_sha256=hashlib.sha256(args.report.read_bytes()).hexdigest(),
                  route_inventory_pins=inventory['pins'], observed=observations, missing=missing,
                  limits=['Ordinary exits require matching selector publication, intro and settled play.',
                          'Tally exits require the published bonus selector followed by the next circuit in intro; not visual proof.',
                          'Does not prove which player activated an exit or how a room was cleared.'])
    with args.out.open('x', encoding='utf-8') as f:
        json.dump(result, f, indent=2)
    print(json.dumps(dict(options=len(edges), observed={m:len(s) for m,s in covered.items()}, missing=missing)))


if __name__ == '__main__':
    main()
