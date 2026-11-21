"""Summarize completed native stage experiments without promoting them to gates."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import pressure
from summarize_pressure import pool
from stage_integrity import read_integrity, censor_pressure

ROOT = Path(__file__).resolve().parents[2]
ROOMS = {(c, w) for c, last in ((0, 10), (1, 18), (2, 22))
         for w in range(1, last + 1) if c == 0 or w != 10}

def input_prefixes(path, frames):
    """Hash complete input records through each observation's inclusive frame."""
    result, digest = {}, hashlib.sha256()
    previous = -1
    with path.open('rb') as stream:
        line = stream.readline()
        for frame in sorted(set(frames)):
            while line and int(line.split()[0]) <= frame:
                current = int(line.split()[0])
                if current < previous:
                    raise ValueError(f'Out-of-order recorded inputs: {path}')
                previous = current
                digest.update(line)
                line = stream.readline()
            result[frame] = digest.hexdigest()
    return result


def distinct_pool(named_runs):
    """Retain one copy of identical observations with identical input histories."""
    result = pool(named_runs)
    kept, duplicates, seen = [], [], {}
    for name, run in named_runs:
        for row in run['observations']:
            key = (run['executable'], row['player'], row['frame'],
                   run['input_prefixes'][row['frame']])
            values = tuple(row[k] for k in ('circuit','wave','frames','deaths','avg','peak'))
            if key in seen:
                original, expected = seen[key]
                if values != expected:
                    raise ValueError(f'Identical input history has different pressure results: {name}')
                duplicates.append(dict(row, source_run=name, duplicate_of=original))
                continue
            seen[key] = (name, values)
            kept.append(dict(row, source_run=name))
    result['observations'] = kept
    result['duplicate_observations'] = duplicates
    return result


