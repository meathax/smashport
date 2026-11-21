"""Extend an existing pressure pool with complete, matched new seed pairs."""
import argparse
import copy
import json
from pathlib import Path
import coverage_gap_manifest as gaps
import pressure
import rebaseline as r
from replay_c3_routes import reference_executable

ROOT = Path(__file__).resolve().parents[2]


def checked_analysis_pin(pin, prior_parser=None):
    # Only this explicitly supplied prior analysis source may be recovered.
    # Runtime, observers, inputs, logs and other dependencies still match exactly.
    if prior_parser is not None and gaps.resolve_pin(pin).resolve() == Path(pressure.__file__).resolve():
        recovery=dict(pin,path=str(prior_parser.resolve()))
        return gaps.checked(recovery)
    return gaps.checked(pin)


def verify_binary_copy(exe, reference):
    originals = [Path(p) for p in reference.get('pins', {}) if Path(p).name == 'stv_l8_lockstep.exe']
    r.require(len(originals) == 1, 'Reference recorder identity is ambiguous')
    expected = reference_executable(originals[0], reference)
    actual = r.pin(exe)
    r.require(all(actual[k] == expected[k] for k in ('bytes','sha256')),
              'Frozen recorder contents differ from reference')


def matrix(report, verified_at):
    r.require(report['schema'] == 'stv.pressure_capture.v1' and
              report['result'] == 'COMPLETE_OBSERVATIONS_GATE_OPEN' and
              not report['owned_live_pids'], 'Seed batch is not terminal')
    r.require(report['verified_at'] == verified_at, 'Seed batch source differs')
    steps = {s['name']: s for s in report['steps']}
    jobs = report['jobs']
    r.require(jobs and len(steps) == len(report['steps']) == len(jobs) and
              all(s['state'] == 'PASS' and s['exit_code'] == 0 for s in steps.values()),
              'Duplicate or incomplete seed steps')
    pairs = {}
    names = set()
    for name, mode, players, seed, final in jobs:
        r.require(mode in ('reference', 'enhanced') and players in (1, 2) and
                  type(seed) is int and seed > 0 and type(final) is int and final > 0,
                  'Invalid seed job')
        r.require(name == f'{mode}-{players}p-seed{seed}' and name in steps and name not in names,
                  'Duplicate or inconsistent seed identity')
        names.add(name)
        pairs.setdefault((players, seed), {})[mode] = final
    r.require(all(set(pair) == {'reference', 'enhanced'} and len(set(pair.values())) == 1
                  for pair in pairs.values()), 'Unpaired seed or unequal frame budget')
    return steps


def append_run(group, name, run):
    r.require(not any(row.get('source_run') == name for rows in group.values() for row in rows),
              'Seed already present in pooled observations')
    for key in ('observations', 'partial_visits', 'boundary_life_decreases'):
        group[key].extend(dict(row, source_run=name) for row in run[key])


def build(base_path, batch_path, prior_parser=None):
    base = json.loads(base_path.read_text(encoding='utf-8'))
    batch = json.loads(batch_path.read_text(encoding='utf-8'))
    r.require(base['result'] == 'TARGET_VISITS_ADDED_CLEARABILITY_OPEN',
              'Expected the existing target-augmented pool')
    steps = matrix(batch, base['verified_at'])
    for pin in base['pins'] + list(batch['pins'].values()):
        checked_analysis_pin(pin,prior_parser)
    ref_path = Path(batch['reference_report'])
    reference = json.loads(ref_path.read_text(encoding='utf-8'))
    gaps.complete_steps(reference)
    r.require(reference['verified_at'] == base['verified_at'], 'Reference source differs')
    r.check_d10_sources(ROOT, base['verified_at'])
    rooms = pressure.read_rooms(gaps.checked(batch['room_inventory']))
    groups = {players: copy.deepcopy(row['source_visits']) for players, row in base['pooled'].items()}
    pins = [r.pin(p) for p in (base_path, batch_path, ref_path, Path(__file__), Path(pressure.__file__))]
    if prior_parser is not None:pins.append(r.pin(prior_parser))
    added = []
    for name, mode, players, seed, final in batch['jobs']:
        step = steps[name]
        command = step['command']
        verify_binary_copy(Path(command[0]), reference)
        r.require(int(command[3]) == final and '--bot' in command and '--inputs' not in command and
                  '--bot-after' not in command and int(command[command.index('--bot-seed') + 1]) == seed and
                  ('--bot2' in command) == (players == 2) and
                  ('--enhanced' in command) == (mode == 'enhanced'), 'Command and seed identity differ')
        log = gaps.checked(step['proof']['artifacts'][name + '.log'])
        r.require(str(log) == step['log'] and r.sha(log) == step['log_sha256'], 'Log identity differs')
        run = pressure.read_run(log, players)
        r.require(run['terminal']['frames'] == final and not run['terminal']['big_field'],
                  'Incomplete seed endpoint')
        append_run(groups[str(players)][mode], name, run)
        added.append(dict(name=name,frames=final,completed_visits=len(run['observations'])))
        pins.append(r.pin(log))
    pooled = {}
    for players, group in groups.items():
        result = pressure.compare(group['reference'], group['enhanced'], players=int(players),
                                  minimum_frames=1000, lower=.67, upper=1.5, required_rooms=rooms)
        result['source_visits'] = group
        pooled[players] = result
    return dict(schema='stv.extended_pressure_seeds.v1', result='NEW_SEEDS_POOLED_CLEARABILITY_OPEN',
                gate_proof=False, verified_at=base['verified_at'], pins=pins, added=added, pooled=pooled,
                summary={key: dict(paired=len(value['rows']), missing=len(value['missing_required']),
                    out_of_band=sum(not row['in_band'] for row in value['rows'])) for key,value in pooled.items()},
                limits=['Visit-weighted means retain all partial visits and ambiguous boundary life decreases.',
                        'No repeated replay prefix or duplicate seed is admitted.',
                        'More observations are not a balance waiver, clearance proof or causal diagnosis.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pool', type=Path, required=True)
    parser.add_argument('--batch', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--prior-parser', type=Path, help='Exact preserved parser source used by older capture receipts')
    args = parser.parse_args()
    out = args.out.resolve()
    r.require(out.is_relative_to(ROOT/'build') and not out.exists(), 'Use a fresh build output')
    result = build(args.pool.resolve(), args.batch.resolve(), args.prior_parser)
    r.atomic_json(out, result)
    print(json.dumps(result['summary']))
