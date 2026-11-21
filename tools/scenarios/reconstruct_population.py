"""Reconstruct retail room population/pacing records without claiming balance."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
from mproc_continuation_audit import R,K,I,M,BASE,expect,load_current,window,bind_relocated_source,pin,require
from reconstruct_wave_positions import read_words
import rebaseline as runner

ROOT=Path(__file__).resolve().parents[2]
TYPES=('ATRAIN','ORB','TANK','FAT','WLMN','HULK','MINE','MUTD','FACE','RBTA','GALS3','GALS1','GALS1A','GALS2','COBRA','SNK')
FIELDS=('enemy_type','spawn_budget_raw','difficulty','release_interval_ticks','initial_onscreen','initial_delay_ticks')


def scalar(token):
    token=token.strip()
    if token in TYPES:return TYPES.index(token)+1
    if re.fullmatch('[0-9]+',token):return int(token)
    if re.fullmatch('[0-9A-Fa-f]+H',token):return int(token[:-1],16)
    raise ValueError('Unsupported wave value: '+token)


def source_table(text,label):
    lines=text.splitlines();starts=[i for i,s in enumerate(lines) if s.strip()==label+':']
    require(len(starts)==1,'Missing/duplicate wave table label')
    words=[];locations=[];waves=[];rows=[]
    for i in range(starts[0]+1,len(lines)):
        line=lines[i].split(';',1)[0].strip()
        if not line:continue
        if not line.startswith('.WORD'):break
        match=re.fullmatch(r'\.WORD\s+(.+)',line);require(match is not None,'Malformed wave row')
        values=[scalar(t) for t in match[1].split(',')]
        require(values==[0] or (len(values)==6 and 1<=values[0]<=16 and all(0<=v<=65535 for v in values)),
            'Invalid six-field wave row')
        words.extend(values);locations.extend([i+1]*len(values))
        if values==[0]:
            require(rows,'Empty source wave');waves.append(rows);rows=[]
        else:rows.append(dict(values=values,source_line=i+1))
    require(waves and not rows,'Unterminated source wave')
    return words,locations,waves


def retail_table(words,start):
    waves=[];rows=[];i=0
    while i<len(words):
        if words[i]==0:
            require(rows,'Empty retail wave');waves.append(rows);rows=[];i+=1;continue
        require(1<=words[i]<=16 and i+6<=len(words),'Invalid/truncated retail wave row')
        values=words[i:i+6];rows.append(dict(address=f'{start+i*16:08x}',values=values,fields=dict(zip(FIELDS,values))))
        i+=6
    require(not rows,'Unterminated retail wave')
    return waves


def build():
    source=ROOT/'original/archive/smashtv-main/ROBO.ASM';equ=ROOT/'original/archive/smashtv-main/ROBO.EQU'
    text=source.read_text();eq=equ.read_text()
    for value,name in enumerate(TYPES,1):require(re.search(r'^'+name+r'\s+\.EQU\s+'+str(value)+r'\s*(?:;.*)?$',eq,re.M), 'Enemy type equate differs')
    originals=[source_table(text,label) for label in ('WAVE_INFO','W2_INFO','W3_INFO')]
    require([len(x[2]) for x in originals]==[10,18,21],'Source table count differs')
    _,_,compiled,rom,ins,generated=load_current(ROOT)
    p1p=ROOT/'build/p1/robo.bin';mp=ROOT/'build/p1/robo.bin.relmask';lp=ROOT/'build/p1/listing.json'
    p1=p1p.read_bytes();mask=mp.read_bytes();listing=json.loads(lp.read_text())['instructions'];bindings={}
    def bind(name,seq,lines,patterns):
        require(len(seq)==len(patterns),'Instruction extent differs')
        for instruction,pattern in zip(seq,patterns):expect(instruction,*pattern)
        bindings[name]=bind_relocated_source(seq,rom,p1,mask,listing,'ROBO.ASM',lines,text.splitlines())
    bind('root',window(ins,0xffe18400,0xffe18480),[1197,1198,1199,1200],
        [('MOVE',M('ABS',None,0x010eb610),R('A0'),K(0)),('SLL',K(5),R('A0')),
         ('ADDI',I(0xffe13790),R('A0')),('MOVE',M('IND','A0'),R('A1'),K(1))])
    bind('following_code',window(ins,0xffe18010,0xffe18020),[1152],[('CLR',R('A0'))])
    for line,name,offset,reg in [(1253,'initial_count',64,'A2'),(1317,'remaining_budget',16,'A1'),
        (1320,'release_counter',80,'A1'),(1324,'release_reload',48,'A1')]:
        candidates=[r for r in listing if r[2]=='ROBO.ASM' and r[3]==line]
        require(len(candidates)==1,'Missing field consumer')
        instruction=ins[int(candidates[0][0],16)+0x100]
        bind(name,[instruction],[line],[('MOVE',M('DISP','A0',offset),R(reg),K(0))])
    original_root=read_words(p1,int(bindings['root']['instructions'][2]['p1_pc'],16)+16,1)[0]
    original_roots=read_words(p1,original_root,3)
    for addr,(values,_,_) in zip(original_roots,originals):
        actual=list(struct.unpack_from('<'+str(len(values))+'H',p1,(addr-BASE)//8))
        require(actual==values,'Source expressions differ from linked-source table')
    roots=read_words(rom,0xffe13790,3);require(roots==[0xffe137f0,0xffe14970,0xffe16050],'Retail table roots differ')
    ends=roots[1:]+[0xffe18010];tables=[];differences=[]
    for circuit,(start,end,original) in enumerate(zip(roots,ends,originals)):
        words=list(struct.unpack_from('<'+str((end-start)//16)+'H',rom,(start-BASE)//8))
        waves=retail_table(words,start);require(len(waves)==[10,18,22][circuit],'Retail table count differs')
        for wave,rows in enumerate(waves,1):
            old=original[2][wave-1] if wave<=len(original[2]) else None
            same=old is not None and [r['values'] for r in old]==[r['values'] for r in rows]
            record=dict(circuit=circuit,wave=wave,rows=rows,source_rows=old,exact_source_match=same)
            tables.append(record)
            if not same:differences.append(dict(circuit=circuit,wave=wave,source_present=old is not None))
    return dict(schema='stv.reconstructed_population.v1',result='RETAIL_WAVE_PARAMETERS_RECONSTRUCTED_BALANCE_OPEN',
        gate_proof=False,verified_at=subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=ROOT).strip(),
        pins=[pin(p) for p in [source,equ,p1p,mp,lp,Path(__file__)]]+generated,bindings=bindings,
        source_counts=[10,18,21],retail_counts=[10,18,22],rooms=tables,source_differences=differences,
        limits=['New inventory, not recovery of any missing historical inventory hash.',
            'Spawn budget semantics vary by enemy owner; boss rows can have zero budget and nonzero initial count.',
            'Release counters are scheduler ticks; pause, caps, gangs and per-enemy routines affect realized pacing.',
            'These are original retail table values, not measured enhanced player-count balance or room-clearance proof.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh output')
    result=build();runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],rooms=len(result['rooms']),differences=result['source_differences'],first=result['rooms'][0]['rows'])))
