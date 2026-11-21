"""Bound the laser animation's A10 preservation; owner/alias proof stays open."""
import argparse
import json
from pathlib import Path
import mproc_continuation_audit as a

ROOT=Path(__file__).resolve().parents[2]
# Exclude only the bad-ROM-pointer error and special-palette paths. The two
# retail rotation scripts contain three ordinary five-tick frames and a zero.
WINDOWS=((0xffe08050,0xffe080b0),(0xffe08170,0xffe08200),
         (0xffe083f0,0xffe08590),(0xffe04790,0xffe047f0),
         (0xffe048b0,0xffe04c20),(0xffe04510,0xffe04790),
         (0xffe04d50,0xffe04f20),(0xffe070b0,0xffe071e0))
CALLS={0xffe04790,0xffe0a490,0xffe04d50,0xffe04510,0xffe070e0}
GUARDS={0xffe080a0:0xffe08170,0xffe081f0:0xffe083f0,0xffe047e0:0xffe048b0}


def preserve_a10(seq):
    allowed={'MMTM','MMFM','CMPI','CMP','MOVE','ZEXT','JREQ','JRNE','JRNC','JRC',
             'JRLT','JR','CALLA','CALLR','RETS','EXGPC','CLRC','SETC','CLR',
             'XOR','ADD','ADDI','ADDK','ADDXY','SUB','SUBI','SLL','SRL','MOVX',
             'MOVY','BTST','NEG','DEC','MPYS','SUBK'}
    pcs={x.pc for x in seq}
    for x in seq:
        a.require(x.mnem in allowed,'Unreviewed animation operation')
        a.require(all(o.reg!='A10' for o in x.ops),'Animation directly touches A10')
        if x.mnem in ('MMTM','MMFM'):
            slots,_=a.mask_slots(x.mnem,x.ops[1].value,0)
            a.require('A10' not in slots,'Animation implicitly touches A10')
        if x.mnem=='MPYS':
            a.require(x.ops[-1].reg not in ('A9','A11'),'Multiply pair may touch A10')
        if x.mnem in ('CALLA','CALLR'):
            a.require(x.ops[0].value in CALLS,'Unreviewed animation helper')
        elif x.mnem.startswith('JR'):
            a.require(x.ops[0].value in pcs,'Branch leaves audited animation paths')
            # Conditional guards have an omitted fallthrough, proved from ROM data.
            following=x.pc+x.words*16
            a.require(x.mnem=='JR' or following in pcs or
                      GUARDS.get(x.pc)==x.ops[0].value,'Unjustified omitted branch')
        if x.mnem=='EXGPC':a.expect(x,'EXGPC',a.R('A7'))
    return dict(a10_direct_or_implicit_clobbers=0,instructions=len(seq),
                calls=sorted({f'{x.ops[0].value:08x}' for x in seq if x.mnem in ('CALLA','CALLR')}))


def scripts(rom,base):
    def data(address,size):
        offset=(address-a.BASE)//8
        a.require(address%16==0 and offset>=0 and offset+size<=len(rom),'Script outside ROM')
        return rom[offset:offset+size]
    result=[]
    for i in range(4):
        address=int.from_bytes(data(base+i*96+32,4),'little')
        raw=data(address,22)
        frames=[]
        for j in range(3):
            image=int.from_bytes(raw[j*6:j*6+4],'little')
            delay=int.from_bytes(raw[j*6+4:j*6+6],'little')
            a.require(a.BASE<=image<=0xffffffff and delay==5,'Unexpected image/special frame')
            frames.append(dict(image=f'{image:08x}',ticks=delay))
        a.require(raw[18:22]==bytes(4),'Rotation script lacks terminal zero')
        result.append(dict(direction=i,address=f'{address:08x}',sha256=a.sha(raw),frames=frames))
    return result


