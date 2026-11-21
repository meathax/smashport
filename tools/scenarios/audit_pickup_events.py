"""Validate passive pickup-handler ownership against a strict full-RAM capture."""
import argparse
import collections
import json
from pathlib import Path
import re
import rebaseline as r
import coverage_gap_manifest as gaps
from replay_c3_routes import strict_marker

ROOT=Path(__file__).resolve().parents[2]
SITES={0xffed2620:(0xffed2600,0),0xffe31ac0:(0xffe31aa0,2),0xffe31d00:(0xffe31ce0,3),
       0xffe278c0:(0xffe278a0,4),0xffe31740:(0xffe31720,5),0xffe2fd80:(0xffe2fd60,6),
       0xffe31500:(0xffe314e0,7),0xffe2ffa0:(0xffe2ff80,8),0xffe30270:(0xffe30250,9),
       0xffe2ce30:(0xffe2ce10,15),0xffe2d970:(0xffe2d950,None),0xffe2d110:(0xffe2d0f0,14)}
NAMES=('POW1ON','POW2ON','SPRAYON','PODON','EXTRA','GRENON','RINGON','NSTPON',
       'KEYON','SHOEON','SMRTON','TPZ1','TPZ2','TPZ3','TCSH','TITM')


def pool(address,first,stride,count):
    return type(address) is int and first<=address<first+stride*count and (address-first)%stride==0


def validate_event(e):
    r.require(e.get('kind')=='pickup_handler','Wrong event kind')
    subtype=e['subtype'];r.require(type(subtype) is int and 0<=subtype<16,'Invalid pickup subtype')
    pc=e['pc_raw'];phase=e['phase']
    if phase=='owner_read':
        r.require(pc in SITES,'Unknown owner-read boundary')
        selected=SITES[pc][1]
        r.require(subtype==selected if selected is not None else subtype in (11,12,13),'Handler/subtype mismatch')
    elif phase=='bomb_flag_store':r.require(pc==0xffe304c0 and subtype==10,'Wrong bomb boundary')
    elif phase=='helper_delete_entry':r.require(pc==0xffe03240 and subtype==1,'Wrong helper boundary')
    else:r.require(False,'Unknown observer phase')
    for key in ('icon_active','player_object_active','torso_active','legs_active','process_list_complete','object_list_complete'):
        r.require(e.get(key) is True,'Incomplete ownership: '+key)
    player=e['player'];r.require(type(player) is int and player in (1,2),'Invalid player')
    r.require(type(e['frame']) is int and e['frame']>=0 and e['circuit'] in (0,1,2),'Invalid event position')
    for key in ('torso','legs'):r.require(pool(e[key],0x01086790,0x5a0,240),'Invalid process pool slot')
    for key in ('icon','player_object','torso_object','legs_object'):
        r.require(pool(e[key],0x010003a0,0x1f0,420),'Invalid object pool slot')
    r.require(e['torso']!=e['legs'] and e['torso_pid']==e['legs_pid']==246+player and
              e['player_number']==player and e['legs_backlink']==e['torso'] and
              e['torso_object']==e['legs_object'],'Player owner links differ')
    r.require(e['icon']!=e['player_object'] and e['icon_oid']==0x0c00+subtype and
              e['player_oid']==0x4100+player,'Object identity/handler differs')
    return dict(handler=NAMES[subtype],subtype=subtype,player=player,frame=e['frame'],
                circuit=e['circuit'],wave=e['wave'],icon=e['icon'],torso=e['torso'],legs=e['legs'])


