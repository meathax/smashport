"""Separate source-bound object Y stores from saved PA10 writers; aliases stay open."""
import argparse
import json
from pathlib import Path
import re
import mproc_continuation_audit as a

ROOT=Path(__file__).resolve().parents[2]


def pool_range(base,count,stride):
    a.require(type(base) is int and type(count) is int and type(stride) is int and
        count>0 and stride>=256 and base%16==0 and stride%16==0,'Invalid pool geometry')
    end=base+count*stride
    a.require(0x01000000<=base<end<=0x01100000,'Pool escapes work RAM')
    return dict(base=f'{base:08x}',end_exclusive=f'{end:08x}',count=count,stride_bits=stride)


def disjoint(first,second):
    a.require(int(first['end_exclusive'],16)<=int(second['base'],16) or
        int(second['end_exclusive'],16)<=int(first['base'],16),'Object/process pools overlap')


def offset_writes(instructions):
    # Deliberately only an explicit MOVE displacement census, not all stores:
    # postincrement, indexed pointers, MMTM and aliases need separate proofs.
    return [i for i in instructions.values() if i.mnem=='MOVE' and len(i.ops)==3 and
        i.ops[-2].kind=='DISP' and i.ops[-2].value==a.LAYOUT['PA10']]


def initials_writer(instructions,compiled):
    seq=a.window(instructions,0xffea6c70,0xffea6d70)
    a.require(len(seq)==7,'Initials timer writer extent differs')
    target=seq[1].ops[0].value
    patterns=[('MOVI',a.I(0x10b),a.R('A1')),('MOVI',a.I(target),a.R('A7')),
        ('CALLA',a.I(0xffe0aba0)),('MOVI',a.I(30),a.R('A1')),
        ('MOVE',a.R('A1'),a.M('ABS',None,0x010ee620),a.K(0)),
        ('MOVE',a.R('A1'),a.M('DISP','A0',0x80),a.K(1)),('RETS',)]
    for i,p in zip(seq,patterns):a.expect(i,*p)
    a.require(target in compiled,'Initials timer target is not compiled')
    return dict(writer='ffea6d40',child_target=f'{target:08x}',process_id=0x10b,initial_a10=30,
        owner='A0 returned by immediately preceding GETPRC; no intervening A0 writer',
        preconditions=['Normal GETPRC allocation succeeds and returns a fresh valid process.',
            'ACTIVE/FREE are disjoint and links/stack/IRQ state remain valid.'],
        lifetime_and_alias_proof='OPEN')


def player_leg_writer(instructions,pc,player):
    a.require((pc,player) in ((0xffe1ca60,0),(0xffe1cc50,1)),'Unknown player-leg store')
    seq=a.window(instructions,pc-0x70,pc+0x50)
    a.require(len(seq)==5,'Player-leg writer extent differs')
    patterns=[('MOVE',a.M('ABS',None,0x010de2b0+32*player),a.R('A0'),a.K(1)),
        ('MOVE',a.M('DISP','A0',0x300),a.R('A0'),a.K(1)),
        ('MOVE',a.R('A2'),a.M('DISP','A0',0x60),a.K(1)),
        ('MOVE',a.R('A3'),a.M('DISP','A0',0x80),a.K(1))]
    for i,p in zip(seq,patterns):a.expect(i,*p)
    a.expect(seq[-1],'CALLA',a.I(0xffe03940))
    return seq


def rescue_object_stores(instructions):
    loads=a.window(instructions,0xffe7be80,0xffe7bec0)
    stores=a.window(instructions,0xffe7bfc0,0xffe7c010)
    a.require(len(loads)==2 and len(stores)==3,'Rescue field windows differ')
    for i,p in zip(loads,[('MOVE',a.M('DISP','A1',0x100),a.R('A8'),a.K(1)),
                         ('MOVE',a.M('DISP','A1',0x300),a.R('A9'),a.K(1))]):a.expect(i,*p)
    for i,p in zip(stores,[('MOVE',a.R('A0'),a.M('DISP','A8',0x80),a.K(1)),
                          ('MOVE',a.R('A0'),a.M('DISP','A9',0x80),a.K(1)),
                          ('SLL',a.K(16),a.R('A0'))]):a.expect(i,*p)
    return loads,stores


