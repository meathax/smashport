"""Reconstruct operator menu descriptors from source-bound retail root loads."""
import argparse
import json
from pathlib import Path
import re
import struct
import mproc_continuation_audit as a
from reconstruct_wave_positions import read_words

ROOT=Path(__file__).resolve().parents[2]


def terminated_text(image,address):
    offset=(address-a.BASE)//8
    a.require(address%8==0 and 0<=offset<len(image),'Menu text outside ROM')
    end=image.find(b'\0',offset,min(offset+256,len(image)))
    a.require(end>=offset,'Menu text not terminated within256 bytes')
    return image[offset:end].decode('ascii')


def absolute_jump(image,address):
    offset=(address-a.BASE)//8
    a.require(address%16==0 and 0<=offset and offset+6<=len(image),'Jump wrapper outside ROM')
    opcode,target=struct.unpack_from('<HI',image,offset)
    a.require(opcode==0xc080,'Expected unconditional absolute jump wrapper')
    return target


def label_entry(source,label,records,module):
    labels=[n for n,s in enumerate(source.splitlines(),1) if re.match(r'^'+re.escape(label)+r'(?::|\s|$)',s)]
    a.require(len(labels)==1,'Missing/ambiguous routine label')
    candidates=[r for r in records if r[2]==module and r[3]>=labels[0]]
    a.require(candidates,'No linked instructions after routine label')
    line=min(r[3] for r in candidates)
    a.require(line-labels[0]<=5,'Routine label too far from linked entry')
    first=[r for r in candidates if r[3]==line]
    a.require(len(first)==1,'Ambiguous linked routine entry')
    return first[0]


def descriptors(text):
    lines=text.splitlines();result=[]
    for n,raw in enumerate(lines):
        match=re.match(r'^(MEN_[A-Z0-9_]+|ANY_MENU):?\s*(.*)',raw.split(';')[0])
        if not match:continue
        label,tail=match.groups();fields=[];entries=[];started=False
        for line in [tail]+lines[n+1:]:
            line=line.split(';')[0].strip()
            if not line or line.startswith('*'):continue
            if re.match(r'\w+\s+EQU\s',line):continue
            m=re.fullmatch(r'\.(LONG|WORD)\s+(.+)',line)
            if not m:break
            kind,values=m.groups();values=[v.strip() for v in values.split(',')]
            if len(fields)<13:
                a.require(len(values)==1,'Invalid menu header')
                fields.append((kind,values[0]));continue
            a.require(kind=='LONG','Invalid menu entry width')
            if values==['0']:
                started=True;break
            a.require(len(values)==3,'Invalid menu triple');entries.append(values)
        if not fields:continue
        a.require(started and fields[:2]==[('LONG','RD7FONT'),('LONG','STRCNRM')] and
                  [k for k,v in fields]==['LONG']*3+['WORD']*10 and entries,'Invalid menu descriptor '+label)
        result.append(dict(label=label,source_line=n+1,header=fields,entries=entries))
    a.require(len({r['label'] for r in result})==len(result),'Duplicate menu label')
    return result


