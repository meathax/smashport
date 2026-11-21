#!/usr/bin/env python3
"""Verify repeatable first resumed Hulk motion-field writes in paired L8 runs."""
from __future__ import annotations
import argparse, hashlib, json, re
from pathlib import Path

MAME_SHA256="af6966108d9b52c22465c6d50f4e5d50cc371b50f2d27dc443935f287aad37a3"
ROM_SHA256="7bced2e7e177e9e01005c00d7c56b6daf8d9471faf4fd604096c03d340e2b757"
PROCESSES=["0108b0b0","0108c190","0108ab10","0108ccd0"]
POSITION_OFFSETS={"x_low":0x60,"x_high":0x70,"y_low":0x80,"y_high":0x90}


def sha(p:Path)->str:return hashlib.sha256(p.read_bytes()).hexdigest()
def fields(line:str)->dict[str,str]:
 out={}
 for t in line.split():
  if "=" in t:
   k,v=t.split("=",1);out[k.lower()]=v.lower()
 return out
def require(ok:bool,msg:str):
 if not ok:raise ValueError(msg)
def i32(v:int)->int:return v if v<0x80000000 else v-0x100000000

def read_run(path:Path)->dict[str,object]:
 text=path.read_text(encoding="utf-8-sig",errors="replace")
 for marker in ["L8_HULK_MOTION_PROBE target=paid_PRCSLP_to_first_MV_and_position_deltas",
                "L8_HULK_WAKE_ARMED frame=3528","L8_HULK_MOTION_DONE frame=3950",
                "L8_ACTION coin1_press frame=2600","L8_ACTION start1_press frame=3150","Average speed:"]:
  require(marker in text,f"{path}: missing {marker}")
 require("[LUA ERROR]" not in text,f"{path}: Lua error")
 lines=[line for line in text.splitlines() if "L8_HULK_WAKE_TAP" in line]
 stream="\n".join(re.sub(r"\\s+"," ",x.strip().lower()) for x in lines)+"\n"
 return {"path":path,"sha256":sha(path),"text":text,"rows":[fields(x) for x in lines],"event_lines":lines,
         "event_stream":stream,"event_stream_sha256":hashlib.sha256(stream.encode()).hexdigest()}

def select(rows,stage,process):return [r for r in rows if r.get("stage")==stage.lower() and r.get("process")==process]

