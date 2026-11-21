"""Renew paired offline pressure captures; observations never certify clears."""
import argparse
import json
import os
from pathlib import Path
import shutil
import threading

import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner
from replay_c3_routes import reference_executable

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--reference-report', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--workers', type=int, choices=(1, 2, 3), default=3)
    ap.add_argument('--prepare-only', action='store_true')
    args = ap.parse_args()
    source = args.reference_report.resolve()
    baseline = json.loads(source.read_text(encoding='utf-8'))
    exe = args.exe.resolve()
    reference_executable(exe, baseline)
    runner.check_d10_sources(ROOT, baseline['verified_at'])
    out = args.out.resolve()
    runner.require(out.is_relative_to(ROOT / 'build'), 'Capture outputs must remain under build/')
    out.mkdir(parents=True, exist_ok=False)
    frozen = out / 'stv_l8_lockstep-frozen.exe'
    shutil.copyfile(exe, frozen)
    inventory_path = ROOT / 'evidence/c3-content-observations-2026-10-02.json'
    inventory = json.loads(inventory_path.read_text(encoding='utf-8'))
    rooms = [r['selector'] for r in inventory['items'] if r.get('kind') == 'wave_selector' and r.get('role') == 'wave_room']
    runner.require(len(rooms) == len({tuple(r) for r in rooms}) == 48, 'Normal-room inventory differs')
    runner.atomic_json(out / 'required-rooms.json', rooms)
    rom = ROOT / 'original/user-supplied-roms/smashtv.zip'
    pins = {str(p): runner.pin(p) for p in [frozen, exe, rom, inventory_path,
        ROOT / 'tools/lockstep/bot.hpp', ROOT / 'tools/lockstep/content_observer.hpp',
        ROOT / 'tools/scenarios/pressure.py', ROOT / 'tools/scenarios/rebaseline.py',
        ROOT / 'tools/scenarios/replay_c3_routes.py', ROOT / 'tools/scenarios/coverage_gap_manifest.py',
        Path(__file__).resolve()]}
    for p in [rom, ROOT / 'tools/lockstep/bot.hpp', ROOT / 'tools/lockstep/content_observer.hpp']:
        runner.require(baseline['pins'].get(str(p)) == runner.pin(p), 'Capture input differs from reference source')
    jobs = [(f'{profile}-{players}p-seed{seed}', profile, players, seed, frames)
            for seed in (1, 2) for players, frames in ((1, 800000 if seed == 1 else 430000), (2, 440000))
            for profile in ('reference', 'enhanced')]
    report = runner.Report(out, dict(schema='stv.pressure_capture.v1', result='PREPARING', gate_proof=False,
        verified_at=baseline['verified_at'], head_at_launch=runner.git(ROOT, 'rev-parse', 'HEAD'),
        owner_pid=os.getpid(), reference_report=str(source), pins=pins, steps=[], comparisons={},
        jobs=jobs, room_inventory=runner.pin(out / 'required-rooms.json'),
        required_inventory_provenance='48 normal selectors declared by the preserved C3 receipt; missing backing inventories and clearance proof remain open.',
        scope='Matched executable, ROM, bot, seed and frame budget; only enhanced geometry differs. Separate closed-loop input streams are retained.'))
    if args.prepare_only:
        report.update(report.data, result='PREPARED_NOT_RUN')
        return 0
    report.update(report.data, result='RUNNING')
    timer = threading.Timer(7200, report.cancel_children, args=('Two-hour capture-batch deadline exceeded',))
    timer.daemon = True
    timer.start()
    try:
        def capture(case):
            name, profile, players, seed, frames = case
            dest = out / name
            dest.mkdir()
            command = [frozen, rom, dest / 'native.bin', frames, '--native-hcount',
                '--bot', dest / 'recorded.inputs', '--bot-seed', seed,
                '--snd', dest / 'native.snd', '--coverage', dest / 'native.cov', '--content', dest / 'content.jsonl']
            if players == 2: command.append('--bot2')
            if profile == 'enhanced': command.append('--enhanced')
            def verify(_):
                parsed = pressure.read_run(dest / (name + '.log'), players)
                runner.require(parsed['terminal']['frames'] == frames and parsed['terminal']['big_field'] == 0,
                               'Capture endpoint differs')
                return dict(endpoint=runner.main_endpoint(dest / 'native.bin', frames),
                    sound=runner.sound_endpoint(dest / 'native.snd', frames),
                    content=runner.content_samples(dest / 'content.jsonl', frames),
                    pressure_observations=len(parsed['observations']), unfinished_visits=len(parsed['partial_visits']),
                    artifacts={p.name: runner.pin(p) for p in dest.iterdir() if p.is_file()})
            report.run(name, command, dest, verify)
        runner.run_main_group(report, jobs, args.workers, capture)
        for seed in (1, 2):
            for players in (1, 2):
                pair = f'{players}p-seed{seed}'
                ref, enhanced = [pressure.read_run(out / f'{mode}-{pair}' / f'{mode}-{pair}.log', players)
                                 for mode in ('reference', 'enhanced')]
                comparison = pressure.compare(ref, enhanced, players=players, minimum_frames=1000,
                                              lower=0.67, upper=1.5, required_rooms=rooms)
                result_path = out / (pair + '.json')
                runner.atomic_json(result_path, comparison)
                report.data['comparisons'][pair] = dict(result=comparison['result'], artifact=runner.pin(result_path))
                report.save()
        gaps.complete_steps(json.loads(source.read_text(encoding='utf-8')))
        runner.check_d10_sources(ROOT, baseline['verified_at'])
        for pin in pins.values(): gaps.checked(pin)
        report.update(report.data, result='COMPLETE_OBSERVATIONS_GATE_OPEN', finished_at_utc=runner.now(),
                      reference_receipt=runner.pin(source))
        return 3
    except BaseException as error:
        report.cancel_children(str(error) or type(error).__name__)
        report.update(report.data, result='FAIL', error=str(error) or type(error).__name__, finished_at_utc=runner.now())
        raise
    finally:
        timer.cancel()
        print(json.dumps({'result': report.data['result'], 'report': str(report.path), 'owned_live_pids': report.data['owned_live_pids']}))


if __name__ == '__main__':
    raise SystemExit(main())
