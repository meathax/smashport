"""Fixed four-seed opening-room balance experiment; unchanged native runtime."""
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

ROOT=Path(__file__).resolve().parents[2]
SEEDS=(3,4,5,6)
FRAMES=30000


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('exe','reference-report','out'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--prepare-only',action='store_true')
    args=parser.parse_args()
    baseline=json.loads(args.reference_report.read_text(encoding='utf-8'))
    reference_executable(args.exe.resolve(),baseline)
    gaps.complete_steps(baseline)
    runner.check_d10_sources(ROOT,baseline['verified_at'])
    out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build directory')
    out.mkdir(parents=True)
    frozen=out/'stv_l8_lockstep-frozen.exe';shutil.copy2(args.exe,frozen)
    rom=ROOT/'original/user-supplied-roms/smashtv.zip'
    dependencies=[rom,ROOT/'tools/lockstep/bot.hpp',ROOT/'tools/lockstep/content_observer.hpp']
    for path in dependencies:
        runner.require(baseline['pins'].get(str(path))==runner.pin(path),'Reference input identity differs')
    pins=[runner.pin(p) for p in dependencies+[args.reference_report,args.exe,frozen,Path(__file__),
        Path(pressure.__file__),Path(runner.__file__)]]
    jobs=[(f'{mode}-2p-seed{seed}',mode,seed) for seed in SEEDS for mode in ('reference','enhanced')]
    report=runner.Report(out,dict(schema='stv.opening_pressure_capture.v1',result='PREPARED_NOT_RUN',
        verified_at=baseline['verified_at'],head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),
        owner_pid=os.getpid(),pins=pins,jobs=jobs,steps=[],gate_proof=False,
        experiment=dict(seeds=list(SEEDS),players=2,final_frame=FRAMES,primary_room=[0,1],
            policy='Retain all four seeds irrespective of result; no early stopping or seed selection.',
            scope='Opening-room repeatability; no all-room balance or clearance claim.')))
    if args.prepare_only:return 0
    report.update(report.data,result='RUNNING')
    timer=threading.Timer(3600,report.cancel_children,args=('One-hour opening experiment deadline exceeded',))
    timer.daemon=True;timer.start()
    try:
        def capture(job):
            name,mode,seed=job;dest=out/name;dest.mkdir()
            command=[frozen,rom,dest/'native.bin',FRAMES,'--native-hcount','--bot',dest/'recorded.inputs',
                '--bot2','--bot-seed',seed,'--snd',dest/'native.snd','--coverage',dest/'native.cov',
                '--content',dest/'content.jsonl']
            if mode=='enhanced':command.append('--enhanced')
            def verify(_):
                parsed=pressure.read_run(dest/(name+'.log'),2)
                runner.require(parsed['terminal']['frames']==FRAMES and not parsed['terminal']['big_field'],'Invalid endpoint')
                return dict(endpoint=runner.main_endpoint(dest/'native.bin',FRAMES),
                    sound=runner.sound_endpoint(dest/'native.snd',FRAMES),
                    content=runner.content_samples(dest/'content.jsonl',FRAMES),
                    artifacts={p.name:runner.pin(p) for p in dest.iterdir() if p.is_file()})
            report.run(name,command,dest,verify)
        runner.run_main_group(report,jobs,1,capture)
        for pin in pins:gaps.checked(pin)
        runner.check_d10_sources(ROOT,baseline['verified_at'])
        report.update(report.data,result='COMPLETE_OPENING_OBSERVATIONS_BALANCE_OPEN',finished_at_utc=runner.now())
        return 0
    except BaseException as error:
        report.cancel_children(str(error) or type(error).__name__)
        report.update(report.data,result='FAIL',error=str(error),finished_at_utc=runner.now())
        raise
    finally:
        timer.cancel()
        print(json.dumps(dict(result=report.data['result'],report=str(report.path),owned_live_pids=report.data['owned_live_pids'])))


if __name__=='__main__':raise SystemExit(main())
