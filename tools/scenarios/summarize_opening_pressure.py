"""Compare all six declared opening seeds without selecting favorable results."""
import argparse
import json
import hashlib
from pathlib import Path
import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner
from summarize_pressure import pool
from recover_c3_variants import events

ROOT=Path(__file__).resolve().parents[2]


def job_matrix(report, opening):
    if opening:
        runner.require(report['result']=='COMPLETE_OPENING_OBSERVATIONS_BALANCE_OPEN','Opening experiment incomplete')
        jobs=[(name,mode,2,seed,report['experiment']['final_frame']) for name,mode,seed in report['jobs']]
        expected={(mode,2,seed) for mode in ('reference','enhanced') for seed in (3,4,5,6)}
    else:
        runner.require(report['result']=='COMPLETE_OBSERVATIONS_GATE_OPEN','Baseline incomplete')
        jobs=report['jobs']
        expected={(mode,players,seed) for mode in ('reference','enhanced') for players in (1,2) for seed in (1,2)}
    runner.require(not report['owned_live_pids'],'Capture children remain live')
    runner.require(len(jobs)==len(expected) and len({j[0] for j in jobs})==len(jobs) and
        {(mode,p,seed) for _,mode,p,seed,_ in jobs}==expected,'Missing or duplicate seed/profile jobs')
    steps={step['name']:step for step in report['steps']}
    runner.require(len(steps)==len(report['steps'])==len(jobs) and set(steps)=={j[0] for j in jobs} and
        all(s['state']=='PASS' and s['exit_code']==0 for s in steps.values()),'Capture steps failed or differ')
    return jobs,steps


def opening_only(run):
    return dict(observations=[r for r in run['observations'] if (r['circuit'],r['wave'])==(0,1)],
        partial_visits=[r for r in run['partial_visits'] if (r['circuit'],r['wave'])==(0,1)],
        boundary_life_decreases=[r for r in run['boundary_life_decreases'] if any(
            (r[s+'_circuit'],r[s+'_wave'])==(0,1) for s in ('from','to'))])


def trajectory_key(raw, visit):
    """Same boot prefix and endpoint means the same deterministic opening trial."""
    rows=[r[:4] for r in events(raw) if r[0]<=visit['frame']]
    runner.require(rows,'Opening visit has no input prefix')
    return hashlib.sha256(json.dumps([visit['frame'],rows],separators=(',',':')).encode()).hexdigest()


def build(baseline_path,opening_path):
    runs={};pins=[];identity=None;trajectories={}
    for path,is_opening in ((baseline_path,False),(opening_path,True)):
        report=json.loads(path.read_text(encoding='utf-8'));jobs,steps=job_matrix(report,is_opening)
        if identity is None:identity=report['verified_at']
        runner.require(identity==report['verified_at'],'Native identities differ')
        for pin in (report['pins'].values() if isinstance(report['pins'],dict) else report['pins']):gaps.checked(pin)
        pins.append(runner.pin(path))
        for name,mode,players,seed,frames in jobs:
            if players!=2:continue
            step=steps[name];log=gaps.checked(step['proof']['artifacts'][name+'.log'])
            runner.require(str(log)==step['log'] and runner.sha(log)==step['log_sha256'],'Log identity differs')
            run=pressure.read_run(log,players)
            runner.require(run['terminal']['frames']==frames and not run['terminal']['big_field'],'Endpoint differs')
            runs[mode,seed]=(name,opening_only(run));pins.append(runner.pin(log))
            inputs=gaps.checked(step['proof']['artifacts']['recorded.inputs'])
            raw=inputs.read_bytes();pins.append(runner.pin(inputs))
            for player in (1,2):
                visits=[v for v in runs[mode,seed][1]['observations'] if v['player']==player and v['frames']>=1000]
                runner.require(visits,'Missing initial opening visit')
                first=min(visits,key=lambda v:v['frame']);key=trajectory_key(raw,first)
                group=trajectories.setdefault((mode,player,key),dict(profile=mode,player=player,
                    prefix_sha256=key,final_frame=first['frame'],seeds=[],visits=[]))
                group['seeds'].append(seed)
                group['visits'].append({k:v for k,v in first.items() if k not in ('line','source_run')})
                runner.require(all(v==group['visits'][0] for v in group['visits']),
                    'Identical inputs and endpoint yielded different measured opening visits')
    def compare(groups):
        return pressure.compare(groups['reference'],groups['enhanced'],players=2,minimum_frames=1000,
            lower=.67,upper=1.5,required_rooms={(0,1)})
    per_seed={str(seed):compare({mode:runs[mode,seed][1] for mode in ('reference','enhanced')}) for seed in range(1,7)}
    groups={mode:pool([runs[mode,seed] for seed in range(1,7)]) for mode in ('reference','enhanced')}
    combined=compare(groups)
    return dict(schema='stv.opening_pressure_analysis.v1',result='SIX_SEED_OBSERVATIONS_BALANCE_OPEN',
        verified_at=identity,gate_proof=False,pins=pins+[runner.pin(__file__)],per_seed=per_seed,
        pooled=combined,source_visits=groups,initial_trajectory_groups=list(trajectories.values()),
        initial_unique_trajectories={mode:len({key for m,p,key in trajectories if m==mode and p==1})
            for mode in ('reference','enhanced')},limits=[
            'Raw pooled repeat counts are descriptive, not independent trials; inspect initial trajectory groups.',
            'All six seeds retained, including every eligible opening-room revisit; no favorable seed selection.',
            'Seeds1/2 have longer capture windows; visit counts and game stages can differ.',
            'This measures closed-loop bot outcomes, not identical player actions or attributed damage.',
            'Only circuit1 room1 assessed; no clearance or all-room balance claim.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','opening','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.baseline.resolve(),args.opening.resolve());runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['pooled']['result'],rows=result['pooled']['rows'])))
