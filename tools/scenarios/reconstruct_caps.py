"""Bind retail table-driven enemy caps and conditional status/difficulty arithmetic."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import coverage_gap_manifest as gaps
import rebaseline as runner
from mproc_continuation_audit import R,K,I,M,BASE,expect,load_current,bind_relocated_source,pin,require
from reconstruct_wave_positions import read_words

ROOT=Path(__file__).resolve().parents[2]
CONTEXTS=[('c3_rbta','RMAX2',568,2,5,True),('c3_orb','OMAX',578,2,2,False),
 ('c3_gal','GMAX2',605,2,2,False),('c3_hulk','HMAX2',613,2,5,True),
 ('c2_rbta','RMAX',649,1,2,True),('c2_orb','OMAX',658,1,0,False),
 ('c2_gal','GMAX',664,1,2,False),('c1_hulk','HMAX',685,0,0,True),
 ('c1_tank','TMAX',692,0,0,False),('c1_fat','FMAX',698,0,0,False)]


def expression(text):
    text=text.replace(' ','')
    require(re.fullmatch(r'-?\d+(?:[+-]\d+)*',text) is not None,'Unsupported cap expression')
    return sum(int(term) for term in re.findall(r'[+-]?\d+',text))


def values(text,label,directive):
    lines=text.splitlines();start=[i for i,s in enumerate(lines) if re.match(r'^'+label+r'(?:\s|$)',s)]
    require(len(start)==1,'Missing/duplicate cap label');i=start[0];line=lines[i][len(label):].split(';',1)[0].strip()
    result=[]
    while True:
        if line:
            match=re.fullmatch(r'\.'+directive+r'\s+(.+)',line)
            if not match:break
            result.extend(expression(t) for t in match[1].split(','))
        i+=1
        if i>=len(lines):break
        line=lines[i].split(';',1)[0].strip()
        if line and not line.startswith('.'+directive):break
    require(result,'Empty cap table');return result


def cap_byte(base,status,single_delta,modifier):
    return (base-(single_delta if status!=3 else 0)+modifier)&255


def build(population_path):
    population=json.loads(population_path.read_text())
    require(population['result']=='RETAIL_WAVE_PARAMETERS_RECONSTRUCTED_BALANCE_OPEN','Population inventory incomplete')
    for p in population['pins']:gaps.checked(p)
    source=ROOT/'original/archive/smashtv-main/ROBO.ASM';text=source.read_text();lines=text.splitlines()
    _,rows,_,rom,ins,generated=load_current(ROOT)
    pp=ROOT/'build/p1/robo.bin';mp=ROOT/'build/p1/robo.bin.relmask';lp=ROOT/'build/p1/listing.json'
    p1=pp.read_bytes();mask=mp.read_bytes();listing=json.loads(lp.read_text())['instructions'];bindings={}
    def bind(name,records,delta):
        seq=[ins[int(r[0],16)+delta] for r in records]
        bindings[name]=bind_relocated_source(seq,rom,p1,mask,listing,'ROBO.ASM',[r[3] for r in records],lines)
        return seq
    one=bind('ONEPADJ',[r for r in listing if r[2]=='ROBO.ASM' and 632<=r[3]<=637],0x80)
    patterns=[('MOVE',M('ABS',None,0x010ee600),R('A2'),K(0)),('CMPI',I(3),R('A2')),
        ('JREQ',M('REL',None,0xffe12590)),('SUB',R('A1'),R('A0')),('RETS',)]
    require(len(one)==len(patterns),'ONEPADJ extent differs')
    for i,p in zip(one,patterns):expect(i,*p)
    adj=bind('ADJMAX',[r for r in listing if r[2]=='ROBO.ASM' and 678<=r[3]<=680],0x80)
    require(len(adj)==3,'ADJMAX extent differs')
    for i,p in zip(adj,[('MOVE',M('ABS',None,0x010de310),R('A2'),K(0)),('ADD',R('A2'),R('A0')),('RETS',)]):expect(i,*p)
    grunt=values(text,'GRUNTTBL','WORD');require(all(-32768<=v<=32767 for v in grunt),'Signed modifier invalid')
    mod_record=next(r for r in listing if r[2]=='ROBO.ASM' and r[3]==291)
    mod_ins=bind('modifier_table',[mod_record],0x80)[0]
    expect(mod_ins,'ADDI',I(mod_ins.ops[0].value),R('A0'))
    old_root=read_words(p1,int(mod_record[0],16)+16,1)[0]
    require(list(struct.unpack_from('<'+str(len(grunt))+'h',p1,(old_root-BASE)//8))==grunt,'Source signed modifiers differ')
    modifiers=list(struct.unpack_from('<'+str(len(grunt))+'h',rom,(mod_ins.ops[0].value-BASE)//8))
    tables={};contexts=[]
    for name,label,line,circuit,subtract,adjust in CONTEXTS:
        records=[r for r in listing if r[2]=='ROBO.ASM' and r[3]>=line-2]
        end=next(i for i,r in enumerate(records) if re.match(r'MOVB\s+A0,@MAX',r[4].strip()))
        records=records[:end+1];seq=bind(name,records,0x80)
        root=seq[2].ops[0].value
        for i,p in zip(seq[:4],[('MOVE',M('ABS',None,0x010eb630),R('A0'),K(0)),('SLL',K(3),R('A0')),
            ('ADDI',I(root),R('A0')),('MOVB',M('IND','A0'),R('A0'))]):expect(i,*p)
        middle=[]
        if subtract:middle.extend([('MOVK',K(subtract),R('A1')),('CALLR',M('REL',None,0xffe12520))])
        if adjust:middle.append(('CALLR',M('REL',None,0xffe12970)))
        require(len(seq)==5+len(middle),'Cap adjustment extent differs')
        for i,p in zip(seq[4:-1],middle):expect(i,*p)
        expect(seq[-1],'MOVB',R('A0'),M('ABS',None,seq[-1].ops[1].value))
        source_values=values(text,label,'BYTE');require(all(0<=v<=255 for v in source_values),'Invalid source cap byte')
        old=read_words(p1,int(records[2][0],16)+16,1)[0]
        require(list(p1[(old-BASE)//8:(old-BASE)//8+len(source_values)])==source_values,'Linked source cap bytes differ')
        retail=list(rom[(root-BASE)//8:(root-BASE)//8+len(source_values)])
        table=dict(address=f'{root:08x}',source_values=source_values,retail_values_in_source_extent=retail)
        require(label not in tables or table==tables[label],'Shared cap root differs');tables[label]=table
        contexts.append(dict(name=name,table=label,circuit=circuit,subtract_unless_status3=subtract,
            add_signed_gruntmod=adjust,destination=f'{seq[-1].ops[1].value:08x}'))
    rooms=[];outside=[]
    for room in population['rooms']:
        caps=[]
        for ctx in contexts:
            if ctx['circuit']!=room['circuit']:continue
            table=tables[ctx['table']];root=int(table['address'],16);index=room['wave'];base=rom[(root-BASE)//8+index]
            record=dict(context=ctx['name'],table=ctx['table'],index=index,base_byte=base,
                outside_source_extent=index>=len(table['source_values']),
                status3=[cap_byte(base,3,ctx['subtract_unless_status3'],g if ctx['add_signed_gruntmod'] else 0) for g in modifiers],
                status_other=[cap_byte(base,1,ctx['subtract_unless_status3'],g if ctx['add_signed_gruntmod'] else 0) for g in modifiers])
            caps.append(record)
            if record['outside_source_extent']:outside.append(dict(circuit=room['circuit'],wave=index,**record))
        rooms.append(dict(circuit=room['circuit'],wave=room['wave'],table_driven_caps=caps))
    return dict(schema='stv.reconstructed_caps.v1',result='CONDITIONAL_RETAIL_CAP_ARITHMETIC_BOUND_BALANCE_OPEN',gate_proof=False,
        verified_at=subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=ROOT).strip(),bindings=bindings,
        tables=tables,contexts=contexts,source_signed_modifiers=grunt,retail_signed_modifiers=modifiers,rooms=rooms,
        outside_source_extent_reads=outside,pins=[pin(p) for p in [population_path,source,pp,mp,lp,Path(__file__)]]+generated,
        limits=['Values are conditional on STATUS and signed GRUNTMOD; current CMOS/status owners remain separate.',
            'Stored cap arithmetic wraps to8 bits; no invented clamp.',
            'Reads beyond source-declared extents are retained as retail bytes, not silently extended source tables.',
            'Fixed cap assignments, spawn geometry and runtime admission/overshoot still require binding.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for k in ('population','out'):parser.add_argument('--'+k,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh output')
    result=build(args.population.resolve());runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],tables=len(result['tables']),rooms=len(result['rooms']),outside=result['outside_source_extent_reads'])))
