#!/usr/bin/env python3
"""Independent source-data/renderer oracle for the v0.4 scope.
No import of the native implementation or generation parser. No CPU emulation.
Validates linked metadata, two native PPMs and 4096 tank arithmetic vectors.
"""
from __future__ import annotations
import argparse, csv, hashlib, io, json, re, struct
from pathlib import Path


def number(s: str) -> int:
    s=s.strip().upper();sign=-1 if s.startswith('-') else 1;s=s.lstrip('+-')
    if s.startswith('>'):return sign*int(s[1:],16)
    if s.endswith('H'):return sign*int(s[:-1],16)
    if re.fullmatch(r'\d+',s):return sign*int(s)
    raise ValueError(f'Not a literal: {s}')


def data_from(path: Path, label: str, count: int) -> list[int]:
    text=path.read_text('latin1');lines=text.splitlines();collect=False;out=[]
    for raw in lines:
        line=raw.split(';')[0]
        match=re.match(r'^'+re.escape(label)+r'\b\s*:?\s*(.*)',line,re.I)
        if not collect:
            if not match:continue
            line=match[1];collect=True
        m=re.search(r'\.(?:WORD|LONG|BYTE)\s+(.+)',line,re.I)
        if m:
            for token in m[1].split(','):
                out.append(number(token))
                if len(out)==count:return out
        elif line.strip() and not line.lstrip().startswith('.'):
            # Labels may intentionally fall through into an adjacent table.
            if not re.fullmatch(r'\w+:?',line.strip()):raise ValueError(f'Non-data before {label} completed: {raw}')
    raise ValueError(f'Incomplete {label}: {len(out)}/{count}')


def bdd(path: Path) -> tuple[list,dict]:
    f=io.BytesIO(path.read_bytes());blocks=[];palettes={}
    count=int(f.readline())
    for _ in range(count):
        label,w,h,field=f.readline().decode('ascii').split();w,h=int(w),int(h)
        pixels=f.read(w*h);assert len(pixels)==w*h
        blocks.append((w,h,pixels,int(label,16),int(field)))
    while (line:=f.readline()):
        if not line.strip():continue
        name,n=line.decode('ascii').split();n=int(n);palettes[name]=list(struct.unpack('<'+'H'*n,f.read(n*2)))
    return blocks,palettes


