"""Prepare legal per-profile input prefixes targeting missing pressure rooms."""
import argparse
import json
from pathlib import Path
import coverage_gap_manifest as gaps
import rebaseline as runner
from recover_c3_variants import prefix_bytes

ROOT=Path(__file__).resolve().parents[2]
STRATEGIES={1:[(0,4,0,[5]),(1,1,0,[2,3]),(2,1,0,[2,3]),(2,18,0,[16])],
            2:[(0,4,0,[5]),(1,1,0,[2,3]),(2,1,0,[2,3]),(2,20,1,[22])]}
STRATEGY_SETS={'original':STRATEGIES,'rescue-fixed':{
    1:STRATEGIES[1],
    2:[(0,4,0,[5]),(1,1,0,[2,3]),(2,1,0,[2,3]),(1,14,0,[15,17,18,16])]},
    'boss-geometry':{
    1:STRATEGIES[1],
    2:[(0,4,0,[5]),(1,1,0,[2,3]),(2,1,0,[2,3]),(1,13,0,[14,15,17,18,16])]},
    'dual-mode':{
    1:[(0,4,0,[5]),(1,1,0,[2,3]),(2,1,0,[2,3]),
       (2,1,5,[13,14,15,17,18,16,6,7,19,20,22,21])],
    2:[(0,4,0,[5]),(1,1,0,[2,3]),(2,1,0,[2,3])]}}


def strategy_set(name):
    runner.require(name in STRATEGY_SETS,'Unknown pressure strategy set')
    return STRATEGY_SETS[name]


def case_key(case):
    return (case['profile'],case['players'],case['circuit'],case['source_wave'],case['bot_seed'])


def expected_case_keys(name):
    return {(profile,p,c,w,seed) for profile in ('reference','enhanced')
            for p,rows in strategy_set(name).items() for c,w,seed,_ in rows}


def stable_handoffs(samples, wanted):
    result={};previous=None
    for row in samples:
        key=(row['circuit'],row['wave'])
        if previous and row['frame']-previous['frame']==60 and row['gamstate']==previous['gamstate']==16 and \
                key==(previous['circuit'],previous['wave']) and key in wanted and key not in result:
            result[key]=row['frame']
        previous=row
    return result


def verify_paths(inventory, strategies_by_players=STRATEGIES):
    edges={(c,r['physical_wave'],o['slot'],o['destination_field']) for r in inventory['physical_records']
           for c in r['table_users'] for o in r['options']}
    for strategies in strategies_by_players.values():
        for circuit,start,seed,targets in strategies:
            for target in targets:
                slots={s for c,w,s,_ in edges if (c,w)==(circuit,start)}
                runner.require(slots,'Missing retail predecessor')
                # Bot's initial choice is seed modulo the current exit count.
                slot=seed%(max(slots)+1)
                runner.require((circuit,start,slot,target) in edges,'Proposed route is not a retail exit-table path')
                start=target


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('report','pool','inventory','out'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--strategy-set',choices=STRATEGY_SETS,default='original')
    args=parser.parse_args()
    selected=strategy_set(args.strategy_set)
    report=json.loads(args.report.read_text(encoding='utf-8'))
    pooled=json.loads(args.pool.read_text(encoding='utf-8'))
    inventory=json.loads(args.inventory.read_text(encoding='utf-8'))
    runner.require(report['result']=='COMPLETE_OBSERVATIONS_GATE_OPEN' and not report['owned_live_pids'],
                   'Pressure batch is not complete')
    runner.require(pooled['pins'][0]==runner.pin(args.report),'Pool belongs to another batch')
    for pin in pooled['pins']+inventory['pins']:gaps.checked(pin)
    verify_paths(inventory,selected)
    steps={s['name']:s for s in report['steps']}
    runner.require(len(steps)==8 and all(s['state']=='PASS' for s in steps.values()),'Incomplete source captures')
    runner.check_d10_sources(ROOT,report['verified_at'])
    candidates={}
    for name,profile,players,seed,final in report['jobs']:
        artifacts=steps[name]['proof']['artifacts']
        content=gaps.checked(artifacts['content.jsonl']);inputs=gaps.checked(artifacts['recorded.inputs'])
        wanted={(c,w) for c,w,_,_ in selected[players]}
        found=stable_handoffs(gaps.samples(content),wanted)
        for key,frame in found.items():
            slot=(profile,players,*key)
            candidate=dict(base=name,bot_after=frame,base_content=artifacts['content.jsonl'],
                           base_inputs=artifacts['recorded.inputs'])
            if slot not in candidates or frame<candidates[slot]['bot_after']:candidates[slot]=candidate
    out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use a fresh output directory')
    out.mkdir(parents=True)
    cases=[]
    for players,strategies in selected.items():
        missing={(r['circuit'],r['wave']) for r in pooled['pooled'][str(players)]['missing_required']}
        runner.require(missing=={(c,t) for c,_,_,targets in strategies for t in targets},'Pressure gaps changed')
        for circuit,start,seed,targets in strategies:
            for profile in ('reference','enhanced'):
                slot=(profile,players,circuit,start)
                runner.require(slot in candidates,f'No sampled legal handoff for {slot}')
                case=candidates[slot].copy()
                name=f'{profile}-{players}p-c{circuit+1}-from{start}-seed{seed}'
                path=out/(name+'.inputs')
                path.write_bytes(prefix_bytes(Path(case['base_inputs']['path']).read_bytes(),case['bot_after']))
                cases.append(dict(case,name=name,profile=profile,players=players,circuit=circuit,
                    source_wave=start,targets=targets,bot_seed=seed,final_frame=case['bot_after']+60000,
                    prefix=runner.pin(path),state='PREPARED_NOT_RUN'))
    plan=dict(schema='stv.pressure_gap_plan.v1',result='PREPARED_LEGAL_PROFILE_PREFIXES_NOT_RUN',gate_proof=False,
        strategy_set=args.strategy_set,
        verified_at=report['verified_at'],pins=[runner.pin(p) for p in (args.report,args.pool,args.inventory,
            Path(__file__),ROOT/'tools/scenarios/recover_c3_variants.py')],cases=cases,
        limits=['Seed chooses an initial exit preference, not guaranteed route success.',
                'Each profile reuses only its own complete capture inputs; no RAM writes or cross-profile snapshots.',
                'Handoff uses two consecutive live-state samples, not room-clearance proof.',
                'Capture full endpoints, preserve prefix events, then verify target visits and paired statistics.'])
    runner.atomic_json(out/'plan.json',plan)
    print(json.dumps(dict(cases=len(cases),total_frames=sum(c['final_frame'] for c in cases),out=str(out/'plan.json'))))


if __name__=='__main__':main()
