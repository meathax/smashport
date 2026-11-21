"""Diagnostic old/new opening-cap comparison; never a D10 or balance gate proof."""
import argparse
import json
import os
from pathlib import Path
import shutil
import threading
import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner
from summarize_opening_pressure import opening_only

ROOT=Path(__file__).resolve().parents[2]
FINAL=10000


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','candidate','out'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build directory')
    out.mkdir(parents=True)
    exes={}
    for name in ('baseline','candidate'):
        exes[name]=out/(name+'.exe');shutil.copy2(getattr(args,name),exes[name])
    rom=ROOT/'original/user-supplied-roms/smashtv.zip'
    sources=[ROOT/p for p in ('src/gsp/enhanced.cpp','data/enhanced_data_roots.json',
        'tools/gsp2cpp/emit.py','tools/lockstep/bot.hpp','tools/lockstep/content_observer.hpp')]
    pins=[runner.pin(p) for p in sources+[rom,Path(__file__),Path(pressure.__file__),*exes.values()]]
    jobs=[(revision,mode,players) for players in (1,2) for mode in ('reference','enhanced')
          for revision in ('baseline','candidate')]
    report=runner.Report(out,dict(schema='stv.cap_candidate.v1',result='RUNNING',
        base_commit=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),pins=pins,
        jobs=jobs,steps=[],gate_proof=False,
        limits=['Uncommitted candidate with source fingerprints; D10 renewal remains open.',
                'One deterministic full-from-boot trajectory per profile/player count; no statistical independence.',
                'Wave transition statistics do not prove clearance.']))
    timer=threading.Timer(1800,report.cancel_children,args=('Candidate diagnostic deadline exceeded',))
    timer.daemon=True;timer.start()
    try:
        def capture(job):
            revision,mode,players=job;name=f'{revision}-{mode}-{players}p';dest=out/name;dest.mkdir()
            command=[exes[revision],rom,dest/'native.bin',FINAL,'--native-hcount',
                '--bot',dest/'recorded.inputs','--bot-seed',3,'--snd',dest/'native.snd',
                '--content',dest/'content.jsonl']
            if mode=='enhanced':command.append('--enhanced')
            if players==2:command.append('--bot2')
            def verify(_):
                run=pressure.read_run(dest/(name+'.log'),players)
                # Existing retail unmapped-access observations are retained in
                # the log/terminal, as in the established pressure recorder.
                # They are not a CPU stop or a zero-fault qualification claim.
                runner.require(run['terminal']['frames']==FINAL and
                    not run['terminal']['big_field'],'Invalid capture endpoint')
                return dict(endpoint=runner.main_endpoint(dest/'native.bin',FINAL),
                    sound=runner.sound_endpoint(dest/'native.snd',FINAL),
                    opening=opening_only(run),artifacts={p.name:runner.pin(p) for p in dest.iterdir() if p.is_file()})
            report.run(name,command,dest,verify)
        runner.run_main_group(report,jobs,1,capture)
        for pin in pins:gaps.checked(pin)
        steps={s['name']:s for s in report.data['steps']}
        reference_identity={}
        comparisons={}
        for players in (1,2):
            prefix=lambda revision,mode:f'{revision}-{mode}-{players}p'
            reference_identity[str(players)]={name:
                steps[prefix('baseline','reference')]['proof']['artifacts'][name]['sha256']==
                steps[prefix('candidate','reference')]['proof']['artifacts'][name]['sha256']
                for name in ('native.bin','native.snd','recorded.inputs','content.jsonl')}
            runner.require(all(reference_identity[str(players)].values()),'Reference behavior changed')
            comparisons[str(players)]={revision:pressure.compare(
                steps[prefix(revision,'reference')]['proof']['opening'],
                steps[prefix(revision,'enhanced')]['proof']['opening'],players=players,
                minimum_frames=1000,lower=.67,upper=1.5,required_rooms={(0,1)})
                for revision in ('baseline','candidate')}
        report.update(report.data,result='COMPLETE_DIAGNOSTIC_BALANCE_OPEN',
            reference_identity=reference_identity,comparisons=comparisons)
        print(json.dumps(dict(result=report.data['result'],reference_identity=reference_identity,
            comparison_results={p:{r:v['result'] for r,v in rows.items()} for p,rows in comparisons.items()})))
        return 0
    except Exception as exc:
        report.update(report.data,result='FAILED_DIAGNOSTIC',error=str(exc))
        raise
    finally:
        timer.cancel();report.cancel_children('Candidate diagnostic finished')


if __name__=='__main__':raise SystemExit(main())