def boundary_contract():
    blocks={};pins=[]
    for path in sorted((ROOT/'generated/private/l8').glob('l8_chunk_*.cpp')):
        pins.append(r.pin(path))
        for block in re.split(r'(?=^L_[0-9a-f]{8}:)',path.read_text(),flags=re.M):
            match=re.match(r'L_([0-9a-f]{8}):',block)
            if match:blocks[int(match[1],16)]=block
    for end,(start,_) in SITES.items():
        block=blocks[start]
        r.require('rf(m,c.a[0]+0x00000160u,1)' in block and 'c.a[0]=v;' in block and
                  f'm.boundary(0x{end:08x}u)' in block,'Owner-read instruction binding changed')
    r.require('wb(m,0x010eeab0u,c.a[1]);' in blocks[0xffe30490] and
              'm.boundary(0xffe304c0u)' in blocks[0xffe30490],'Bomb store binding changed')
    r.require('rf(m,c.a[2],1)' in blocks[0xffe03230] and 'm.boundary(0xffe03240u)' in blocks[0xffe03230],
              'DELOBJ traversal binding changed')
    r.require(blocks[0xffe031d0].count('c.sp-=0x20u;')==5 and
              'push(m,c,0xffed3bf0u);' in blocks[0xffed3bc0] and 'return 0xffe031d0u;' in blocks[0xffed3bc0],
              'POW2 call/DELOBJ saved stack binding changed')
    r.require('push(m,c,0xffe2d040u);' in blocks[0xffe2d020],'Cash shared SEND caller changed')
    return pins


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--capture',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();out=args.out.resolve()
    r.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    capture=json.loads(args.capture.read_text())
    r.require(capture['result']=='PASS_FULL_NATIVE_ARCADE_RAM_EXIT_OBSERVATIONS' and capture['owned_live_pids']==[],
              'Capture is not successful/terminal')
    r.check_d10_sources(ROOT,capture['verified_at']);pins=[r.pin(args.capture),r.pin(Path(__file__))]
    observer=ROOT/'tools/lockstep/pickup_events.lua'
    observer_pins=[pin for pin in capture['pins'] if Path(pin['path']).resolve()==observer]
    r.require(len(observer_pins)==1 and observer_pins[0]==r.pin(observer),'Pickup observer changed or is unpinned')
    gaps.checked(observer_pins[0]);pins.append(observer_pins[0])
    for name in ('native','mame','strict'):
        steps=[s for s in capture['steps'] if s['name']==name]
        r.require(len(steps)==1 and steps[0]['state']=='PASS' and steps[0]['exit_code']==0,'Incomplete capture step')
    artifacts={}
    for name in ('native.bin','mame.bin','strict.log','pickup-events.jsonl'):
        matches=[x for x in capture['artifacts'] if Path(x['path']).name==name]
        r.require(len(matches)==1,'Missing/duplicate artifact: '+name)
        artifacts[name]=gaps.checked(matches[0]);pins.append(matches[0])
    strict_marker(artifacts['strict.log'].read_text(),capture['final_frame'])
    for name in ('native.bin','mame.bin'):
        r.require(capture['endpoints'][name]==dict(frames=capture['final_frame']+1,last_frame=capture['final_frame'],full_regions=True),
                  'Incomplete full-RAM endpoint')
    rows=[json.loads(s) for s in artifacts['pickup-events.jsonl'].read_text().splitlines()]
    r.require(rows[0]['schema']=='stv.mame.pickup_handlers.v1' and rows[-1]==dict(kind='end',frame=capture['final_frame'],events=len(rows)-2),
              'Incomplete event sidecar')
    events=[validate_event(e) for e in rows[1:-1]]
    r.require(events and all(0<=e['frame']<=capture['final_frame'] for e in events) and
              all(a['frame']<=b['frame'] for a,b in zip(events,events[1:])),'Empty/out-of-order/out-of-range events')
    pins.extend(boundary_contract())
    observed=collections.Counter(e['handler'] for e in events)
    result=dict(result='BOUND_NATURAL_PICKUP_HANDLER_OWNERS',gate_proof=False,verified_at=capture['verified_at'],
        events=events,counts=dict(observed),missing_handlers=[n for n in NAMES if n not in observed],pins=pins,
        limits=['Passive MAME handler-boundary observations with matching complete native/arcade RAM streams.',
                'No instruction-by-instruction native event equivalence or universal future allocation lifetime claim.',
                'Unobserved handlers remain explicitly missing; global C2/C3 closure remains open.'])
    r.atomic_json(out,result);print(json.dumps({k:result[k] for k in ('result','counts','missing_handlers')}))


if __name__=='__main__':main()
