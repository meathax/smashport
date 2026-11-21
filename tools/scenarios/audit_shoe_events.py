"""Attribute passive retail shoe-handler stores after complete RAM comparison."""
import argparse
import json
from pathlib import Path
import rebaseline as r
import coverage_gap_manifest as gaps
from replay_c3_routes import strict_marker
import reconstruct_pickups
ROOT=Path(__file__).resolve().parents[2]


def validate_event(e):
    r.require(e.get('kind')=='shoe_assignment' and e['pc_raw']==0xffe30380,
              'Event is not the bound SHOEON timer store')
    for key in ('icon_active','object_list_complete','process_list_complete','live_owner_links'):
        r.require(e.get(key) is True,'Incomplete/inactive pickup owner: '+key)
    player=e['player'];r.require(type(player) is int and player in (1,2),'Invalid player')
    for key in ('process','torso'):
        address=e[key]
        r.require(type(address) is int and 0x01086790<=address<0x010dad90
                  and (address-0x01086790)%0x5a0==0,'Owner is not a process-pool slot')
    r.require(e['process']!=e['torso'] and e['address']==e['process']+0x180
        and e['a0_raw']==e['process'] and e['legs_backlink']==e['torso']
        and e['torso_pid']==e['legs_pid']==246+player and e['player_number']==player,
        'Player/process ownership mismatch')
    r.require(e['torso_object']==e['legs_object'],'Torso/leg object link mismatch')
    for address in (e['icon'],e['torso_object']):
        r.require(0x010003a0<=address<0x01033160 and (address-0x010003a0)%0x1f0==0,
                  'Object outside retail pool')
    expected=500 if e['circuit']==2 else 300
    r.require(e['circuit'] in (0,1,2) and e['icon_oid']==0x0c09 and e['mask']==0xffff
              and e['merged_timer']==e['data']==expected,'Wrong collectible or timer assignment')
    return dict(player=player,frame=e['frame'],circuit=e['circuit'],wave=e['wave'],
        torso=e['torso'],legs=e['process'],icon=e['icon'],old_timer=e['old_timer'],new_timer=expected)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();out=args.out.resolve()
    r.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    capture=json.loads(args.capture.read_text());r.require(
        capture['result']=='PASS_FULL_NATIVE_ARCADE_RAM_EXIT_OBSERVATIONS' and capture['owned_live_pids']==[],
        'Capture is not terminal/qualified')
    r.check_d10_sources(ROOT,capture['verified_at'])
    for name in ('native','mame','strict'):
        steps=[s for s in capture['steps'] if s['name']==name]
        r.require(len(steps)==1 and steps[0]['state']=='PASS' and steps[0]['exit_code']==0,
                  'Capture step not uniquely successful')
    checked=[]
    for name in ('native.bin','mame.bin','strict.log'):
        matches=[x for x in capture['artifacts'] if Path(x['path']).name==name]
        r.require(len(matches)==1,'Missing/duplicate proof artifact')
        path=gaps.checked(matches[0]);checked.append(matches[0])
        if name=='strict.log':strict_marker(path.read_text(),capture['final_frame'])
        else:r.require(capture['endpoints'][name]==dict(frames=capture['final_frame']+1,
            last_frame=capture['final_frame'],full_regions=True),'Full endpoint differs')
    inventory=reconstruct_pickups.build()
    shoes=[x for x in inventory['items'] if x['source']['label']=='SHOEI']
    r.require(len(shoes)==1 and shoes[0]['retail_fields']['oid']==0x0c09,
              'Retail shoe descriptor differs from event contract')
    artifacts=[x for x in capture['artifacts'] if Path(x['path']).name=='shoe-events.jsonl']
    r.require(len(artifacts)==1,'Missing/duplicate shoe sidecar')
    path=gaps.checked(artifacts[0]);rows=[json.loads(s) for s in path.read_text().splitlines()]
    r.require(rows[0]['schema']=='stv.mame.shoe_assignments.v1' and rows[-1]==dict(
        kind='end',frame=capture['final_frame'],events=len(rows)-2),'Incomplete event stream')
    events=[validate_event(e) for e in rows[1:-1]]
    r.require(events,'No shoe assignments observed')
    result=dict(result='SOURCE_BOUND_SHOE_ASSIGNMENTS_TO_LIVE_PLAYERS',gate_proof=False,
        verified_at=capture['verified_at'],events=events,shoe_descriptor=shoes[0],
        pins=[r.pin(args.capture),artifacts[0],r.pin(Path(__file__))]+checked+inventory['pins'],
        limits=['Events are passive MAME write observations; native complete RAM stream matches that capture.',
                'Store-phase ownership is verified, not future allocation lifetime, icon deletion or audio output.'])
    r.atomic_json(out,result);print(json.dumps(dict(result=result['result'],events=events)))


if __name__=='__main__':main()
