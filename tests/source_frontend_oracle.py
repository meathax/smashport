#!/usr/bin/env python3
"""Independent source-byte/native-output checks; no generator or C++ parser import.
Pixel comparisons validate selected native renderers, NOT original board video.
"""
from __future__ import annotations
import argparse,csv,hashlib,json,re,struct,zipfile,zlib
from pathlib import Path
from source_arena_tank_oracle import number,data_from,bdd,color

def raw_tokens(path:Path,label:str,count:int,kind='LONG')->list[str]:
    active=False;result=[]
    for raw in path.read_text('latin1').splitlines():
        line=raw.split(';',1)[0]
        m=re.match(r'^'+re.escape(label)+r'\b\s*:?\s*(.*)',line,re.I)
        if not active:
            if not m:continue
            active=True;line=m[1]
        m=re.search(r'\.'+kind+r'\s+(.+)',line,re.I)
        if m:
            result.extend(x.strip() for x in m[1].split(','))
            if len(result)>=count:return result[:count]
    raise AssertionError((path,label,count))

def image_bank(path:Path):
    raw=path.read_bytes();count,_,directory=struct.unpack_from('<HHI',raw);out={}
    for i in range(count):
        header=directory+42*i;name=raw[header:header+16].split(b'\0')[0].decode('ascii')
        ax,ay,w,h,sel,offset=struct.unpack_from('<hhHHHI',raw,header+16);pitch=4*((w+3)//4)
        pixels=b''.join(raw[offset+y*pitch:offset+y*pitch+w] for y in range(h));record=(ax,ay,w,h,pixels)
        if name in out:assert out[name]==record
        out[name]=record
    return out

def put(frame,x,y,rgb):
    if 0<=x<400 and 0<=y<260:frame[(y*400+x)*3:(y*400+x)*3+3]=rgb

def ppm(frame):return b'P6\n400 260\n255\n'+bytes(frame)
def test(source:Path,meta:Path,captures:Path,vectors:Path):
    ui=json.loads((meta/'frontend_tables.json').read_text());pres=json.loads((meta/'presentation_tables.json').read_text());results={}
    def compare(name,expected):
        actual=(captures/name).read_bytes();assert actual==expected,name
        results[name]={'pixels':400*260,'sha256':hashlib.sha256(actual).hexdigest()}
    for fi,(label,bank) in enumerate([('RD7FONT','TROGF7.IMG'),('RD15FONT','TROGF15.IMG')]):
        names=raw_tokens(source/'TEXT.ASM',label,93);assert names==ui['fonts'][fi]
        images=image_bank(source/'IMG'/bank);frame=bytearray(400*260*3)
        for i,name in enumerate(names):
            ax,ay,w,h,pixels=images[name];ox=10+(i%16)*24-ax;oy=12+(i//16)*36-ay
            for y in range(h):
                for x in range(w):
                    if pixels[y*w+x]:put(frame,ox+x,oy+y,b'\xff\xff\xff')
        compare(f'source-font-{fi}.ppm',ppm(frame))
    total_headers=total_placements=0
    for si,expected in enumerate(pres['scenes']):
        mod=expected['name'];bg=source/'BACKUP/BGNDTBL.ASM'
        dims=data_from(bg,mod,3);assert dims==expected['module']
        place_label,header_label,palette_label=raw_tokens(bg,mod,3)
        nh=len(expected['headers']);rows=data_from(bg,header_label,nh*3);headers=[rows[i:i+3] for i in range(0,len(rows),3)];assert headers==expected['headers']
        words=data_from(bg,place_label,dims[2]*4+1);assert words[-1]==65535
        placements=[]
        for i in range(0,len(words)-1,4):
            flags,x,y,index=words[i:i+4];assert index%2==0
            placements.append([flags,x,y,(index&4095)//2,(flags&15)+((index>>12)<<4)])
        assert placements==expected['placements']
        palnames=[]
        for filename in ['BGNDPAL.ASM','IMGPAL.ASM']:
            path=source/'BACKUP'/filename
            if re.search(r'^'+palette_label+r'\s*:?',path.read_text('latin1'),re.M|re.I):
                assert not palnames;palnames=raw_tokens(path,palette_label,len(expected['palettes']))
        assert palnames==expected['palettes'];pals=[]
        for label in palnames:
            found=[]
            for filename in ['BGNDPAL.ASM','IMGPAL.ASM']:
                path=source/'BACKUP'/filename
                if re.search(r'^'+label+r'\s*:?',path.read_text('latin1'),re.M|re.I):
                    n=data_from(path,label,1)[0]&511;found.append(data_from(path,label,n+1)[1:])
            assert len(found)==1 and found[0]==pres['palettes'][label];pals.append(found[0])
        blocks,_=bdd(source/'IMG'/(expected['bank']+'.BDD'));assert len(blocks)==nh
        frame=bytearray(400*260*3);ox,oy=expected['offset']
        for flags,x,y,block,palette in sorted(placements,key=lambda p:p[0]>>8):
            w,h,raw,*_=blocks[block];assert [w,h]==headers[block][:2]
            for sy in range(h):
                for sx in range(w):
                    pixel=raw[(h-1-sy if flags&32 else sy)*w+(w-1-sx if flags&16 else sx)]
                    if not pixel and flags&64:continue
                    put(frame,ox+x+sx,oy+y+sy,color(pals[palette][pixel]))
        compare(f'source-scene-{si}.ppm',ppm(frame));total_headers+=nh;total_placements+=len(placements)
    # Verify editor title images against the actual linked headers/palettes,
    # rather than trusting their names or normalizing signed anchor fields.
    art=(source/'IMG/SMASHLOG.IMG').read_bytes()
    count,_,directory=struct.unpack_from('<HHI',art)
    title_records=[]
    for i in range(count):
        at=directory+42*i
        name=art[at:at+16].split(b'\0')[0].decode('ascii')
        if name not in ('SMASH','TVBOX'):continue
        ax,ay,w,h,selector,_=struct.unpack_from('<hhHHHI',art,at+16)
        linked=data_from(source/'BACKUP/IMGTBL.ASM',name,4)
        assert linked==[w,h,ax,ay],name
        at=directory+42*count+26*((selector&255)-3)
        colors,offset=struct.unpack_from('<HI',art,at+12)
        words=list(struct.unpack_from('<'+'H'*colors,art,offset))
        palette='PALSMSH' if name=='SMASH' else 'PALTV'
        assert words==data_from(source/'BACKUP/IMGPAL.ASM',palette,colors+1)[1:]
        title_records.append({'name':name,'geometry':linked,'palette':palette,'palette_words':colors,'result':'PASS'})
    assert len(title_records)==2
    def fmt(value):return f'{value//60:,}:{value%60:02d}'
    def rounded(a,b):return 0 if b==0 else a//b+int(((a%b)*2)&0xffffffff>=b)
    n=0
    for row in csv.DictReader(vectors.open()):
        a={k:int(row[k]) for k in ('a14','a15','a16','a17','a18','a19')}
        plays=sum(a[k] for k in ('a14','a15','a16','a17'))&0xffffffff
        weighted=(a['a18']+2*a['a19'])&0xffffffff;elapsed=(a['a18']+a['a19'])&0xffffffff
        assert row['player_average']==fmt(rounded((weighted*5)&0xffffffff,plays))
        assert row['elapsed_average']==fmt(rounded((elapsed*5)&0xffffffff,plays));n+=1
    assert n==4096
    native=(vectors.parent/'native-operator-state.bin').read_bytes();assert len(native)==640 and native[:8]==b'STVNV5\0\0'
    assert struct.unpack_from('<I',native,48)[0]==zlib.crc32(native[64:])
    assert native[16:48]==bytes.fromhex(hashlib.sha256(json.dumps(ui['source_files'],sort_keys=True,separators=(',',':')).encode()).hexdigest())
    return {'result':'PASS','font_references':186,'linked_headers':total_headers,'linked_placements':total_placements,'pixel_comparisons':results,'title_header_palette_matches':title_records,'audit_vectors':n,'native_envelope_crc':'PASS','scope':'Independent literal source/byte/render arithmetic checks. Not MAME, PCB, full-game or Windows execution.'}
if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('source',type=Path);ap.add_argument('generated',type=Path);ap.add_argument('captures',type=Path);ap.add_argument('vectors',type=Path);ap.add_argument('--report',type=Path,required=True);a=ap.parse_args();result=test(a.source,a.generated,a.captures,a.vectors);a.report.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
