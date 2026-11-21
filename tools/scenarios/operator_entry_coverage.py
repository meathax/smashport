"""Map bound operator routine entries to pinned current instruction coverage."""
import argparse
import json
from pathlib import Path
import re
import coverage_gap_manifest as gaps
import rebaseline as runner

ROOT=Path(__file__).resolve().parents[2]


def coverage_hits(data,addresses):
    runner.require(len(data)==len(addresses) and set(data)<={0,1},'Coverage extent/value differs')
    runner.require(len(set(addresses))==len(addresses),'Duplicate instruction address')
    return {pc for pc,value in zip(addresses,data) if value}


def build(inventory_path,reference_path):
    inventory=json.loads(inventory_path.read_text(encoding='utf-8'))
    reference=json.loads(reference_path.read_text(encoding='utf-8'))
    gaps.complete_steps(reference);runner.check_d10_sources(ROOT,reference['verified_at'])
    for pin in inventory['pins']:gaps.checked(pin)
    generated=runner.generated_root()
    index=generated/'l8/l8_translation_index.csv'
    addresses=[int(line.split(',',1)[0],16) for line in index.read_text().splitlines()[1:]]
    slots={};chunks=sorted((generated/'l8').glob('l8_chunk_*.cpp'))
    for p in chunks:
        for pc,slot in re.findall(r'L_([0-9a-f]{8}):[^\n]*\n\s*l8_cov\[(\d+)\] = 1;',p.read_text()):
            runner.require(int(slot) not in slots,'Duplicate generated coverage slot')
            slots[int(slot)]=int(pc,16)
    runner.require(slots==dict(enumerate(addresses)),'Generated coverage slots differ from translation index')
    pins=[runner.pin(p) for p in [inventory_path,reference_path,index,Path(__file__),ROOT/'tools/lockstep/native_main.cpp',*chunks]]
    captures={}
    for step in reference['steps']:
        pin=step.get('proof',{}).get('coverage')
        if pin:
            runner.require(step['state']=='PASS','Incomplete coverage scenario')
            captures[step['name']]=coverage_hits(gaps.checked(pin).read_bytes(),addresses);pins.append(pin)
    runner.require(captures,'No current coverage records')
    rows=[]
    for menu in inventory['menus']:
        for entry in menu['records']:
            if entry['middle_pointer_role']!='routine':continue
            runner.require(entry['routine_entry_binding'] and not entry['routine_entry_open'],'Unbound routine entry')
            pc=int(entry['retail'][1],16)
            rows.append(dict(menu=menu['label'],entry=entry['index'],text=entry['text'],symbol=entry['source_symbols'][1],
                             pc=f'{pc:08x}',scenarios=[name for name,hit in captures.items() if pc in hit]))
    return dict(schema='stv.operator_entry_coverage.v1',result='CURRENT_ENTRY_HITS_CLASSIFIED_EFFECTS_OPEN',
        verified_at=reference['verified_at'],gate_proof=False,pins=pins,entries=rows,
        summary=dict(callable_entries=len(rows),hit_entries=sum(bool(r['scenarios']) for r in rows),
            missing_entries=sum(not r['scenarios'] for r in rows),captures=len(captures)),
        limits=['An entry hit does not prove menu selection, complete handler execution, expected diagnostics result or persistent effects.',
                'Return entries, adjustment data, page help and lifecycle paths require different ownership checks.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('inventory','reference','out'):p.add_argument('--'+n,type=Path,required=True)
    args=p.parse_args();out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.inventory.resolve(),args.reference.resolve())
    out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n');print(json.dumps(result['summary']))
