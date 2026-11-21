"""Trace opening life writes in MAME with a strict complete native comparison."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import coverage_gap_manifest as gaps
import rebaseline as runner
from recover_c3_variants import prefix_bytes
from replay_c3_routes import strict_marker
from verify_route_gap_oracles import MAME,MAME_SHA

ROOT=Path(__file__).resolve().parents[2]
FINAL=5000


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('source','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();source=json.loads(args.source.read_text(encoding='utf-8'))
    runner.require(source['result']=='COMPLETE_OPENING_OBSERVATIONS_BALANCE_OPEN' and not source['owned_live_pids'],'Source incomplete')
    for pin in source['pins']:gaps.checked(pin)
    runner.require(runner.sha(MAME)==MAME_SHA,'Wrong MAME executable')
    runner.check_d10_sources(ROOT,source['verified_at'])
    step=next(s for s in source['steps'] if s['name']=='reference-2p-seed3')
    runner.require(step['state']=='PASS' and step['exit_code']==0,'Source case failed')
    original=gaps.checked(step['proof']['artifacts']['recorded.inputs'])
    out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    out.mkdir(parents=True);inputs=out/'prefix.inputs';inputs.write_bytes(prefix_bytes(original.read_bytes(),FINAL+1))
    observer=ROOT/'tools/lockstep/life_events.lua';base=ROOT/'tools/lockstep/record.lua'
    pins=[runner.pin(p) for p in [args.source,original,inputs,Path(step['command'][0]),Path(step['command'][1]),MAME,observer,base,Path(__file__)]]
    report=dict(schema='stv.opening_life_trace.v1',result='RUNNING',gate_proof=False,verified_at=source['verified_at'],
        head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),owned_live_pids=[],pins=pins,steps=[])
    def save():runner.atomic_json(out/'report.json',report)
    deadline=time.monotonic()+1200
    def run(name,command,env):
        entry=dict(name=name,state='RUNNING',command=list(map(str,command)));report['steps'].append(entry);save()
        with (out/(name+'.log')).open('wb') as log:
            child=subprocess.Popen(entry['command'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            report['owned_live_pids']=[child.pid];entry['pid']=child.pid;save()
            try:
                entry['exit_code']=child.wait(timeout=max(1,deadline-time.monotonic()))
                runner.require(entry['exit_code']==0,name+' failed')
                entry['state']='PASS'
            finally:
                if child.poll() is None:
                    child.terminate()
                    try:child.wait(timeout=5)
                    except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
                report['owned_live_pids']=[];save()
    save()
    try:
        env=runner.child_environment()
        run('native',[step['command'][0],step['command'][1],out/'native.bin',FINAL,'--native-hcount','--inputs',inputs,'--snd',out/'native.snd'],env)
        runner.main_endpoint(out/'native.bin',FINAL)
        command=[MAME,'smashtv','-rompath',ROOT/'original/user-supplied-roms']
        for flag,leaf in [('cfg','cfg'),('nvram','nvram'),('diff','diff'),('snapshot','snap'),('input','inp'),('state','sta')]:
            (out/leaf).mkdir();command.extend([f'-{flag}_directory',out/leaf])
        command.extend(['-noreadconfig','-skip_gameinfo','-video','none','-sound','none','-nothrottle','-seconds_to_run','700','-autoboot_script',observer])
        env.update(STV_LS_OUT=str(out/'mame.bin'),STV_LS_FRAMES=str(FINAL),STV_LS_INPUTS=str(inputs),
            STV_LIFE_EVENTS_OUT=str(out/'events.jsonl'),STV_LIFE_BASE_RECORDER=str(base))
        report['mame_environment']={k:v for k,v in env.items() if k.startswith('STV_')};save()
        run('mame',command,env);runner.main_endpoint(out/'mame.bin',FINAL)
        runner.require(f'STV_LS_DONE frames={FINAL} ' in (out/'mame.log').read_text(),'MAME terminal marker missing')
        run('strict-diff',[sys.executable,ROOT/'tools/lockstep/lsfile.py','diff',out/'native.bin',out/'mame.bin'],env)
        strict_marker((out/'strict-diff.log').read_text(),FINAL)
        rows=[json.loads(s) for s in (out/'events.jsonl').read_text().splitlines()]
        runner.require(rows[0]['schema']=='stv.mame.life_writes.v1' and rows[-1]==dict(kind='end',frame=FINAL,events=len(rows)-2),'Incomplete event stream')
        report['in_game_decreases']=[r for r in rows[1:-1] if r['gamstate']==16 and r['merged']<r['old']]
        for pin in pins:gaps.checked(pin)
        report.update(result='STRICT_OPENING_MAIN_MATCH_RAW_LIFE_EVENTS',artifacts={p.name:runner.pin(p) for p in out.iterdir() if p.is_file() and p.name!='report.json'})
    except BaseException as error:report.update(result='FAIL',error=str(error));raise
    finally:
        report['finished_at_utc']=runner.now();save();print(json.dumps(dict(result=report['result'],report=str(out/'report.json'))))


if __name__=='__main__':main()
