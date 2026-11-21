"""Owned headless working-tree experiments; explicitly not a release/fidelity gate."""
import argparse, concurrent.futures, hashlib, json, os, pathlib, shutil, subprocess, threading, time
from stage_integrity import read_integrity
ROOT=pathlib.Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def parse_route_targets(values):
 result={}
 for value in values:
  seed_text,sep,targets=value.partition('=')
  if not sep:raise ValueError('route target requires SEED=C0,C1,C2')
  seed=int(seed_text);waves=[int(v) for v in targets.split(',')]
  if seed<0 or seed in result or len(waves)!=3 or any(w<1 or w>last for w,last in zip(waves,(10,18,22))):
   raise ValueError('invalid or duplicate route target')
  result[seed]=','.join(map(str,waves))
 return result

def main():
 p=argparse.ArgumentParser();p.add_argument('--exe',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--kind',choices=['probe','pressure'],required=True);p.add_argument('--frames',type=int,default=450000);p.add_argument('--players',type=int,choices=[1,2]);p.add_argument('--branch',type=int,default=0);p.add_argument('--mode',choices=['all','classic','wide'],default='all');p.add_argument('--seeds',type=int,nargs='+');p.add_argument('--route-target',action='append',default=[])
 p.add_argument('--require-integrity',action='store_true',help='Require complete passive list-health evidence; retain Classic retail failures for censoring.')
 a=p.parse_args()
 try:target_map=parse_route_targets(a.route_target)
 except ValueError as error:p.error(str(error))
 out=a.out.resolve();assert out.is_relative_to(ROOT/'build') and not out.exists();out.mkdir(parents=True)
 exe=out/'stage-audit-frozen.exe';shutil.copy2(a.exe,exe)
 rom=ROOT/'original/user-supplied-roms/smashtv.zip'
 jobs=[(wide,players,seed) for wide in ((0,1) if a.mode=='all' else (int(a.mode=='wide'),)) for players in ((a.players,) if a.players else ((2,) if a.kind=='probe' else (1,2))) for seed in (a.seeds or ((0,1,2) if a.kind=='probe' else (1,2)))]
 report={'result':'RUNNING','kind':a.kind,'branch':a.branch,'route_targets':os.environ.get('STV_STAGE_ROUTE_TARGETS'),'route_targets_by_seed':target_map,'gate_proof':False,'owner_pid':os.getpid(),'base_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'exe_sha256':sha(exe),'rom_sha256':sha(rom),'translation_index_sha256':sha(ROOT/'generated/private/l8/l8_translation_index.csv'),'diagnostics':os.environ.get('STV_STAGE_DIAGNOSTICS'),'jobs':{},'live_pids':[],'note':'Probe uses retail immunity/neutral landing window; pressure uses legal bot inputs only. No sound output or desktop window.'}
 report['require_integrity']=a.require_integrity
 lock=threading.Lock()
 def save():
  tmp=out/'report.tmp';tmp.write_text(json.dumps(report,indent=2)+'\n');os.replace(tmp,out/'report.json')
 def run(job):
  wide,players,seed=job;name=f'{"wide" if wide else "classic"}-{players}p-seed{seed}';dest=out/name;dest.mkdir()
  frames=a.frames if a.kind=='probe' or players==2 else max(a.frames,600000)
  cmd=[str(exe),str(rom),str(dest),str(wide),str(players),str(seed),str(frames),'1' if a.kind=='probe' else '0',str(a.branch)]
  with (dest/'run.log').open('w') as log:
   child_env=os.environ.copy()
   if seed in target_map:child_env['STV_STAGE_ROUTE_TARGETS']=target_map[seed]
   process=subprocess.Popen(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,env=child_env)
   with lock:report['jobs'][name]={'state':'RUNNING','pid':process.pid,'command':cmd,'frames':frames,'route_targets':child_env.get('STV_STAGE_ROUTE_TARGETS')};report['live_pids'].append(process.pid);save()
   try:code=process.wait(timeout=2400)
   except subprocess.TimeoutExpired:process.kill();code=process.wait()
  artifacts={key:sha(dest/name) for key,name in (('csv_sha256','entrances.csv'),('coverage_sha256','native.cov'),('clear_events_sha256','clear-events.csv')) if (dest/name).exists()}
  text=(dest/'run.log').read_text();terminal=f'STAGE_TERMINAL frames={frames} ' in text
  health,error=None,None
  if a.require_integrity:
   try:
    health=read_integrity(dest,text,frames)
    if wide and health['first_invalid_frame'] is not None:error='Invalid widescreen list state'
   except (OSError,ValueError) as failure:error=str(failure)
  rooms=sorted({tuple(map(int,line.split()[1:])) for line in text.splitlines() if line.startswith('ROOM ')})
  with lock:
   report['jobs'][name].update(**artifacts,state='PASS' if code==0 and terminal and not error else 'FAIL',exit_code=code,terminal=terminal,rooms=rooms,log_sha256=sha(dest/'run.log'),list_integrity=health,list_integrity_error=error);report['live_pids'].remove(process.pid);save()
  return code==0 and terminal and not error
 save()
 with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:ok=all(list(pool.map(run,jobs)))
 report['result']='COMPLETE' if ok else 'FAILED';report['live_pids']=[];save();print(json.dumps({'result':report['result'],'report':str(out/'report.json')}));return 0 if ok else 1
if __name__=='__main__':raise SystemExit(main())
