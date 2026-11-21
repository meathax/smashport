"""Attribute visible projectile states in qualified every-frame RAM captures.

Adjacent observations are not allocation lifetimes: creation/reuse can occur
between dumps, and neutral pre-arm/explosion states are deliberately excluded.
"""
import argparse
import collections
import json
from pathlib import Path
import struct
import player_owner_audit as owner
import sample_shoe_owners as players
import audit_pickup_events as pickups
import rebaseline as r
import coverage_gap_manifest as gaps
from replay_c3_routes import strict_marker

ROOT=Path(__file__).resolve().parents[2]
FAMILIES={0x4201:'normal',0x4202:'spray',0x4203:'pod',0x4204:'grenade',0x4205:'nonstop',0x4206:'pod'}


def snapshot(image):
    read=lambda a,w:owner.read_bits(image,a,w)
    if read(0x010865a0,16)!=16:return [],['outside_game']
    processes,complete=owner.process_list(image)
    if not complete:return [],['incomplete_process_list']
    if any(not pickups.pool(p['address'],0x01086790,0x5a0,240) for p in processes):
        return [],['nonpool_process_list']
    objects=set();node=read(0x01000040,32)
    while node:
        if node in objects or not pickups.pool(node,0x010003a0,0x1f0,420):
            return [],['incomplete_object_list']
        if not read(node+0xb0,16)&0x8000:return [],['inactive_object_in_list']
        objects.add(node);node=read(node,32)
    bound,reasons=players.owners(image)
    live={p['player']:p for p in bound if read(p['torso']+0x100,32) in objects and
          read(read(p['torso']+0x100,32)+0x150,16)==0x4100+p['player']}
    result=[]
    for p in processes:
        if p['pid']!=286:continue
        obj=p['saved_a8'];process=p['address']
        if obj not in objects or read(obj+0x160,32)!=process:
            reasons.append('projectile_object_not_linked');continue
        oid=read(obj+0x150,16)
        if oid not in FAMILIES:
            reasons.append('neutral_or_unclassified_projectile_state');continue
        player=read(process+0x138,8)
        if player not in live:
            reasons.append('projectile_player_not_live');continue
        if read(process+0x100,16) not in range(1,9):
            reasons.append('projectile_direction_not_bound');continue
        result.append(dict(player=player,family=FAMILIES[oid],oid=oid,process=process,object=obj,
                           torso=live[player]['torso'],legs=live[player]['legs']))
    return result,reasons


def images(path,final):
    image=bytearray(owner.RAM_SIZE);initialized=set();count=0
    for frame,records in r.lsfile.frames(path):
        r.require(frame==count and frame<=final,'Incomplete/nonsequential RAM stream')
        summaries=0;seen=set()
        for kind,f,payload in records:
            r.require(f==frame,'Record frame differs')
            if kind in (1,2,3):
                r.require(not summaries and len(payload)==258,'Expected full chunks before summary')
                index=struct.unpack_from('<H',payload)[0]
                r.require(index<r.lsfile.SIZES[kind]//256 and (kind,index) not in seen,'Bad/duplicate full chunk')
                seen.add((kind,index))
                if kind==1:
                    image[index*256:(index+1)*256]=payload[2:];initialized.add(index)
            else:
                r.require(kind==9 and len(payload)==8,'Unexpected full RAM record');summaries+=1
        r.require(summaries==1 and len(initialized)==owner.RAM_SIZE//256,'Missing summary or initial RAM')
        yield frame,image
        count+=1
    r.require(count==final+1,'Final frame missing')


def scan(path,final):
    groups={};excluded=collections.Counter();previous=set();frames=0
    for frame,image in images(path,final):
        rows,reasons=snapshot(image);excluded.update(reasons);current=set();frames+=1
        for row in rows:
            key=(row['player'],row['family']);identity=(key,row['process'],row['object'],row['torso'])
            if key not in groups:groups[key]=dict(player=key[0],family=key[1],first_frame=frame,last_frame=frame,
                object_frame_observations=0,observation_runs=0,slot_pairs=set())
            g=groups[key];g['last_frame']=frame;g['object_frame_observations']+=1
            g['observation_runs']+=identity not in previous;g['slot_pairs'].add((row['process'],row['object']))
            current.add(identity)
        previous=current
    for g in groups.values():g['distinct_slot_pairs']=len(g.pop('slot_pairs'))
    return dict(frames=frames,groups=list(groups.values()),exclusions=dict(excluded))


def qualify(capture):
    r.require(capture['result']=='PASS_FULL_NATIVE_ARCADE_RAM_EXIT_OBSERVATIONS' and
              capture['owned_live_pids']==[],'Capture not terminal/qualified')
    runtime=capture['verified_at'];r.check_d10_sources(ROOT,runtime)
    r.require(capture['dump_every']==1 and capture['compact'] is False,'Every-frame full RAM required')
    final=capture['final_frame'];r.require(type(final) is int and final>0,'Bad endpoint')
    for name in ('native','mame','strict'):
        steps=[s for s in capture['steps'] if s['name']==name]
        r.require(len(steps)==1 and steps[0]['state']=='PASS' and steps[0]['exit_code']==0,'Incomplete capture step')
    for name in ('native.bin','mame.bin'):
        r.require(capture['endpoints'][name]==dict(frames=final+1,last_frame=final,full_regions=True),'Endpoint differs')
    paths={};pins=[]
    for name in ('native.bin','mame.bin','strict.log'):
        matches=[p for p in capture['artifacts'] if Path(p['path']).name==name]
        r.require(len(matches)==1,'Missing/duplicate artifact');paths[name]=gaps.checked(matches[0]);pins+=matches
    strict_marker(paths['strict.log'].read_text(),final)
    return paths,pins


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--capture',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();out=args.out.resolve();r.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    capture=json.loads(args.capture.read_text());paths,pins=qualify(capture)
    result=scan(paths['native.bin'],capture['final_frame']);r.check_d10_sources(ROOT,capture['verified_at'])
    result.update(result='BOUND_SAMPLED_PROJECTILE_OWNERS',gate_proof=False,verified_at=capture['verified_at'],
        pins=pins+[r.pin(Path(x)) for x in (args.capture,__file__,owner.__file__,players.__file__,pickups.__file__)],
        limits=['Every-frame visible-state observations, not firing/collision events or allocation-generation identity.',
                'Neutral pre-arm/explosion states and incomplete/unbound owners are excluded explicitly.',
                'Adjacent observation runs do not prove uninterrupted lifetime or exclude within-frame slot reuse.',
                'Global content, clearance and control-flow gates remain open.'])
    r.atomic_json(out,result);print(json.dumps({k:result[k] for k in ('result','frames','groups','exclusions')}))


if __name__=='__main__':main()
