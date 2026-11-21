"""Bind per-wave pickup selector pools and retain source-extent spill reads."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import reconstruct_pickups as pickups
import mproc_continuation_audit as a
from reconstruct_wave_positions import read_words

ROOT=Path(__file__).resolve().parents[2]


def pointer_labels(text,label):
    lines=text.splitlines();start=next((n for n,s in enumerate(lines) if s.strip()==label+':'),None)
    a.require(start is not None,'Missing circuit pickup table');values=[]
    for raw in lines[start+1:]:
        s=raw.split(';',1)[0].strip()
        if not s:continue
        if not s.startswith('.LONG'):break
        values.extend(t.strip() for t in s[5:].split(','))
    a.require(values and values[0]=='0' and all(re.fullmatch('ICN[0-9]+',v) for v in values[1:]),'Invalid circuit pickup table')
    return values


def source_pools(text,constants):
    result={};labels=[];values=[]
    def flush():
        if labels:
            a.require(values,'Empty named pickup pool')
            for name in labels:
                a.require(name not in result,'Duplicate pool label');result[name]=list(values)
    for raw in text.splitlines():
        s=raw.split(';',1)[0].strip()
        if not s:continue
        match=re.fullmatch(r'(ICN\d+):',s)
        if match:
            if values:flush();labels=[];values=[]
            labels.append(match[1]);continue
        if not labels:continue
        if s.startswith('.BYTE'):
            for token in s[5:].split(','):
                token=token.strip();value=int(token) if token.isdecimal() else constants.get(token)
                a.require(type(value) is int and 0<=value<=11,'Unknown pool selector')
                values.append(value)
        else:
            flush();labels=[];values=[]
    flush();return result


def sampled_slots(raw,extent):
    a.require(len(raw)==21 and type(extent) is int and extent>0 and all(v<=11 for v in raw),'Invalid selector observation')
    return dict(index_domain=[1,20],selector_counts=dict(sorted(Counter(raw[1:21]).items())),
        beyond_source_extent=[dict(index=i,selector=raw[i]) for i in range(1,21) if i>=extent])


def build():
    src=ROOT/'original/archive/smashtv-main/ICONS.ASM';util=ROOT/'original/archive/smashtv-main/UTIL.ASM'
    text=src.read_text(encoding='latin-1');constants=pickups.scalar_equates(text);pools=source_pools(text,constants)
    _,_,_,rom,ins,pins=a.load_current(ROOT);a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail image')
    p1path=ROOT/'build/p1/robo.bin';maskpath=ROOT/'build/p1/robo.bin.relmask';listingpath=ROOT/'build/p1/listing.json'
    p1=p1path.read_bytes();mask=maskpath.read_bytes();listing=json.loads(listingpath.read_text())['instructions'];bindings=[]
    def bind(seq,module,lines):
        source=text if module=='ICONS.ASM' else util.read_text(encoding='latin-1')
        proof=a.bind_relocated_source(seq,rom,p1,mask,listing,module,lines,source.split('\n'));bindings.append(proof);return proof
    # The multiply high word is0..19 for an arbitrary32-bit random value and range20;
    # RANDU's INC makes the selected byte index1..20, not0..19.
    uniform=a.window(ins,0xffe06cb0,0xffe06d40)
    patterns=[('MOVE',a.R('A1'),a.M('DEC','SP'),a.K(1)),('MOVE',a.R('A0'),a.R('A1')),
        ('CALLR',a.M('REL',None,0xffe06b50)),('MPYU',a.R('A1'),a.R('A0')),('INC',a.R('A0')),
        ('MMFM',a.R('SP'),a.M('LIST',None,2)),('RETS',)]
    a.require(len(uniform)==len(patterns),'RANDU extent differs')
    for i,p in zip(uniform,patterns):a.expect(i,*p)
    bind(uniform[1:],'UTIL.ASM',list(range(911,917)))
    # Bind the complete range wrapper, including its actual banked save/restore bytes.
    ranged=a.window(ins,0xffe06df0,0xffe06ec0)
    bind(ranged[2:7],'UTIL.ASM',list(range(942,947)))
    bind(ranged[-1:],'UTIL.ASM',[949])
    # Macro-expanded PUSH/PULL records have synthetic listing module names.
    # Keep the whole unchanged utility windows byte-exact to P1 rather than
    # falsely binding those expansions as ordinary source instruction rows.
    helper_bytes=[]
    for seq in (uniform,ranged):
        first=seq[0].pc;end=seq[-1].pc+seq[-1].words*16
        lo=(first-a.BASE)//8;hi=(end-a.BASE)//8
        a.require(p1[lo:hi]==rom[lo:hi],'Utility macro-expanded bytes differ')
        helper_bytes.append(dict(start=f'{first:08x}',end_exclusive=f'{end:08x}',sha256=a.sha(rom[lo:hi])))
    for pc,p in {0xffe06e10:('SUB',a.R('B0'),a.R('B1')),
        0xffe06e20:('MOVE',a.R('B1'),a.R('A0')),
        0xffe06e30:('CALLR',a.M('REL',None,0xffe06cb0)),
        0xffe06e50:('MOVE',a.R('B0'),a.R('A1')),0xffe06e60:('ADD',a.R('A1'),a.R('A0'))}.items():a.expect(ins[pc],*p)
    select=a.window(ins,0xffed6600,0xffed6680)
    patterns=[('CLR',a.R('B0')),('MOVK',a.K(20),a.R('B1')),('CALLA',a.I(0xffe06df0)),
        ('SLL',a.K(3),a.R('A0')),('ADD',a.R('A9'),a.R('A0')),('MOVB',a.M('IND','A0'),a.R('A14'))]
    a.require(len(select)==len(patterns),'Random selector consumer differs')
    for i,p in zip(select,patterns):a.expect(i,*p)
    def raw(image,address,size):
        off=(address-a.BASE)//8;a.require(address%8==0 and off>=0 and off+size<=len(image),'Pool outside ROM')
        return list(image[off:off+size])
    tables=[];records={}
    for circuit,label,pc,line in [(0,'ICONTYP',0xffed6060,111),(1,'ICONTYP2',0xffed6010,107),(2,'ICONTYP3',0xffed5fc0,103)]:
        names=pointer_labels(text,label);root=ins[pc].ops[0].value;a.expect(ins[pc],'ADDI',a.I(root),a.R('A0'))
        binding=bind([ins[pc]],'ICONS.ASM',[line]);oldpc=int(binding['instructions'][0]['p1_pc'],16)
        oldroot=read_words(p1,oldpc+16,1)[0];old=read_words(p1,oldroot,len(names));new=read_words(rom,root,len(names))
        a.require(old[0]==new[0]==0,'Circuit null pool differs')
        tables.append(dict(circuit=circuit,root=f'{root:08x}',source_extent=len(names),pools=[dict(wave=i,label=n,address=f'{new[i]:08x}') for i,n in enumerate(names)]))
        for index,name in enumerate(names[1:],1):
            expected=pools[name];a.require(raw(p1,old[index],len(expected))==expected,'Linked pool/source differs')
            current=raw(rom,new[index],len(expected))
            observation=sampled_slots(raw(rom,new[index],21),len(expected))
            value=dict(source_slot_label=name,address=f'{new[index]:08x}',source_bytes=expected,source_extent=len(expected),
                retail_bytes_in_corresponding_source_extent=current,
                source_differences=[dict(index=n,source=x,retail=y) for n,(x,y) in enumerate(zip(expected,current)) if x!=y],**observation)
            key=(name,new[index])
            a.require(key not in records or records[key]==value,'Repeated slot comparison changed data');records[key]=value
    a.require({name for name,_ in records}==set(pools),'Not all named source pools accounted for')
    pins += [a.pin(p) for p in [src,util,p1path,maskpath,listingpath,Path(__file__),Path(pickups.__file__),ROOT/'tools/scenarios/mproc_continuation_audit.py',ROOT/'tools/scenarios/reconstruct_wave_positions.py']]
    return dict(schema='stv.pickup_pools.v1',result='RETAIL_PICKUP_POOLS_RECONSTRUCTED_EXTENT_SPILLS_RETAINED',gate_proof=False,
        pins=pins,tables=tables,pools=list(records.values()),source_bindings=bindings,exact_p1_utility_windows=helper_bytes,
        selector_consumer=[dict(pc=f'{i.pc:08x}',typed=a.shape(i)) for i in select],
        summary=dict(source_labels=len(pools),slot_comparisons=len(records),unique_addresses=len({v['address'] for v in records.values()}),
            source_differences=[v['source_slot_label'] for v in records.values() if v['source_differences']],
            pools_with_source_extent_spills=[v['source_slot_label'] for v in records.values() if v['beyond_source_extent']]),
        limits=['Normal range-wrapper entry and preserved helper/stack state assumed; no probability or dynamic spawn proof.',
            'Source labels identify corresponding circuit/wave slots, not proved retail pool identity. Retail changes are retained.',
            'Source extent is kept separate from actual indexed reads1..20; spill values are raw observations, not a source or retail table extension.',
            'Zero selector suppresses an item; nonzero selectors refer to ICINIT. No pickup effects/ownership or historical hash recovery claimed.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);path=p.parse_args().out.resolve()
    a.require(path.is_relative_to(ROOT/'build') and not path.exists(),'Use fresh build output');result=build()
    path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n');print(json.dumps(result['summary']))
