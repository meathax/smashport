"""Capture legal targeted pressure variants with bounded, owned child jobs."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import threading
import coverage_gap_manifest as gaps
import prepare_pressure_gaps as preparation
import pressure
import rebaseline as runner
from recover_c3_variants import events,prefix_bytes
from replay_c3_routes import reference_executable

ROOT=Path(__file__).resolve().parents[2]


def validate_case(case, strategy_name='original'):
    runner.require(type(case.get('name')) is str and re.fullmatch('[a-z0-9-]+',case['name']), 'Unsafe case name')
    runner.require(case.get('profile') in ('reference','enhanced') and type(case.get('players')) is int
                   and case['players'] in (1,2),'Invalid profile/player count')
    for key in ('circuit','source_wave','bot_seed','bot_after','final_frame'):
        runner.require(type(case.get(key)) is int,'Noninteger capture argument')
    runner.require(0<case['bot_after']<case['final_frame']<=2000000 and
                   case['final_frame']==case['bot_after']+60000,'Invalid bounded endpoint')
    strategy=(case['circuit'],case['source_wave'],case['bot_seed'],case.get('targets'))
    runner.require(strategy in preparation.strategy_set(strategy_name)[case['players']],'Unreviewed route strategy')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('plan','exe','reference-report','out'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--prepare-only',action='store_true')
    args=parser.parse_args();plan=json.loads(args.plan.read_text(encoding='utf-8'))
    runner.require(plan['result']=='PREPARED_LEGAL_PROFILE_PREFIXES_NOT_RUN','Wrong prepared plan')
    strategy_name=plan.get('strategy_set','original')
    selected=preparation.strategy_set(strategy_name)
    for pin in plan['pins']:gaps.checked(pin)
    baseline=json.loads(args.reference_report.read_text(encoding='utf-8'))
    reference_executable(args.exe.resolve(),baseline)
    runner.require(plan['verified_at']==baseline['verified_at'],'Runtime source identities differ')
    runner.check_d10_sources(ROOT,baseline['verified_at'])
    source=json.loads(gaps.checked(plan['pins'][0]).read_text(encoding='utf-8'))
    runner.require(source['result']=='COMPLETE_OBSERVATIONS_GATE_OPEN' and not source['owned_live_pids'],
                   'Pressure source not terminal')
    bases={row[0]:row for row in source['jobs']};steps={s['name']:s for s in source['steps']}
    cases=plan['cases'];seen=set()
    for case in cases:
        validate_case(case,strategy_name)
        key=preparation.case_key(case)
        runner.require(key not in seen,'Duplicate targeted capture');seen.add(key)
        base=bases[case['base']];step=steps[case['base']]
        runner.require(base[1:3]==[case['profile'],case['players']] and step['state']=='PASS','Wrong profile base')
        for field,leaf in [('base_inputs','recorded.inputs'),('base_content','content.jsonl')]:
            runner.require(case[field]==step['proof']['artifacts'][leaf],'Base artifact role changed')
        inputs=gaps.checked(case['base_inputs']);content=gaps.checked(case['base_content'])
        prefix=gaps.checked(case['prefix'])
        runner.require(prefix.read_bytes()==prefix_bytes(inputs.read_bytes(),case['bot_after']),'Changed legal prefix')
        found=preparation.stable_handoffs(gaps.samples(content),{(case['circuit'],case['source_wave'])})
        runner.require(found.get((case['circuit'],case['source_wave']))==case['bot_after'],'Unverified stable handoff')
    expected=preparation.expected_case_keys(strategy_name)
    runner.require(seen==expected and len(cases)==len(expected)==len({c['name'] for c in cases}),
                   'Incomplete case matrix')
    out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    out.mkdir(parents=True);frozen=out/'stv_l8_lockstep-frozen.exe';shutil.copy2(args.exe,frozen)
    rom=ROOT/'original/user-supplied-roms/smashtv.zip'
    pins=plan['pins']+[runner.pin(p) for p in [args.plan,args.reference_report,args.exe,frozen,rom,Path(__file__),
         Path(preparation.__file__),Path(pressure.__file__),ROOT/'tools/scenarios/rebaseline.py']]
    report=runner.Report(out,dict(schema='stv.pressure_gap_capture.v1',result='PREPARED_NOT_RUN',gate_proof=False,
        strategy_set=strategy_name,
        verified_at=baseline['verified_at'],head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),
        pins=pins,cases=cases,steps=[],targets={},scope='Legal per-profile capture; no oracle or clearance claim.'))
    if args.prepare_only:return 0
    report.update(report.data,result='RUNNING')
    timer=threading.Timer(7200,report.cancel_children,args=('Two-hour targeted pressure deadline exceeded',));timer.daemon=True;timer.start()
    try:
        def capture(job):
            case=job[1];name=case['name'];directory=out/name;directory.mkdir()
            command=[frozen,rom,directory/'native.bin',case['final_frame'],'--native-hcount',
                '--inputs',case['prefix']['path'],'--bot',directory/'recorded.inputs','--bot-after',case['bot_after'],
                '--bot-seed',case['bot_seed'],'--snd',directory/'native.snd','--coverage',directory/'native.cov',
                '--content',directory/'content.jsonl']
            if case['players']==2:command.append('--bot2')
            if case['profile']=='enhanced':command.append('--enhanced')
            def verify(_):
                final=case['final_frame'];recorded=events((directory/'recorded.inputs').read_bytes(),final)
                prefix=events(Path(case['prefix']['path']).read_bytes())
                runner.require([r[:4] for r in recorded if r[0]<case['bot_after']]==[r[:4] for r in prefix],
                               'Capture did not preserve prefix events')
                run=pressure.read_run(directory/(name+'.log'),case['players'])
                runner.require(run['terminal']['frames']==final and run['terminal']['big_field']==0,'Wrong terminal')
                visits=[v for v in run['observations'] if v['circuit']==case['circuit'] and
                    v['wave'] in case['targets'] and v['frame']>case['bot_after'] and v['frames']>=1000]
                return dict(endpoint=runner.main_endpoint(directory/'native.bin',final),
                    sound=runner.sound_endpoint(directory/'native.snd',final),
                    content=runner.content_samples(directory/'content.jsonl',final),target_visits=visits,
                    missing_target_players=[[p,w] for p in range(1,case['players']+1) for w in case['targets']
                        if not any(v['player']==p and v['wave']==w for v in visits)],
                    artifacts={p.name:runner.pin(p) for p in directory.iterdir() if p.is_file()})
            report.run(name,command,directory,verify)
        runner.run_main_group(report,[(c['name'],c) for c in cases],2,capture)
        for pin in pins:gaps.checked(pin)
        runner.check_d10_sources(ROOT,baseline['verified_at'])
        report.update(report.data,result='COMPLETE_TARGETED_PRESSURE_OBSERVATIONS_GATE_OPEN')
        return 3
    except BaseException as error:
        report.cancel_children(str(error));report.update(report.data,result='FAIL',error=str(error));raise
    finally:
        timer.cancel();report.update(report.data,finished_at_utc=runner.now())
        print(json.dumps(dict(result=report.data['result'],report=str(report.path),owned_live_pids=report.data['owned_live_pids'])))


if __name__=='__main__':raise SystemExit(main())
