"""Reuse a pinned complete native opening stream for passive damage tracing."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import coverage_gap_manifest as gaps
import rebaseline as runner
from replay_c3_routes import strict_marker

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('source','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();parent=args.source.resolve().parent
    source=json.loads(args.source.read_text(encoding='utf-8'))
    runner.require(source['result']=='STRICT_OPENING_MAIN_MATCH_RAW_LIFE_EVENTS' and not source['owned_live_pids'],'Parent trace incomplete')
    for pin in source['pins']:gaps.checked(pin)
    for pin in source['artifacts'].values():gaps.checked(pin)
    runner.check_d10_sources(ROOT,source['verified_at'])
    processes={e['player']:e['a13_raw'] for e in source['in_game_decreases']}
    runner.require(processes=={1:0x01087e10,2:0x01089490},'Scenario process slots differ')
    native=gaps.checked(source['artifacts']['native.bin']);runner.main_endpoint(native,5000)
    out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    out.mkdir(parents=True)
    for leaf in ('cfg','nvram','diff','snap','inp','sta'):(out/leaf).mkdir()
    observer=ROOT/'tools/lockstep/damage_events.lua'
    command=next(s['command'] for s in source['steps'] if s['name']=='mame')
    command=[str(out/Path(v).relative_to(parent)) if Path(v).is_relative_to(parent) else v for v in command]
    command[command.index('-autoboot_script')+1]=str(observer)
    env=runner.child_environment();env.update(source['mame_environment'])
    env.update(STV_LS_OUT=str(out/'mame.bin'),STV_LIFE_EVENTS_OUT=str(out/'life-events.jsonl'),
        STV_DAMAGE_EVENTS_OUT=str(out/'damage-events.jsonl'),STV_DAMAGE_LIFE_OBSERVER=str(ROOT/'tools/lockstep/life_events.lua'))
    pins=[runner.pin(p) for p in [args.source,native,observer,Path(__file__)]]
    report=dict(schema='stv.opening_damage_trace.v1',result='RUNNING',gate_proof=False,verified_at=source['verified_at'],
        head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),owned_live_pids=[],pins=pins,steps=[])
    def save():runner.atomic_json(out/'report.json',report)
    deadline=time.monotonic()+1200
    def run(name,argv):
        step=dict(name=name,state='RUNNING',command=list(map(str,argv)));report['steps'].append(step);save()
        with (out/(name+'.log')).open('wb') as log:
            child=subprocess.Popen(step['command'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            report['owned_live_pids']=[child.pid];step['pid']=child.pid;save()
            try:
                code=child.wait(timeout=max(1,deadline-time.monotonic()));step['exit_code']=code
                runner.require(code==0,name+' failed');step['state']='PASS'
            finally:
                if child.poll() is None:
                    child.terminate()
                    try:child.wait(timeout=5)
                    except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
                report['owned_live_pids']=[];save()
    save()
    try:
        run('mame',command);runner.main_endpoint(out/'mame.bin',5000)
        run('strict-diff',[sys.executable,ROOT/'tools/lockstep/lsfile.py','diff',native,out/'mame.bin'])
        strict_marker((out/'strict-diff.log').read_text(),5000)
        runner.require((out/'life-events.jsonl').read_bytes()==gaps.checked(source['artifacts']['events.jsonl']).read_bytes(),'Original life events changed')
        rows=[json.loads(line) for line in (out/'damage-events.jsonl').read_text().splitlines()]
        runner.require(rows[0]['schema']=='stv.mame.damage_writes.v1' and rows[-1]==dict(kind='end',frame=5000,events=len(rows)-2),'Incomplete damage events')
        report['events']=rows[1:-1]
        for pin in pins:gaps.checked(pin)
        report.update(result='STRICT_OPENING_MATCH_RAW_DAMAGE_EVENTS',artifacts={p.name:runner.pin(p) for p in out.iterdir() if p.is_file() and p.name!='report.json'})
    except BaseException as error:report.update(result='FAIL',error=str(error));raise
    finally:report['finished_at_utc']=runner.now();save();print(json.dumps(dict(result=report['result'],report=str(out/'report.json'))))


if __name__=='__main__':main()
