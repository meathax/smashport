"""Assess actual state diversity and post-takeover opening pressure sensitivity."""
import argparse
import json
from pathlib import Path
import coverage_gap_manifest as gaps
import pressure
import rebaseline as runner
from capture_opening_variants import VARIANTS,FINAL
from summarize_opening_pressure import opening_only
from summarize_pressure import pool

ROOT=Path(__file__).resolve().parents[2]


def player_positions(sample):
    return sorted((o['oid'],o['x'],o['y']) for o in sample['objects']
                  if o['oid']&0xc000==0x4000 and o['oid']&0x1f00==0x100)


def build(path):
    report=json.loads(path.read_text(encoding='utf-8'))
    runner.require(report['result']=='COMPLETE_VARIANT_OBSERVATIONS_BALANCE_OPEN' and not report['owned_live_pids'],
        'Variant captures are not terminal')
    for pin in report['pins']:gaps.checked(pin)
    cases=report['cases'];steps={s['name']:s for s in report['steps']}
    expected={(m,v) for m in ('reference','enhanced') for v in VARIANTS}
    runner.require(len(cases)==len(expected) and {(c['profile'],c['variant']) for c in cases}==expected and
        len(steps)==len(report['steps'])==len(cases) and set(steps)=={c['name'] for c in cases},'Case/step matrix differs')
    runs={};positions={};pins=[runner.pin(path),runner.pin(__file__)];handoffs={}
    for case in cases:
        step=steps[case['name']];runner.require(step['state']=='PASS' and step['exit_code']==0,'Capture failed')
        proof=step['proof'];log=gaps.checked(proof['artifacts'][case['name']+'.log'])
        runner.require(runner.sha(log)==step['log_sha256'],'Log hash differs')
        run=pressure.read_run(log,2);runner.require(run['terminal']['frames']==FINAL,'Endpoint differs')
        run=opening_only(run)
        runner.require([r for r in run['observations'] if r['frames']>=1000]==proof['opening_visits'],'Raw visits differ')
        content=gaps.checked(proof['artifacts']['content.jsonl'])
        sample=next((s for s in gaps.samples(content) if s['frame']==case['bot_after']),None)
        runner.require(sample==proof['handoff_sample'],'Handoff state differs from raw observation')
        mode=case['profile'];variant=case['variant'];handoffs.setdefault(mode,set()).add(case['bot_after'])
        positions[mode,variant]=player_positions(sample);runner.require(positions[mode,variant],'No player-type objects at takeover')
        runs[mode,variant]=(case['name'],run);pins.extend([runner.pin(log),runner.pin(content)])
    runner.require(all(len(frames)==1 for frames in handoffs.values()),'Takeover windows differ within profile')
    def compare(groups):return pressure.compare(groups['reference'],groups['enhanced'],players=2,
        minimum_frames=1000,lower=.67,upper=1.5,required_rooms={(0,1)})
    per_variant={v:compare({m:runs[m,v][1] for m in ('reference','enhanced')}) for v in VARIANTS}
    groups={m:pool([runs[m,v] for v in VARIANTS]) for m in ('reference','enhanced')}
    return dict(schema='stv.opening_variant_analysis.v1',result='LEGAL_INPUT_SENSITIVITY_MEASURED_BALANCE_OPEN',
        verified_at=report['verified_at'],gate_proof=False,pins=pins,per_variant=per_variant,pooled=compare(groups),
        player_type_positions={m:{v:positions[m,v] for v in VARIANTS} for m in ('reference','enhanced')},
        unique_position_sets={m:len({tuple(positions[m,v]) for v in VARIANTS}) for m in ('reference','enhanced')},
        limits=['Only post-takeover opening-room observations compared; matching control included.',
            'Raw player-type objects show positional diversity, not individual torso ownership or attributed damage.',
            'Five deterministic interventions measure sensitivity, not a statistical population or all-room balance.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('report','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.report.resolve());runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['pooled']['result'],unique_position_sets=result['unique_position_sets'],
        variants={k:v['result'] for k,v in result['per_variant'].items()})))
