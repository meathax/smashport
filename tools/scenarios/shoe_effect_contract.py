"""Bind shoe assignment and movement consumption to retail/source bytes."""
import argparse
import json
from pathlib import Path
import mproc_continuation_audit as a
from reconstruct_wave_positions import read_words
ROOT=Path(__file__).resolve().parents[2]


def validate_assignment(seq):
    patterns=[('MMTM',a.R('SP'),a.M('LIST',None,0x6000)),
        ('MOVE',a.M('DISP','A0',0x160),a.R('A0'),a.K(1)),
        ('MOVE',a.M('DISP','A0',0x2c0),a.R('A0'),a.K(1)),
        ('MOVI',a.I(300),a.R('A1')),
        ('MOVE',a.M('ABS',None,0x010eb610),a.R('A2'),a.K(0)),
        ('CMPI',a.I(2),a.R('A2')),('JRNE',a.M('REL',None,0xffe30330)),
        ('MOVI',a.I(500),a.R('A1')),
        ('MOVB',a.R('A1'),a.M('ABS',None,0x010eeab0)),
        ('MOVE',a.R('A1'),a.M('DISP','A0',0x180),a.K(0))]
    a.require(len(seq)==len(patterns),'Shoe assignment extent differs');a.contiguous(seq)
    for ins,pattern in zip(seq,patterns):a.expect(ins,*pattern)


def validate_movement(seq):
    patterns=[('MOVE',a.M('DISP','A13',0x180),a.R('A0'),a.K(0)),
        ('JREQ',a.M('REL',None,0xffe1feb0)),('DEC',a.R('A0')),
        ('MOVE',a.R('A0'),a.M('DISP','A13',0x180),a.K(0))]
    for x,y in [(0xffe1fb20,0xffe1fc60),(0xffe1f8a0,0xffe1f9e0)]:
        for address,reg in [(x,'A0'),(y,'A1')]:
            patterns += [('MOVI',a.I(address),a.R('A2')),('ADD',a.R('A6'),a.R('A2')),
                         ('MOVE',a.M('IND','A2'),a.R(reg),a.K(1))]
        if x==0xffe1fb20:patterns.append(('JR',a.M('REL',None,0xffe1ff50)))
    patterns += [('MOVE',a.R('A1'),a.M('DISP','A8',0x40),a.K(1)),
        ('MOVE',a.R('A0'),a.M('DISP','A8',0x20),a.K(1)),
        ('MOVE',a.M('DISP','A13',0x100),a.R('A3'),a.K(1)),
        ('MOVE',a.R('A1'),a.M('DISP','A3',0x40),a.K(1)),
        ('MOVE',a.R('A0'),a.M('DISP','A3',0x20),a.K(1)),('RETS',)]
    a.require(len(seq)==len(patterns),'Shoe movement extent differs');a.contiguous(seq)
    for ins,pattern in zip(seq,patterns):a.expect(ins,*pattern)


def validate_cleanup(seq):
    patterns=[('MOVE',a.R('A8'),a.R('A0')),
        ('CALLA',a.I(0xffe031d0)),('MOVI',a.I(0xffe633f0),a.R('A0')),
        ('CALLA',a.I(0xffe609e0)),('MMFM',a.R('SP'),a.M('LIST',None,6)),('RETS',)]
    a.require(len(seq)==len(patterns),'Shoe cleanup extent differs');a.contiguous(seq)
    for ins,pattern in zip(seq,patterns):a.expect(ins,*pattern)


def build():
    _,_,_,rom,ins,pins=a.load_current(ROOT);a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail image')
    pp=ROOT/'build/p1/robo.bin';mp=ROOT/'build/p1/robo.bin.relmask';lp=ROOT/'build/p1/listing.json'
    p1=pp.read_bytes();mask=mp.read_bytes();listing=json.loads(lp.read_text())['instructions']
    bindings={};tables=[]
    for name,start,end,validate,module,lines in [
        ('assignment',0xffe30230,0xffe30380,validate_assignment,'SHOTS.ASM',
         [577,578,579,580,581,582,583,584,586,587]),
        ('cleanup',0xffe30380,0xffe30450,validate_cleanup,'SHOTS.ASM',[588,589,590,591,592,593]),
        ('movement',0xffe1fda0,0xffe20000,validate_movement,'ROBO.ASM',
         [2327,2328,2329,2330,2332,2333,2334,2336,2337,2338,2339,
          2341,2342,2343,2345,2346,2347,2349,2350,2353,2354,2355,2356])]:
        seq=a.window(ins,start,end);validate(seq);src=ROOT/'original/archive/smashtv-main'/module
        bindings[name]=a.bind_relocated_source(seq,rom,p1,mask,listing,module,lines,src.read_text().splitlines())
        pins.append(a.pin(src))
        if name=='cleanup':
            oldpc=int(bindings[name]['instructions'][2]['p1_pc'],16)
            oldroot=read_words(p1,oldpc+16,1)[0]
            values=read_words(rom,0xffe633f0,2)
            a.require(values==read_words(p1,oldroot,2)==[0x0034f4ee,0x00008036],
                      'Shoe WOO2 descriptor differs from linked source')
            tables.append(dict(name='WOO2',address='ffe633f0',values=values,
                               source='original/archive/smashtv-main/SOUNDS.ASM:562'))
            pins.append(a.pin(ROOT/'original/archive/smashtv-main/SOUNDS.ASM'))
        if name=='movement':
            for index,label in [(4,'fast_x'),(7,'fast_y'),(11,'normal_x'),(14,'normal_y')]:
                oldpc=int(bindings[name]['instructions'][index]['p1_pc'],16)
                oldroot=read_words(p1,oldpc+16,1)[0];root=seq[index].ops[0].value
                values=read_words(rom,root,10)
                a.require(values==read_words(p1,oldroot,10),'Velocity table differs from linked source')
                tables.append(dict(name=label,address=f'{root:08x}',values=values))
    pins += [a.pin(p) for p in (pp,mp,lp,Path(__file__),ROOT/'tools/scenarios/mproc_continuation_audit.py',ROOT/'tools/scenarios/reconstruct_wave_positions.py')]
    return dict(result='SHOE_ASSIGNMENT_AND_MOVEMENT_BOUND',gate_proof=False,pins=pins,bindings=bindings,tables=tables,
        facts=['Object+0x160 -> torso process; torso+0x2c0 -> leg process; timer at leg+0x180.',
               'Assignment overwrites timer with300, or500 for rawcircuit2; icon flag stores low8 bits44 or244.',
               'Nonzero timer decremented then fast table selected; zero on entry selects normal table.',
               'Cleanup calls DELOBJ on A8, requests WOO2 through ONESND, restores A1/A2 and returns.',
               'WOO2 descriptor: priority f4ee, time0034, command8036, terminator0000.',
               'Velocity written to leg object and linked torso object.'],
        limits=['Cleanup call sites and sound descriptor are source-bound; helper effects require execution evidence.',
                'Sound-port writes are not physical listening proof; sound admission may legitimately reject WOO2.',
                'Full sound-helper execution requires retail field setup: signed16 field0,32-bit field1; default CPU reset fields are insufficient.',
                'Requires valid owner links and direction offset in0..9 times32; lifetime/caller bounds remain open.',
                'Movement invocation cadence is not inferred as wall-clock seconds.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    out=p.parse_args().out.resolve();a.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build();out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(result['result'])
