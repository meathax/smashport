"""Validate paired passive projectile/enemy collision-dispatch observations."""
import argparse
import collections
import json
from pathlib import Path
import re
import sample_projectile_owners as projectiles
import audit_pickup_events as pickups
import rebaseline as r
import coverage_gap_manifest as gaps
ROOT=Path(__file__).resolve().parents[2]


def contract():
    compiled=set();pins=[];blocks={}
    wanted={0xffe55790,0xffe55860,0xffe55800,0xffe55850}
    for path in sorted((ROOT/'generated/private/l8').glob('l8_chunk_*.cpp')):
        pins.append(r.pin(path))
        for block in re.split(r'(?=^L_[0-9a-f]{8}:)',path.read_text(),flags=re.M):
            match=re.match(r'L_([0-9a-f]{8}):',block)
            if not match:continue
            pc=int(match[1],16);compiled.add(pc)
            if pc in wanted:blocks[pc]=block
    for start,end,reg in [(0xffe55790,0xffe557c0,6),(0xffe55860,0xffe55890,8)]:
        text=blocks[start]
        r.require('rf(m,0x01000060u,1)' in text and f'c.a[{reg}]=v;' in text and
                  f'm.boundary(0x{end:08x}u)' in text,'Collision free-list read binding changed')
    for pc,index in [(0xffe55800,1),(0xffe55850,2)]:
        r.require(f'c.b[{index}]&0xfffffff0u' in blocks[pc],'Collision callback dispatch changed')
    return compiled,pins


def validate(rows,final,compiled):
    r.require(rows[0]==dict(schema='stv.mame.collision_dispatch.v1',scope='projectile_enemy_pairs'),'Wrong collision schema')
    end=rows[-1];r.require(end==dict(kind='end',frame=final,events=len(rows)-2,pairs=(len(rows)-2)//2,overlaps=0,unpaired=0),
                         'Incomplete/overlapping collision stream')
    pending={};next_id=1;previous_frame=-1;bound=collections.Counter();unbound=0;unbound_reasons=collections.Counter()
    for e in rows[1:-1]:
        phase=e['phase'];r.require(e['kind']=='collision_dispatch' and phase in ('before','after'),'Wrong collision event')
        r.require(type(e['frame']) is int and previous_frame<=e['frame']<=final,'Invalid collision frame order');previous_frame=e['frame']
        r.require(e['pc_raw']==(0xffe557c0 if phase=='before' else 0xffe55890),'Wrong collision boundary')
        r.require(type(e['sp']) is int and 0x01000000<=e['sp']<0x01100000 and e['sp']%16==0,'Invalid collision stack')
        for n in (1,2):
            r.require(pickups.pool(e['object'+str(n)],0x010003a0,0x1f0,420),'Invalid collision object slot')
        r.require(e['object1']!=e['object2'],'Self collision')
        if phase=='before':
            r.require(e['pair']==next_id and e['pair'] not in pending,'Duplicate/out-of-order pair');next_id+=1
            r.require(e['object_list_complete'] is True and e['active1'] is True and e['active2'] is True and
                      e['flags1']&0x8000 and e['flags2']&0x8000,'Incomplete/inactive collision objects')
            r.require((e['callback1'] or e['callback2']) and all(e['callback'+str(n)]==0 or e['callback'+str(n)] in compiled for n in (1,2)),
                      'Missing/uncompiled selected collision callback')
            choices=[n for n in (1,2) if e['oid'+str(n)] in projectiles.FAMILIES and e['oid'+str(3-n)]&0xc000==0x8000]
            r.require(len(choices)==1,'Not a unique projectile/enemy collision');pending[e['pair']]=(e,choices[0])
        else:
            r.require(e['pair'] in pending,'Unpaired collision return');before,n=pending.pop(e['pair'])
            r.require(all(e[k]==before[k] for k in ('sp','object1','object2')),'Collision return context changed')
            player=before['myplayer'+str(n)]
            checks=[('incomplete_process_list',before['process_list_complete'] is True),
                    ('inactive_process',before['process_active'+str(n)] is True),
                    ('unexpected_process_id',before['pid'+str(n)]==286),
                    ('invalid_process_slot',pickups.pool(before['process'+str(n)],0x01086790,0x5a0,240)),
                    ('invalid_player_field',player in (1,2)),
                    ('missing_live_player_owner',player not in (1,2) or before['owner'+str(player)] is True)]
            reasons=[name for name,valid in checks if not valid]
            if reasons:
                unbound+=1;unbound_reasons.update(reasons);continue
            outcome='absent_after' if not e['active'+str(n)] else 'same_id_present' if e['oid'+str(n)]==before['oid'+str(n)] else 'changed_id'
            key=(player,projectiles.FAMILIES[before['oid'+str(n)]],before['oid'+str(3-n)],before['callback1'],before['callback2'],outcome)
            bound[key]+=1
    r.require(not pending and next_id-1==end['pairs'] and end['pairs']>0,'Missing/empty collision pairs')
    return dict(pairs=end['pairs'],bound_pairs=sum(bound.values()),unbound_pairs=unbound,
                unbound_reasons=dict(unbound_reasons),
                groups=[dict(player=k[0],family=k[1],enemy_oid=k[2],callback1=k[3],callback2=k[4],projectile_after=k[5],count=v) for k,v in bound.items()])


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--capture',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();out=args.out.resolve();r.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    capture=json.loads(args.capture.read_text());_,pins=projectiles.qualify(capture)
    lua=ROOT/'tools/lockstep/collision_events.lua';matched=[p for p in capture['pins'] if Path(p['path']).resolve()==lua]
    r.require(len(matched)==1 and matched[0]==r.pin(lua),'Observer not pinned/current');pins+=matched
    matched=[p for p in capture['artifacts'] if Path(p['path']).name=='collision-events.jsonl']
    r.require(len(matched)==1,'Missing/duplicate collision sidecar');path=gaps.checked(matched[0]);pins+=matched
    compiled,source_pins=contract();result=validate([json.loads(s) for s in path.read_text().splitlines()],capture['final_frame'],compiled)
    result.update(result='PAIRED_NATURAL_PROJECTILE_ENEMY_DISPATCH',gate_proof=False,verified_at=capture['verified_at'],
        pins=pins+source_pins+[r.pin(Path(x)) for x in (args.capture,__file__,projectiles.__file__,pickups.__file__)],
        limits=['Passive MAME dispatch boundaries with matching complete native/arcade RAM, not native instruction-level event identity.',
                'Selected callbacks and before/after object states observed; same-address reuse is not allocation identity.',
                'Current player registry need not be original firing torso; unbound player events remain explicit.',
                'No universal collision, damage/score, content or target-closure claim.'])
    r.atomic_json(out,result);print(json.dumps({k:result[k] for k in ('result','pairs','bound_pairs','unbound_pairs','groups')}))


if __name__=='__main__':main()
