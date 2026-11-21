"""Exercise recorder host-read isolation using the pinned MAME executable.

Separate from the portable default suite: requires the private arcade oracle
and ROM inputs. A failed Lua assertion may still yield MAME exit zero, so both
the success marker and absence of Lua errors are required.
"""
import argparse
import json
from pathlib import Path
import subprocess
import rebaseline as r
from capture_full_exits import MAME

ROOT=Path(__file__).resolve().parents[2]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--recorder',type=Path,default=ROOT/'tools/lockstep/record.lua')
    args=ap.parse_args();out=args.out.resolve();recorder=args.recorder.resolve()
    r.require(out.is_relative_to(ROOT/'build') and not out.exists(),'Use fresh build output')
    r.require(recorder.is_relative_to(ROOT) and recorder.is_file(),'Recorder must be a local project file')
    exe=r.pin(MAME);r.require(exe['sha256']==r.MAME_SHA,'Unexpected MAME executable')
    lua=ROOT/'tests/observer_read_guard.lua';rom=ROOT/'original/user-supplied-roms/smashtv.zip'
    pins=[exe,r.pin(lua),r.pin(recorder),r.pin(rom),r.pin(Path(__file__))]
    out.mkdir();env=r.child_environment()
    env.update(STV_LS_OUT=str(out/'main.bin'),STV_LS_FRAMES='2',STV_LS_DUMP_EVERY='1',STV_GUARD_RECORDER=str(recorder))
    cmd=[str(MAME),'smashtv','-rompath',str(rom.parent),'-noreadconfig','-skip_gameinfo',
         '-video','none','-sound','none','-nothrottle','-seconds_to_run','2','-autoboot_script',str(lua)]
    for flag in ('cfg','nvram','diff','snapshot','input','state'):
        directory=out/flag;directory.mkdir();cmd.extend(['-'+flag+'_directory',str(directory)])
    with (out/'mame.log').open('wb') as log:
        completed=subprocess.run(cmd,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90)
    text=(out/'mame.log').read_text(errors='replace')
    passed=completed.returncode==0 and 'STV_GUARD_PROBE PASS guarded=' in text and '[LUA ERROR]' not in text and 'STV_LS_DONE frames=2 ' in text
    result=dict(result='PASS' if passed else 'FAIL',verified_at=r.git(ROOT,'rev-parse','HEAD'),
                command=cmd,exit_code=completed.returncode,pins=pins,log=r.pin(out/'mame.log'),
                limits=['Real API guard regression only; not full-game fidelity or event coverage.'])
    r.atomic_json(out/'result.json',result);print(json.dumps(dict(result=result['result'],report=str(out/'result.json'))))
    r.require(passed,'Recorder read-tap guard regression failed')


if __name__=='__main__':main()