def build():
    _,_,compiled,rom,ins,pins=a.load_current(ROOT)
    pp=ROOT/'build/p1/robo.bin';mp=ROOT/'build/p1/robo.bin.relmask';lp=ROOT/'build/p1/listing.json'
    p1=pp.read_bytes();mask=mp.read_bytes();listing=json.loads(lp.read_text())['instructions']
    cm=ROOT/'generated/private/l8_codemap.json';hints=json.loads(cm.read_text())['instructions']
    sources={name:(ROOT/'original/archive/smashtv-main'/name).read_text(encoding='latin-1') for name in ['MENU.ASM','ADJUST.ASM','TEST.ASM','DIAG.ASM']}
    symbol_path=ROOT/'build/p1/robo.map.json';symbols=json.loads(symbol_path.read_text())['globals']
    menus=[dict(d,module=name) for name in ['MENU.ASM','ADJUST.ASM'] for d in descriptors(sources[name])]
    a.require(len(menus)==11,'Expected eleven source menu descriptors')
    consumers=[]
    for line,offset,reg in [(1491,0,'A2'),(1492,32,'A3'),(1528,64,'A8')]:
        pcs=[int(pc,16) for pc,meta in hints.items() if len(meta)>2 and meta[2]==f'TEST.ASM:{line}']
        a.require(len(pcs)==1,'Ambiguous menu field consumer')
        i=ins[pcs[0]]
        a.expect(i,'MOVE',a.M('DISP','A1',offset),a.R(reg),a.K(1))
        consumers.append(a.bind_relocated_source([i],rom,p1,mask,listing,'TEST.ASM',[line],sources['TEST.ASM'].split('\n')))
    for menu in menus:
        refs=[]
        for module,source in sources.items():
            for n,line in enumerate(source.splitlines(),1):
                if not re.match(r'\s*MOVI\s+'+menu['label']+r',A8\b',line):continue
                for pc,meta in hints.items():
                    if len(meta)<3 or meta[2]!=f'{module}:{n}':continue
                    i=ins[int(pc,16)]
                    proof=a.bind_relocated_source([i],rom,p1,mask,listing,module,[n],source.split('\n'))
                    a.expect(i,'MOVI',a.I(i.ops[0].value),a.R('A8'))
                    oldpc=int(proof['instructions'][0]['p1_pc'],16)
                    refs.append(dict(retail=i.ops[0].value,p1=read_words(p1,oldpc+16,1)[0],proof=proof))
        a.require(refs and len({(r['retail'],r['p1']) for r in refs})==1,'Menu root missing/ambiguous: '+menu['label'])
        root,old=refs[0]['retail'],refs[0]['p1'];count=len(menu['entries'])
        offset=(root-a.BASE)//8;oldoff=(old-a.BASE)//8;size=32+12*count+4
        a.require(root%16==old%16==0 and offset>=0 and offset+size<=len(rom),'Menu outside ROM')
        current=rom[offset:offset+size];prior=p1[oldoff:oldoff+size]
        a.require(len(prior)==size and current[-4:]==prior[-4:]==bytes(4),'Menu terminator differs')
        a.require(current[12:32]==prior[12:32],'Menu scalar layout differs from linked source')
        menu.update(retail_root=f'{root:08x}',p1_root=f'{old:08x}',root_bindings=refs,
                    retail_header_words=list(struct.unpack_from('<10h',current,12)),
                    p1_header_words=list(struct.unpack_from('<10h',prior,12)))
        records=[]
        for number,names in enumerate(menu['entries']):
            values=struct.unpack_from('<III',current,32+12*number)
            previous=struct.unpack_from('<III',prior,32+12*number)
            # Adjustment help=2 means the middle pointer is DATA, not executable code.
            role='return' if values[1]==0 else 'adjustment_data' if values[2]==2 else 'routine'
            a.require(role!='routine' or values[1] in compiled,'Uncompiled operator routine')
            text=terminated_text(rom,values[0]);oldtext=terminated_text(p1,previous[0])
            a.require(text==oldtext,'Retail/P1 menu text differs')
            entry_proof=None;entry_open=None
            if role=='routine':
                try:
                    owners=[(module,source) for module,source in sources.items() if
                        any(re.match(r'^'+re.escape(names[1])+r'(?::|\s|$)',line) for line in source.splitlines())]
                    a.require(len(owners)==1,'Missing/ambiguous routine module')
                    module,source=owners[0]
                    entry=label_entry(source,names[1],listing,module)
                    a.require(int(entry[0],16)==previous[1],'Routine label/P1 pointer differs')
                    entry_proof=a.bind_relocated_source([ins[values[1]]],rom,p1,mask,listing,
                        module,[entry[3]],source.split('\n'))
                except a.ContractError as error:entry_open=str(error)
                if names[1]=='GO_CPUTEST':
                    a.require(re.search(r'^GO_CPUTEST:\s*\n\s*JAUC\s+CPU_CHK\s*$',sources['MENU.ASM'],re.M),
                              'CPU-test source wrapper changed')
                    prior_target=absolute_jump(p1,previous[1]);target=absolute_jump(rom,values[1])
                    a.require(prior_target==int(symbols['CPU_CHK'],16) and target in compiled,'CPU-test wrapper target differs')
                    a.expect(ins[values[1]],'JA',a.I(target))
                    entry=label_entry(sources['DIAG.ASM'],'CPU_CHK',listing,'DIAG.ASM')
                    a.require(int(entry[0],16)==prior_target,'CPU-test linked entry differs')
                    entry_proof=dict(wrapper='GO_CPUTEST JAUC CPU_CHK',retail_target=f'{target:08x}',
                        p1_target=f'{prior_target:08x}',target_entry=a.bind_relocated_source([ins[target]],rom,p1,mask,
                        listing,'DIAG.ASM',[entry[3]],sources['DIAG.ASM'].split('\n')))
                    entry_open=None
            records.append(dict(index=number+1,source_symbols=names,retail=[f'{v:08x}' for v in values],
                                p1=[f'{v:08x}' for v in previous],middle_pointer_role=role,text=text,
                                routine_entry_binding=entry_proof,routine_entry_open=entry_open))
        menu['records']=records
    pins.extend(a.pin(p) for p in [pp,mp,lp,cm,symbol_path,Path(__file__),ROOT/'tools/scenarios/mproc_continuation_audit.py',
        ROOT/'tools/scenarios/reconstruct_wave_positions.py',ROOT/'original/archive/smashtv-main/MENU.EQU',
        *[ROOT/'original/archive/smashtv-main'/n for n in sources]])
    return dict(schema='stv.operator_inventory.v1',result='MENU_ROOTS_BOUND_RETAIL_DESCRIPTORS_OBSERVED',gate_proof=False,pins=pins,menus=menus,field_consumers=consumers,
        summary=dict(menus=len(menus),entries=sum(len(m['records']) for m in menus),
            routine_entries_bound=sum(bool(r['routine_entry_binding']) for m in menus for r in m['records']),
            routine_entries_open=sum(bool(r['routine_entry_open']) for m in menus for r in m['records'])),
        limits=['Root MOVI and text/routine/help field consumers are source-bound; menu text matches P1. Bound routine entries prove only the first instruction, not whole bodies or effects; help identities and full traversal remain open.',
                'Adjustment middle pointers are data when help selector is2; do not treat them as missing code.',
                'Observed records do not prove all pages reached or operator effects, CMOS persistence and diagnostics behavior.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);args=p.parse_args()
    out=args.out.resolve();a.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build();out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n');print(json.dumps(result['summary']))
