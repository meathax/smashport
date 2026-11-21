"""Capture full native/arcade RAM at every frame with passive arcade exit lists."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import rebaseline as r
import coverage_gap_manifest as gaps
from replay_c3_routes import reference_executable, strict_marker
from validated_exit_replay import validate as prepared_replay

ROOT=Path(__file__).resolve().parents[2]
MAME=Path('D:/Arcade/AI/mameexe/mame.exe')


def bounded_final(source_final, requested):
    if requested is None:return source_final
    r.require(type(requested) is int and 0<requested<=source_final,
              'Bounded capture endpoint must be within the qualified recording')
    return requested


def targeted_replay(report, name, players, verified_at, exe):
    """Select a sealed reference-profile recording, never the bot's input prefix."""
    r.require(report.get('result')=='COMPLETE_TARGETED_PRESSURE_OBSERVATIONS_GATE_OPEN'
        and report.get('owned_live_pids')==[] and report.get('verified_at')==verified_at,
        'Target recording is not terminal/current')
    cases=[c for c in report['cases'] if c['name']==name]
    steps=[s for s in report['steps'] if s['name']==name]
    r.require(len(cases)==len(steps)==1,'Target recording is missing or duplicated')
    case,step=cases[0],steps[0]
    r.require(case['profile']=='reference' and case['players']==players,
        'Target must use the requested reference player count')
    r.require(step['state']=='PASS' and step['exit_code']==0,'Target capture failed')
    final=case['final_frame']; endpoint=step['proof']['endpoint']
    r.require(type(final) is int and final>0 and endpoint['first_frame']==0
        and endpoint['last_frame']==final and endpoint['frames']==final+1,
        'Target endpoint is incomplete')
    command=step['command']
    r.require('--bot' in command and '--enhanced' not in command
        and int(command[3])==final,'Unexpected target recording command')
    source_exe=Path(command[0])
    executable_pins=[p for p in report['pins'] if Path(p['path'])==source_exe]
    r.require(len(executable_pins)==1,'Target executable is not uniquely pinned')
    gaps.checked(executable_pins[0])
    r.require(r.sha(source_exe)==r.sha(exe),'Target executable differs from current reference')
    pin=step['proof']['artifacts']['recorded.inputs']
    inputs=gaps.checked(pin)
    r.require(inputs==Path(command[command.index('--bot')+1]),
        'Sealed replay does not match recorded bot output')
    return inputs,final


def register_child(report, child, row):
    # Report.save derives live PIDs from this registry, not caller-supplied data.
    with report.lock:
        report.children[child.pid]=(child,row)
        report.update(row,pid=child.pid)


