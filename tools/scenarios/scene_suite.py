#!/usr/bin/env python3
"""Capture and compare native/expanded scene policy using actual translated code.

Every child has a fresh directory and bounded lifetime. The native-image hashes
and original stream comparisons are independent of host viewport centering.
Rendered-window centering and uncapped throughput require separate host evidence.
"""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading

ROOT = Path(__file__).resolve().parents[2]

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def read_frames(path):
    with Path(path).open(encoding="utf-8") as source:
        header = json.loads(next(source))
        if header.get("schema") != "stv.scene.frames.v1":
            raise ValueError("unexpected scene evidence schema")
        return [json.loads(line) for line in source]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=3, choices=(1,2,3,4))
    args = parser.parse_args()
    exe = args.native.resolve()
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    rom = ROOT / "original/user-supplied-roms/smashtv.zip"
    report = {"schema":1, "status":"RUNNING", "started_utc":utc(),
              "native":str(exe), "native_sha256":sha(exe), "rom_sha256":sha(rom),
              "tool_sha256":sha(__file__), "cases":[], "comparisons":[],
              "scope":"Native raster and scene policy; actual host presentation is separate."}
    lock = threading.Lock()
    status = out / "status.json"
    def save():
        temporary = out / "status.tmp"
        temporary.write_text(json.dumps(report,indent=2),encoding="utf-8")
        os.replace(temporary,status)
    save()
    files = {}
    for name, starts in (("one-player",[(3150,4)]),
                         ("two-player",[(3150,4),(3150,0x20)]),
                         ("player-two",[(3150,0x20)]),
                         ("join-intro",[(3150,4),(3200,0x20)])):
        events = [(f,1,v) for base in (2600,2640,2680,2720)
                  for f,v in ((base,1),(base+12,0))]
        for frame,mask in starts:
            events.extend(((frame,mask,1),(frame+20,mask,0)))
        path = out / (name+".inputs")
        path.write_text("".join(f"{frame} :IN1 {mask:x} {value}\n"
                               for frame,mask,value in sorted(events)),encoding="ascii")
        files[name]=path
    cases = []
    for mode in ("reference","enhanced"):
        cases.extend(((mode+"-attract",mode,None,9000),
                      (mode+"-one-player",mode,files["one-player"],8000),
                      (mode+"-two-player",mode,files["two-player"],5000),
                      (mode+"-player-two",mode,files["player-two"],5000),
                      (mode+"-operator",mode,ROOT/"tests/scenarios/operator_walk.inputs",14720)))
    cases.append(("enhanced-join-intro","enhanced",files["join-intro"],5000))

    def run(case):
        name,mode,inputs,final = case
        dest = out / name
        dest.mkdir()
        if name.endswith("attract"):
            snapshots = list(range(100,final+1,100))
        elif name.endswith("operator"):
            snapshots = list(range(500,final+1,500))+[final]
        else:
            snapshots = list(range(3100,4701,10))+list(range(4800,final+1,100))+[1000,1600,2000,2500]
        cmd = [str(exe),str(rom),str(dest/"native.bin"),str(final),
               "--native-hcount","--snd",str(dest/"native.snd"),
               "--scene-report",str(dest/"scenes.jsonl"),
               "--content",str(dest/"content.jsonl"),
               "--snap",",".join(map(str,snapshots)),"--snap-dir",str(dest)]
        if mode == "enhanced": cmd.append("--enhanced")
        if inputs: cmd.extend(("--inputs",str(inputs)))
        row = {"case":name,"profile":mode,"final_frame":final,"command":cmd,
               "input_sha256":sha(inputs) if inputs else None,"started_utc":utc(),"live":True}
        env=os.environ.copy()
        env.update(STV_LS_COMPACT="1",STV_LS_SUMMARY="1",STV_LS_DUMP_EVERY="60")
        with (dest/"run.log").open("wb") as log:
            child=subprocess.Popen(cmd,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
            row["pid"]=child.pid
            with lock: report["cases"].append(row);save()
            try: row["exit_code"]=child.wait(timeout=600)
            except subprocess.TimeoutExpired:
                child.kill();child.wait();row["exit_code"]=-999
        row["live"]=False;row["finished_utc"]=utc()
        if row["exit_code"] == 0:
            observed=read_frames(dest/"scenes.jsonl")
            row["observed_frames"]=len(observed)
            row["first_frame"]=observed[0]["frame"] if observed else None
            row["last_frame"]=observed[-1]["frame"] if observed else None
            row["native_frames"]=sum(x["view_width"]==410 for x in observed)
            row["wide_frames"]=sum(x["view_width"]==547 for x in observed)
            row["out_of_view_frames"]=sum(x["outside_nonzero_indices"]!=0 for x in observed)
            row["states"]=sorted({x["gamstate"] for x in observed})
            row["png_count"]=len(list(dest.glob("native_*.png")))
            row["endpoint_ok"]=row["first_frame"]==0 and row["last_frame"]==final and len(observed)==final+1
        with lock: save()
        return row

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
            list(pool.map(run,cases))
        for scene in ("attract","operator"):
            ref=out/("reference-"+scene)
            enh=out/("enhanced-"+scene)
            a=read_frames(ref/"scenes.jsonl");b=read_frames(enh/"scenes.jsonl")
            mismatch=next(({"reference":x,"enhanced":y} for x,y in zip(a,b)
                if any(x[k]!=y[k] for k in ("frame","gamstate","background","view_width","image_hash","circuit","wave","world_x","world_y"))),None)
            if len(a)!=len(b):mismatch={"lengths":[len(a),len(b)]}
            check={"scene":scene,"frame_images_equal":mismatch is None,
                   "first_image_difference":mismatch,"frames":len(a),"streams":[]}
            for kind,tool in (("bin","lsfile.py"),("snd","sndfile.py")):
                cmd=[sys.executable,str(ROOT/"tools/lockstep"/tool),"diff",
                     str(ref/("native."+kind)),str(enh/("native."+kind))]
                log=out/(scene+"-"+kind+"-diff.log")
                with log.open("wb") as stream:
                    result=subprocess.run(cmd,cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,timeout=180)
                check["streams"].append({"kind":kind,"command":cmd,"exit_code":result.returncode,"log":str(log)})
            report["comparisons"].append(check);save()
        ok = all(x.get("exit_code")==0 and x.get("endpoint_ok") and x.get("out_of_view_frames")==0
                 for x in report["cases"])
        ok = ok and all(x["frame_images_equal"] and all(y["exit_code"]==0 for y in x["streams"])
                        for x in report["comparisons"])
        report["status"]="PASS_CAPTURE_AND_NATIVE_SCENE_IDENTITY" if ok else "FAIL"
    except Exception as error:
        report["status"]="FAIL";report["error"]=str(error)
    report["finished_utc"]=utc();save()
    print(json.dumps({"status":report["status"],"report":str(status),
                      "cases":len(report["cases"]),"live":sum(x["live"] for x in report["cases"])}))
    return 0 if report["status"].startswith("PASS") else 1

if __name__ == "__main__":
    raise SystemExit(main())
