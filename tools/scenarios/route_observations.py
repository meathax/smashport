"""Compare strict-reference sampled selector changes with reconstructed exit fields.

This is route-candidate coverage, not proof of door activation or room clearance.
"""
import argparse
import json
from pathlib import Path

import coverage_gap_manifest as gaps
import rebaseline as runner
import reconstruct_routes

ROOT=Path(__file__).resolve().parents[2]
PLAYER_CAPTURES=frozenset(('1p','2p','route:bonus','route:lower','route:scorpion'))


def transitions(samples):
    previous=None
    for sample in samples:
        for key in ('frame','gamstate','circuit','wave'):
            runner.require(type(sample.get(key)) is int, 'Malformed sampled selector')
        if previous is not None:
            runner.require(sample['frame'] > previous['frame'], 'Unordered samples')
            # Never bridge missing samples, menu/boot state, or circuit rack-up.
            if (sample['frame']-previous['frame']==60 and
                previous['gamstate'] in (16,24) and sample['gamstate'] in (16,24) and
                previous['circuit']==sample['circuit'] and previous['wave']!=sample['wave']):
                yield dict(circuit=sample['circuit'],source_wave=previous['wave'],
                    destination_wave=sample['wave'],before_frame=previous['frame'],after_frame=sample['frame'])
        previous=sample


def match_options(change, options):
    return [i for i,row in enumerate(options) if row['circuit']==change['circuit'] and
            row['source_wave']==change['source_wave'] and row['destination_field']==change['destination_wave']]


def targeted_captures(report, verified_at, recorder_sha, rom_sha):
    """Admit terminal, same-runtime targeted replays, not arbitrary sidecars."""
    runner.require(report.get('result')=='PASS_SIX_COMPLETE_TARGETED_ROUTES_CLEARANCE_OPEN'
        and report.get('owned_live_pids')==[] and report.get('verified_at')==verified_at,
        'Targeted route report is not terminal/current')
    cases=report.get('cases',[]);steps={s['name']:s for s in report.get('steps',[])}
    runner.require(len(cases)==6 and len({c['name'] for c in cases})==6
        and len(steps)==len(report.get('steps',[]))==12, 'Targeted route matrix is incomplete/duplicated')
    pins={p['path']:p for p in report['pins']}
    for pin in pins.values():gaps.checked(pin)
    captures=[]
    for case in cases:
        name=case['name'];final=case['final_frame']
        native=steps.get('native-'+name,{});diff=steps.get('diff-'+name,{})
        runner.require(case.get('state')=='PASS' and type(final) is int and final>0
            and all(x.get('state')=='PASS' and x.get('exit_code')==0 for x in (native,diff)),
            'Targeted route case is incomplete')
        command=native.get('command',[])
        runner.require(len(command)>6 and pins.get(command[0],{}).get('sha256')==recorder_sha
            and pins.get(command[1],{}).get('sha256')==rom_sha and command[3]==str(final)
            and all(flag in command and command.index(flag)+1<len(command) for flag in ('--inputs','--content'))
            and '--native-hcount' in command
            and not any(x in command for x in ('--enhanced','--bot','--load-state')),
            'Targeted route executable/profile/input provenance differs')
        proof=native['proof'];endpoint=proof['endpoint'];strict=diff.get('proof',{})
        runner.require(endpoint.get('first_frame')==0 and endpoint.get('last_frame')==final
            and endpoint.get('frames')==final+1 and strict.get('frames')==final+1
            and strict.get('allow_prefix') is False, 'Targeted route comparison is not complete/strict')
        artifacts=proof['artifacts']
        for pin in [case['inputs'],case['oracle'],*artifacts.values()]:gaps.checked(pin)
        runner.require(command[2]==artifacts['native.bin']['path']
            and command[command.index('--inputs')+1]==case['inputs']['path']
            and command[command.index('--content')+1]==artifacts['content.jsonl']['path']
            and diff['command'][-2:]==[case['oracle']['path'],artifacts['native.bin']['path']],
            'Targeted route comparison does not bind its claimed artifacts')
        captures.append(dict(name='targeted:'+name,content=artifacts['content.jsonl'],
            main=artifacts['native.bin'],oracle=case['oracle']))
    return captures


def build(reference_report, route_report=None, targeted_report=None):
    coverage=gaps.build(reference_report,route_report)
    player_names=set(PLAYER_CAPTURES)
    if targeted_report:
        reference=json.loads(reference_report.read_text(encoding='utf-8'))
        native_command=next(s['command'] for s in reference['steps'] if s['name']=='1p-native')
        recorder,rom=native_command[:2]
        extra=targeted_captures(json.loads(targeted_report.read_text(encoding='utf-8')),
            coverage['verified_at'],reference['pins'][recorder]['sha256'],reference['pins'][rom]['sha256'])
        coverage['current_captures'].extend(extra)
        player_names.update(c['name'] for c in extra)
    inventory=reconstruct_routes.build()
    normal={tuple(r['selector']) for r in coverage['normal_room_obligations']}
    options=[]
    for record in inventory['physical_records']:
        for circuit in record['table_users']:
            if (circuit,record['physical_wave']) not in normal: continue
            for option in record['options']:
                options.append(dict(circuit=circuit,source_wave=record['physical_wave'],
                    record=record['address'],**option,observations=[],activation_proved=False,clearance_proved=False))
    unmatched=[]
    for capture in coverage['current_captures']:
        # Attract/operator state changes are not player route evidence.
        if capture['name'] not in player_names: continue
        for change in transitions(gaps.samples(gaps.checked(capture['content']))):
            candidates=match_options(change,options)
            observation=dict(scenario=capture['name'],**change,matching_options=len(candidates))
            if not candidates: unmatched.append(observation)
            for index in candidates: options[index]['observations'].append(observation)
    return dict(schema='stv.route_observations.v1',result='SAMPLED_ROUTE_CANDIDATES_CLEARANCES_OPEN',gate_proof=False,
        verified_at=coverage['verified_at'],reference_report=runner.pin(reference_report),
        route_report=runner.pin(route_report) if route_report else None,tool=runner.pin(__file__),
        targeted_report=runner.pin(targeted_report) if targeted_report else None,
        inventory=inventory,normal_obligations=coverage['obligation_pins'],
        captures=[dict(name=c['name'],content=c['content'],main=c['main'],oracle=c['oracle']) for c in coverage['current_captures']],
        options=options,unmatched_changes=unmatched,
        summary=dict(options=len(options),options_with_sampled_candidate=sum(bool(o['observations']) for o in options),
                     options_without_sampled_candidate=sum(not o['observations'] for o in options),
                     activation_proved=0,clearances_proved=0),
        limits=['Normal-room obligations retain their declared inventory provenance.',
                'Adjacent samples can miss intervening actions; matching destination fields do not prove a legal door transition.',
                'No process lifetime, player readiness, exit collision or clearance claim.',
                'No attract/operator transitions counted as player routes; no gaps or circuit changes bridged.'])


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--report',type=Path,required=True)
    ap.add_argument('--route-report',type=Path)
    ap.add_argument('--targeted-report',type=Path)
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use a fresh output under build/')
    result=build(args.report.resolve(),args.route_report.resolve() if args.route_report else None,
        args.targeted_report.resolve() if args.targeted_report else None)
    runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],summary=result['summary'],out=str(out))))


if __name__=='__main__':main()