def main()->int:
 root=Path(__file__).resolve().parent.parent
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument("run_a",type=Path);p.add_argument("run_b",type=Path)
 p.add_argument("--mame",type=Path,required=True);p.add_argument("--rom-zip",type=Path,required=True)
 p.add_argument("--probe",type=Path,default=root/"tools/mame_l8_hulk_motion_deltas.lua")
 p.add_argument("--sleep-report",type=Path,default=root/"evidence/l8-hulk-sleep-resume-2026-09-23.json")
 p.add_argument("--disassembly-dir",type=Path,default=root/"evidence/l8-hulk-sleep-resume-disassembly")
 p.add_argument("--report",type=Path,default=root/"evidence/l8-hulk-motion-deltas-2026-09-23.json")
 a=p.parse_args()
 require(sha(a.mame)==MAME_SHA256,"pinned MAME hash mismatch")
 require(sha(a.rom_zip)==ROM_SHA256,"supplied ROM archive hash mismatch")
 prev=json.loads(a.sleep_report.read_text(encoding="utf-8"))
 disasm_manifest=a.disassembly_dir/"manifest.json"
 require(sha(disasm_manifest)==prev["probe"]["disassembly_manifest_sha256"],"pinned source listing manifest mismatch")
 for item in prev["probe"]["targets"].values():
  listing=a.disassembly_dir/item["listing"]
  require(sha(listing)==item["listing_sha256"],f"disassembly changed: {listing}")
 listing_v=(a.disassembly_dir/"ohulk-resumed-velocity.txt").read_text(encoding="utf-8").lower()
 listing_p=(a.disassembly_dir/"object-position-store.txt").read_text(encoding="utf-8").lower()
 for ins in ["ffe3ff60: move   a1,*a8(20h),1","ffe3ff80: move   a0,*a8(40h),1","ffe40080: move   a1,*a8(20h),1","ffe400a0: move   a0,*a8(40h),1"]:
  require(ins in listing_v,f"velocity listing missing {ins}")
 for ins in ["ffe04b80: move   a6,*a8(80h),1","ffe04bd0: move   a7,*a8(60h),1"]:
  require(ins in listing_p,f"position listing missing {ins}")
 r1,r2=read_run(a.run_a),read_run(a.run_b)
 require(r1["event_stream"]==r2["event_stream"],"paired motion-delta event streams differ")
 rows=r1["rows"]
 prev_actors={x["process_pointer"].lower().removeprefix("0x"):x for x in prev["actors"]}
 actors=[]
 for process in PROCESSES:
  base=int(process,16); prior=prev_actors[process];obj=int(prior["object_pointer"],16)
  mv=select(rows,"FIRST_RESUMED_MV_ENTRY",process)
  require(len(mv)==1 and mv[0]["frame"]=="3901",f"{process}: resumed MV entry changed")
  writes=select(rows,"POST_MV_FIELD_WRITE",process)
  require(len(writes)==20,f"{process}: expected 20 velocity/position word writes, got {len(writes)}")
  by_offset={}
  for row in writes:
   off=int(row["object_offset"],16)
   require(int(row["address"],16)==obj+off and int(row["actor_object"],16)==obj,f"{process}: object identity/offset mismatch")
   by_offset.setdefault(off,[]).append(row)
  require(set(by_offset)=={0x20,0x30,0x40,0x50,0x60,0x70,0x80,0x90},f"{process}: field offset set changed")
  for off in (0x20,0x30,0x40,0x50):
   require(len(by_offset[off])==1 and by_offset[off][0]["frame"]=="3901",f"{process}: first velocity word not frame3901")
  for off in (0x60,0x70,0x80,0x90):
   require(len(by_offset[off])==4 and [r["occurrence"] for r in by_offset[off]]==["1","2","3","4"],f"{process}: expected four position samples at offset {off:x}")
  samples=[]
  for idx in range(4):
   words={off:int(by_offset[off][idx]["data"],16) for off in (0x60,0x70,0x80,0x90)}
   fr={by_offset[off][idx]["frame"] for off in (0x60,0x70,0x80,0x90)}
   require(len(fr)==1,f"{process}: position word pair frames differ")
   x=((words[0x70]<<16)|words[0x60])&0xffffffff
   y=((words[0x90]<<16)|words[0x80])&0xffffffff
   samples.append({"occurrence":idx+1,"frame":int(next(iter(fr))),"raw_words":{f"0x{off:02x}":f"0x{value:04x}" for off,value in words.items()},"x_fixed_word":f"0x{x:08x}","y_fixed_word":f"0x{y:08x}"})
  require([s["frame"] for s in samples]==[3902,3907,3912,3917],f"{process}: position update cadence changed")
  deltas=[]
  for previous,current in zip(samples,samples[1:]):
   dx=i32((int(current["x_fixed_word"],16)-int(previous["x_fixed_word"],16))&0xffffffff)
   dy=i32((int(current["y_fixed_word"],16)-int(previous["y_fixed_word"],16))&0xffffffff)
   deltas.append({"from_frame":previous["frame"],"to_frame":current["frame"],"x_signed_raw_delta":dx,"y_signed_raw_delta":dy})
  prior_first={w["offset"].lower():w for w in prior["first_object_field_writes"]}
  first_fields=[]
  for off in (0x20,0x30,0x40,0x50):
   row=by_offset[off][0]
   require(row["data"]==prior_first[f"0x{off:02x}"]["data_word"].lower(),f"{process}: first velocity write diverges from preceding report")
   first_fields.append({"offset":f"0x{off:02x}","data_word":row["data"],"pc_after_store":row["pc"]})
  for off in (0x60,0x70,0x80,0x90):
   require(by_offset[off][0]["data"]==prior_first[f"0x{off:02x}"]["data_word"].lower(),f"{process}: initial position word diverges from preceding report")
  actors.append({"process_pointer":f"0x{process}","object_pointer":f"0x{obj:08x}","first_mv_frame":int(mv[0]["frame"]),"first_velocity_words":first_fields,"position_samples":samples,"five_frame_signed_raw_deltas":deltas})
 tap_count=len(rows)
 require(tap_count==196,f"unexpected total tap count: {tap_count}")
 report={"date":"2026-09-23","result":"PASS","scope":"Two paired fresh-config/NVRAM MAME L8 paid-start runs repeat the first resumed Hulk MV entry, velocity-field writes, and four position-field samples for the first four processes.",
  "reference":{"mame_version":"0.289","mame_executable_sha256":MAME_SHA256,"rom_archive_sha256":ROM_SHA256,"capture_armed_frame":3528,"first_mv_frame":3901,"position_sample_frames":[3902,3907,3912,3917],"fresh_config_and_nvram_each_run":True},
  "probe":{"lua_probe":str(a.probe),"lua_probe_sha256":sha(a.probe),"sleep_resume_report":str(a.sleep_report),"sleep_resume_report_sha256":sha(a.sleep_report),"disassembly_manifest":str(disasm_manifest),"disassembly_manifest_sha256":sha(disasm_manifest)},
  "runs":[{"log":str(x["path"]),"log_sha256":x["sha256"],"event_count":len(x["rows"]),"event_stream_sha256":x["event_stream_sha256"]} for x in (r1,r2)],
  "reference_event_stream_sha256":r1["event_stream_sha256"],"paired_stream_identical":True,"event_count":tap_count,"actors":actors,
  "limits":["Selected MAME L8 reference only; no physical-PCB/full-game identity claim.","The report records raw fixed-point words and signed deltas over five-frame sample intervals; it does not infer monitor pixels, 16:9 positions, or a host-independent movement model.","The initial MV/velocity/position fields are source-mapped; full DOHULK/SETUP_DIR/GET_RUN decision flow and all intermediate frame updates remain open.","Captured process/object pointers and values do not feed native gameplay."],
  "next_action":"Trace the first resumed DOHULK/SETUP_DIR/GET_RUN branch and velocity selection at the observed PCs, then capture contiguous per-frame movement state before translating an actor continuation."}
 a.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
 print(json.dumps({"result":"PASS","report":str(a.report),"event_count":tap_count,"event_stream_sha256":r1["event_stream_sha256"],"position_samples":[3902,3907,3912,3917],"actors":[{"process":x["process_pointer"],"object":x["object_pointer"],"deltas":x["five_frame_signed_raw_deltas"]} for x in actors]},indent=2))
 return 0
if __name__=="__main__":raise SystemExit(main())


