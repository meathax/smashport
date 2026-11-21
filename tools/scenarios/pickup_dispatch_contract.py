"""Bind retail ICPL selection and its table; collection effects remain separate."""
import argparse
import json
from pathlib import Path
import mproc_continuation_audit as a
from reconstruct_wave_positions import read_words

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('POW1ON','POW2ON','SPRAYON','PODON','EXTRA','GRENON','RINGON','NSTPON',
         'KEYON','SHOEON','SMRTON','TPZ1','TPZ2','TPZ3','TCSH','TITM')


def validate(seq):
    patterns = [
        ('MMTM',a.R('SP'),a.M('LIST',None,0x6000)),
        ('MOVE',a.M('DISP','A2',0x150),a.R('A2'),a.K(0)),
        ('ANDI',a.I(3),a.R('A2')), ('CMPI',a.I(3),a.R('A2')),
        ('JREQ',a.M('REL',None,0xffe5e000)),
        ('MOVE',a.M('DISP','A1',0x150),a.R('A1'),a.K(0)),
        ('ANDI',a.I(63),a.R('A1')), ('SLL',a.K(5),a.R('A1')),
        ('ADDI',a.I(0xffe5e040),a.R('A1')),
        ('MOVE',a.M('IND','A1'),a.R('A1'),a.K(1)),
        ('MOVE',a.R('A1'),a.R('B0')),
        ('MMFM',a.R('SP'),a.M('LIST',None,6)), ('RETS',),
        ('CLR',a.R('B0')), ('MMFM',a.R('SP'),a.M('LIST',None,6)), ('RETS',)]
    a.require(len(seq)==len(patterns),'Pickup dispatcher extent differs')
    a.contiguous(seq)
    for ins, pattern in zip(seq, patterns): a.expect(ins,*pattern)


def build():
    _,_,compiled,rom,ins,pins=a.load_current(ROOT)
    a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail image')
    seq=a.window(ins,0xffe5de80,0xffe5e040);validate(seq)
    src=ROOT/'original/archive/smashtv-main/COLL2.ASM'
    pp=ROOT/'build/p1/robo.bin';mp=ROOT/'build/p1/robo.bin.relmask'
    lp=ROOT/'build/p1/listing.json';sp=ROOT/'build/p1/robo.map.json'
    source=src.read_text().splitlines();p1=pp.read_bytes()
    binding=a.bind_relocated_source(seq,rom,p1,mp.read_bytes(),
        json.loads(lp.read_text())['instructions'],'COLL2.ASM',
        [957,958,960,961,962,963,964,965,966,967,968,969,970,971,972,973],source)
    names=','.join(line.split('.LONG')[1].strip() for line in source[974:976]).split(',')
    a.require(tuple(names)==NAMES,'Source pickup handler order differs')
    oldpc=int(binding['instructions'][8]['p1_pc'],16)
    oldroot=read_words(p1,oldpc+16,1)[0]
    oldtargets=read_words(p1,oldroot,16);targets=read_words(rom,0xffe5e040,16)
    symbols=json.loads(sp.read_text())['globals']
    rows=[]
    for n,(name,old,target) in enumerate(zip(NAMES,oldtargets,targets)):
        a.require(old==int(symbols[name],16),'P1 table slot does not name expected source handler')
        a.require(target in compiled,'Retail pickup target not accepted by translator')
        rows.append(dict(subtype=n,source_slot_name=name,p1_target=f'{old:08x}',
                         retail_target=f'{target:08x}',compiled=True))
    pins += [a.pin(p) for p in (src,pp,mp,lp,sp,Path(__file__),
        ROOT/'tools/scenarios/mproc_continuation_audit.py',
        ROOT/'tools/scenarios/reconstruct_wave_positions.py')]
    return dict(result='RETAIL_PICKUP_DISPATCH_BOUND_EFFECTS_OPEN',gate_proof=False,pins=pins,
        binding=binding,table_root='ffe5e040',handlers=rows,
        predicate='Return B0=0 when player-object OID low2 bits equal3; otherwise select ICONTBL[item OID &63].',
        limits=['Table has16 named source slots but dispatcher masks6 bits; legal dynamic subtype range remains a caller obligation.',
                'Source slot names match P1 pointers; retail targets are observed table entries, not full handler-body equivalence proof.',
                'Selection does not establish invocation, collection ownership, score, lifetime or effect duration.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    out=p.parse_args().out.resolve()
    a.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build();out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(result=result['result'],handlers=result['handlers'])))
