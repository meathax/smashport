"""Apply existing source-bound action contracts to complete current reference captures."""
import argparse
import json
from pathlib import Path

import c3_action_terminal_audit as actions
import coverage_gap_manifest as gaps
import rebaseline as runner
from route_observations import PLAYER_CAPTURES

ROOT=Path(__file__).resolve().parents[2]


def build(reference, routes=None):
    coverage=gaps.build(reference,routes)
    report=json.loads(reference.read_text(encoding='utf-8'))
    steps=gaps.complete_steps(report)
    route_steps={}
    if routes:
        route_steps={s['name']:s for s in json.loads(routes.read_text(encoding='utf-8'))['steps']}
    captures=[]
    for capture in coverage['current_captures']:
        name=capture['name']
        if name.startswith('route:'):
            pin=route_steps['native-'+name.split(':',1)[1]]['proof']['artifacts']['native.cov']
        else:
            pin=steps[name+'-native']['proof']['coverage']
        data=gaps.checked(pin).read_bytes()
        captures.append(dict(name=name,player_scenario=name in PLAYER_CAPTURES,
            coverage=pin,executable=capture['executable'],main=capture['main'],oracle=capture['oracle'],
            actions=actions.coverage_actions(data,coverage['source_contract'])))
    summaries=[]
    for name,*_ in actions.WEAPONS:
        observations=[]
        for capture in captures:
            weapon=next(w for w in capture['actions']['weapons'] if w['weapon']==name)
            if capture['player_scenario'] and all(weapon['sites_executed'].values()):
                observations.append(capture['name'])
        summaries.append(dict(weapon=name,player_captures_with_all_bound_sites=observations,
                              player_event_proved=False,lifetime_proved=False))
    return dict(schema='stv.current_action_coverage.v1',result='CURRENT_ACTION_INSTRUCTION_COVERAGE_OWNERSHIP_OPEN',
        gate_proof=False,verified_at=coverage['verified_at'],reference_report=runner.pin(reference),
        route_report=runner.pin(routes) if routes else None,
        tools=[runner.pin(p) for p in [Path(__file__),Path(actions.__file__),Path(gaps.__file__),
                                       ROOT/'tools/scenarios/route_observations.py']],
        contract=coverage['source_contract'],captures=captures,weapons=summaries,
        missing_weapon_site_coverage=[w['weapon'] for w in summaries if not w['player_captures_with_all_bound_sites']],
        limits=['Only instruction execution within each complete strict-reference capture is established.',
                'Attract/operator coverage is retained separately and cannot satisfy player-scenario coverage.',
                'Pickup, firing, collision and reward events need per-player lifetime/owner proof.',
                'This does not reconstruct the complete missing item/pool inventory or prove all content.'])


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--report',type=Path,required=True)
    ap.add_argument('--route-report',type=Path)
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use a fresh output under build/')
    result=build(args.report.resolve(),args.route_report.resolve() if args.route_report else None)
    runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],missing_weapon_site_coverage=result['missing_weapon_site_coverage'],out=str(out))))


if __name__=='__main__':main()