def build():
    _,rows,compiled,rom,ins,pins=a.load_current(ROOT)
    a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail program')
    listing_path=ROOT/'build/p1/listing.json';p1path=ROOT/'build/p1/robo.bin';maskpath=ROOT/'build/p1/robo.bin.relmask'
    listing=json.loads(listing_path.read_text())['instructions'];p1=p1path.read_bytes();mask=maskpath.read_bytes()
    source_root=ROOT/'original/archive/smashtv-main';sources={};bindings=[]
    def bind(seq,module,lines):
        if module not in sources:sources[module]=(source_root/module).read_text(encoding='latin-1').split('\n')
        proof=a.bind_relocated_source(seq,rom,p1,mask,listing,module,lines,sources[module]);bindings.append(proof)
        return proof
    specs=[('objects','NDSP1.ASM',858,0xffe02790,0xffe02c10,894,0xffe02c70,0xffe02cd0,[897,898,899,900,901]),
           ('processes','MPROC.ASM',154,0xffe0a7f0,0xffe0a850,157,0xffe0a8b0,0xffe0a910,[160,161,162,163,164])]
    pools={}
    for name,module,count_line,count_pc,base_pc,base_line,start,end,lines in specs:
        count=ins[count_pc];base=ins[base_pc];loop=a.window(ins,start,end)
        a.expect(count,'MOVI',a.I(count.ops[0].value),a.R('A3'))
        a.expect(base,'MOVI',a.I(base.ops[0].value),a.R('A1'))
        a.require(len(loop)==5,'Pool initializer extent differs')
        stride=loop[1].ops[0].value
        patterns=[('MOVE',a.R('A1'),a.R('A2')),
            ('ADDI',a.I(stride),a.R('A1')),('MOVE',a.R('A1'),a.M('IND','A2'),a.K(1)),
            ('DSJS',a.R('A3'),a.M('REL',None,start)),('MOVE',a.R('A0'),a.M('IND','A2'),a.K(1))]
        for i,p in zip(loop,patterns):a.expect(i,*p)
        bind([count],module,[count_line]);bind([base],module,[base_line]);bind(loop,module,lines)
        pools[name]=pool_range(base.ops[0].value,count.ops[0].value,stride)
    a.require(pools['objects']['count']==420 and pools['objects']['stride_bits']==0x1f0 and
        pools['processes']['count']==240 and pools['processes']['stride_bits']==a.LAYOUT['PRCSIZ'],
        'Pool geometry differs from source contract')
    disjoint(pools['objects'],pools['processes'])
    hints={int(r['address'],16):r['source'] for r in rows};stores=[]
    recovered={0xffe7b0a0:('WAVES.ASM',[1002,1003,1004,1005,1006],2),
        0xffe7b0c0:('WAVES.ASM',[1003,1004,1005,1006,1007],2),
        0xffe7c990:('WAVES.ASM',[1220,1221,1222,1223,1224],2),
        0xffe7fa40:('WAVES.ASM',[1584,1585,1586,1587,1588],2),
        0xffe7fa60:('WAVES.ASM',[1585,1586,1587,1588,1589],2),
        0xffe7fc30:('WAVES.ASM',[1597,1598,1599,1600],3),
        0xffe7fc50:('WAVES.ASM',[1598,1599,1600,1601],3),
        0xffefaea0:('MUTOID.ASM',[1308,1309,1310,1312,1313],2)}
    pcs=sorted(ins)
    for i in offset_writes(ins):
        record=dict(pc=f'{i.pc:08x}',typed=a.shape(i),source_hint=hints[i.pc],classification='UNBOUND_RETAIL_POINTER_OWNER')
        match=re.fullmatch(r'([A-Z0-9]+\.ASM):(\d+)',hints[i.pc])
        if match:
            module,line=match[1],int(match[2]);proof=bind([i],module,[line])
            statement=sources[module][line-1].split(';',1)[0]
            field=re.search(r'\*A\d+\((PA10|OYVAL|SCRSUB)\)',statement,re.I)
            a.require(field is not None,'Unclassified source field at saved-register offset')
            record.update(classification={'PA10':'SAVED_PA10','OYVAL':'OBJECT_Y','SCRSUB':'PLAYER_SCORE'}[field[1].upper()],
                source_binding=proof,owner_proof='OPEN: pointer must belong to its declared live pool')
        elif i.pc in recovered:
            module,lines,before=recovered[i.pc];start=pcs.index(i.pc)-before
            seq=[ins[pc] for pc in pcs[start:start+len(lines)]]
            proof=bind(seq,module,lines)
            a.require(re.search(r'\*A\d+\(OYVAL\)',sources[module][lines[before]-1]),'Recovered field is not OYVAL')
            record.update(classification='OBJECT_Y',source_binding=proof,
                owner_proof='OPEN: source field bound in instruction context; dynamic object membership not proved')
        elif i.pc in (0xffe1ca60,0xffe1cc50):
            player=int(i.pc==0xffe1cc50);seq=player_leg_writer(ins,i.pc,player)
            first=1846 if player==0 else 1865
            record.update(classification='OBJECT_Y',source_binding=bind(seq,'ROBO.ASM',list(range(first,first+5))),
                player_registry_slot=f'{0x010de2b0+32*player:08x}',object_pointer_field_bits=0x300,
                owner_proof='OPEN: registry must hold a live player descriptor and LEG2_PTR a live object')
        elif i.pc in (0xffe7bfc0,0xffe7bfe0):
            loads,seq=rescue_object_stores(ins)
            record.update(classification='OBJECT_Y',source_binding=bind(seq,'WAVES.ASM',[1135,1136,1137]),
                object_field_loads=bind(loads,'WAVES.ASM',[1127,1128]),
                owner_proof='OPEN: valid player descriptor, live torso/legs and preservation across OFSCRTST required')
        stores.append(record)
    helper_entries={0xffe0b3c0,0xffe0b420,0xffe0b440}
    references=[dict(pc=f'{i.pc:08x}',typed=a.shape(i)) for i in ins.values()
        if any(o.kind in ('IMM','REL') and o.value in helper_entries for o in i.ops)]
    timer_writer=initials_writer(ins,compiled)
    timer_writer['source_binding']=bind(a.window(ins,0xffea6cf0,0xffea6d70),'HSTD.ASM',[554,555,556,557])
    pins += [a.pin(p) for p in [listing_path,p1path,maskpath,Path(__file__),
        ROOT/'tools/scenarios/mproc_continuation_audit.py',source_root/'MPROC.EQU',source_root/'DISP.EQU']]
    pins += [a.pin(source_root/module) for module in sources]
    return dict(schema='stv.process_alias_inventory.v1',result='POOL_RANGES_AND_EXPLICIT_FIELD_STORES_BOUND_OWNERS_OPEN',
        gate_proof=False,pins=pins,pools=pools,initializer_bindings=bindings[:6],
        stores=stores,summary={key:sum(s['classification']==key for s in stores)
            for key in ('SAVED_PA10','OBJECT_Y','PLAYER_SCORE','UNBOUND_RETAIL_POINTER_OWNER')},
        accepted_instruction_literal_helper_references=references,
        initials_timer_writer=timer_writer,
        limits=['Static pool intervals are disjoint; dynamic membership and lifetime are not proved.',
            'Offset80h is PA10 in a process, OYVAL in an object and SCRSUB in player score data; offset alone is not an owner.',
            'Only explicit MOVE displacement stores counted. Generic pointers, multi-register stores, other widths and aliases remain open.',
            'Recovered source windows classify fields; even zero unbound field names does not establish global pointer/lifetime safety.',
            'No literal references to helpers does not prove them unreachable: computed/alternate entries remain open.',
            'Initialization shape assumes normal entry, A0 zero, preserved A3 count, field1=32 and valid stack.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve()
    a.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build();out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(dict(result=result['result'],pools=result['pools'],summary=result['summary'],
        helper_references=len(result['accepted_instruction_literal_helper_references']))))
