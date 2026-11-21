"""Bind observed collision death tags and wave difficulty writes to retail code."""
import argparse
import json
from pathlib import Path
import subprocess
import coverage_gap_manifest as gaps
import rebaseline as runner
from mproc_continuation_audit import R,K,I,M,expect,load_current,window,bind_relocated_source,pin,require

ROOT=Path(__file__).resolve().parents[2]


def merged_dead(event):
    base=(event['process']+0x208)&~15
    require(event['address'] in (base,base+16),'Write outside DEAD storage words')
    for key in ('mask','data','old_dead','merged_dead'):require(0<=event[key]<=65535,'Invalid word')
    shift=8 if event['address']==base else -8
    mask=(event['mask']>>shift)&255 if shift>0 else (event['mask']&255)<<8
    data=(event['data']>>shift)&255 if shift>0 else (event['data']&255)<<8
    return ((event['old_dead']&~mask)|(data&mask))&65535


def death_assignment(event):
    if event['kind']!='dead_write' or event['pc_raw']!=0xffe199b0:return False
    require(merged_dead(event)==event['merged_dead'],'Incorrect unaligned DEAD merge')
    if event['merged_dead']==event['old_dead']:return False
    require(event['old_dead']==0 and event['merged_dead']==event['a11_raw']==event['killer_oid_raw']!=0,'Death tag/OID differs')
    require(event['process']==event['a0_raw']==event['a7_raw']==event['object_oplink_raw'],'Collision process links differ')
    require(event['player_slot']==event['process_player_raw'] and event['player_slot'] in (1,2),'Player owner differs')
    require(0x01000000<=event['a10_raw']<0x01100000,'Killer object outside RAM')
    return True


def difficulty_assignment(event):
    if event['kind']!='difficulty_write':return False
    root=event['a0_raw']
    require(event['pc_raw']==0xffe19690 and event['address']==root+32 and
        0<=root-0x010ddef0<960 and (root-0x010ddef0)%96==0,'Wrong wave parameter slot')
    require(event['entry_type_raw']==event['a2_raw']==6 and event['a10_raw']==event['merged']==0 and
        event['mask']==65535 and event['circuit']==0 and event['wave'] in (1,2),'Wrong adaptive-write context')
    return True


def build(capture_path,life_path):
    capture=json.loads(capture_path.read_text());life=json.loads(life_path.read_text())
    require(capture['result']=='STRICT_OPENING_MATCH_RAW_DAMAGE_EVENTS' and not capture['owned_live_pids'],'Trace incomplete')
    require(life['result']=='RETAIL_DEATH_DECREMENTS_BOUND_DAMAGE_OBJECT_OPEN','Life binding incomplete')
    for p in capture['pins']+life['pins']:gaps.checked(p)
    raw=gaps.checked(capture['artifacts']['damage-events.jsonl'])
    rows=[json.loads(line) for line in raw.read_text().splitlines()]
    require(rows[1:-1]==capture['events'] and rows[-1]==dict(kind='end',frame=5000,events=len(rows)-2),'Damage receipt differs')
    _,_,_,rom,instructions,generated=load_current(ROOT)
    p1=ROOT/'build/p1/robo.bin';mask=ROOT/'build/p1/robo.bin.relmask';listing=ROOT/'build/p1/listing.json'
    bindings={}
    specs=[('death','ROBO.ASM',0xffe19980,0xffe199b0,[1434,1435],[('MOVE',R('A7'),R('A0')),('MOVE',R('A11'),M('DISP','A0',520),K(0))]),
        ('difficulty','ROBO.ASM',0xffe19660,0xffe19690,[1400,1401],[('CLR',R('A10')),('MOVE',R('A10'),M('DISP','A0',32),K(0))]),
        ('wave_root','HULKS.ASM',0xffe3eb40,0xffe3eb70,[720],[('MOVI',I(0x010ddef0),R('A0'))])]
    for name,module,start,end,lines,patterns in specs:
        seq=window(instructions,start,end);require(len(seq)==len(patterns),'Instruction extent differs')
        for ins,pattern in zip(seq,patterns):expect(ins,*pattern)
        bindings[name]=bind_relocated_source(seq,rom,p1.read_bytes(),mask.read_bytes(),json.loads(listing.read_text())['instructions'],
            module,lines,(ROOT/'original/archive/smashtv-main'/module).read_text().splitlines())
    deaths=[e for e in rows[1:-1] if death_assignment(e)]
    difficulty=[e for e in rows[1:-1] if difficulty_assignment(e)]
    links=[]
    for e in deaths:
        matches=[v for v in life['bound'] if v['player']==e['player_slot'] and v['a13_raw']==e['process'] and
            v['dead_raw']==e['merged_dead'] and v['frame']>=e['frame']]
        require(len(matches)==1,'Death assignment lacks unique later life decrement')
        v=matches[0]
        require(not any(r['kind']=='dead_write' and r['process']==e['process'] and e['frame']<r['frame']<v['frame'] and
            r['merged_dead']!=e['merged_dead'] for r in rows[1:-1]),'Death tag changed before life decrement')
        links.append(dict(assignment=e,decrement=v,delay_frames=v['frame']-e['frame']))
    require(len(links)==2 and {x['assignment']['player_slot'] for x in links}=={1,2},'Expected both opening players')
    return dict(schema='stv.bound_opening_damage.v1',result='COLLISION_TAG_AND_DIFFICULTY_WRITES_BOUND_BALANCE_OPEN',gate_proof=False,
        verified_at=subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=ROOT).strip(),native_protected_source=capture['verified_at'],
        bindings=bindings,death_links=links,difficulty_writes=difficulty,
        difficulty_value_changes=sum(e['old']!=e['merged'] for e in difficulty),
        pins=[pin(p) for p in [capture_path,life_path,raw,p1,mask,listing,Path(__file__),ROOT/'original/archive/smashtv-main/ROBO.ASM',
            ROOT/'original/archive/smashtv-main/HULKS.ASM']]+generated,
        limits=['Observed collision-handler ownership at assignment and persistence to decrement; not universal process-lifetime closure.',
            'Object positions and OID do not establish sprite collision geometry or enhanced-profile causality.',
            'Adaptive-write execution may leave an already-zero value unchanged; do not infer a difficulty reduction.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('capture','life','out'):parser.add_argument('--'+key,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh output')
    result=build(args.capture.resolve(),args.life.resolve());runner.atomic_json(out,result)
    print(json.dumps(dict(result=result['result'],deaths=len(result['death_links']),difficulty_writes=len(result['difficulty_writes']),difficulty_value_changes=result['difficulty_value_changes'])))
