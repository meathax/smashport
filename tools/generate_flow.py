#!/usr/bin/env python3
"""Extract explicitly selected original DATA tables; no runtime CPU instructions.
Reject unknown expressions/record widths; retain labels, lines and source hashes.
"""
from __future__ import annotations
import argparse, hashlib, json, pathlib, re
import generate_animations as asm

def split_operands(text:str)->list[str]:
    """Split assembler operands without breaking packed [Y,X] literals."""
    result=[];start=0;depth=0
    for i,char in enumerate(text):
        if char=='[':depth+=1
        elif char==']':
            depth-=1
            if depth<0:raise ValueError('Unbalanced packed operand')
        elif char==',' and depth==0:
            result.append(text[start:i].strip());start=i+1
    if depth:raise ValueError('Unbalanced packed operand')
    result.append(text[start:].strip())
    return result

def parse_wave_offsets(path:pathlib.Path)->list[list[int]]:
    """Lower WAVES.ASM SET_WAVE's literal CRCT1YX/2YX/3YX data tables."""
    lines=path.read_text('latin1').splitlines();tables=[]
    for label in ('CRCT1YX','CRCT2YX','CRCT3YX'):
        found=False;values=[]
        for raw in lines:
            text=raw.split(';')[0].strip()
            if not text:continue
            if re.fullmatch(label+r':?',text,re.I):
                found=True;continue
            if not found:continue
            # The table is contiguous literal .LONG data and ends at the next
            # label/instruction. Unknown forms fail closed rather than being
            # mistaken for an offset.
            m=re.fullmatch(r'\.LONG\s+(.+)',text,re.I)
            if not m:
                if re.fullmatch(r'[A-Za-z_]\w*:?',text):
                    if values:break
                    raise ValueError(f'{label}: empty table before {text}')
                raise ValueError(f'{label}: unsupported table form {text}')
            for operand in split_operands(m[1]):
                if operand.startswith('[') and operand.endswith(']'):
                    pair=split_operands(operand[1:-1])
                    if len(pair)!=2:raise ValueError(f'{label}: expected packed [Y,X], got {operand}')
                    y,x=(asm.number(part) for part in pair)
                    if not 0<=x<=0xffff or not 0<=y<=0xffff:raise ValueError(f'{label}: packed coordinate out of range')
                    values.append((y<<16)|x)
                else:
                    value=asm.number(operand)
                    if not 0<=value<=0xffffffff:raise ValueError(f'{label}: offset out of range')
                    values.append(value)
        if not found or not values:raise ValueError(f'{path.name}: missing {label} table')
        tables.append(values)
    return tables

