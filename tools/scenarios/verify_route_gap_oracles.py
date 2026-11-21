"""Replay completed legal route variants in pinned MAME with raw exit events."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import coverage_gap_manifest as gaps
import rebaseline as runner
from replay_c3_routes import strict_marker
from route_write_events import classify

ROOT=Path(__file__).resolve().parents[2]
MAME=Path('D:/Arcade/AI/mameexe/mame.exe')
MAME_SHA='af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3'


def matching_reuse_case(previous,case):
    runner.require(previous.get('result') in ('FAIL','PASS_STRICT_ROUTES_AND_EXIT_WRITES_CLEARANCE_OPEN')
                   and previous.get('owned_live_pids')==[], 'Reuse owner is not terminal')
    matches=[r for r in previous['cases'] if r['name']==case['name']]
    runner.require(len(matches)==1,'Missing/duplicate reuse case')
    old=matches[0]
    runner.require(all(old[k]==case[k] for k in ('name','target','final_frame','inputs','native')),
                   'Reuse input/native/endpoint roles differ')
    return old


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-report',type=Path,required=True)
    parser.add_argument('--inventory',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--reuse-report',type=Path)
    args=parser.parse_args()
    native=json.loads(args.native_report.read_text(encoding='utf-8'))
    runner.require(native['result']=='COMPLETE_NATIVE_ROUTE_VARIANTS_ORACLE_OPEN' and
                   not native['owned_live_pids'],'Native group is not terminal')
    runner.require(runner.sha(MAME)==MAME_SHA,'Wrong MAME executable')
    inventory=json.loads(args.inventory.read_text(encoding='utf-8'))
    for pin in inventory['pins']:gaps.checked(pin)
    for pin in native['pins']:gaps.checked(pin)
    steps={s['name']:s for s in native['steps']}
    cases=[]
    for case in native['cases']:
        step=steps[case['name']]
        runner.require(step['state']=='PASS' and step['proof']['target_candidate_observed'],
                       'Target candidate absent/failed')
        artifacts=step['proof']['artifacts']
        for pin in artifacts.values():gaps.checked(pin)
        runner.main_endpoint(gaps.checked(artifacts['native.bin']),case['final_frame'])
        cases.append(dict(name=case['name'],target=case['target'],final_frame=case['final_frame'],
                          inputs=artifacts['recorded.inputs'],native=artifacts['native.bin'],state='QUEUED'))
    out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use a fresh build directory')
    out.mkdir(parents=True)
    pins=[runner.pin(p) for p in [args.native_report,args.inventory,MAME,Path(__file__),
          ROOT/'tools/scenarios/route_write_events.py',ROOT/'tools/lockstep/record.lua',
          ROOT/'tools/lockstep/route_events.lua',ROOT/'tools/lockstep/lsfile.py']]
    previous=None
    if args.reuse_report:
        previous=json.loads(args.reuse_report.read_text(encoding='utf-8'))
        # Reuse only captures made with these exact ROM-input receipts and
        # recorders. The old classifier/controller may be the cause of failure.
        for path in (args.native_report,args.inventory,MAME,ROOT/'tools/lockstep/record.lua',
                     ROOT/'tools/lockstep/route_events.lua'):
            expected=runner.pin(path)
            runner.require(expected in previous['pins'],'Reuse recorder/input provenance differs')
        pins.append(runner.pin(args.reuse_report))
    report=dict(schema='stv.route_gap_oracles.v1',result='RUNNING',gate_proof=False,
                clearance_proved=False,owner_pid=os.getpid(),owned_live_pids=[],
                started_at_utc=runner.now(),pins=pins,cases=cases,max_parallel_children=1,
                deadline_seconds=21600,verified_at=native['verified_at'])
    deadline=time.monotonic()+21600
    def save():runner.atomic_json(out/'report.json',report)
    def run(command,env,log,seconds):
        remaining=min(seconds,deadline-time.monotonic())
        runner.require(remaining>0,'Global oracle deadline reached')
        with log.open('wb') as stream:
            child=subprocess.Popen(list(map(str,command)),cwd=ROOT,env=env,stdout=stream,
                stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            report['owned_live_pids']=[child.pid];save()
            try:
                code=child.wait(timeout=remaining)
                runner.require(code==0,f'Child exit {code}: {log}')
            finally:
                if child.poll() is None:
                    child.terminate()
                    try:child.wait(timeout=5)
                    except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
                report['owned_live_pids']=[];save()
    save()
    try:
        for case in cases:
            directory=out/case['name'];directory.mkdir()
            command=[MAME,'smashtv','-rompath',ROOT/'original/user-supplied-roms']
            for flag,leaf in [('cfg','cfg'),('nvram','nvram'),('diff','diff'),('snapshot','snap'),
                              ('input','inp'),('state','sta')]:
                (directory/leaf).mkdir();command += [f'-{flag}_directory',directory/leaf]
            command += ['-noreadconfig','-skip_gameinfo','-video','none','-sound','none','-nothrottle',
                        '-seconds_to_run',str(math.ceil(case['final_frame']/50)+600),
                        '-autoboot_script',ROOT/'tools/lockstep/route_events.lua']
            env={k:v for k,v in os.environ.items() if not k.upper().startswith('STV_')}
            env.update(STV_LS_OUT=str(directory/'mame.bin'),STV_LS_FRAMES=str(case['final_frame']),
                STV_LS_INPUTS=case['inputs']['path'],STV_LS_COMPACT='1',STV_LS_SUMMARY='1',STV_LS_DUMP_EVERY='60',
                STV_ROUTE_BASE_RECORDER=str(ROOT/'tools/lockstep/record.lua'),
                STV_ROUTE_EVENTS_OUT=str(directory/'events.jsonl'))
            case.update(state='RUNNING',command=list(map(str,command)),started_at_utc=runner.now());save()
            reuse=matching_reuse_case(previous,case) if previous else None
            origin=args.reuse_report.resolve().parent/case['name'] if reuse else None
            if origin and (origin/'mame.bin').exists():
                runner.main_endpoint(origin/'mame.bin',case['final_frame'])
                runner.require(f"STV_LS_DONE frames={case['final_frame']} " in (origin/'mame.log').read_text(),
                               'Reuse MAME did not report the intended terminal frame')
                case['reused_artifacts']={leaf:runner.pin(origin/leaf) for leaf in ('mame.bin','events.jsonl','mame.log')}
                for leaf,pin in case['reused_artifacts'].items():
                    shutil.copy2(gaps.checked(pin),directory/leaf)
                case['mame_execution']='REUSED_TERMINAL_CAPTURE';save()
            else:
                run(command,env,directory/'mame.log',5400)
            case['endpoint']=runner.main_endpoint(directory/'mame.bin',case['final_frame'])
            diff=[sys.executable,ROOT/'tools/lockstep/lsfile.py','diff',case['native']['path'],directory/'mame.bin']
            run(diff,env,directory/'strict-diff.log',180)
            strict_marker((directory/'strict-diff.log').read_text(),case['final_frame'])
            observed=classify(directory/'events.jsonl',inventory,case['final_frame'])
            matching=[e for e in observed['exit_writes'] if
                [e['event']['circuit'],e['event']['old_wave'],e['event']['data']]==case['target']]
            runner.require(matching,'Target lacks source-bound exit write')
            runner.atomic_json(directory/'classified-events.json',observed)
            case.update(state='PASS',target_exit_writes=matching,finished_at_utc=runner.now(),
                artifacts={p.name:runner.pin(p) for p in directory.iterdir() if p.is_file()});save()
        for pin in pins:gaps.checked(pin)
        runner.check_d10_sources(ROOT,native['verified_at'])
        report['result']='PASS_STRICT_ROUTES_AND_EXIT_WRITES_CLEARANCE_OPEN'
    except BaseException as error:
        report.update(result='FAIL',error=str(error));raise
    finally:
        report['finished_at_utc']=runner.now();save()
        print(json.dumps(dict(result=report['result'],report=str(out/'report.json'))))
    return 3


if __name__=='__main__':raise SystemExit(main())
