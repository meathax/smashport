"""Reconstruct ICINIT's retail pickup descriptors; no pickup/lifetime proof."""
import argparse
import json
from pathlib import Path
import re
import struct
import mproc_continuation_audit as a
from reconstruct_wave_positions import read_words

ROOT=Path(__file__).resolve().parents[2]


def source_selectors(text):
    match=re.search(r'^ICINIT\s+\.LONG\s+([^\n]+)\n\s*\.LONG\s+([^\n]+)',text,re.M)
    a.require(match is not None,'Missing ICINIT source extent')
    labels=[s.strip() for s in (match[1]+','+match[2]).split(',')]
    a.require(len(labels)==12 and labels[0]=='0' and len(set(labels))==12 and
        all(re.fullmatch('[A-Z][A-Z0-9]*',s) for s in labels[1:]),'Invalid pickup selector table')
    return labels


def source_descriptor(text,label):
    pattern=(r'^'+re.escape(label)+r':\s*\n\s*\.LONG\s+0,0,([A-Z][A-Z0-9]*)\s*\n'
        r'\s*\.WORD\s+(\d+),DMAWNZ,(CLSNEUT\|TYPICON\|[A-Z0-9]+)\s*\n\s*\.LONG\s+0,0\s*$')
    matches=list(re.finditer(pattern,text,re.M))
    a.require(len(matches)==1,'Missing/ambiguous pickup descriptor: '+label)
    m=matches[0]
    return dict(label=label,image_symbol=m[1],z=int(m[2]),oid_expression=m[3],
        source_line=text[:m.start()].count('\n')+1)


def scalar_equates(text):
    result={}
    for label,token in re.findall(r'^([A-Z][A-Z0-9]*)\s+\.EQU\s+(>?[0-9A-Fa-f]+H?)\s*(?:;[^\n]*)?$',text,re.M):
        if token.startswith('>'):value=int(token[1:],16)
        elif token.endswith('H'):value=int(token[:-1],16)
        elif token.isdecimal():value=int(token)
        else:continue
        result[label]=value
    return result


def record(image,address):
    offset=(address-a.BASE)//8
    a.require(address%16==0 and offset>=0 and offset+26<=len(image),'Pickup record outside ROM or unaligned')
    values=struct.unpack_from('<IIIHHHII',image,offset)
    return dict(zip(('x','y','image','z','dma_flags','oid','x_velocity','y_velocity'),values))


def validate_record(value,source,image_pointer,oid):
    a.require(value['x']==value['y']==value['x_velocity']==value['y_velocity']==0,'Pickup descriptor has unexpected position/velocity')
    a.require(value['z']==source['z'] and value['oid']==oid,'Pickup Z/type/subtype differs from source')
    a.require(value['image']==image_pointer and a.BASE<=image_pointer<=0xfffffff0 and image_pointer%16==0,
              'Pickup image symbol/pointer differs')


