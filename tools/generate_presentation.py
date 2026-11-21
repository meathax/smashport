#!/usr/bin/env python3
"""Extract linked CHAMPS/TITLE data and bind by complete block-header geometry.
Only supplied files are used. These source snapshot tables do not prove retail identity.
"""
from __future__ import annotations
import argparse, hashlib, json, pathlib, re
from generate_animations import parse_source, number

def bdd_dimensions(path):
    data=path.read_bytes();pos=0
    def line():
        nonlocal pos
        end=pos
        while end<len(data) and data[end] not in (10,13):end+=1
        row=data[pos:end];pos=end
        if pos<len(data) and data[pos]==13:pos+=1
        if pos<len(data) and data[pos]==10:pos+=1
        return row
    n=int(line());result=[]
    for i in range(n):
        fields=line().split()
        if len(fields)!=4:raise ValueError('Bad BDD header')
        w,h=int(fields[1]),int(fields[2]);result.append([w,h]);pos+=w*h
        if pos>len(data):raise ValueError('Short BDD')
    return result

def generate(src,out):
    ls,rs=parse_source(src/'BACKUP/BGNDTBL.ASM')
    def label(name):
        matches=[k for k in ls if k.upper()==name.upper()]
        if len(matches)!=1:raise ValueError('Ambiguous scene label '+name)
        return ls[matches[0]][0]
    sources=['BACKUP/BGNDTBL.ASM','BACKUP/BGNDPAL.ASM','BACKUP/IMGPAL.ASM','ROBOATT.ASM','HSTD.ASM']
    pal_sources=[parse_source(src/'BACKUP'/p) for p in ['BGNDPAL.ASM','IMGPAL.ASM']]
    palettes={}
    def read_palette(name):
        matches=[]
        for pls,prs in pal_sources:
            for k,(i,ln) in pls.items():
                if k.upper()==name.upper():matches.append((prs,i))
        if len(matches)!=1:raise ValueError('Palette '+name)
        prs,i=matches[0];n=number(prs[i][1])&511
        if n<1 or n>256 or any(x[0]!='WORD' for x in prs[i:i+n+1]):raise ValueError('Palette size')
        return [number(x[1]) for x in prs[i+1:i+n+1]]
    scenes=[]
    for name,xoff,yoff in [('tenBMOD',10,-4),('TITPGBMOD',4,0)]:
        i=label(name);module=rs[i:i+6]
        if [x[0] for x in module]!=['WORD']*3+['LONG']*3:raise ValueError('Module header')
        dims=[number(x[1]) for x in module[:3]]
        pstart,hstart,pals=[r[1] for r in module[3:]]
        h=[];i=label(hstart)
        while [r[0] for r in rs[i:i+3]]==['WORD','WORD','LONG']:
            h.append([number(r[1]) for r in rs[i:i+3]]);i+=3
        if not h or len(h)>64:raise ValueError('Unsupported scene headers')
        candidates=[]
        for p in (src/'IMG').glob('*.BDD'):
            if bdd_dimensions(p)==[r[:2] for r in h]:candidates.append(p)
        if len(candidates)!=1:raise ValueError((name,'Geometry binding not unique',candidates))
        bank=candidates[0];sources.extend(['IMG/'+bank.name,'IMG/'+bank.with_suffix('.BDB').name])
        pmatches=[]
        for pls,prs in pal_sources:
            for k,(i,ln) in pls.items():
                if k.upper()==pals.upper():pmatches.append((pls,prs,i))
        if len(pmatches)!=1:raise ValueError('Palette table')
        pls,prs,i=pmatches[0];pn=[]
        while i<len(prs) and prs[i][0]=='LONG':
            pn.append(prs[i][1]);i+=1
            if i in [start for start,_ in pls.values()]:break
        for p in pn:
            if p not in palettes:palettes[p]=read_palette(p)
        i=label(pstart);placements=[]
        while True:
            if rs[i][0]!='WORD':raise ValueError('Placement directive')
            flags=number(rs[i][1]);i+=1
            if flags==65535:break
            rest=rs[i:i+3];i+=3
            if [r[0] for r in rest]!=['WORD']*3:raise ValueError('Placement shape')
            x,y,hdr=[number(r[1]) for r in rest]
            palette=(flags&15)+((hdr>>12)<<4);idx=(hdr&4095)//2
            if hdr&1 or idx>=len(h) or palette>=len(pn):raise ValueError('Placement reference')
            placements.append([flags,x,y,idx,palette])
        if len(placements)!=dims[2]:raise ValueError('Placement count')
        scenes.append({'name':name,'bank':bank.stem,'module':dims,'offset':[xoff,yoff],'headers':h,'placements':placements,'palettes':pn})
    report={'source_files':{p:hashlib.sha256((src/p).read_bytes()).hexdigest() for p in sorted(set(sources))},'scenes':scenes,'palettes':palettes,'scope':'Literal source-linked presentation; not a retail program match'}
    js=lambda x:json.dumps(x,ensure_ascii=True)
    cpp=['// Generated source-linked presentation metadata. See presentation_tables.json.','#include "stv/frontend.hpp"','namespace stv {']
    for i,(name,words) in enumerate(palettes.items()):cpp+=['static const u16 pres_palette_%d[] = {'%i+','.join(str(x) for x in words)+'};']
    indices={name:i for i,name in enumerate(palettes)}
    for i,s in enumerate(scenes):
        cpp+=['static const UiSceneHeader pres_headers_%d[] = {'%i]+['{'+','.join(str(x) for x in r)+'},' for r in s['headers']]+['};']
        cpp+=['static const UiScenePlacement pres_placements_%d[] = {'%i]+['{'+','.join(str(x) for x in r)+'},' for r in s['placements']]+['};']
        cpp+=['static const UiScenePalette pres_palettes_%d[] = {'%i]+['{'+js(p)+','+str(len(palettes[p]))+',pres_palette_%d},'%indices[p] for p in s['palettes']]+['};']
    cpp+=['const UiSceneDefinition source_ui_scenes[] = {']+['{'+js(s['name'])+','+js(s['bank'])+',pres_headers_%d,%d,pres_placements_%d,%d,pres_palettes_%d,%d,%d,%d},'%(i,len(s['headers']),i,len(s['placements']),i,len(s['palettes']),*s['offset']) for i,s in enumerate(scenes)]+['};',f'const unsigned source_ui_scene_count={len(scenes)};','}']
    out.write_bytes(('\n'.join(cpp)+'\n').encode('utf-8'))
    out.with_suffix('.json').write_bytes((json.dumps(report,indent=2)+'\n').encode('utf-8'))
    print([(s['name'],s['bank'],len(s['headers']),len(s['placements']),len(s['palettes'])) for s in scenes])
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args();generate(a.source,a.output)
