"""Pair passive arcade exits with full native/arcade frame RAM; retain gaps."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import rebaseline as r
import coverage_gap_manifest as gaps
import ending_contract

ROOT=Path(__file__).resolve().parents[2]


def snapshot(image, frame):
    r.require(len(image)==0x20000,'Wrong RAM extent')
    def word(a):return struct.unpack_from('<H',image,(a-0x01000000)//8)[0]
    def long(a):return struct.unpack_from('<I',image,(a-0x01000000)//8)[0]
    ptr=long(0x01000040);seen=set();objects=[];boundary='complete'
    while ptr:
        if not (0x010003a0<=ptr<0x01033160 and (ptr-0x010003a0)%0x1f0==0):
            boundary='outside_pool';break
        if ptr in seen:boundary='cycle';break
        seen.add(ptr);objects.append(dict(address=ptr,oid=word(ptr+0x150)));ptr=long(ptr)
    return dict(frame=frame,state=[word(0x010865a0),word(0x010eb610),word(0x010eb630)],
        gratson=word(0x010ee130),objects=objects,list_boundary=boundary,boundary_pointer=ptr,
        enemy_class_objects=sum((o['oid']&0xc000)==0x8000 for o in objects),
        ram_sha256=hashlib.sha256(image).hexdigest())


def scan(path, final, selected):
    image=bytearray(0x20000);initialized=set();samples={};prior=None;transitions=[];count=0
    prior_marker=None;marker_changes=[]
    for frame,records in r.lsfile.frames(path):
        r.require(frame==count and frame<=final,'Invalid frame sequence')
        for kind,f,payload in records:
            r.require(f==frame,'Record frame mismatch')
            if kind!=1:continue
            r.require(len(payload)==258,'Expected full RAM chunk, not compact hash')
            idx=int.from_bytes(payload[:2],'little');r.require(idx<512,'RAM chunk out of range')
            image[idx*256:(idx+1)*256]=payload[2:];initialized.add(idx)
        r.require(len(initialized)==512,'Initial RAM incomplete')
        state=[struct.unpack_from('<H',image,(a-0x01000000)//8)[0]
            for a in (0x010865a0,0x010eb610,0x010eb630)]
        changed=prior is not None and state!=prior
        marker=struct.unpack_from('<H',image,(0x010ee130-0x01000000)//8)[0]
        marker_changed=prior_marker is not None and marker!=prior_marker
        if marker_changed:marker_changes.append(dict(frame=frame,before=prior_marker,after=marker,state=state))
        if changed:transitions.append(dict(frame=frame,before=prior,after=state))
        if frame in selected or changed or marker_changed:samples[frame]=snapshot(image,frame)
        prior=state;prior_marker=marker;count+=1
    r.require(count==final+1,'Incomplete final frame')
    return dict(samples=samples,state_transitions=transitions,gratson_changes=marker_changes)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();capture=json.loads(args.capture.read_text());out=args.out.resolve()
    r.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    r.require(capture['result']=='PASS_FULL_NATIVE_ARCADE_RAM_EXIT_OBSERVATIONS' and
        capture['owned_live_pids']==[] and capture['dump_every']==1 and capture['compact'] is False,
        'Capture does not establish complete every-frame full RAM')
    for pin in capture['artifacts']:gaps.checked(pin)
    directory=args.capture.resolve().parent;final=capture['final_frame']
    rows=[json.loads(s) for s in (directory/'objects.jsonl').read_text().splitlines()]
    r.require(rows[0]['schema']=='stv.mame.exit_objects.v1' and
        rows[-1]==dict(kind='end',frame=final,events=len(rows)-2),'Incomplete exit events')
    events=rows[1:-1];selected=set()
    for event in events:
        r.require(event['kind']=='exit_objects' and type(event['frame']) is int and
            0<=event['frame']<=final and event['pc_raw']==0xffec95a0 and event['mask']==65535,
            'Unexpected exit event contract')
        selected.update(f for f in (event['frame']-1,event['frame'],event['frame']+1) if 0<=f<=final)
    native=scan(directory/'native.bin',final,selected);arcade=scan(directory/'mame.bin',final,selected)
    r.require(native==arcade,'Native/arcade frame observations differ')
    paired=[]
    for event in events:
        nearby=[t for t in native['state_transitions'] if abs(t['frame']-event['frame'])<=1 and
            t['before']==[16,event['circuit'],event['old_wave']] and
            t['after']==[16,event['circuit'],event['new_wave']]]
        paired.append(dict(event=event,matching_frame_transitions=nearby,
            pairing='unique' if len(nearby)==1 else 'unresolved',clearance_proved=False))
    markers=ending_contract.build()
    report=dict(result='PASS_NATIVE_ARCADE_EXIT_FRAME_OBSERVATIONS',verified_at=capture['verified_at'],
        ending_marker_contract=markers,
        gate_proof=False,pins=[r.pin(args.capture),r.pin(Path(__file__))],observations=native,exits=paired,
        summary=dict(events=len(events),uniquely_paired=sum(x['pairing']=='unique' for x in paired),
            unresolved=sum(x['pairing']!='unique' for x in paired),
            incomplete_sample_lists=sum(x['list_boundary']!='complete' for x in native['samples'].values())),
        limits=['Frame pairing does not establish continuous process/list ownership, all-room clearance or endings.',
            'POST RAM test patterns also change GRATSON; marker changes retain raw game state and are not automatically completion events.',
            'Unpaired writes and invalid/cyclic list boundaries are retained, not silently discarded.',
            'Zero enemy-class objects is not a universal clearance rule; retail raw circuit2/wave7 has an exception.'])
    r.atomic_json(out,report);print(json.dumps(report['summary']))


if __name__=='__main__':main()
