"""Add only new target-room visits; never count replayed prefixes twice."""
import argparse
import copy
import json
from pathlib import Path
import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner
import prepare_pressure_gaps as preparation

ROOT=Path(__file__).resolve().parents[2]


def target_visits(run,case):
    def visit(row):
        return row['frame']>case['bot_after'] and row['circuit']==case['circuit'] and row['wave'] in case['targets']
    def boundary(row):
        return row['frame']>case['bot_after'] and any(row[side+'_circuit']==case['circuit'] and
             row[side+'_wave'] in case['targets'] for side in ('from','to'))
    return {key:[dict(row,source_run=case['name']) for row in run[key] if predicate(row)]
            for key,predicate in [('observations',visit),('partial_visits',visit),('boundary_life_decreases',boundary)]}


def retry_visits(report, verified_at, target_pin):
    runner.require(report['schema']=='stv.pressure_target_retry.v1' and
        report['result']=='COMPLETE_RETRY_OBSERVATIONS_GATE_OPEN' and not report['owned_live_pids'],
        'Retry is not terminal')
    runner.require(report['verified_at']==verified_at and target_pin in report['pins'],
        'Retry source or parent batch differs')
    for pin in report['pins']:gaps.checked(pin)
    runner.require(len(report['steps'])==1,'Expected one retry step')
    step=report['steps'][0]
    runner.require(step['state']=='PASS' and step['exit_code']==0,'Retry failed')
    log=gaps.checked(step['proof']['artifacts']['retry.log'])
    runner.require(runner.sha(log)==step['log_sha256'],'Retry log identity differs')
    case=dict(report['case'],name='retry-'+runner.sha(log))
    run=pressure.read_run(log,case['players'])
    runner.require(run['terminal']['frames']==case['final_frame'],'Retry endpoint differs')
    selected=target_visits(run,case)
    admitted=[{k:v for k,v in row.items() if k!='source_run'} for row in selected['observations'] if row['frames']>=1000]
    runner.require(admitted==step['proof']['target_visits'],'Retry visit receipt differs')
    return case,selected,runner.pin(log)


def build(base_path,target_path,retry_path=None):
    base=json.loads(base_path.read_text(encoding='utf-8'));target=json.loads(target_path.read_text(encoding='utf-8'))
    runner.require(base['schema']=='stv.current_pressure_pool.v1','Expected original pooled pressure observations')
    runner.require(target['result']=='COMPLETE_TARGETED_PRESSURE_OBSERVATIONS_GATE_OPEN' and
                   not target['owned_live_pids'],'Target batch is not terminal')
    runner.require(base['verified_at']==target['verified_at'],'Native source identities differ')
    for pin in base['pins']+target['pins']:gaps.checked(pin)
    runner.require(runner.pin(base_path) in target['pins'],'Target plan does not bind this pooled base')
    steps={s['name']:s for s in target['steps']}
    expected=preparation.expected_case_keys(target.get('strategy_set','original'))
    runner.require({preparation.case_key(c) for c in target['cases']}==expected and
                   len(steps)==len(target['steps'])==len(target['cases'])==len(expected) and
                   all(s['state']=='PASS' and s['exit_code']==0 for s in steps.values()),'Incomplete/duplicate target cases')
    groups={players:copy.deepcopy(row['source_visits']) for players,row in base['pooled'].items()}
    pins=[runner.pin(base_path),runner.pin(target_path),runner.pin(__file__),runner.pin(pressure.__file__)]
    added=[];names=set()
    for case in target['cases']:
        name=case['name'];runner.require(name not in names,'Duplicate target case');names.add(name)
        step=steps[name];artifacts=step['proof']['artifacts'];log=gaps.checked(artifacts[name+'.log'])
        runner.require(runner.sha(log)==step['log_sha256'],'Target log identity differs')
        run=pressure.read_run(log,case['players'])
        runner.require(run['terminal']['frames']==case['final_frame'],'Target endpoint differs')
        selected=target_visits(run,case)
        admitted=[{k:v for k,v in row.items() if k!='source_run'} for row in selected['observations'] if row['frames']>=1000]
        runner.require(admitted==step['proof']['target_visits'],'Target visit receipt differs from raw log')
        group=groups[str(case['players'])][case['profile']]
        runner.require(not any(r.get('source_run')==name for rows in group.values() for r in rows),'Target already pooled')
        for key,rows in selected.items():group[key].extend(rows)
        added.append(dict(name=name,eligible_visits=len(admitted),missing_target_players=step['proof']['missing_target_players']))
        pins.append(runner.pin(log))
    retry_paths=[] if retry_path is None else ([retry_path] if isinstance(retry_path,Path) else list(retry_path))
    runner.require(len({p.resolve() for p in retry_paths})==len(retry_paths),'Duplicate retry reports')
    for retry_path in retry_paths:
        report=json.loads(retry_path.read_text(encoding='utf-8'))
        case,selected,log_pin=retry_visits(report,base['verified_at'],runner.pin(target_path))
        group=groups[str(case['players'])][case['profile']]
        runner.require(not any(r.get('source_run')==case['name'] for rows in group.values() for r in rows),'Retry already pooled')
        for key,rows in selected.items():group[key].extend(rows)
        pins.extend([runner.pin(retry_path),log_pin])
        added.append(dict(name=case['name'],eligible_visits=sum(r['frames']>=1000 for r in selected['observations']),
                          missing_target_players=report['steps'][0]['proof']['missing_players']))
    original=json.loads(gaps.checked(base['pins'][0]).read_text(encoding='utf-8'))
    rooms=pressure.read_rooms(gaps.checked(original['room_inventory']))
    results={}
    for players,group in groups.items():
        result=pressure.compare(group['reference'],group['enhanced'],players=int(players),minimum_frames=1000,
                                lower=.67,upper=1.5,required_rooms=rooms)
        result['source_visits']=group;results[players]=result
    return dict(schema='stv.augmented_pressure.v1',result='TARGET_VISITS_ADDED_CLEARABILITY_OPEN',gate_proof=False,
        verified_at=base['verified_at'],pins=pins,added=added,pooled=results,
        summary={key:dict(paired=len(value['rows']),missing=len(value['missing_required']),
            out_of_band=sum(not row['in_band'] for row in value['rows'])) for key,value in results.items()},
        limits=['Only post-handoff visits in declared target rooms added; replayed prefixes excluded.',
                'Paired profiles have independent legal prefixes and bot trajectories, not identical player actions.',
                'Sampled transitions/life-counter drops remain distinct from clearances/attributed damage.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('base','targets','out'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--retry',type=Path,action='append')
    args=parser.parse_args();out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.base.resolve(),args.targets.resolve(),[p.resolve() for p in args.retry] if args.retry else None);runner.atomic_json(out,result)
    print(json.dumps(result['summary']))