def generate(source:pathlib.Path, output:pathlib.Path)->dict:
    # Import only literal symbols needed for these tables, from their actual include.
    equates=source/'ROBO.EQU'
    for raw in equates.read_text('latin1').splitlines():
        m=re.match(r'^\s*(\w+)\s+\.EQU\s+([^;]+)',raw,re.I)
        if m:
            try: asm.CONSTANTS[m[1].upper()]=asm.number(m[2])
            except (ValueError,SyntaxError): pass  # unrelated structure/layout expressions
    out=['// Generated from supplied original DATA only; do not hand-edit.',
         '#include "stv/player_lifecycle.hpp"','#include "stv/waves.hpp"','namespace stv {']
    waves_source=source/'WAVES.ASM'
    report={'inputs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [equates,source/'ROBO.ASM',source/'ROBOATT.ASM',waves_source]},'movements':[],'waves':[],'wave_offsets':[]}
    labels,records=asm.parse_source(source/'ROBOATT.ASM'); movements=[]
    for label in ['LDRS','LDRS2','RDRS','RDRS2']:
        pos,line=labels[label];pairs=[]
        for _ in range(256):
            kind,text,_=records[pos];pos+=1
            if kind!='WORD':raise ValueError(f'{label}: expected WORD, got {kind}')
            bits=asm.number(text)
            if bits==0xffff:break
            kind,text,_=records[pos];pos+=1
            if kind!='WORD':raise ValueError(f'{label}: duration not WORD')
            ticks=asm.number(text)
            if bits&~15 or not 0<ticks<65536:raise ValueError(f'{label}: unsupported input/time')
            pairs.append((bits,ticks))
        else:raise ValueError(f'{label}: no terminator')
        ident='move_'+label.lower();out.append(f'// ROBOATT.ASM:{line} {label}')
        out.append('static const SourceMove '+ident+'[] = {'+','.join('{%d,%d}'%v for v in pairs)+'};')
        movements.append('{"%s",%s,%d,%d}'%(label,ident,len(pairs),line))
        report['movements'].append({'label':label,'line':line,'steps':pairs,'ticks':sum(x[1] for x in pairs)})
    out.append('const SourceMovement source_movements[] = {'+','.join(movements)+'};')
    out.append('const unsigned source_movement_count=sizeof(source_movements)/sizeof(source_movements[0]);')
    labels,records=asm.parse_source(source/'ROBO.ASM');waves=[]
    for circuit,label in enumerate(['WAVE_INFO','W2_INFO','W3_INFO']):
        pos,_=labels[label];end=labels[['W2_INFO','W3_INFO','FAKE_INFO'][circuit]][0]
        number=1;rows=[];first=0
        while pos<end:
            kind,text,line=records[pos]
            if kind!='WORD':
                # W3 ends at the following .IF DEBUG directive; never read debug waves.
                if text.upper()=='.IF\tDEBUG' or re.fullmatch(r'\.IF\s+DEBUG',text,re.I):break
                raise ValueError(f'{label}:{line}: unsupported table directive {text}')
            value=asm.number(text);pos+=1
            if value==0:
                if not rows or len(rows)>8:raise ValueError(f'{label}: invalid wave width')
                ident=f'wave_{circuit}_{number}'
                out.append(f'// ROBO.ASM:{first} {label} wave {number}')
                out.append('static const WaveRecord '+ident+'[] = {'+','.join('{'+','.join(map(str,r))+'}' for r in rows)+'};')
                waves.append('{%d,%d,%s,%d,%d}'%(circuit,number,ident,len(rows),first))
                report['waves'].append({'circuit':circuit,'wave':number,'line':first,'records':rows})
                rows=[];number+=1;continue
            if not rows:first=line
            row=[value]
            for _ in range(5):
                kind,text,line=records[pos];pos+=1
                if kind!='WORD':raise ValueError('Incomplete original wave record')
                row.append(asm.number(text))
            if any(not 0<=v<=65535 for v in row):raise ValueError('Word overflow')
            rows.append(row)
        if rows:raise ValueError('Unterminated wave')
    out.append('const SourceWave source_waves[] = {'+','.join(waves)+'};')
    out.append('const unsigned source_wave_count=sizeof(source_waves)/sizeof(source_waves[0]);')
    scroll_tables=parse_wave_offsets(waves_source)
    for circuit,values in enumerate(scroll_tables):
        ident=f'source_wave_yx_{circuit}'
        out.append(f'// WAVES.ASM:CRCT{circuit+1}YX')
        out.append(f'static const u32 {ident}[] = {{'+','.join(f'0x{value:08x}u' for value in values)+'};')
        report['wave_offsets'].append({'circuit':circuit,'label':f'CRCT{circuit+1}YX','count':len(values),'values':[f'0x{value:08x}' for value in values]})
    out.append('Status source_wave_yx(unsigned circuit,unsigned wave,u32& out) {')
    out.append('  const u32* table=nullptr;u32 count=0;')
    for circuit,values in enumerate(scroll_tables):
        out.append(f'  if(circuit=={circuit}){{table=source_wave_yx_{circuit};count={len(values)}u;}}')
    out.append('  if(!table)return Status::fail("WAVES SET_WAVE circuit is outside CRCTTAB",circuit);')
    out.append('  if(wave>=count)return Status::fail("WAVES SET_WAVE wave is outside its CRCTYX table",wave);')
    out.append('  out=table[wave];return {};')
    out.append('}\n}')
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text('\n'.join(out)+'\n')
    output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    return report
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args()
    r=generate(a.source,a.output);print(f"Generated {len(r['waves'])} original waves, {len(r['movements'])} movement tables and {len(r['wave_offsets'])} SET_WAVE scroll tables")
