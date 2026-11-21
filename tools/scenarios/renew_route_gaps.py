"""Renew native targeted routes against preserved complete arcade streams."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import rebaseline as runner
import coverage_gap_manifest as gaps
import replay_c3_routes as routes

ROOT=Path(__file__).resolve().parents[2]


def cases_from(report):
    runner.require(report.get('result')=='PASS_STRICT_ROUTES_AND_EXIT_WRITES_CLEARANCE_OPEN' and
                   report.get('owned_live_pids')==[],'Oracle owner not successfully terminal')
    cases=report['cases'];names=set()
    runner.require(len(cases)==6,'Expected six targeted routes')
    for c in cases:
        runner.require(c['state']=='PASS' and re.fullmatch('[a-z0-9-]+',c['name']) and c['name'] not in names,
                       'Invalid/duplicate oracle case')
        runner.require(type(c['final_frame']) is int and 0<c['final_frame']<=300000,'Invalid route endpoint')
        names.add(c['name'])
    return cases


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('exe','reference-report','oracle-report','out'):p.add_argument('--'+n,type=Path,required=True)
    args=p.parse_args();exe=args.exe.resolve();base=json.loads(args.reference_report.read_text(encoding='utf-8'))
    routes.reference_executable(exe,base);gaps.complete_steps(base);runner.check_d10_sources(ROOT,base['verified_at'])
    old=json.loads(args.oracle_report.read_text(encoding='utf-8'));cases=cases_from(old)
    rom=ROOT/'original/user-supplied-roms/smashtv.zip';comparator=ROOT/'tools/lockstep/lsfile.py'
    pins=[runner.pin(p) for p in [exe,rom,comparator,args.reference_report,args.oracle_report,Path(__file__),
        ROOT/'tools/scenarios/rebaseline.py',ROOT/'tools/scenarios/replay_c3_routes.py']]
    schedules=[]
    for c in cases:
        # Completed oracle records include final artifact pins even when captures were reused.
        oracle=c['artifacts']['mame.bin'];gaps.checked(oracle);gaps.checked(c['inputs'])
        runner.main_endpoint(Path(oracle['path']),c['final_frame'])
        pins.extend([oracle,c['inputs']]);schedules.append(dict(name=c['name'],final_frame=c['final_frame'],
            target=c['target'],inputs=c['inputs'],oracle=oracle,state='QUEUED'))
    out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    out.mkdir(parents=True)
    report=runner.Report(out,dict(schema='stv.route_gap_renewal.v1',result='RUNNING',gate_proof=False,
        verified_at=base['verified_at'],head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),
        pins=pins,steps=[],cases=schedules,limits=['Complete main streams only; sound recorded without per-case sound oracle.',
        'Stored arcade exit events remain historical evidence; no new native write taps or clearance proof.']))
    try:
        for c in schedules:
            dest=out/c['name'];dest.mkdir();final=c['final_frame'];report.update(c,state='RUNNING')
            command=[exe,rom,dest/'native.bin',final,'--native-hcount','--inputs',c['inputs']['path'],
                '--snd',dest/'native.snd','--coverage',dest/'native.cov','--content',dest/'content.jsonl']
            def verify(text):
                return dict(terminal=routes.terminal(text,final),endpoint=runner.main_endpoint(dest/'native.bin',final),
                    sound=runner.sound_endpoint(dest/'native.snd',final),content=runner.content_samples(dest/'content.jsonl',final),
                    artifacts={n:runner.pin(dest/n) for n in ['native.bin','native.snd','native.cov','content.jsonl']})
            routes.bounded(report,'native-'+c['name'],command,dest,600,verify)
            routes.bounded(report,'diff-'+c['name'],[sys.executable,comparator,'diff',c['oracle']['path'],dest/'native.bin'],
                           dest,90,lambda text:routes.strict_marker(text,final))
            report.update(c,state='PASS')
        runner.check_d10_sources(ROOT,base['verified_at'])
        for pin in pins:gaps.checked(pin)
        report.update(report.data,result='PASS_SIX_COMPLETE_TARGETED_ROUTES_CLEARANCE_OPEN')
        return 3
    except BaseException as error:
        report.cancel_children(str(error));report.update(report.data,result='FAIL',error=str(error));raise
    finally:
        report.update(report.data,finished_at_utc=runner.now())
        print(json.dumps(dict(result=report.data['result'],report=str(report.path),owned_live_pids=report.data['owned_live_pids'])))


if __name__=='__main__':raise SystemExit(main())
