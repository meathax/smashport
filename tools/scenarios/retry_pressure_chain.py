"""Retry one missed room from a completed legal capture using a new bot seed."""
import argparse
import json
import os
from pathlib import Path
import threading
import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner
from prepare_pressure_gaps import stable_handoffs
from recover_c3_variants import events,prefix_bytes

ROOT=Path(__file__).resolve().parents[2]


def bounded_endpoint(handoff, frames_after):
    runner.require(type(handoff) is int and handoff>0 and type(frames_after) is int
        and frames_after in (60000,120000,180000) and handoff+frames_after<=2000000,
        'Invalid bounded retry horizon')
    return handoff+frames_after


def parent_case(source, name):
    runner.require(not source.get('owned_live_pids'),'Parent batch is not terminal')
    if source.get('result')=='COMPLETE_TARGETED_PRESSURE_OBSERVATIONS_GATE_OPEN':
        cases=[c for c in source['cases'] if c['name']==name]
        steps=[s for s in source['steps'] if s['name']==name]
        runner.require(len(cases)==len(steps)==1,'Missing/duplicate parent case')
        case,step=cases[0],steps[0];missing=step['proof']['missing_target_players']
    else:
        runner.require(source.get('schema')=='stv.pressure_target_retry.v1' and
            source.get('result')=='COMPLETE_RETRY_OBSERVATIONS_GATE_OPEN' and
            name=='retry' and len(source['steps'])==1,'Parent retry is not terminal/unique')
        case=source['case'];step=source['steps'][0];missing=step['proof']['missing_player_rooms']
    runner.require(step['state']=='PASS' and step['exit_code']==0,'Parent capture failed')
    return case,step,missing


def preferred_exit(options,target,seed):
    runner.require(type(seed) is int and 0<=seed<=100,'Invalid retry seed')
    matches=[i for i,option in enumerate(options) if option['destination_field']==target]
    runner.require(len(matches)==1 and seed%len(options)==matches[0],
                   'Seed initial exit preference does not match unique target')
    return options[matches[0]]