def verify_report(path, report):
    """Reject incomplete or modified observations before interpreting coverage."""
    if report.get("result") != "COMPLETE" or report.get("live_pids"):
        raise ValueError(f"Incomplete stage report: {path}")
    if not report.get("jobs"):
        raise ValueError(f"Empty stage report: {path}")
    for name, job in report["jobs"].items():
        if Path(name).name != name or name in (".", ".."):
            raise ValueError(f"Invalid stage job name: {name}")
        if job.get("state") != "PASS" or job.get("exit_code") != 0 or not job.get("terminal"):
            raise ValueError(f"Unsuccessful stage job: {name}")
        log = path.parent / name / "run.log"
        if hashlib.sha256(log.read_bytes()).hexdigest() != job.get("log_sha256"):
            raise ValueError(f"Modified stage log: {log}")
        if report.get('require_integrity'):
            measured = read_integrity(log.parent, log.read_text(), job['frames'])
            if job.get('list_integrity') != measured or job.get('list_integrity_error'):
                raise ValueError(f'List-integrity receipt differs: {name}')
            if name.startswith('wide-') and measured['first_invalid_frame'] is not None:
                raise ValueError(f'Invalid widescreen state: {name}')
        for key, filename in (("csv_sha256", "entrances.csv"), ("coverage_sha256", "native.cov"),
                              ("clear_events_sha256", "clear-events.csv")):
            if key in job:
                artifact = path.parent / name / filename
                if hashlib.sha256(artifact.read_bytes()).hexdigest() != job[key]:
                    raise ValueError(f"Modified stage artifact: {artifact}")


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('--reports', nargs='+', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    pins, landings, invalid, runs, censored = [], [], [], {}, []
    for path in args.reports:
        report = json.loads(path.read_text())
        verify_report(path, report)
        pins.append(dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                         executable=report['exe_sha256']))
        for name, job in report['jobs'].items():
            mode, players, _ = name.split('-')
            players = int(players[0])
            directory = path.parent / name
            if report['kind'] == 'pressure':
                run = pressure.read_run(directory / 'run.log', players)
                if report.get('require_integrity'):
                    first = job['list_integrity']['first_invalid_frame']
                    run, removed = censor_pressure(run, first)
                    if removed:
                        censored.append(dict(source_run=str(directory),
                            first_invalid_frame=first, records=removed))
                run['executable'] = report['exe_sha256']
                run['input_prefixes'] = input_prefixes(
                    directory / 'inputs.txt', [r['frame'] for r in run['observations']])
                pins.append(dict(path=str(directory / 'inputs.txt'),
                                 sha256=hashlib.sha256((directory / 'inputs.txt').read_bytes()).hexdigest()))
                runs.setdefault((mode, players), []).append(
                    (str(directory), run))
                continue
            previous_state, valid_entry = None, False
            previous_room = None
            for row in csv.DictReader((directory / 'entrances.csv').open()):
                state = int(row['state'])
                room = (int(row['circuit']), int(row['wave']))
                if state != previous_state or room != previous_room:
                    valid_entry = state == 16 and previous_state == 24
                previous_state, previous_room = state, room
                if row['event'] != 'settled':
                    continue
                # REDREG publishes the destination selector before SET_WAVE
                # changes the scene. A long exit prelude is not an arrival.
                if not valid_entry:
                    continue
                c, w = int(row['circuit']), int(row['wave'])
                if (c, w) not in ROOMS:
                    continue
                item = dict(mode=mode, circuit=c, wave=w, frame=int(row['frame']),
                            source=str(directory / 'entrances.csv'),
                            players=[[float(row[f'p{n}_{a}']) for a in ('x', 'y')] for n in range(1, players+1)])
                landings.append(item)
                if any(not (15 < x < (532 if mode == 'wide' else 395) and 15 < y < 245)
                       for x, y in item['players']):
                    invalid.append(item)
    missing = {mode: sorted(ROOMS - {(r['circuit'], r['wave']) for r in landings if r['mode'] == mode})
               for mode in ('classic', 'wide')}
    comparisons, distinct = {}, {}
    for players in (1, 2):
        if all((mode, players) in runs for mode in ('classic', 'wide')):
            comparisons[str(players)] = pressure.compare(
                pool(runs['classic', players]), pool(runs['wide', players]),
                players=players, minimum_frames=1000, lower=.67, upper=1.5, required_rooms=ROOMS)
            groups = {mode: distinct_pool(runs[mode, players]) for mode in ('classic','wide')}
            distinct[str(players)] = dict(
                comparison=pressure.compare(groups['classic'], groups['wide'],
                    players=players, minimum_frames=1000, lower=.67, upper=1.5, required_rooms=ROOMS),
                removed_duplicates={mode: group['duplicate_observations'] for mode, group in groups.items()})
    result = dict(schema=1, gate_proof=False, reports=pins, required_stages=48,
                  landing_missing=missing, invalid_landings=invalid, landings=landings,
                  pressure=comparisons, distinct_input_observations=distinct,
                  invalid_state_censoring=censored,
                  limits=['Protected probes verify natural route entry, not survival or human difficulty.',
                          'Pressure runs use legal inputs, with continues; transitions are not independent clearance proof.',
                          'Axis bounds reject offscreen landings but do not establish an exact intended foot position.',
                          'Population guards and mode isolation are checked separately by l8_enhanced_layout.'])
    result['limits'].append('Distinct input histories are recorded trajectories, not independent human difficulty trials. '
                            'Partial visits and boundary decreases remain fully retained.')
    result['limits'].append('For integrity-qualified captures, whole visits ending at/after the first invalid '
                            'list sample are excluded from ratios and retained in invalid_state_censoring. '
                            'Legacy reports without integrity qualification have no such guarantee.')
    args.out.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(dict(missing=missing, invalid_landings=len(invalid),
                         pressure={p: dict(paired=len(v['rows']), missing=len(v['missing_required']),
                                           outliers=sum(not r['in_band'] for r in v['rows']))
                                   for p, v in comparisons.items()})))


if __name__ == '__main__':
    main()