def contract(instructions,rom,compiled):
    a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail image')
    table=a.laser_table_precondition(instructions,rom,compiled)
    rotations=scripts(rom,int(table['table_address'],16))
    seq=[x for lo,hi in WINDOWS for x in a.window(instructions,lo,hi)]
    check=preserve_a10(seq)
    # ANI preserves its caller's script cursor A9 on the hardware stack.
    a.expect(instructions[0xffe04790],'MMTM',a.R('SP'),a.M('LIST',None,0xbf40))
    a.expect(instructions[0xffe04bf0],'MMFM',a.R('SP'),a.M('LIST',None,0x2fd))
    a.expect(instructions[0xffe081c0],'MOVE',a.M('INC','A9'),a.R('A0'),a.K(0))
    a.expect(instructions[0xffe081d0],'CMPI',a.I(256),a.R('A0'))
    a.expect(instructions[0xffe08070],'CMPI',a.I(a.BASE),a.R('A9'))
    a.expect(instructions[0xffe047b0],'CMPI',a.I(a.BASE),a.R('A1'))
    for pc,target in GUARDS.items():
        a.expect(instructions[pc], 'JRC' if pc==0xffe081f0 else 'JRNC', a.M('REL',None,target))
    layout=a.validate_layout(instructions,a.LAYOUT)
    a.require(layout['register_slots_bits']['A10']==0x80,'Sleep loses A10 slot')
    return dict(schema='stv.laser_animation_contract.v1',result='PASS_CONDITIONAL_A10_PRESERVATION_OWNER_OPEN',
        gate_proof=False,global_sites_closed=0,table_precondition=table,rotation_scripts=rotations,
        animation=check,sleep_context=layout,
        typed_paths=[dict(pc=f'{x.pc:08x}',shape=a.shape(x)) for x in seq],
        omitted_paths={'invalid_script':'All four table scripts are in ROM.',
                      'special_frame':'All rotation delays are5, below256.',
                      'invalid_image':'All six rotation image fields are in ROM.'},
        preconditions=['Normal LZRORB/FRANIM/ANI/PSTOP entry and returns; field0=16, field1=32.',
          'Same live process resumes with unchanged PA10 header; no recycled or externally overwritten context.',
          'Valid disjoint object/process/hardware stacks and no aliasing writes or IRQ corruption.',
          'Read-only pinned ROM and preserved ANI hardware-stack save of A9.'],
        remaining=['Prove same-owner lifetime and exclusion of aliases/alternate entry/IRQ corruption globally.',
                   'This conditional register audit does not close the last global LZRORB obligation.'])


def writer_bridge(instructions):
    """Check both caller arms, including the exact sleeping-call continuation."""
    seq=a.window(instructions,0xfff15df0,0xfff15f50)
    expected=[
        ('MOVE',a.M('DISP','A8',0x130),a.R('A1'),a.K(1)),
        ('CMP',a.R('A0'),a.R('A1')),
        ('JREQ',a.M('REL',None,0xfff15eb0)),
        ('MOVK',a.K(1),a.R('A1')),
        ('GETPC',a.R('A7')),
        ('ADDI',a.I(0x60),a.R('A7')),
        ('MOVE',a.R('A7'),a.M('DEC','A12'),a.K(1)),
        ('JA',a.I(0xffe08050)),
        ('CALLA',a.I(0xffe070b0)),
        ('CLR',a.R('A1')),
        ('MOVI',a.I(0x11d),a.R('A1')),
        ('MOVE',a.M('IND','A10'),a.R('A7'),a.K(1)),
        ('CALLA',a.I(0xffe0aba0)),
    ]
    a.require(len(seq)==len(expected),'Laser writer bridge extent differs')
    for item,pattern in zip(seq,expected):a.expect(item,*pattern)
    # GETPC produces the following instruction address. The process-stack
    # continuation therefore resumes at PSTOP, exactly like the no-rotation arm.
    a.require(seq[4].pc+seq[4].words*16+seq[5].ops[0].value==seq[8].pc,
              'Animation continuation does not rejoin PSTOP')
    return dict(start='fff15df0',writer='fff15f20',rejoin='fff15eb0',
        animation_entry='ffe08050',continuation_stack='A12 predecrement32',
        instructions=[dict(pc=f'{i.pc:08x}',shape=a.shape(i)) for i in seq])


def writer_domain(instructions,rom,compiled):
    proof=contract(instructions,rom,compiled)
    bridge=writer_bridge(instructions)
    return dict(wake_entries=sorted(set(proof['table_precondition']['conditional_child_entries'])),
        proof_scope='Both normal LASTAB producer arms through FRANIM/PSTOP to GETPRC; conditional only',
        bridge=bridge,animation_contract=proof,
        preconditions=proof['preconditions'],
        process_owner_alias_lifetime_and_alternate_entry_proof='OPEN')


def build():
    _,_,compiled,rom,instructions,pins=a.load_current(ROOT)
    proof=contract(instructions,rom,compiled)
    proof['writer_bridge']=writer_bridge(instructions)
    proof['pins']=pins+[a.pin(ROOT/'tools/scenarios/mproc_continuation_audit.py'),a.pin(Path(__file__))]
    return proof


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    path=p.parse_args().out.resolve()
    a.require(path.is_relative_to(ROOT/'build') and not path.exists(),'Use fresh build output')
    result=build();path.write_bytes((json.dumps(result,indent=2)+'\n').encode())
    print(result['result'])
