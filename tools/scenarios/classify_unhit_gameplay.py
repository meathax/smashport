"""Refine unhit priorities using retail instructions, without claiming dead code."""
import argparse
import json
from pathlib import Path
import coverage_gap_manifest as gaps
import rebaseline as runner
from mproc_continuation_audit import load_current

ROOT=Path(__file__).resolve().parents[2]


def shape(ins):
    return ins.mnem,[(o.kind,o.reg,o.value) for o in ins.ops]


def build(inventory_path,coverage_receipt):
    inventory=json.loads(inventory_path.read_text(encoding='utf-8'))
    receipt=json.loads(coverage_receipt.read_text(encoding='utf-8'))
    for p in inventory['pins']:gaps.checked(p)
    runner.require(inventory['result']=='PASS_RETAIL_ROUTE_RECORDS_RECONSTRUCTED_C3_OPEN',
                   'Unqualified route inventory')
    runner.check_d10_sources(ROOT,receipt['verified_at'])
    _,rows,_,_,ins,generated=load_current(ROOT)
    coverage=gaps.checked(receipt['combined']).read_bytes()
    runner.require(len(coverage)==len(rows) and set(coverage)<={0,1},'Wrong coverage index')
    hits={row['address']:bool(coverage[i]) for i,row in enumerate(rows)}
    expected={
        0xffec9600:('CMPI',[('IMM',None,1),('R','A0',None)]),
        0xffec9620:('JRNE',[('REL',None,0xffec9680)]),
        0xffec9630:('CALLR',[('REL',None,0xffecc410)]),
        0xffec9650:('CALLR',[('REL',None,0xffecc360)]),
        0xffec9680:('CMPI',[('IMM',None,2),('R','A0',None)]),
        0xfff1b9e0:('MOVB',[('DISP','A13',0x100),('R','A1',None)]),
        0xfff1ba00:('CMPI',[('IMM',None,7),('R','A1',None)]),
        0xfff1ba20:('JRLT',[('REL',None,0xfff1ba50)]),
        0xfff1ba30:('MOVI',[('IMM',None,2),('R','A1',None)]),
        0xfff1ba50:('DEC',[('R','A1',None)]),
        0xfff1ba60:('MOVB',[('R','A1',None),('DISP','A13',0x100)]),
        0xfff1ba80:('JRNN',[('REL',None,0xfff1bfa0)]),
        0xfff1ba90:('MOVK',[('K',None,2),('R','A0',None)]),
        0xfff1baa0:('MOVB',[('R','A0',None),('DISP','A13',0x100)]),
    }
    for pc,wanted in expected.items():
        runner.require(shape(ins[pc])==wanted,f'Retail contract changed at {pc:08x}')
    init=next(i for i,row in enumerate(rows) if row['source']=='WLMAN.ASM:110')
    init_pc=int(rows[init]['address'],16);clear_pc=int(rows[init-2]['address'],16)
    runner.require(shape(ins[init_pc])==('MOVB',[('R','A0',None),('DISP','A13',0x100)]),
                   'SPDLY initialization changed')
    runner.require(shape(ins[clear_pc])==('CLR',[('R','A0',None)]),'SPDLY initializer not zero')
    doors=[o['from_door_field'] for record in inventory['physical_records'] for o in record['options']]
    runner.require(len(doors)==40 and set(doors)=={2,3,4},'Retail door domain changed')
    # This finite local invariant assumes entry through the normal initializer
    # and absence of intervening alias writes, not proven process lifetimes.
    cycle={v:(v-1 if v else 2) for v in (0,1,2)}
    runner.require(set(cycle.values())=={0,1,2},'Local delay invariant changed')
    priorities=json.loads((ROOT/'evidence/current-unexecuted-content-triage-2026-11-13.json').read_text(encoding='utf-8'))
    return dict(result='RETAIL_UNHIT_PRIORITIES_REFINED_GLOBAL_REACHABILITY_OPEN',gate_proof=False,
        verified_at=receipt['verified_at'],runtime=receipt['runtime'],
        priority_hits=[dict(address=b['address'],hit=hits[b['address']]) for b in priorities['priority_branches']],
        exit_cleanup=dict(pc='ffec9650',actual_guard='FROMDR == 1',target='ffecc360',
            earlier_triage_claim='FROMDR == 2 invokes KL',earlier_claim_correct=False,
            retail_option_count=40,retail_door_domain=sorted(set(doors)),
            classification='Not selected by reconstructed retail exit-table values; alternate entry and aliases open.'),
        spin_delay=dict(pc='fff1ba30',actual_guard='signed SPDLY >= 7',initializer=f'{init_pc:08x}',
            initializer_value=0,normal_entry_values=[0,1,2],local_transition=cycle,
            transient_store_on_zero=255,
            classification='Defensive reset outside local normal-entry invariant; process aliases and alternate entry open.'),
        checked_instruction_pcs=[f'{pc:08x}' for pc in expected],
        instructions_proved_dead=0,global_target_obligations_closed=0,
        pins=[runner.pin(p) for p in (inventory_path,coverage_receipt,Path(__file__),
              ROOT/'evidence/current-unexecuted-content-triage-2026-11-13.json')]+generated,
        limits=['Source annotations are hints; actual decoded retail guards take precedence.',
                'No instruction deleted or generated code changed.',
                'No claim that sampled coverage proves unreachable code or complete content.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('inventory','coverage-receipt','out'):parser.add_argument('--'+key,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve()
    runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.inventory.resolve(),args.coverage_receipt.resolve())
    runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],priority_hits=result['priority_hits'])))