def build():
    source_path=ROOT/'original/archive/smashtv-main/ICONS.ASM';equates_path=ROOT/'original/archive/smashtv-main/ROBO.EQU'
    text=source_path.read_text(encoding='latin-1');labels=source_selectors(text);constants=scalar_equates(equates_path.read_text())
    _,_,compiled,rom,ins,pins=a.load_current(ROOT);a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail image')
    p1path=ROOT/'build/p1/robo.bin';maskpath=ROOT/'build/p1/robo.bin.relmask';listingpath=ROOT/'build/p1/listing.json';mappath=ROOT/'build/p1/robo.map.json'
    p1=p1path.read_bytes();mask=maskpath.read_bytes();listing=json.loads(listingpath.read_text())['instructions'];symbols=json.loads(mappath.read_text())['globals']
    seq=a.window(ins,0xffed6910,0xffed6990);root=seq[1].ops[0].value
    patterns=[('SLL',a.K(5),a.R('A14')),('ADDI',a.I(root),a.R('A14')),
        ('MOVE',a.M('IND','A14'),a.R('A14'),a.K(1)),('CALLA',a.I(0xffe7c3e0))]
    a.require(len(seq)==len(patterns),'Pickup table consumer extent differs')
    for i,p in zip(seq,patterns):a.expect(i,*p)
    a.require(seq[-1].ops[0].value in compiled,'Descriptor consumer is not compiled')
    binding=a.bind_relocated_source(seq,rom,p1,mask,listing,'ICONS.ASM',[187,188,189,191],text.split('\n'))
    old_pc=int(binding['instructions'][1]['p1_pc'],16);old_root=read_words(p1,old_pc+16,1)[0]
    retail=read_words(rom,root,12);linked=read_words(p1,old_root,12)
    a.require(retail[0]==linked[0]==0 and len(set(retail))==12,'Pickup table zero/unique selectors differ')
    def descriptor(label,old_address,new_address):
        source=source_descriptor(text,label);oid=0
        for term in source['oid_expression'].split('|'):
            a.require(term in constants,'Unresolved pickup type expression');oid|=constants[term]
        old=record(p1,old_address);current=record(rom,new_address)
        a.require(source['image_symbol'] in symbols,'Missing linked image symbol')
        validate_record(old,source,int(symbols[source['image_symbol']],16),oid)
        validate_record(current,source,current['image'],oid)
        a.require({k:v for k,v in current.items() if k!='image'}=={k:v for k,v in old.items() if k!='image'},
            'Retail pickup scalar fields differ from linked source')
        old_off=(old_address-a.BASE)//8;new_off=(new_address-a.BASE)//8
        changed=[n for n in range(26) if p1[old_off+n]!=rom[new_off+n]]
        a.require(all(n in range(8,12) and mask[old_off+n] for n in changed),'Unexplained descriptor byte changes')
        return dict(source=source,descriptor=f'{new_address:08x}',
            p1_descriptor=f'{old_address:08x}',retail_fields=current,source_fields=old,
            changed_relocated_bytes=changed,descriptor_sha256=a.sha(rom[new_off:new_off+26]))
    items=[dict(selector=n,**descriptor(label,linked[n],retail[n])) for n,label in enumerate(labels[1:],1)]
    extra={};extra_tables=[]
    for label,pc,line in [('PZLST',0xffed7960,347),('CZLST',0xffed8ab0,496),('CZ2LST',0xffed8a30,492)]:
        match=re.search(r'^'+label+r'\s+\.LONG\s+([^\n]+)',text,re.M)
        a.require(match is not None,'Missing prize/cash table');names=[s.strip() for s in match[1].split(',')]
        table=ins[pc].ops[0].value;a.expect(ins[pc],'ADDI',a.I(table),a.R('A0'))
        proof=a.bind_relocated_source([ins[pc]],rom,p1,mask,listing,'ICONS.ASM',[line],text.split('\n'))
        oldpc=int(proof['instructions'][0]['p1_pc'],16);oldtable=read_words(p1,oldpc+16,1)[0]
        oldptrs=read_words(p1,oldtable,len(names));ptrs=read_words(rom,table,len(names))
        for n,name in enumerate(names):
            if name=='0':a.require(oldptrs[n]==ptrs[n]==0,'Prize/cash null slot differs');continue
            value=descriptor(name,oldptrs[n],ptrs[n])
            a.require(name not in extra or extra[name]==value,'Repeated prize/cash descriptor differs');extra[name]=value
        extra_tables.append(dict(name=label,root=f'{table:08x}',labels=names,source_binding=proof,
            pointers=[f'{p:08x}' for p in ptrs]))
    a.require(len(extra)==6,'Prize/cash descriptor census differs')
    pins += [a.pin(p) for p in [source_path,equates_path,p1path,maskpath,listingpath,mappath,Path(__file__),
        ROOT/'tools/scenarios/mproc_continuation_audit.py',ROOT/'tools/scenarios/reconstruct_wave_positions.py']]
    return dict(schema='stv.pickup_descriptors.v1',result='SEVENTEEN_RETAIL_ITEM_DESCRIPTORS_RECONSTRUCTED',
        gate_proof=False,pins=pins,table_root=f'{root:08x}',selector_zero='NULL descriptor; not a spawnable item',
        consumer=binding,items=items,prize_cash_descriptors=list(extra.values()),prize_cash_tables=extra_tables,
        limits=['This is a new inventory, not recovery of the missing historical artifact/hash.',
            'Selector and descriptor data do not prove spawn reachability, collection effects or player ownership.',
            'DMAWNZ is retained as the linked-source numeric flags value; the unavailable external SYS.INC definition is not invented.',
            'Retail image pointers are observed and source image symbols bound in P1; image-content identity is not established.',
            'Random selection pools are a separate inventory. Reward/collection effects and safe dynamic selector ranges remain open.',
            'Raw source annotations are not binding: retail FFED8A30 reads CZ2LST despite its old CZLST source hint; actual pointer-table contents distinguish them.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    path=p.parse_args().out.resolve();a.require(path.is_relative_to(ROOT/'build') and not path.exists(),'Use fresh build output')
    result=build();path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(dict(result=result['result'],items=len(result['items']),prize_cash=len(result['prize_cash_descriptors']),root=result['table_root'])))
