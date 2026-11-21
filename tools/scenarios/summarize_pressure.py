"""Pool completed pressure seeds without treating sampled transitions as clears."""
import argparse
import json
from pathlib import Path
import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner

ROOT=Path(__file__).resolve().parents[2]


def pool(named_runs):
    runner.require(named_runs and len({name for name,_ in named_runs})==len(named_runs),
                   'Empty or duplicate pooled runs')
    result={key:[] for key in ('observations','partial_visits','boundary_life_decreases')}
    for name,run in named_runs:
        for key in result:
            result[key].extend(dict(row,source_run=name) for row in run[key])
    return result


def build(path):
    report=json.loads(path.read_text(encoding='utf-8'))
    runner.require(report['result']=='COMPLETE_OBSERVATIONS_GATE_OPEN' and not report['owned_live_pids'],
                   'Pressure batch must finish before pooling')
    steps={step['name']:step for step in report['steps']}
    runner.require(len(steps)==len(report['steps'])==8 and
                   all(s.get('state')=='PASS' and s.get('exit_code')==0 for s in steps.values()),
                   'Duplicate, incomplete or failed pressure captures')
    rooms=pressure.read_rooms(gaps.checked(report['room_inventory']))
    runs={}
    pins=[runner.pin(path),report['room_inventory'],runner.pin(__file__),runner.pin(pressure.__file__)]
    expected={(mode,players,seed) for mode in ('reference','enhanced') for players in (1,2) for seed in (1,2)}
    actual={(mode,players,seed) for _,mode,players,seed,_ in report['jobs']}
    runner.require(actual==expected and len(report['jobs'])==8,'Unexpected pressure job matrix')
    for name,mode,players,seed,final in report['jobs']:
        step=steps[name]
        log=gaps.checked(step['proof']['artifacts'][name+'.log'])
        runner.require(str(log)==step['log'] and runner.sha(log)==step['log_sha256'],'Log provenance differs')
        run=pressure.read_run(log,players)
        runner.require(run['terminal']['frames']==final and not run['terminal']['big_field'],
                       'Incomplete pressure endpoint')
        runs[mode,players,seed]=(name,run)
        pins.append(runner.pin(log))
    pooled={}
    for players in (1,2):
        groups={mode:pool([runs[mode,players,seed] for seed in (1,2)]) for mode in ('reference','enhanced')}
        comparison=pressure.compare(groups['reference'],groups['enhanced'],players=players,
            minimum_frames=1000,lower=.67,upper=1.5,required_rooms=rooms)
        comparison['source_visits']=groups
        pooled[str(players)]=comparison
    return dict(schema='stv.current_pressure_pool.v1',result='POOLED_OBSERVATIONS_CLEARABILITY_OPEN',
        gate_proof=False,verified_at=report['verified_at'],pins=pins,pooled=pooled,
        summary={key:dict(paired=len(value['rows']),missing=len(value['missing_required']),
                    out_of_band=sum(not row['in_band'] for row in value['rows'])) for key,value in pooled.items()},
        limits=['Visit-weighted pooled means, not the mean of per-seed ratios.',
                'Bot seeds can follow different routes and have unequal visit counts.',
                'Observed life-counter decreases and transitions are not damage/room-clear proofs.',
                'Missing rooms require targeted coverage; outliers require owner-backed diagnosis before tuning.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.report.resolve());runner.atomic_json(out,result)
    print(json.dumps(result['summary']))
