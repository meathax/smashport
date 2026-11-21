"""Bind raw life writes to the retail player-death decrement, retaining limits."""
import argparse
import json
from pathlib import Path
import subprocess
from mproc_continuation_audit import (R,K,M,expect,load_current,window,bind_relocated_source,pin,require)
import coverage_gap_manifest as gaps
import rebaseline as runner

ROOT=Path(__file__).resolve().parents[2]


def bound_decrement(event):
    require(event['kind']=='life_write' and event['player'] in (1,2),'Invalid life write')
    for field in ('data','mask','old','merged'):require(type(event[field]) is int and 0<=event[field]<=65535,'Invalid word field')
    require(event['merged']==((event['old']&~event['mask'])|(event['data']&event['mask']))&65535,'Merged value differs')
    if event['pc_raw']!=0xffe1c6c0:return False
    address=(0x01086360,0x01086520)[event['player']-1]
    require(event['address']==address and event['a1_raw']+320==address,'Player-data owner differs')
    require(event['mask']==65535 and event['old']==event['merged']+1 and event['a2_raw']==event['merged'],
        'Not a full-word decrement by one')
    require(event['process_player_raw']==event['player'],'Raw process player differs')
    return True


def build(path):
    capture=json.loads(path.read_text(encoding='utf-8'))
    require(capture['result']=='STRICT_OPENING_MAIN_MATCH_RAW_LIFE_EVENTS' and not capture['owned_live_pids'],'Trace not terminal')
    for p in capture['pins']:gaps.checked(p)
    event_path=gaps.checked(capture['artifacts']['events.jsonl'])
    rows=[json.loads(line) for line in event_path.read_text().splitlines()]
    require(rows[0]['schema']=='stv.mame.life_writes.v1' and rows[-1]==dict(kind='end',frame=5000,events=len(rows)-2),'Incomplete events')
    require(all(0<=r['frame']<=5000 for r in rows[1:-1]) and
        [r['frame'] for r in rows[1:-1]]==sorted(r['frame'] for r in rows[1:-1]),'Event order differs')
    _,_,_,rom,instructions,generated=load_current(ROOT)
    seq=window(instructions,0xffe1c670,0xffe1c6c0)
    require(len(seq)==3,'Decrement instruction extent differs')
    for ins,pattern in zip(seq,[('MOVE',M('DISP','A1',320),R('A2'),K(0)),('DEC',R('A2')),
                               ('MOVE',R('A2'),M('DISP','A1',320),K(0))]):expect(ins,*pattern)
    source=ROOT/'original/archive/smashtv-main/ROBO.ASM'
    p1=ROOT/'build/p1/robo.bin';mask=ROOT/'build/p1/robo.bin.relmask';listing=ROOT/'build/p1/listing.json'
    binding=bind_relocated_source(seq,rom,p1.read_bytes(),mask.read_bytes(),json.loads(listing.read_text())['instructions'],
        'ROBO.ASM',[1812,1813,1814],source.read_text().splitlines())
    decreases=[r for r in rows[1:-1] if r['gamstate']==16 and r['merged']<r['old']]
    bound=[];unbound=[]
    for event in decreases:
        (bound if bound_decrement(event) else unbound).append(event)
    require(decreases==capture['in_game_decreases'],'Capture decrease receipt differs')
    return dict(schema='stv.bound_life_decrements.v1',result='RETAIL_DEATH_DECREMENTS_BOUND_DAMAGE_OBJECT_OPEN',
        gate_proof=False,verified_at=subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=ROOT).strip(),
        native_protected_source=capture['verified_at'],binding=binding,bound=bound,unbound=unbound,
        pins=[pin(p) for p in [path,event_path,source,p1,mask,listing,Path(__file__)]]+generated,
        limits=['Raw PC is the instruction end; writer beginsFFE1C6A0 and matches ROBO.ASM1814.',
            'Process-player byte and player-data address agree, but continuous process lifetime is not proven.',
            'Recorded DEAD tags do not identify a specific damaging object or prove collision geometry.',
            'This reference replay does not explain enhanced avoidance or close balance/clearance.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('capture','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve()
    require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    result=build(args.capture.resolve());runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],bound=len(result['bound']),unbound=len(result['unbound']))))
