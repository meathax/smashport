"""Bind the normal DMA interrupt/helper path and its A-register preservation."""
import argparse
import json
from pathlib import Path
import mproc_continuation_audit as a
ROOT=Path(__file__).resolve().parents[2]


def validate(entry,helper):
    a.require(entry[0].pc==0xffe00000 and helper[0].pc==0xffe00710,'Wrong DMA entry')
    a.require(len(entry)==14 and len(helper)==45,'DMA path extent differs')
    a.contiguous(entry);a.contiguous(helper)
    pcs={i.pc for i in entry+helper}
    allowed={'MMTM','MMFM','MOVE','MOVI','DEC','CLR','JRN','JRNE','JREQ','JR','CALLR','RETI','RETS','SUBI','SRL','ANDI'}
    for ins in entry+helper:
        a.require(ins.mnem in allowed,'Unreviewed DMA instruction')
        for op in ins.ops:
            a.require(op.reg is None or op.reg.startswith('B'),'DMA path touches A register or SP explicitly')
            if op.kind=='REL':a.require(op.value in pcs,'DMA branch leaves checked path')
        if ins.mnem=='CALLR':a.expect(ins,'CALLR',a.M('REL',None,0xffe00710))
        if ins.mnem in ('MMTM','MMFM'):
            expected=0x3c0 if ins.mnem=='MMTM' else 0x7c0
            a.require(ins.ops[0].reg in ('B12','B14') and ins.ops[1].value==expected,
                      'DMA multi-register operation changed file/mask')
    a.require([i.pc for i in entry if i.mnem=='RETI']==[0xffe000c0,0xffe00130],
              'DMA interrupt return paths differ')
    a.require([i.pc for i in helper if i.mnem=='RETS']==[0xffe00c30,0xffe00cd0],
              'DMA helper return paths differ')


def build():
    _,_,_,rom,ins,pins=a.load_current(ROOT);a.require(a.sha(rom)==a.RETAIL_SHA,'Wrong retail image')
    entry=a.window(ins,0xffe00000,0xffe00140);helper=a.window(ins,0xffe00710,0xffe00ce0)
    validate(entry,helper)
    src=ROOT/'original/archive/smashtv-main/NDSP1.ASM'
    pp=ROOT/'build/p1/robo.bin';mp=ROOT/'build/p1/robo.bin.relmask';lp=ROOT/'build/p1/listing.json'
    listing=json.loads(lp.read_text())['instructions'];source=src.read_text().splitlines()
    lines1=[85,86,87,88,89,91,92,94,95,97,98,99,100,101]
    lines2=[284,285,286,287,288,289,290,291,292,293,295,296,297,298,299,300,301,302,303,
            305,306,307,308,309,310,311,312,313,315,316,317,318,319,320,321,322,
            324,325,326,327,328,330,331,332,333]
    bindings=[a.bind_relocated_source(seq,rom,pp.read_bytes(),mp.read_bytes(),listing,
        'NDSP1.ASM',lines,source) for seq,lines in [(entry,lines1),(helper,lines2)]]
    pins += [a.pin(p) for p in (src,pp,mp,lp,Path(__file__),ROOT/'tools/scenarios/mproc_continuation_audit.py')]
    return dict(result='NORMAL_DMA_HANDLER_A_REGISTERS_PRESERVED_CONDITIONALLY',gate_proof=False,
        pins=pins,bindings=bindings,instructions=len(entry)+len(helper),
        facts=['Normal DMAINT path has one direct helper call; all direct branches remain in the bound path.',
               'Explicit register operands and multi-register transfers use B registers only; no A0..A14 writes.',
               'Helper selects TOPQ0,TOPQ1,BOTQ0,BOTQ1 in order; empty queues disable X1.'],
        limits=['Valid DMA-register/queue pointers, return stack and memory disjointness are premises.',
                'No exclusion of alternate entries or malicious/invalid pointer aliases is claimed.',
                'Hardware IRQ entry/RETI and other handlers are separate; global C2 remains open.'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    out=p.parse_args().out.resolve();a.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    r=build();out.write_text(json.dumps(r,indent=2)+'\n',encoding='utf-8');print(r['result'])
