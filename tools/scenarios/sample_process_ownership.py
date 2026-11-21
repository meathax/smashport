"""Audit sampled process-pool membership; never infer continuous lifetime proof."""
import argparse
import json
from pathlib import Path
import coverage_gap_manifest as gaps
import mproc_continuation_audit as a
import rebaseline as runner

ROOT=Path(__file__).resolve().parents[2]


def inspect_node(node,base,end,stride,compiled):
    address=node['address'];issues=[]
    if not (base<=address<end and (address-base)%stride==0):return ['outside_process_pool']
    sp=node['saved_sp']
    if sp%16 or not address+a.LAYOUT['PSDATA']<=sp<=address+stride:
        issues.append('saved_stack_outside_source_reservation')
    if node['wake'] not in compiled:issues.append('wake_not_compiled')
    return issues


def build(captures_path,pools_path):
    captures=json.loads(captures_path.read_text(encoding='utf-8'));pools=json.loads(pools_path.read_text(encoding='utf-8'))
    a.require(captures['result']=='COMPLETE_OBSERVATIONS_GATE_OPEN' and not captures['owned_live_pids'],'Pressure capture is not terminal')
    a.require(all(s['state']=='PASS' for s in captures['steps']) and len(captures['steps'])==8,'Pressure matrix incomplete')
    for pin in pools['pins']:gaps.checked(pin)
    pool=pools['pools']['processes'];base=int(pool['base'],16);end=int(pool['end_exclusive'],16);stride=pool['stride_bits']
    _,_,compiled,_,_,pins=a.load_current(ROOT)
    pins.extend(runner.pin(p) for p in [captures_path,pools_path,Path(__file__),ROOT/'tools/lockstep/content_observer.hpp'])
    rows=[]
    for step in captures['steps']:
        pin=step['proof']['artifacts']['content.jsonl'];path=gaps.checked(pin);pins.append(pin)
        row=dict(name=step['name'],samples=0,complete_samples=0,partial_samples=0,nodes=0,max_complete_active=0,
                 issue_counts={},examples=[])
        previous=-1
        with path.open(encoding='utf-8') as f:
            header=json.loads(next(f));a.require(header['schema']=='stv.content.samples.v1' and header['cadence']==60,'Unknown sample format')
            for line in f:
                sample=json.loads(line);frame=sample['frame'];a.require(frame>previous and frame%60==0,'Sample cadence differs');previous=frame
                row['samples']+=1
                if not sample['process_list_complete']:
                    row['partial_samples']+=1
                    continue
                row['complete_samples']+=1;nodes=sample['processes'];row['nodes']+=len(nodes)
                row['max_complete_active']=max(row['max_complete_active'],len(nodes))
                for node in nodes:
                    issues=inspect_node(node,base,end,stride,compiled)
                    for issue in issues:row['issue_counts'][issue]=row['issue_counts'].get(issue,0)+1
                    if issues and len(row['examples'])<20:
                        row['examples'].append(dict(frame=frame,gamstate=sample['gamstate'],circuit=sample['circuit'],wave=sample['wave'],node=node,issues=issues))
        rows.append(row)
    return dict(schema='stv.sampled_process_ownership.v1',result='SAMPLED_PROCESS_CONTRACT_OBSERVATIONS_CONTINUOUS_PROOF_OPEN',
        gate_proof=False,capture_revision=captures['verified_at'],pins=pins,pool=pool,cases=rows,
        limits=['Historical capture revision retained; newer recorder reference renewal is separate.',
            'Only complete ACTIVE samples checked; partial POST/list boundaries retained as counts, not silently treated as valid.',
            '60-frame sampling cannot exclude transient overflows, reuse, free-list aliasing or writes between samples.',
            'Saved stack reservation follows source PSDATA; outliers require owner analysis before declaring corruption.',
            'Compiled wake membership is not proof of valid entry conditions or whole-code equivalence.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('captures','pools','out'):p.add_argument('--'+n,type=Path,required=True)
    args=p.parse_args();out=args.out.resolve();a.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.captures.resolve(),args.pools.resolve());out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps([{k:r[k] for k in ('name','complete_samples','partial_samples','max_complete_active','issue_counts')} for r in result['cases']]))
