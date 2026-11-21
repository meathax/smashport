"""Check conditional cap arithmetic against existing full-RAM player-owner captures."""
import argparse
import json
from pathlib import Path
import struct
import coverage_gap_manifest as gaps
import rebaseline as runner
import player_owner_audit as owner
from reconstruct_caps import cap_byte

ROOT=Path(__file__).resolve().parents[2]


def build(caps_path,captures_path):
    caps=json.loads(caps_path.read_text());captures=json.loads(captures_path.read_text())
    runner.require(caps['result']=='CONDITIONAL_RETAIL_CAP_ARITHMETIC_BOUND_BALANCE_OPEN','Caps not bound')
    runner.require(captures['schema']=='stv.fullram.owner.capture.v1' and
        captures['result']=='CAPTURES_COMPLETE_IDENTITY_PENDING' and not captures['owned_live_pids'],'Full-RAM captures incomplete')
    for p in caps['pins']+list(captures['pins'].values()):gaps.checked(p)
    pins=[runner.pin(p) for p in [caps_path,captures_path,Path(__file__),Path(owner.__file__)]];observations=[]
    steps={s['name']:s for s in captures['steps']}
    runner.require(set(steps)=={'1p-native-fullram','2p-native-fullram'} and len(captures['steps'])==2,'Owner capture matrix differs')
    for players in (1,2):
        step=steps[f'{players}p-native-fullram'];runner.require(step['state']=='PASS' and step['exit_code']==0,'Capture failed')
        artifacts=step['proof']['artifacts'];full=gaps.checked(artifacts['native-full.bin']);content=gaps.checked(artifacts['content.jsonl'])
        sample=next((r for r in gaps.samples(content) if r['frame']==2100),None)
        runner.require(sample is not None,'Missing requested sample')
        image=bytearray(owner.RAM_SIZE);initialized=set();previous=-1;found=False
        for frame,records in runner.lsfile.frames(full,keep_pc=True):
            runner.require(frame==previous+1,'Full prefix frame gap');previous=frame;seen=set()
            for kind,record_frame,payload in records:
                runner.require(record_frame==frame,'Record frame differs')
                if kind==1:
                    runner.require(frame%60==0 and len(payload)==258,'Not full RAM data')
                    index,=struct.unpack_from('<H',payload)
                    runner.require(index<owner.RAM_SIZE//256 and index not in seen,'Invalid/duplicate RAM chunk')
                    seen.add(index);initialized.add(index);image[index*256:(index+1)*256]=payload[2:]
            if frame==2100:found=True;break
        runner.require(found and len(initialized)==owner.RAM_SIZE//256,'Incomplete RAM at requested sample')
        bound,excluded=owner.owners(image,sample)
        runner.require({o['player'] for o in bound}==set(range(1,players+1)),'Active player ownership differs')
        status=owner.read_bits(image,0x010ee600,16);raw=owner.read_bits(image,0x010de310,16)
        modifier=raw-65536 if raw&32768 else raw
        room=next(r for r in caps['rooms'] if (r['circuit'],r['wave'])==(sample['circuit'],sample['wave']))
        values=[]
        for row in room['table_driven_caps']:
            context=next(c for c in caps['contexts'] if c['name']==row['context'])
            actual=owner.read_bits(image,int(context['destination'],16),8)
            predicted=cap_byte(row['base_byte'],status,context['subtract_unless_status3'],modifier if context['add_signed_gruntmod'] else 0)
            runner.require(actual==predicted,'Measured cap differs from bound arithmetic')
            values.append(dict(context=context['name'],address=context['destination'],actual=actual,predicted=predicted))
        observations.append(dict(players=players,frame=2100,circuit=sample['circuit'],wave=sample['wave'],status=status,
            grunt_modifier_raw=raw,grunt_modifier_signed=modifier,bound_players=bound,caps=values))
        pins.extend([runner.pin(full),runner.pin(content)])
    return dict(schema='stv.sampled_cap_state.v1',result='SAMPLED_PLAYER_CAPS_MATCH_BOUND_ARITHMETIC',gate_proof=False,
        verified_at=caps['verified_at'],native_protected_source=captures['native_protected_source'],pins=pins,observations=observations,
        limits=['Two current reference samples only; not all-room, enhanced or continuous cap validation.',
            'STATUS observations bind these active-player samples, not every lifecycle value.',
            'GRUNTMOD observed as signed-1; current CMOS field interpretation remains separate.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('caps','captures','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();runner.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh output')
    result=build(args.caps.resolve(),args.captures.resolve());runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],samples=len(result['observations']))))
