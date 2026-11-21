"""Replay sealed legal-input interventions against a fingerprinted candidate."""
import argparse
import json
import os
from pathlib import Path
import shutil
import threading
import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner
from capture_opening_variants import FINAL,VARIANTS
from recover_c3_variants import events
from summarize_opening_pressure import opening_only

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('source','candidate','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();source=json.loads(args.source.read_text(encoding='utf-8'))
    runner.require(source['result']=='COMPLETE_VARIANT_OBSERVATIONS_BALANCE_OPEN' and
        not source['owned_live_pids'],'Source interventions are not complete')
    for pin in source['pins']:gaps.checked(pin)
    expected={(m,v) for m in ('reference','enhanced') for v in VARIANTS}
    runner.require(len(source['cases'])==len(expected) and
        {(c['profile'],c['variant']) for c in source['cases']}==expected,'Source case matrix differs')
    out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh directory')
    out.mkdir(parents=True);exe=out/'candidate.exe';shutil.copy2(args.candidate,exe)
    pins=[runner.pin(p) for p in [args.source,args.candidate,exe,Path(__file__),
        ROOT/'src/gsp/enhanced.cpp',ROOT/'data/enhanced_data_roots.json',ROOT/'tools/lockstep/bot.hpp']]
    old={s['name']:s for s in source['steps']}
    for case in source['cases']:
        runner.require(old[case['name']]['state']=='PASS','Source case failed')
        for pin in old[case['name']]['proof']['artifacts'].values():gaps.checked(pin)
    report=runner.Report(out,dict(schema='stv.candidate_variants.v1',result='RUNNING',gate_proof=False,
        base_commit=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),pins=pins,steps=[],
        cases=source['cases'],limits=['Candidate preparation only; no D10 or all-room balance proof.',
            'Matched bot takeover differs from full-from-boot behavior; retain both experiments.']))
    timer=threading.Timer(1800,report.cancel_children,args=('Candidate intervention deadline',))
    timer.daemon=True;timer.start()
    try:
        def capture(job):
            case=job[1];name=case['name'];dest=out/name;dest.mkdir()
            prefix=gaps.checked(case['prefix']);end=case['bot_after']
            cmd=[exe,case['command'][1],dest/'native.bin',FINAL,'--native-hcount','--inputs',prefix,
                '--bot',dest/'recorded.inputs','--bot2','--bot-seed',1,'--bot-after',end,
                '--snd',dest/'native.snd','--coverage',dest/'native.cov','--content',dest/'content.jsonl']
            if case['profile']=='enhanced':cmd.append('--enhanced')
            def verify(_):
                recorded=events((dest/'recorded.inputs').read_bytes(),FINAL)
                runner.require([r[:4] for r in recorded if r[0]<end]==
                    [r[:4] for r in events(prefix.read_bytes())],'Intervention replay differs')
                run=pressure.read_run(dest/(name+'.log'),2)
                runner.require(run['terminal']['frames']==FINAL and not run['terminal']['big_field'],'Endpoint differs')
                if case['profile']=='reference':
                    for artifact in ('native.bin','native.snd','content.jsonl','recorded.inputs'):
                        runner.require(runner.sha(dest/artifact)==old[name]['proof']['artifacts'][artifact]['sha256'],
                            'Reference intervention changed: '+artifact)
                return dict(endpoint=runner.main_endpoint(dest/'native.bin',FINAL),
                    sound=runner.sound_endpoint(dest/'native.snd',FINAL),opening=opening_only(run),
                    artifacts={p.name:runner.pin(p) for p in dest.iterdir() if p.is_file()})
            report.run(name,cmd,dest,verify)
        runner.run_main_group(report,[(c['name'],c) for c in source['cases']],1,capture)
        steps={s['name']:s for s in report.data['steps']}
        comparisons={v:pressure.compare(steps['reference-'+v]['proof']['opening'],
            steps['enhanced-'+v]['proof']['opening'],players=2,minimum_frames=1000,
            lower=.67,upper=1.5,required_rooms={(0,1)}) for v in VARIANTS}
        for pin in pins:gaps.checked(pin)
        report.update(report.data,result='COMPLETE_CANDIDATE_VARIANTS_BALANCE_OPEN',comparisons=comparisons)
        print(json.dumps({v:c['result'] for v,c in comparisons.items()}));return 0
    except BaseException as error:
        report.cancel_children(str(error));report.update(report.data,result='FAIL',error=str(error));raise
    finally:
        timer.cancel();report.cancel_children('Candidate interventions finished')


if __name__=='__main__':raise SystemExit(main())