def full_endpoint(path, final):
    total=0
    initialized={1:set(),2:set(),3:set()}
    for frame, records in r.lsfile.frames(path):
        r.require(frame==total and frame<=final,'Invalid full-stream frame sequence')
        r.require(sum(k==9 and len(p)==8 for k,f,p in records)==1,'Missing/duplicate full-stream summary')
        for kind,f,payload in records:
            r.require(f==frame,'Record frame mismatch')
            if kind in initialized:
                r.require(len(payload)==258,'Expected full region chunk')
                index=int.from_bytes(payload[:2],'little')
                r.require(index<r.lsfile.SIZES[kind]//256,'Region chunk out of range')
                initialized[kind].add(index)
            else:r.require(kind==9 and len(payload)==8,'Unexpected full-stream record')
        r.require(all(len(initialized[k])==r.lsfile.SIZES[k]//256 for k in initialized),'Incomplete initial regions')
        total+=1
    r.require(total==final+1,'Incomplete full-stream endpoint')
    return dict(frames=total,last_frame=final,full_regions=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('exe','reference-report','out'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--case',choices=('1p','2p'),required=True)
    selection=p.add_mutually_exclusive_group()
    selection.add_argument('--target-report',type=Path)
    selection.add_argument('--prepared-plan',type=Path)
    p.add_argument('--target-case')
    observations=p.add_mutually_exclusive_group()
    observations.add_argument('--shoe-events',action='store_true')
    observations.add_argument('--pickup-events',action='store_true')
    observations.add_argument('--collision-events',action='store_true')
    p.add_argument('--final-frame',type=int)
    args=p.parse_args();base=json.loads(args.reference_report.read_text())
    reference_executable(args.exe.resolve(),base);gaps.complete_steps(base)
    r.check_d10_sources(ROOT,base['verified_at'])
    r.require(r.sha(MAME)=='af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3','Wrong arcade executable')
    step=next(s for s in base['steps'] if s['name']==args.case+'-native')
    command=step['command'];inputs=Path(command[command.index('--inputs')+1]);final=step['proof']['endpoint']['last_frame']
    r.require(bool(args.target_report)==bool(args.target_case),'Supply both target report and case')
    if args.target_report:
        inputs,final=targeted_replay(json.loads(args.target_report.read_text()),
            args.target_case,int(args.case[0]),base['verified_at'],args.exe.resolve())
    plan=None
    if args.prepared_plan:
        plan=json.loads(args.prepared_plan.read_text())
        inputs,final=prepared_replay(plan,int(args.case[0]),base['verified_at'],args.exe.resolve())
    source_final=final;final=bounded_final(source_final,args.final_frame)
    out=args.out.resolve();r.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    out.mkdir();frozen=out/'stv_l8_lockstep-frozen.exe';shutil.copyfile(args.exe,frozen)
    rom=ROOT/'original/user-supplied-roms/smashtv.zip'
    pins=[r.pin(x) for x in [frozen,inputs,rom,MAME,args.reference_report,Path(__file__),
        ROOT/'tools/lockstep/clearance_events.lua',ROOT/'tools/lockstep/route_events.lua',
        ROOT/'tools/lockstep/record.lua',ROOT/'tools/lockstep/lsfile.py']]
    if args.target_report:pins.append(r.pin(args.target_report))
    if args.shoe_events:pins.append(r.pin(ROOT/'tools/lockstep/shoe_events.lua'))
    if args.pickup_events:pins.append(r.pin(ROOT/'tools/lockstep/pickup_events.lua'))
    if args.collision_events:pins.append(r.pin(ROOT/'tools/lockstep/collision_events.lua'))
    if plan is not None:
        pins.extend(plan['pins'])
        pins.extend(r.pin(x) for x in [args.prepared_plan,
            ROOT/'tools/scenarios/validated_exit_replay.py',
            ROOT/'tools/scenarios/prepare_recorded_exit_replay.py'])
    report=r.Report(out,dict(result='RUNNING',gate_proof=False,verified_at=base['verified_at'],
        head_at_launch=r.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),steps=[],pins=pins,
        case=args.case,target_case=args.target_case,
        prepared_plan=str(args.prepared_plan.resolve()) if args.prepared_plan else None,
        final_frame=final,source_final_frame=source_final,dump_every=1,compact=False,
        limits=['Every-frame RAM and exit observations do not by themselves prove all-room clears, player ownership or endings.']))
    env={k:v for k,v in os.environ.items() if not k.upper().startswith('STV_')}
    env.update(STV_LS_SUMMARY='1',STV_LS_DUMP_EVERY='1',STV_LS_FRAMES=str(final),
        STV_LS_INPUTS=str(inputs),STV_LS_OUT=str(out/'mame.bin'),
        STV_CLEARANCE_EVENTS_OUT=str(out/'objects.jsonl'),STV_ROUTE_EVENTS_OUT=str(out/'events.jsonl'),
        STV_ROUTE_EVENTS_OBSERVER=str(ROOT/'tools/lockstep/route_events.lua'),
        STV_ROUTE_BASE_RECORDER=str(ROOT/'tools/lockstep/record.lua'))
    if args.shoe_events:
        env.update(STV_SHOE_EVENTS_OUT=str(out/'shoe-events.jsonl'),
            STV_SHOE_BASE_OBSERVER=str(ROOT/'tools/lockstep/clearance_events.lua'))
    if args.pickup_events:
        env.update(STV_PICKUP_EVENTS_OUT=str(out/'pickup-events.jsonl'),
            STV_PICKUP_BASE_OBSERVER=str(ROOT/'tools/lockstep/clearance_events.lua'))
    if args.collision_events:
        env.update(STV_COLLISION_EVENTS_OUT=str(out/'collision-events.jsonl'),
            STV_COLLISION_BASE_OBSERVER=str(ROOT/'tools/lockstep/clearance_events.lua'))
    report.update(report.data,environment={k:v for k,v in env.items() if k.startswith('STV_')})
    deadline=time.monotonic()+21600
    def run(name,command):
        row=dict(name=name,state='RUNNING',command=list(map(str,command)));report.data['steps'].append(row)
        with (out/(name+'.log')).open('wb') as log:
            child=subprocess.Popen(row['command'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            register_child(report,child,row)
            try:
                remaining=min(10800,deadline-time.monotonic());r.require(remaining>0,'Capture deadline exceeded')
                code=child.wait(timeout=remaining)
                row['exit_code']=code
                r.require(code==0,'Failed '+name)
                row['state']='PASS'
            except BaseException as error:
                row.update(state='FAIL',error=str(error));raise
            finally:
                if child.poll() is None:child.kill();child.wait(timeout=10)
                with report.lock:
                    report.children.pop(child.pid,None)
                    report.save()
    try:
        run('native',[frozen,rom,out/'native.bin',final,'--native-hcount','--inputs',inputs,'--snd',out/'native.snd'])
        cmd=[MAME,'smashtv','-rompath',rom.parent]
        for flag in ('cfg','nvram','diff','snapshot','input','state'):
            directory=out/flag;directory.mkdir();cmd.extend(['-'+flag+'_directory',directory])
        cmd.extend(['-noreadconfig','-skip_gameinfo','-video','none','-sound','none','-nothrottle',
            '-seconds_to_run',str(final//50+600),'-autoboot_script',
            ROOT/'tools/lockstep'/('shoe_events.lua' if args.shoe_events else 'pickup_events.lua' if args.pickup_events else 'collision_events.lua' if args.collision_events else 'clearance_events.lua')])
        run('mame',cmd)
        run('strict',[sys.executable,ROOT/'tools/lockstep/lsfile.py','diff',out/'native.bin',out/'mame.bin'])
        strict_marker((out/'strict.log').read_text(),final)
        report.data['endpoints']={n:full_endpoint(out/n,final) for n in ('native.bin','mame.bin')}
        events=[json.loads(s) for s in (out/'objects.jsonl').read_text().splitlines()]
        r.require(events[-1]==dict(kind='end',frame=final,events=len(events)-2),'Incomplete exit sidecar')
        report.data['exit_events']=len(events)-2
        extra=[]
        if args.shoe_events:
            rows=[json.loads(s) for s in (out/'shoe-events.jsonl').read_text().splitlines()]
            r.require(rows[0]['schema']=='stv.mame.shoe_assignments.v1' and
                rows[-1]==dict(kind='end',frame=final,events=len(rows)-2),'Incomplete shoe event sidecar')
            report.data['shoe_events']=len(rows)-2;extra=['shoe-events.jsonl']
        if args.pickup_events:
            rows=[json.loads(s) for s in (out/'pickup-events.jsonl').read_text().splitlines()]
            r.require(rows[0]['schema']=='stv.mame.pickup_handlers.v1' and
                rows[-1]==dict(kind='end',frame=final,events=len(rows)-2),'Incomplete pickup event sidecar')
            report.data['pickup_events']=len(rows)-2;extra=['pickup-events.jsonl']
        if args.collision_events:
            rows=[json.loads(s) for s in (out/'collision-events.jsonl').read_text().splitlines()]
            r.require(rows[0]['schema']=='stv.mame.collision_dispatch.v1' and
                rows[-1]==dict(kind='end',frame=final,events=len(rows)-2,pairs=(len(rows)-2)//2,overlaps=0,unpaired=0),
                'Incomplete/overlapping collision sidecar')
            report.data['collision_pairs']=(len(rows)-2)//2;extra=['collision-events.jsonl']
        for pin in pins:gaps.checked(pin)
        r.check_d10_sources(ROOT,base['verified_at'])
        report.update(report.data,result='PASS_FULL_NATIVE_ARCADE_RAM_EXIT_OBSERVATIONS',
            artifacts=[r.pin(out/n) for n in ['native.bin','mame.bin','native.snd','objects.jsonl','events.jsonl','strict.log']+extra])
    except BaseException as error:
        report.update(report.data,result='FAIL',error=str(error));raise
    finally:
        report.save();print(json.dumps(dict(result=report.data['result'],report=str(report.path),owned_live_pids=report.data['owned_live_pids'])))


if __name__=='__main__':main()
