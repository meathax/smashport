"""Generate legal route-variant inputs from verified prefixes; no oracle claim."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import threading

import coverage_gap_manifest as gaps
import rebaseline as runner
import replay_c3_routes as routes
from recover_c3_variants import events, prefix_bytes
from route_observations import transitions

ROOT=Path(__file__).resolve().parents[2]


def validate_case(case):
    runner.require(type(case.get('name')) is str and re.fullmatch(r'[a-z0-9-]+',case['name']), 'Unsafe case name')
    for key in ('bot_after','bot_seed','final_frame'):
        runner.require(type(case.get(key)) is int,'Noninteger capture bound')
    runner.require(0<case['bot_after']<case['final_frame']<=2000000 and 0<=case['bot_seed']<=100,
                   'Invalid capture bounds/seed')
    runner.require(type(case.get('two_players')) is bool,'Player count flag must be boolean')
    target=case.get('target')
    runner.require(type(target) is list and len(target)==3 and all(type(x) is int for x in target)
                   and 0<=target[0]<3 and all(1<=x<=24 for x in target[1:]),'Invalid route target')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--plan',type=Path,required=True)
    ap.add_argument('--exe',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--prepare-only',action='store_true')
    args=ap.parse_args()
    plan_path=args.plan.resolve()
    plan=json.loads(plan_path.read_text(encoding='utf-8'))
    runner.require(plan.get('result')=='PREPARED_LEGAL_PREFIXES_NOT_RUN' and plan.get('gate_proof') is False,
                   'Not a prepared legal-prefix plan')
    reference_path=gaps.checked(plan['reference_report'])
    baseline=json.loads(reference_path.read_text(encoding='utf-8'))
    exe=args.exe.resolve()
    routes.reference_executable(exe,baseline)
    runner.check_d10_sources(ROOT,baseline['verified_at'])
    steps=gaps.complete_steps(baseline)
    captures=[gaps.current_capture(baseline,steps,'1p',1040000)]
    captures+=routes.read_completed(gaps.checked(plan['route_report']))
    allowed={c['name']:c for c in captures}
    cases=plan['cases']
    runner.require(cases and len(cases)==len({c['name'] for c in cases}),'Empty/duplicate cases')
    runner.require(gaps.checked(plan['prefix_tool']).resolve()==ROOT/'tools/scenarios/recover_c3_variants.py',
                   'Prefix tool pin has the wrong role')
    candidate=json.loads(gaps.checked(plan['candidate_report']).read_text(encoding='utf-8'))
    runner.require(candidate.get('schema')=='stv.route_observations.v1' and
                   candidate.get('result')=='SAMPLED_ROUTE_CANDIDATES_CLEARANCES_OPEN' and
                   candidate['reference_report']==plan['reference_report'] and candidate['route_report']==plan['route_report'],
                   'Candidate report does not bind these reference captures')
    missing={(o['circuit'],o['source_wave'],o['destination_field']) for o in candidate['options'] if not o['observations']}
    pins=[plan['reference_report'],plan['route_report'],plan['candidate_report'],plan['prefix_tool'],runner.pin(plan_path)]
    for case in cases:
        validate_case(case)
        runner.require(case['base'] in allowed,'Unverified base capture')
        base=allowed[case['base']]
        runner.require(case['base_inputs']==base['inputs'] and case['base_content']==base['content'],
                       'Prefix roles differ from complete reference capture')
        runner.require(case['two_players']==case['base'].startswith('route:'),'Player count differs from base scenario')
        original=gaps.checked(case['base_inputs']).read_bytes()
        prefix=gaps.checked(case['prefix'])
        runner.require(prefix.read_bytes()==prefix_bytes(original,case['bot_after']),'Prefix differs from recorded base')
        handoff=next((r for r in gaps.samples(gaps.checked(case['base_content'])) if r['frame']==case['bot_after']),None)
        runner.require(handoff is not None and handoff['gamstate']==16 and handoff['p1_lives']>0
                       and handoff['object_list_complete'] and [handoff['circuit'],handoff['wave']]==case['target'][:2],
                       'Handoff no longer matches sampled live source room')
        pins.extend(case[k] for k in ('base_inputs','base_content','prefix'))
    runner.require({tuple(c['target']) for c in cases}==missing,'Prepared targets differ from current route gaps')
    for pin in pins:gaps.checked(pin)
    out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build'),'Capture output must remain under build/')
    out.mkdir(parents=True,exist_ok=False)
    frozen=out/'stv_l8_lockstep-frozen.exe'
    shutil.copyfile(exe,frozen)
    rom=ROOT/'original/user-supplied-roms/smashtv.zip'
    for path in [rom,ROOT/'tools/lockstep/bot.hpp',ROOT/'tools/lockstep/content_observer.hpp']:
        runner.require(baseline['pins'][str(path)]==runner.pin(path),'Capture input differs from reference')
        pins.append(runner.pin(path))
    pins.extend(runner.pin(p) for p in [exe,frozen,Path(__file__),ROOT/'tools/scenarios/rebaseline.py',
        ROOT/'tools/scenarios/coverage_gap_manifest.py',ROOT/'tools/scenarios/replay_c3_routes.py',
        ROOT/'tools/scenarios/route_observations.py'])
    report=runner.Report(out,dict(schema='stv.route_gap_capture.v1',result='PREPARED_NOT_RUN',gate_proof=False,
        verified_at=baseline['verified_at'],head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),
        pins=pins,cases=cases,steps=[],targets={},oracle_proof=False,
        scope='Legal native input generation and sampled target candidates only; fresh MAME comparison still required.'))
    if args.prepare_only:return 0
    report.update(report.data,result='RUNNING')
    timer=threading.Timer(7200,report.cancel_children,args=('Two-hour route generation deadline exceeded',))
    timer.daemon=True
    timer.start()
    try:
        for case in cases:
            name=case['name'];dest=out/name;dest.mkdir()
            command=[frozen,rom,dest/'native.bin',case['final_frame'],'--native-hcount',
                '--inputs',case['prefix']['path'],'--bot',dest/'recorded.inputs','--bot-after',case['bot_after'],
                '--bot-seed',case['bot_seed'],'--snd',dest/'native.snd','--coverage',dest/'native.cov',
                '--content',dest/'content.jsonl']
            if case['two_players']:command.append('--bot2')
            def verify(_):
                final=case['final_frame']
                recorded=events((dest/'recorded.inputs').read_bytes(),final)
                expected=events(Path(case['prefix']['path']).read_bytes())
                runner.require([e[:4] for e in recorded if e[0]<case['bot_after']]==[e[:4] for e in expected],
                               'Recorded variant lost/changed the legal prefix')
                matched=[r for r in transitions(gaps.samples(dest/'content.jsonl'))
                         if [r['circuit'],r['source_wave'],r['destination_wave']]==case['target']]
                report.data['targets'][name]=dict(target=case['target'],sampled_candidates=matched,
                                                  activation_proved=False,clearance_proved=False)
                return dict(endpoint=runner.main_endpoint(dest/'native.bin',final),
                    sound=runner.sound_endpoint(dest/'native.snd',final),content=runner.content_samples(dest/'content.jsonl',final),
                    target_candidate_observed=bool(matched),
                    artifacts={p.name:runner.pin(p) for p in dest.iterdir() if p.is_file()})
            report.run(name,command,dest,verify)
        for pin in pins:gaps.checked(pin)
        runner.check_d10_sources(ROOT,baseline['verified_at'])
        report.update(report.data,result='COMPLETE_NATIVE_ROUTE_VARIANTS_ORACLE_OPEN',finished_at_utc=runner.now(),
            targets_without_candidate=[name for name,value in report.data['targets'].items() if not value['sampled_candidates']])
        return 3
    except BaseException as error:
        report.cancel_children(str(error) or type(error).__name__)
        report.update(report.data,result='FAIL',error=str(error),finished_at_utc=runner.now())
        raise
    finally:
        timer.cancel()
        print(json.dumps(dict(result=report.data['result'],report=str(report.path),owned_live_pids=report.data['owned_live_pids'])))


if __name__=='__main__':raise SystemExit(main())
