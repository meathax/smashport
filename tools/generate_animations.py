#!/usr/bin/env python3
"""Narrow, fail-closed build-time lowering of explicitly selected original FRANIM tables.
This is DATA extraction, not an arcade CPU interpreter or general assembler.
The original source and line numbers are recorded in the generated output.
"""
from __future__ import annotations
import argparse, ast, hashlib, json, pathlib, re
CONSTANTS={'FLIPBITS':0x4000,'NEWPALET':0x2000,'M_FLIPH':16,'M_FLIPV':32,'DMAWNZ':2,'M_NOCOLL':0x800,'M_PIXSCAN':0x400}
def number(s:str)->int:
    s=s.strip().upper()
    s=re.sub(r'\b([0-9][0-9A-F]*)H\b',lambda m:str(int(m[1],16)),s)
    s=re.sub(r'>\s*([0-9A-F]+)',lambda m:str(int(m[1],16)),s)
    s=re.sub(r'\b([01]+)B\b',lambda m:str(int(m[1],2)),s)
    s=re.sub(r'\b0+([0-9]+)\b',lambda m:str(int(m[1])),s)
    def ev(n):
        if isinstance(n,ast.Constant) and type(n.value) is int:return n.value
        if isinstance(n,ast.Name) and n.id in CONSTANTS:return CONSTANTS[n.id]
        if isinstance(n,ast.UnaryOp) and isinstance(n.op,ast.USub):return -ev(n.operand)
        if isinstance(n,ast.BinOp):
            a,b=ev(n.left),ev(n.right)
            if isinstance(n.op,ast.BitOr):return a|b
            if isinstance(n.op,ast.Add):return a+b
            if isinstance(n.op,ast.Sub):return a-b
            if isinstance(n.op,ast.Mult):return a*b
        raise ValueError('Unsupported expression '+s)
    return ev(ast.parse(s,mode='eval').body)
def parse_source(p:pathlib.Path):
    labels={}; records=[]
    for line,raw in enumerate(p.read_text('latin1').splitlines(),1):
        text=raw.split(';')[0]
        if not text.strip() or text[0] in '*\x1a':continue
        m=re.match(r'^([A-Za-z_][\w]*):?\s*(.*)$',text)
        if m:
            label,rest=m.groups();labels[label.upper()]=(len(records),line);text=rest
        text=text.strip()
        if not text:continue
        m=re.match(r'\.(LONG|WORD)\s+(.+)$',text,re.I)
        if not m:records.append(('CODE',text,line));continue
        for item in m[2].split(','):records.append((m[1].upper(),item.strip(),line))
    return labels,records

def main(source:pathlib.Path,output:pathlib.Path):
    selection={
      'ROBO.ASM':lambda s:s in {'TSPN','LSPN','DIELST','TSHK'} or s.startswith(('PWK_','SHT_','L_SHT_','SHT2_','SHT3_','SHT4_','SHT5_')),
      'HULKS.ASM':lambda s:s in {'HDIE','HDIER','VDIE','VDIED','BLDOTS','BAT8O','BAT4O','BAT10O','BAT2O','BAT8OS','BAT4OS','BAT10OS','BAT2OS'} or bool(re.fullmatch(r'T(?:M|F|2|2M|2F)?_PWK_[A-Z]+',s)),
      'SHOTS.ASM':lambda s:s in {'EXPL','EXPR','EXPT','EXPD'},
    }
    out=['// Generated from user-supplied original source; do not hand-edit.','#include "stv/animation.hpp"','namespace stv {'];scripts=[];report=[]
    for module,predicate in selection.items():
        path=source/module;labels,records=parse_source(path)
        for label,(pos,line) in labels.items():
            if not predicate(label):continue
            frames=[];idx=pos
            def get(kind):
                nonlocal idx
                if idx>=len(records) or records[idx][0]!=kind:raise ValueError(f'{module}:{label}: expected {kind}, got {records[idx] if idx<len(records) else "EOF"}')
                value=records[idx][1];idx+=1;return value
            while True:
                image=get('LONG')
                if image=='0':break
                if not re.fullmatch(r'\w+',image):raise ValueError('Unresolved frame '+image)
                control=number(get('WORD'))
                if not 0<=control<=0xffff or control&~0x60ff:raise ValueError('Unsupported control flags')
                flips=number(get('WORD')) if control&0x4000 else 0
                palette=get('LONG') if control&0x2000 else None
                frames.append((image,control,flips,palette))
                if len(frames)>512:raise ValueError('Missing terminator')
            ident=f'animation_{len(scripts)}';out.append(f'// {module}:{line} {label}')
            out.append(f'static const SourceFrame {ident}[] = {{')
            for image,control,flips,palette in frames:
                out.append('  {"%s",0x%04x,0x%04x,%s},'%(image,control,flips,json.dumps(palette) if palette else 'nullptr'))
            out.append('};');scripts.append(f'{{"{module}","{label}",{line},{ident},{len(frames)}}}')
            report.append({'module':module,'label':label,'line':line,'frames':len(frames),'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    out.append('const SourceAnimation source_animations[] = {\n'+',\n'.join(scripts)+'\n};')
    out.append('const unsigned source_animation_count=sizeof(source_animations)/sizeof(source_animations[0]);\n}')
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text('\n'.join(out)+'\n');output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'Generated {len(scripts)} original scripts / {sum(x["frames"] for x in report)} frames')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args();main(a.source,a.output)
