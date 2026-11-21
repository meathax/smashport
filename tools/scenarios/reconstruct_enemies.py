"""Bind retail enemy dispatch tables to all physical wave population records."""
import argparse
import json
from pathlib import Path
import re
import mproc_continuation_audit as a
import coverage_gap_manifest as gaps
from reconstruct_population import TYPES
from reconstruct_pickups import scalar_equates
from reconstruct_wave_positions import read_words

ROOT=Path(__file__).resolve().parents[2]


def table_labels(text,label):
    lines=text.splitlines();start=next((n for n,s in enumerate(lines) if s.split(';')[0].strip()==label+':'),None)
    a.require(start is not None,'Missing enemy table');values=[]
    for raw in lines[start+1:]:
        s=raw.split(';',1)[0].strip()
        if not s:continue
        if not s.startswith('.LONG'):break
        values.extend(t.strip() for t in s[5:].split(','))
    a.require(len(values)==16 and values[0]=='0' and all(v=='0' or re.fullmatch('[A-Z][A-Z0-9_]*',v) for v in values),'Invalid16-type dispatch table')
    return values


def population_targets(rooms,onscreen,spawn):
    a.require(len(onscreen)==len(spawn)==16,'Wrong dispatch table extent')
    calls=[]
    for room in rooms:
        for row in room['rows']:
            values=row['values']
            a.require(len(values)==6,'Population row must contain six words')
            kind=values[0]
            a.require(1<=kind<=16,'Population type outside table')
            if values[4]:
                a.require(onscreen[kind-1]!=0,'Nonzero initial population reaches null initializer')
                calls.append(dict(circuit=room['circuit'],wave=room['wave'],type=kind,
                    phase='onscreen',target=f'{onscreen[kind-1]:08x}'))
            if values[1]:
                a.require(spawn[kind-1]!=0,'Nonzero spawn budget reaches null spawn entry')
                calls.append(dict(circuit=room['circuit'],wave=room['wave'],type=kind,
                    phase='spawn',target=f'{spawn[kind-1]:08x}'))
    return calls


def build(population_path):
    population=json.loads(population_path.read_text())
    a.require(population['result']=='RETAIL_WAVE_PARAMETERS_RECONSTRUCTED_BALANCE_OPEN','Population inventory invalid')
    for pin in population['pins']:gaps.checked(pin)
    source=ROOT/'original/archive/smashtv-main/ROBO.ASM';equ=ROOT/'original/archive/smashtv-main/ROBO.EQU'
    text=source.read_text(encoding='latin-1');constants=scalar_equates(equ.read_text())
    a.require(all(constants.get(name)==n for n,name in enumerate(TYPES,1)),'Enemy enumeration differs')
    _,_,compiled,rom,ins,pins=a.load_current(ROOT);a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail image')
    pp=ROOT/'build/p1/robo.bin';mp=ROOT/'build/p1/robo.bin.relmask';lp=ROOT/'build/p1/listing.json';sp=ROOT/'build/p1/robo.map.json'
    p1=pp.read_bytes();mask=mp.read_bytes();listing=json.loads(lp.read_text())['instructions'];symbols=json.loads(sp.read_text())['globals']
    tables={};bindings=[]
    for name,label,pc,line,reg in [('onscreen','TYP_TBL',0xffe188d0,1257,'A3'),('spawn','TYP_TBL2',0xffe18f80,1330,'A1')]:
        labels=table_labels(text,label);root=ins[pc].ops[0].value
        a.expect(ins[pc],'ADDI',a.I(root),a.R('A1'))
        a.expect(ins[pc+48],'MOVE',a.M('IND','A1'),a.R(reg),a.K(1))
        a.expect(ins[pc+64],'MOVE',a.R('A0'),a.M('DEC','SP'),a.K(1))
        a.expect(ins[pc+80],'CALL',a.R(reg))
        seq=a.window(ins,pc-32,pc+64)
        a.expect(seq[0],'DEC',a.R('A1'));a.expect(seq[1],'SLL',a.K(5),a.R('A1'))
        proof=a.bind_relocated_source(seq,rom,p1,mask,listing,'ROBO.ASM',[line-2,line-1,line,line+1],text.split('\n'));bindings.append(proof)
        oldpc=int(proof['instructions'][2]['p1_pc'],16);oldroot=read_words(p1,oldpc+16,1)[0]
        current=read_words(rom,root,16);old=read_words(p1,oldroot,16);entries=[]
        for number,(label,target,previous) in enumerate(zip(labels,current,old),1):
            if label=='0':a.require(target==previous==0,'Null enemy table entry changed')
            else:
                a.require(target in compiled,'Enemy target is not compiled')
                if label=='NO':a.expect(ins[target],'RETS')
                else:a.require(symbols.get(label)==f'{previous:08x}','Linked enemy symbol differs')
            entries.append(dict(type=number,name=TYPES[number-1],source_entry_label=label,
                retail_target=f'{target:08x}',p1_target=f'{previous:08x}',compiled=target in compiled,
                behavior='null' if label=='0' else 'no_op_return' if label=='NO' else 'enemy_entry'))
        tables[name]=dict(root=f'{root:08x}',entries=entries)
    # Field meanings and six-word records are already source-bound by the population inventory.
    expected=population_targets(population['rooms'],[int(e['retail_target'],16) for e in tables['onscreen']['entries']],
        [int(e['retail_target'],16) for e in tables['spawn']['entries']])
    pins += [a.pin(p) for p in [population_path,source,equ,pp,mp,lp,sp,Path(__file__),
        ROOT/'tools/scenarios/mproc_continuation_audit.py',ROOT/'tools/scenarios/reconstruct_population.py',
        ROOT/'tools/scenarios/reconstruct_pickups.py',ROOT/'tools/scenarios/reconstruct_wave_positions.py']]
    return dict(schema='stv.enemy_dispatch_inventory.v1',result='RETAIL_ENEMY_TABLES_AND_POPULATION_TARGETS_BOUND',
        gate_proof=False,pins=pins,consumer_bindings=bindings,tables=tables,population_dispatches=expected,
        summary=dict(types=16,physical_rooms=len(population['rooms']),population_rows=sum(len(r['rows']) for r in population['rooms']),
            nonzero_initial_or_spawn_rows=len(expected),unique_nonzero_targets=len({e['retail_target'] for t in tables.values() for e in t['entries'] if e['retail_target']!='00000000'})),
        limits=['Named source/P1 table slots map to accepted retail entry addresses; complete enemy bodies/behavior and bosses are not proved equivalent.',
            'Every original population row selects a non-null entry whenever its initial count or spawn budget is nonzero.',
            'Mutable WAVE_RAM type/counter ownership, actual dispatch reachability, child lifetimes and clearance remain open.',
            'NO is an actual RETS routine; a nonzero spawn budget does not imply an enemy is created.',
            'This reconstructs the wave dispatch inventory, not every projectile, transient child or missing historical artifact hash.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('population','out'):p.add_argument('--'+name,type=Path,required=True)
    args=p.parse_args();out=args.out.resolve();a.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.population.resolve());out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n');print(json.dumps(result['summary']))