def color(word: int) -> bytes:
    return bytes(((word>>shift)&31)*8+((word>>shift)&31)//4 for shift in (10,5,0))


def render(blocks, placements, palettes, foreground: bool) -> bytes:
    # Test viewport/fill, not a claim about arcade display timing or aspect.
    canvas=bytearray(color(6*1024+6*32+6)*400*260)
    def draw(p):
        flags,x,y,block,palette=p;w,h,pixels,*_=blocks[block]
        for dy in range(h):
            if not 0<=y+dy<260:continue
            sy=h-1-dy if flags&32 else dy
            for dx in range(w):
                if not 0<=x+dx<400:continue
                sx=w-1-dx if flags&16 else dx;v=pixels[sy*w+sx]
                if v==0 and flags&64:continue
                at=3*((y+dy)*400+x+dx);canvas[at:at+3]=color(palettes[palette][v])
    static=[p for p in placements if p[0]>>8<100]
    for p in sorted(static,key=lambda p:p[0]>>8):draw(p)
    if foreground:
        def z(p):
            v=p[0]>>8
            return v-100 if v in (118,119) else v+10 if 120<=v<=123 else v+43 if v in (100,101) else v+111 if v==102 else v+128
        # INSOBJ inserts before existing equal z/y. Original placement ties reverse.
        promoted=[(i,p) for i,p in enumerate(placements) if p[0]>>8>=100]
        for i,p in sorted(promoted,key=lambda ip:(z(ip[1]),ip[1][2],-ip[0])):draw(p)
    return b'P6\n400 260\n255\n'+canvas


def signed(x):
    x&=0xffffffff
    return x-(1<<32) if x&(1<<31) else x


def heading(x,y):
    ax=abs(signed(x))&0xffffffff;ay=abs(signed(y))&0xffffffff
    if (ax+ay)&0xffffffff==0:return 0
    if ax>=((3*ay)&0xffffffff):return 1+(signed(x)<0)
    if ay>=((3*ax)&0xffffffff):return 3+(signed(y)<0)
    q=int(signed(x)<0)+2*int(signed(y)<0)
    if signed(ay)>signed(ax):return q+(5 if signed(ay-ax)<0x2800 else 13)
    return q+(5 if signed(ax-ay)<0x2800 else 9)


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('source',type=Path);ap.add_argument('arena_json',type=Path);ap.add_argument('door_directory',type=Path);ap.add_argument('tank_vectors',type=Path);ap.add_argument('--report',type=Path,required=True);a=ap.parse_args()
    source=a.source;meta=json.loads(a.arena_json.read_text());bg=source/'BACKUP/BGNDTBL.ASM'
    hs=data_from(bg,'L7HDRS',64*3);headers=[hs[i:i+3] for i in range(0,len(hs),3)];assert headers==meta['headers']
    words=data_from(bg,'ARENABLKS',66*4+1);assert words[-1]==65535
    places=[]
    for i in range(0,264,4):
        flags,x,y,header=words[i:i+4];assert header&1==0
        places.append([flags,x,y,(header&4095)//2,(flags&15)+((header>>12)<<4)])
    assert places==meta['placements'];assert data_from(bg,'ARENABMOD',3)==meta['module']
    pals=[]
    for file,label in [('IMGPAL.ASM','NUARENAP'),('BGNDPAL.ASM','PRPLARN')]:
        values=data_from(source/'BACKUP'/file,label,65);assert values[0]==64;pals.append(values[1:])
    assert pals==[p['words'] for p in meta['palettes']]
    blocks,editor_palettes=bdd(source/'IMG/FINAL7.BDD')
    assert len(blocks)==64
    for h,b in zip(headers,blocks):assert h[:2]==list(b[:2])
    pixel_results={}
    for name,fore in [('linked-static-background.ppm',False),('linked-arena-before-processes.ppm',True)]:
        expected=render(blocks,places,pals,fore);native=(a.door_directory/name).read_bytes();assert expected==native,name
        pixel_results[name]={'rgb_pixels_compared':400*260,'sha256':hashlib.sha256(native).hexdigest()}
    tank=source/'TANK.ASM';xoff=data_from(tank,'XOFSET',16);yoff=data_from(tank,'YOFSET',16);xv=data_from(tank,'XVL',16);yv=data_from(tank,'YVL',16)
    cvrt=data_from(tank,'CVRT',16)+data_from(tank,'CVRT2',1)
    total=0
    for i,row in enumerate(csv.DictReader(a.tank_vectors.open())):
        r={k:int(v) for k,v in row.items()};rand=r['r'];x,y=r['x'],r['y'];d=r['direction'];index=d-1
        expected={'gate':2+(rand*3//2**32),'seek':1+4*((rand*8//2**32)%4),'distance':26+rand*25//2**32,'heading':heading(x,y)}
        expected['converted']=cvrt[expected['heading']]
        target=i//16%16+1;diff=target-d
        if abs(diff)>=9:diff=-diff
        expected['turn']=((d-1+(1 if diff>=0 else -1))%16)+1;expected['increment']=int(diff>=0)
        expected.update(bullet_x=(x+xoff[index])&0xffffffff,bullet_y=(y+yoff[index])&0xffffffff,bullet_vx=(y+xv[index])&0xffffffff,bullet_vy=(x+yv[index])&0xffffffff)
        for key,value in expected.items():assert r[key]==value,(i,key,r[key],value)
        total+=1
    assert total==4096
    result={'result':'PASS','block_headers':64,'placements':66,'linked_palette_words':128,'source_module_metadata':meta['module'],'native_pixel_comparisons':pixel_results,'tank_vectors':total,'editor_named_palette_equals_linked_palette':editor_palettes['NUARENAP']==pals[0],
        'source_sha256':{f:hashlib.sha256((source/f).read_bytes()).hexdigest() for f in ['TANK.ASM','BACKUP/BGNDTBL.ASM','BACKUP/BGNDPAL.ASM','BACKUP/IMGPAL.ASM','IMG/FINAL7.BDD']},
        'scope':'Independent literal-source/data arithmetic and renderer checks; no original CPU, MAME, hardware or complete-game equivalence test'}
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