def preferred_route(records,circuit,source,targets,seed):
    runner.require(targets and len(set(targets))==len(targets),'Empty or duplicate retry targets')
    result=[]
    for target in targets:
        candidates=[r for r in records if circuit in r['table_users'] and r['physical_wave']==source]
        runner.require(len(candidates)==1,'No unique retail predecessor record')
        result.append(preferred_exit(candidates[0]['options'],target,seed))
        source=target
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('report','inventory','out'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--case',required=True)
    for key in ('source-wave','seed'):parser.add_argument('--'+key,type=int,required=True)
    parser.add_argument('--target-wave',type=int,action='append',required=True)
    parser.add_argument('--frames-after',type=int,choices=(60000,120000,180000),default=60000)
    args=parser.parse_args();source=json.loads(args.report.read_text(encoding='utf-8'))
    for pin in source['pins']:gaps.checked(pin)
    case,step,missing=parent_case(source,args.case)
    runner.require(all(any(w==target for _,w in missing) for target in args.target_wave),
        'Target is not a recorded missing room')
    # Retain the root batch pin so augmentation can validate chained retries.
    target_batch=runner.pin(args.report) if source['result']=='COMPLETE_TARGETED_PRESSURE_OBSERVATIONS_GATE_OPEN' \
        else source.get('target_batch',source['pins'][0])
    root=json.loads(gaps.checked(target_batch).read_text(encoding='utf-8'))
    runner.require(root['result']=='COMPLETE_TARGETED_PRESSURE_OBSERVATIONS_GATE_OPEN' and
        not root['owned_live_pids'] and root['verified_at']==source['verified_at'], 'Wrong retry root batch')
    inventory=json.loads(args.inventory.read_text(encoding='utf-8'))
    for pin in inventory['pins']:gaps.checked(pin)
    route=preferred_route(inventory['physical_records'],case['circuit'],args.source_wave,args.target_wave,args.seed)
    artifacts=step['proof']['artifacts']
    inputs=gaps.checked(artifacts['recorded.inputs']);content=gaps.checked(artifacts['content.jsonl'])
    samples=(r for r in gaps.samples(content) if r['frame']>=case['bot_after'])
    handoff=stable_handoffs(samples,{(case['circuit'],args.source_wave)}).get((case['circuit'],args.source_wave))
    runner.require(handoff is not None,'No stable predecessor sample after original handoff')
    runner.check_d10_sources(ROOT,source['verified_at'])
    out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    out.mkdir(parents=True);prefix=out/'prefix.inputs';prefix.write_bytes(prefix_bytes(inputs.read_bytes(),handoff))
    final=bounded_endpoint(handoff,args.frames_after)
    name='retry';command=list(step['command'])
    command[2:4]=[str(out/'native.bin'),str(final)]
    for flag,value in [('--inputs',prefix),('--bot',out/'recorded.inputs'),('--bot-after',handoff),
        ('--bot-seed',args.seed),('--snd',out/'native.snd'),('--coverage',out/'native.cov'),('--content',out/'content.jsonl')]:
        runner.require(command.count(flag)==1,'Unexpected original capture command')
        command[command.index(flag)+1]=str(value)
    pins=[runner.pin(p) for p in [args.report,args.inventory,inputs,content,prefix,Path(command[0]),Path(command[1]),Path(__file__)]]
    if target_batch not in pins:pins.append(target_batch)
    report=runner.Report(out,dict(schema='stv.pressure_target_retry.v1',result='RUNNING',gate_proof=False,
        verified_at=source['verified_at'],head_at_launch=runner.git(ROOT,'rev-parse','HEAD'),owner_pid=os.getpid(),
        pins=pins,target_batch=target_batch,steps=[],case=dict(profile=case['profile'],players=case['players'],circuit=case['circuit'],
            source_wave=args.source_wave,targets=args.target_wave,bot_after=handoff,bot_seed=args.seed,final_frame=final),
        exit_preference=route[0],route_preferences=route,
        limits=['Initial bot preference does not force a successful exit; unchanged runtime and legal inputs only.']))
    timer=threading.Timer(1800,report.cancel_children,args=('Retry30-minute deadline exceeded',));timer.daemon=True;timer.start()
    try:
        def verify(_):
            actual=events((out/'recorded.inputs').read_bytes(),final);expected=events(prefix.read_bytes())
            runner.require([r[:4] for r in actual if r[0]<handoff]==[r[:4] for r in expected],'Retry lost legal prefix')
            run=pressure.read_run(out/'retry.log',case['players'])
            visits=[r for r in run['observations'] if r['frame']>handoff and r['circuit']==case['circuit']
                    and r['wave'] in args.target_wave and r['frames']>=1000]
            return dict(endpoint=runner.main_endpoint(out/'native.bin',final),sound=runner.sound_endpoint(out/'native.snd',final),
                content=runner.content_samples(out/'content.jsonl',final),target_visits=visits,
                missing_players=[p for p in range(1,case['players']+1)
                    if any(not any(r['player']==p and r['wave']==w for r in visits) for w in args.target_wave)],
                missing_player_rooms=[[p,w] for p in range(1,case['players']+1) for w in args.target_wave
                    if not any(r['player']==p and r['wave']==w for r in visits)],
                artifacts={p.name:runner.pin(p) for p in out.iterdir() if p.is_file() and p.name!='report.json'})
        report.run(name,command,out,verify)
        for pin in pins:gaps.checked(pin)
        runner.check_d10_sources(ROOT,source['verified_at'])
        report.update(report.data,result='COMPLETE_RETRY_OBSERVATIONS_GATE_OPEN')
        return 3
    except BaseException as error:
        report.cancel_children(str(error));report.update(report.data,result='FAIL',error=str(error));raise
    finally:
        timer.cancel();report.update(report.data,finished_at_utc=runner.now())
        print(json.dumps(dict(result=report.data['result'],report=str(report.path),owned_live_pids=report.data['owned_live_pids'])))


if __name__=='__main__':raise SystemExit(main())
