"""Record bounded full-RAM reference windows for sampled player ownership."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import threading
import coverage_gap_manifest as gaps
import player_owner_audit as owner
import rebaseline as runner

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-report',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--frames',type=int,default=100000)
    args=parser.parse_args();baseline=json.loads(args.baseline_report.read_text(encoding='utf-8'))
    steps=gaps.complete_steps(baseline)
    runner.require(0<args.frames<=440000,'Invalid bounded owner window')
    runner.check_d10_sources(ROOT,baseline['verified_at'])
    owner.contract(baseline)
    out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    out.mkdir(parents=True)
    pins={str(p.resolve()):runner.pin(p) for p in [args.baseline_report,Path(__file__),Path(owner.__file__)]}
    commands=[]
    for name in ('1p','2p'):
        command=list(steps[name+'-native']['command']);directory=out/name;directory.mkdir()
        for path in (command[0],command[1],command[command.index('--inputs')+1]):
            actual=runner.pin(path);runner.require(baseline['pins'][str(Path(path).resolve())]==actual,'Source capture identity changed')
            pins[str(Path(path).resolve())]=actual
        command[2]=str(directory/'native-full.bin');command[3]=str(args.frames)
        for flag,leaf in [('--snd','native.snd'),('--coverage','native.cov'),('--content','content.jsonl')]:
            command[command.index(flag)+1]=str(directory/leaf)
        owner.capture_command(command,steps[name+'-native']['command'])
        commands.append((name,directory,command))
    report=runner.Report(out,dict(schema='stv.fullram.owner.capture.v1',result='RUNNING',owner_pid=os.getpid(),
        native_protected_source=baseline['verified_at'],head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),
        capture_driver=runner.pin(__file__),pins=pins,steps=[],gate_proof=False))
    timer=threading.Timer(3600,report.cancel_children,args=('Owner capture one-hour deadline exceeded',));timer.daemon=True;timer.start()
    env={k:v for k,v in os.environ.items() if not k.upper().startswith('STV_')}
    env.update(STV_LS_SUMMARY='1',STV_LS_DUMP_EVERY='60')
    try:
        for name,directory,command in commands:
            log=directory/'capture.log'
            step=dict(name=name+'-native-fullram',state='RUNNING',command=command,
                environment={k:v for k,v in env.items() if k.startswith('STV_')},log=str(log))
            report.data['steps'].append(step);report.save();child=None
            try:
                with log.open('wb') as stream:
                    with report.lock:
                        runner.require(not report.cancelled.is_set(),'Capture cancelled')
                        child=subprocess.Popen(command,cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,
                                               creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                        report.children[child.pid]=(child,step);report.update(step,pid=child.pid)
                    code=child.wait(timeout=1800)
                    report.update(step,exit_code=code)
                runner.require(code==0 and not report.cancelled.is_set(),'Native owner capture failed/cancelled')
                observed=owner.convert_and_observe(directory/'native-full.bin',directory/'content.jsonl',
                                                   directory/'validation-compact.bin',args.frames)
                report.update(step,log_sha256=runner.sha(log),proof=dict(
                    endpoint=runner.main_endpoint(directory/'validation-compact.bin',args.frames),
                    unverified_owner_observations=observed,
                    sound=runner.sound_endpoint(directory/'native.snd',args.frames),
                    content_samples=runner.content_samples(directory/'content.jsonl',args.frames),
                    artifacts={p.name:runner.pin(p) for p in directory.iterdir() if p.is_file()}),state='PASS')
            finally:
                if child is not None:
                    if child.poll() is None:
                        child.terminate()
                        try:child.wait(timeout=5)
                        except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
                    with report.lock:report.children.pop(child.pid,None);report.save()
        for pin in pins.values():gaps.checked(pin)
        runner.check_d10_sources(ROOT,baseline['verified_at'])
        report.update(report.data,result='CAPTURES_COMPLETE_IDENTITY_PENDING')
    except BaseException as error:
        report.cancel_children(str(error));report.update(report.data,result='FAIL',error=str(error));raise
    finally:
        timer.cancel();report.update(report.data,finished_at_utc=runner.now())
        print(json.dumps(dict(result=report.data['result'],report=str(report.path),owned_live_pids=report.data['owned_live_pids'])))


if __name__=='__main__':main()
