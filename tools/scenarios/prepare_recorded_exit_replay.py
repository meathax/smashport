"""Seal a bounded, unmodified prefix of a qualified native bot recording."""
import argparse
import json
from pathlib import Path
import rebaseline as r
import coverage_gap_manifest as gaps
import pressure
from recover_c3_variants import prefix_bytes
from replay_c3_routes import reference_executable

ROOT = Path(__file__).resolve().parents[2]


def select(report, name, players, final, runtime, exe, targets):
    r.require(report.get('result') == 'COMPLETE_OBSERVATIONS_GATE_OPEN'
              and report.get('owned_live_pids') == [] and report.get('verified_at') == runtime,
              'Baseline recording is not current and terminal')
    jobs = [j for j in report['jobs'] if j[0] == name]
    steps = [s for s in report['steps'] if s['name'] == name]
    r.require(len(jobs) == len(steps) == 1, 'Missing or duplicated baseline recording')
    job, step = jobs[0], steps[0]
    r.require(job[1] == 'reference' and job[2] == players, 'Wrong profile/player count')
    r.require(type(final) is int and 0 < final <= job[4], 'Invalid bounded endpoint')
    r.require(step['state'] == 'PASS' and step['exit_code'] == 0, 'Baseline step failed')
    ep = step['proof']['endpoint']
    r.require(ep['first_frame'] == 0 and ep['last_frame'] == job[4]
              and ep['frames'] == job[4] + 1, 'Baseline endpoint incomplete')
    cmd = step['command']
    r.require('--bot' in cmd and '--enhanced' not in cmd and int(cmd[3]) == job[4],
              'Unexpected baseline command')
    source_exe = Path(cmd[0])
    inventory = report['pins']
    pins = [p for p in (inventory.values() if isinstance(inventory, dict) else inventory)
            if Path(p['path']) == source_exe]
    r.require(len(pins) == 1, 'Baseline executable not uniquely pinned')
    gaps.checked(pins[0])
    r.require(r.sha(source_exe) == r.sha(exe), 'Baseline executable differs')
    artifacts = step['proof']['artifacts']
    inputs = gaps.checked(artifacts['recorded.inputs'])
    r.require(inputs == Path(cmd[cmd.index('--bot') + 1]), 'Not the recorded bot output')
    log = gaps.checked(artifacts[name + '.log'])
    run = pressure.read_run(log, players)
    r.require(run['terminal']['frames'] == job[4], 'Baseline log endpoint differs')
    observations = []
    for circuit, wave in targets:
        matches = [v for v in run['observations'] if v['player'] == 1
                   and (v['circuit'], v['wave']) == (circuit, wave) and v['frame'] < final]
        r.require(matches, f'No completed observation before endpoint for {circuit}/{wave}')
        observations.append(min(matches, key=lambda v: v['frame']))
    return prefix_bytes(inputs.read_bytes(), final + 1), observations, inputs, log


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('report', 'reference-report', 'exe', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--case', required=True)
    p.add_argument('--players', type=int, choices=(1, 2), required=True)
    p.add_argument('--final-frame', type=int, required=True)
    p.add_argument('--target', type=int, nargs=2, action='append', required=True)
    args = p.parse_args()
    reference = json.loads(args.reference_report.read_text())
    reference_executable(args.exe.resolve(), reference)
    gaps.complete_steps(reference)
    r.check_d10_sources(ROOT, reference['verified_at'])
    data, observations, source_inputs, log = select(json.loads(args.report.read_text()),
        args.case, args.players, args.final_frame, reference['verified_at'], args.exe, args.target)
    out = args.out.resolve()
    r.require(out.is_relative_to(ROOT / 'build') and not out.exists(), 'Use fresh build output')
    out.mkdir()
    schedule = out / 'recorded.inputs'
    schedule.write_bytes(data)
    r.atomic_json(out / 'plan.json', dict(schema='stv.recorded_exit_replay.v1',
        result='PREPARED_NOT_EXECUTED', gate_proof=False, verified_at=reference['verified_at'],
        players=args.players, source_case=args.case, final_frame=args.final_frame,
        targets=args.target, source_observations=observations, inputs=r.pin(schedule),
        pins=[r.pin(x) for x in (args.report, args.reference_report, args.exe, source_inputs,
            log, Path(__file__), ROOT/'tools/scenarios/recover_c3_variants.py',
            ROOT/'tools/scenarios/pressure.py')],
        limits=['Source WAVESTAT observations select an interval; they do not prove clearance.',
                'No bot reset, input invention or RAM modification; prefix includes endpoint events.',
                'Complete native/MAME comparison and exit ownership remain required.']))
    print(out / 'plan.json')


if __name__ == '__main__':
    main()
