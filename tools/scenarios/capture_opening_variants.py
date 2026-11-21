"""Vary legal opening stick inputs, retaining coin/start and native runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import threading
import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner
from prepare_pressure_gaps import stable_handoffs
from recover_c3_variants import events,prefix_bytes

ROOT=Path(__file__).resolve().parents[2]
VARIANTS={'control':None,'vertical-out':(0x11,0x22),'vertical-in':(0x22,0x11),
          'horizontal-out':(0x44,0x88),'horizontal-in':(0x88,0x44)}
FINAL=10000


def varied_prefix(raw,start,end,variant):
    runner.require(type(start) is int and type(end) is int and 0<start<end and end-start==60,'Invalid intervention window')
    runner.require(variant in VARIANTS,'Unknown stick intervention')
    if variant=='control':return prefix_bytes(raw,end)
    selected=[r[:4] for r in events(raw) if r[0]<end and (r[0]<start or r[1]==':IN1')]
    p1,p2=VARIANTS[variant];mask=p1|(p2<<8)
    selected.extend((start,':IN0',1<<bit,int(bool(mask&(1<<bit)))) for bit in range(16))
    selected.sort(key=lambda r:r[0])
    return ''.join(f'{f} {port} {bits:x} {value}\n' for f,port,bits,value in selected).encode('ascii')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('source','out'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--prepare-only',action='store_true');args=parser.parse_args()
    source=json.loads(args.source.read_text(encoding='utf-8'))
    runner.require(source['result']=='COMPLETE_OPENING_OBSERVATIONS_BALANCE_OPEN' and not source['owned_live_pids'],
        'Source capture is not terminal')
    for pin in source['pins']:gaps.checked(pin)
    runner.check_d10_sources(ROOT,source['verified_at'])
    out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build directory')
    out.mkdir(parents=True);steps={s['name']:s for s in source['steps']};cases=[]
    pins=[runner.pin(args.source),runner.pin(__file__),runner.pin(pressure.__file__),runner.pin(ROOT/'tools/scenarios/recover_c3_variants.py'),runner.pin(ROOT/'tools/scenarios/prepare_pressure_gaps.py')]
    original_exe=Path(steps['reference-2p-seed3']['command'][0]);frozen=out/original_exe.name;shutil.copy2(original_exe,frozen)
    pins.extend([runner.pin(original_exe),runner.pin(frozen)])
    for mode in ('reference','enhanced'):
        step=steps[mode+'-2p-seed3'];runner.require(step['state']=='PASS' and step['exit_code']==0,'Source failed')
        content=gaps.checked(step['proof']['artifacts']['content.jsonl']);inputs=gaps.checked(step['proof']['artifacts']['recorded.inputs'])
        start=stable_handoffs(gaps.samples(content),{(0,1)}).get((0,1))
        runner.require(start is not None and start+60<3000,'Missing early opening window')
        end=start+60;raw=inputs.read_bytes();unique=set();pins.extend([runner.pin(content),runner.pin(inputs)])
        for variant in VARIANTS:
            name=mode+'-'+variant;dest=out/name;dest.mkdir();prefix=dest/'prefix.inputs'
            prefix.write_bytes(varied_prefix(raw,start,end,variant));digest=runner.sha(prefix)
            runner.require(digest not in unique,'Intervention did not vary input prefix');unique.add(digest);pins.append(runner.pin(prefix))
            command=[str(frozen),step['command'][1],str(dest/'native.bin'),str(FINAL),'--native-hcount',
                '--inputs',str(prefix),'--bot',str(dest/'recorded.inputs'),'--bot2','--bot-seed','1','--bot-after',str(end),
                '--snd',str(dest/'native.snd'),'--coverage',str(dest/'native.cov'),'--content',str(dest/'content.jsonl')]
            if mode=='enhanced':command.append('--enhanced')
            cases.append(dict(name=name,profile=mode,variant=variant,start=start,bot_after=end,command=command,prefix=runner.pin(prefix)))
    report=runner.Report(out,dict(schema='stv.opening_variants.v1',result='PREPARED_NOT_RUN',gate_proof=False,
        verified_at=source['verified_at'],head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),
        pins=pins,cases=cases,steps=[],limits=['Only60 frames of legal movement/fire changed; original coin/start events retained.',
        'Bot measurements begin after takeover; compare against same-takeover control, not full initial visits.',
        'Input diversity does not by itself prove changed combat state or independent statistical samples.']))
    if args.prepare_only:return 0
    report.update(report.data,result='RUNNING');timer=threading.Timer(1800,report.cancel_children,args=('30-minute variant deadline exceeded',))
    timer.daemon=True;timer.start()
    try:
        def capture(job):
            case=job[1];name=case['name'];dest=out/name
            def verify(_):
                recorded=events((dest/'recorded.inputs').read_bytes(),FINAL)
                expected=events(gaps.checked(case['prefix']).read_bytes())
                runner.require([r[:4] for r in recorded if r[0]<case['bot_after']]==[r[:4] for r in expected],'Input intervention was not replayed exactly')
                run=pressure.read_run(dest/(name+'.log'),2)
                runner.require(run['terminal']['frames']==FINAL and not run['terminal']['big_field'],'Wrong endpoint')
                visits=[v for v in run['observations'] if v['circuit']==0 and v['wave']==1 and v['frames']>=1000]
                handoff=next((s for s in gaps.samples(dest/'content.jsonl') if s['frame']==case['bot_after']),None)
                runner.require(handoff is not None,'Missing handoff observation')
                return dict(endpoint=runner.main_endpoint(dest/'native.bin',FINAL),sound=runner.sound_endpoint(dest/'native.snd',FINAL),
                    content=runner.content_samples(dest/'content.jsonl',FINAL),opening_visits=visits,handoff_sample=handoff,
                    artifacts={p.name:runner.pin(p) for p in dest.iterdir() if p.is_file()})
            report.run(name,case['command'],dest,verify)
        runner.run_main_group(report,[(c['name'],c) for c in cases],1,capture)
        for pin in pins:gaps.checked(pin)
        runner.check_d10_sources(ROOT,source['verified_at'])
        report.update(report.data,result='COMPLETE_VARIANT_OBSERVATIONS_BALANCE_OPEN');return 0
    except BaseException as error:
        report.cancel_children(str(error));report.update(report.data,result='FAIL',error=str(error));raise
    finally:
        timer.cancel();report.update(report.data,finished_at_utc=runner.now())
        print(json.dumps(dict(result=report.data['result'],report=str(report.path),owned_live_pids=report.data['owned_live_pids'])))


if __name__=='__main__':raise SystemExit(main())
