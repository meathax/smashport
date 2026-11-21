#!/usr/bin/env python3
"""Extract L7HDRS / ARENABLKS and linked palette words, not editor coordinates.
Only literal data directives are accepted. Original files are never modified.
This preserves the surviving BACKUP build-table snapshot; retail identity is
not asserted. Pixel payloads remain borrowed from the separately supplied BDD.
"""
from __future__ import annotations
import argparse,hashlib,json,pathlib
from generate_animations import parse_source,number

def generate(source:pathlib.Path,output:pathlib.Path)->dict:
    labels,records=parse_source(source/'BACKUP/BGNDTBL.ASM')
    headers=records[labels['L7HDRS'][0]:labels['ARENA3BLKS'][0]]
    if len(headers)%3:raise ValueError('Nonliteral L7 header size')
    dims=[]
    for i in range(0,len(headers),3):
        if [x[0] for x in headers[i:i+3]]!=['WORD','WORD','LONG']:raise ValueError('L7 header shape')
        dims.append([number(x[1]) for x in headers[i:i+3]])
    i,line=labels['ARENABLKS'];placements=[]
    while True:
        if records[i][0]!='WORD':raise ValueError('Nonliteral ARENABLKS')
        flags=number(records[i][1]);i+=1
        if flags==65535:break
        rest=records[i:i+3];i+=3
        if any(x[0]!='WORD' for x in rest):raise ValueError('Incomplete placement')
        x,y,hdr=[number(x[1]) for x in rest]
        if hdr&1 or (hdr&0xfff)//2>=len(dims):raise ValueError('Unresolved block header')
        pal=(flags&15)+((hdr>>12)<<4)
        if pal>=2:raise ValueError('Unexpected palette table reference')
        placements.append([flags,x,y,(hdr&0xfff)//2,pal])
    mi=labels['ARENABMOD'][0];module=[number(records[mi+j][1]) for j in range(3)]
    if len(placements)!=module[2]:raise ValueError('Module count and placement list disagree')
    palettes=[]
    for file,name in [('IMGPAL.ASM','NUARENAP'),('BGNDPAL.ASM','PRPLARN')]:
        ls,rs=parse_source(source/'BACKUP'/file);off,ln=ls[name];n=number(rs[off][1]);words=rs[off+1:off+1+n]
        if n!=64 or any(x[0]!='WORD' for x in words):raise ValueError('Unsupported linked palette')
        palettes.append({'name':name,'module':file,'line':ln,'words':[number(x[1]) for x in words]})
    out=['// Generated literal source data; see arena_tables.json for provenance.','#include "stv/source_arena.hpp"','namespace stv {']
    out+=['const ArenaHeader source_arena_headers[] = {']+[' {%d,%d,0x%08xu},'%tuple(d) for d in dims]+['};']
    out+=['const ArenaPlacement source_arena_placements[] = {']+[' {0x%04x,%d,%d,%d,%d},'%tuple(d) for d in placements]+['};']
    for idx,pal in enumerate(palettes):
        b=[z for w in pal['words'] for z in [w&255,w>>8]]
        out+=['static const u8 arena_palette_%d[] = {'%idx,','.join(str(z) for z in b),'};']
    out+=['const EditorPalette source_arena_palettes[2] = {']+[' {"%s",64,{arena_palette_%d,128}},'%(p['name'],idx) for idx,p in enumerate(palettes)]+['};']
    out+=['const unsigned source_arena_header_count=%d;'%len(dims),'const unsigned source_arena_placement_count=%d;'%len(placements),'}']
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text('\n'.join(out)+'\n',encoding='utf-8',newline='\n')
    files=['BACKUP/BGNDTBL.ASM','BACKUP/BGNDPAL.ASM','BACKUP/IMGPAL.ASM','IMG/FINAL7.BDD']
    report={'kind':'literal source-linked ARENA metadata; not retail equivalence','source_files':{f:hashlib.sha256((source/f).read_bytes()).hexdigest() for f in files},'module':module,'label':'ARENABLKS','line':line,'headers':dims,'placements':placements,'palettes':palettes}
    output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n');return report
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args();r=generate(a.source,a.output);print('Extracted',len(r['headers']),'block headers,',len(r['placements']),'placements, 2 linked palettes')
