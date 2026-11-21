"""Source-bound retail final-circuit markers; observed flags are not whole endings."""
import argparse
import json
from pathlib import Path
import mproc_continuation_audit as a

ROOT=Path(__file__).resolve().parents[2]


def build():
    _,_,_,rom,ins,pins=a.load_current(ROOT)
    a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail image')
    src=ROOT/'original/archive/smashtv-main/WAVES.ASM'
    pp=ROOT/'build/p1/robo.bin';mp=ROOT/'build/p1/robo.bin.relmask';lp=ROOT/'build/p1/listing.json'
    bindings=[]
    for name,start,end,lines,patterns in [
        ('circuit_wrap',0xffe7e8a0,0xffe7e980,[1464,1465,1466,1467,1468,1470,1471],[
            ('MOVE',a.M('ABS',None,0x010eb610),a.R('A0'),a.K(0)),('INC',a.R('A0')),
            ('MOVE',a.R('A0'),a.M('ABS',None,0x010eb610),a.K(0)),('CMPI',a.I(3),a.R('A0')),
            ('JRNE',a.M('REL',None,0xffe7ef40)),('CLR',a.R('A0')),
            ('MOVE',a.R('A0'),a.M('ABS',None,0x010eb610),a.K(0))]),
        ('congratulations_return_marker',0xffe7ed40,0xffe7ed80,[1496,1497],[
            ('MOVK',a.K(1),a.R('A0')),('MOVE',a.R('A0'),a.M('ABS',None,0x010ee130),a.K(0))]),
        ('game_over_state',0xffe91850,0xffe918a0,[4165,4166],[
            ('MOVI',a.I(0x50),a.R('A0')),('MOVE',a.R('A0'),a.M('ABS',None,0x010865a0),a.K(0))]),
    ]:
        seq=a.window(ins,start,end);a.require(len(seq)==len(patterns),'Marker extent changed')
        for i,pat in zip(seq,patterns):a.expect(i,*pat)
        binding=a.bind_relocated_source(seq,rom,pp.read_bytes(),mp.read_bytes(),
            json.loads(lp.read_text())['instructions'],'WAVES.ASM',lines,src.read_text().splitlines())
        bindings.append(dict(name=name,**binding))
    writers=sorted(pc for pc,i in ins.items() if i.mnem=='MOVE' and len(i.ops)>1 and
        i.ops[1].kind=='ABS' and i.ops[1].value==0x010ee130)
    a.require(writers==[0xffe76ba0,0xffe7ed50],'Explicit completion flag writers changed')
    pins += [a.pin(p) for p in [src,pp,mp,lp,Path(__file__),ROOT/'tools/scenarios/mproc_continuation_audit.py']]
    return dict(result='FINAL_CIRCUIT_MARKERS_BOUND_ENDING_OPEN',gate_proof=False,pins=pins,bindings=bindings,
        markers=dict(circuit='010eb610',gratson='010ee130',gamstate='010865a0',game_over_value=0x50),
        explicit_gratson_writers=[f'{x:08x}' for x in writers],
        limits=['Exact marker windows, not complete congratulations routine or every indirect writer.',
            'Flag/state transitions require ordered live observations; a set flag alone is not whole-ending proof.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();out=args.out.resolve()
    a.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build();out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(result['result'])
