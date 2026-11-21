"""Sample shoe timers on live reciprocal player owners in qualified full RAM."""
import argparse
import collections
import json
from pathlib import Path
import struct
import player_owner_audit as owner
import rebaseline as r
import coverage_gap_manifest as gaps
from replay_c3_routes import strict_marker
ROOT=Path(__file__).resolve().parents[2]


def owners(image):
    read=lambda address,width:owner.read_bits(image,address,width)
    if read(0x010865a0,16)!=16:return [],['outside_game']
    rows,complete=owner.process_list(image)
    if not complete:return [],['incomplete_process_list']
    active={row['address']:row for row in rows};bound=[];excluded=[]
    for player in (1,2):
        torso=read(owner.REGISTRY+(player-1)*32,32)
        if torso not in active or active[torso]['pid']!=246+player:
            excluded.append('missing_live_torso');continue
        if torso+0x2e0>owner.BASE+len(image)*8:
            excluded.append('torso_out_of_bounds');continue
        legs=read(torso+0x2c0,32)
        if (read(torso+0x1d8,8)!=player or legs==torso or legs not in active
                or active[legs]['pid']!=246+player or read(legs+0x160,32)!=torso
                or read(torso+0x100,32)!=read(legs+0x100,32)):
            excluded.append('owner_link_mismatch');continue
        bound.append(dict(player=player,torso=torso,legs=legs,timer=read(legs+0x180,16),
            circuit=read(0x010eb610,16),wave=read(0x010eb630,16)))
    return bound,excluded


def scan(path,final):
    image=bytearray(owner.RAM_SIZE);initialized=set();count=0
    excluded=collections.Counter();positive={};sample_count=0
    for frame,records in r.lsfile.frames(path):
        r.require(frame==count and frame<=final,'Incomplete/nonsequential RAM stream')
        summaries=0;seen=set()
        for kind,f,payload in records:
            r.require(f==frame,'Record frame differs')
            if kind in (1,2,3):
                r.require(len(payload)==258,'Expected full chunks, not compact hashes')
                index=struct.unpack_from('<H',payload)[0]
                r.require(index<r.lsfile.SIZES[kind]//256 and (kind,index) not in seen,
                          'Bad or duplicated full chunk')
                seen.add((kind,index))
                if kind==1:
                    image[index*256:(index+1)*256]=payload[2:];initialized.add(index)
            else:
                r.require(kind==9 and len(payload)==8,'Unexpected full RAM record')
                summaries+=1
        r.require(summaries==1 and len(initialized)==owner.RAM_SIZE//256,
                  'Missing summary or uninitialized RAM')
        if frame%60==0:
            sample_count+=1;bound,reasons=owners(image);excluded.update(reasons)
            for row in bound:
                if row['timer']==0:continue
                key=(row['player'],row['circuit'],row['wave'])
                if key not in positive:
                    positive[key]=dict(row,first_frame=frame,last_frame=frame,samples=0,
                                      minimum=row['timer'],maximum=row['timer'])
                entry=positive[key];entry['last_frame']=frame;entry['samples']+=1
                entry['minimum']=min(entry['minimum'],row['timer'])
                entry['maximum']=max(entry['maximum'],row['timer'])
        count+=1
    r.require(count==final+1,'Final frame missing')
    return dict(frames=count,samples=sample_count,positive_owner_samples=list(positive.values()),
                exclusions=dict(excluded))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();out=args.out.resolve()
    r.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    capture=json.loads(args.capture.read_text());runtime=capture['verified_at']
    r.require(capture['result']=='PASS_FULL_NATIVE_ARCADE_RAM_EXIT_OBSERVATIONS'
        and capture['owned_live_pids']==[],'Capture not terminal/qualified')
    r.check_d10_sources(ROOT,runtime)
    final=capture['final_frame'];r.require(type(final) is int and final>0,'Bad final frame')
    for name in ('native','mame','strict'):
        steps=[s for s in capture['steps'] if s['name']==name]
        r.require(len(steps)==1 and steps[0]['state']=='PASS' and steps[0]['exit_code']==0,
                  'Capture step not uniquely successful')
    for name in ('native.bin','mame.bin'):
        r.require(capture['endpoints'][name]==dict(frames=final+1,last_frame=final,full_regions=True),
                  'Full capture endpoint differs')
    pins={};paths={}
    for name in ('native.bin','mame.bin','strict.log'):
        matches=[x for x in capture['artifacts'] if Path(x['path']).name==name]
        r.require(len(matches)==1,'Missing/duplicated capture artifact')
        pins[name]=matches[0];paths[name]=gaps.checked(matches[0])
    strict_marker(paths['strict.log'].read_text(),final)
    result=scan(paths['native.bin'],final)
    r.check_d10_sources(ROOT,runtime)
    result.update(result='SAMPLED_SHOE_TIMERS_ON_LIVE_OWNERS',gate_proof=False,verified_at=runtime,
        pins=list(pins.values())+[r.pin(x) for x in (args.capture,Path(__file__),Path(owner.__file__))],
        limits=['Positive timers are observed player-owned state, not collision/collection events.',
                '60-frame sampling does not establish exact onset/expiry or continuous allocation lifetime.',
                'Door/transition code can also set shoe timers; source of each assignment remains unproved.'])
    r.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],frames=result['frames'],samples=result['samples'],
        positive_owner_samples=len(result['positive_owner_samples']),out=str(out))))


if __name__=='__main__':main()
