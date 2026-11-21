"""Executable regressions for exact capture endpoints and requested outputs."""
import argparse
from pathlib import Path
import subprocess
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/scenarios'))
import rebaseline as runner
from snapshot_cmos import read_cmos


class CaptureTests(unittest.TestCase):
    def run_capture(self,name,frames,flags,expected=0):
        directory=ARGS.out/name;directory.mkdir()
        command=[str(ARGS.exe),str(ROOT/'original/user-supplied-roms/smashtv.zip'),str(directory/'main.bin'),str(frames),'--native-hcount',*map(str,flags)]
        p=subprocess.run(command,cwd=ROOT,env=runner.child_environment(),capture_output=True,text=True,timeout=90)
        (directory/'run.log').write_text(p.stdout+p.stderr,encoding='utf-8')
        self.assertEqual(p.returncode,expected,p.stdout+p.stderr)
        return directory,p.stdout+p.stderr

    def test_complete_coupled_and_uncoupled_endpoints_and_outputs(self):
        for mode in ('coupled','uncoupled'):
            d=ARGS.out/mode
            flags=['--inputs',ARGS.inputs,'--cmos-out',d/'cmos.bin']
            if mode=='coupled':flags+=['--snd',d/'sound.bin']
            else:flags+=['--save-state',7900,d/'state.bin']
            d,_=self.run_capture(mode,8000,flags)
            runner.main_endpoint(d/'main.bin',8000)
            p=subprocess.run([sys.executable,str(ROOT/'tools/lockstep/lsfile.py'),'diff',str(ARGS.oracle),str(d/'main.bin')],capture_output=True,text=True,timeout=30)
            (d/'diff.log').write_text(p.stdout+p.stderr,encoding='utf-8');self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            cmos=(d/'cmos.bin').read_bytes();self.assertEqual(len(cmos),32768)
            if mode=='uncoupled':self.assertEqual(len(read_cmos((d/'state.bin').read_bytes())),16384)
        self.assertEqual((ARGS.out/'coupled/cmos.bin').read_bytes(),(ARGS.out/'uncoupled/cmos.bin').read_bytes())
        self.assertEqual((ARGS.out/'coupled/sound.bin').read_bytes(),ARGS.sound_oracle.read_bytes())

    def test_coupled_machine_state_is_rejected(self):
        for option in ('--save-state','--load-state'):
            name=option[2:];d=ARGS.out/name;flags=['--snd',d/'sound.bin',option]
            if option=='--save-state':flags+=[0]
            flags+=[d/'state.bin'];_,message=self.run_capture(name,1,flags,2)
            self.assertIn('unsupported with --snd',message);self.assertFalse((d/'state.bin').exists())

    def test_unreachable_save_frame_is_rejected(self):
        _,message=self.run_capture('late',1,['--save-state',2,ARGS.out/'late-state.bin'],2)
        self.assertIn('exceeds capture endpoint',message)

    def test_failed_snapshot_write_is_reported(self):
        _,message=self.run_capture('write-fail',60,['--save-state',30,ARGS.out/'absent/state.bin'],2)
        self.assertIn('cannot write state',message)

    def test_failed_sound_output_does_not_fall_back_to_uncoupled(self):
        _,message=self.run_capture('sound-fail',1,['--snd',ARGS.out/'absent/sound.bin'],2)
        self.assertIn('cannot write sound stream',message)

    def test_failed_cmos_output_is_reported(self):
        _,message=self.run_capture('cmos-fail',1,['--cmos-out',ARGS.out/'absent/cmos.bin'],2)
        self.assertIn('cannot write CMOS',message)


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ('exe','inputs','oracle','sound-oracle','out'):p.add_argument('--'+name,type=Path,required=True)
    ARGS=p.parse_args()
    for name in ('exe','inputs','oracle','sound_oracle','out'):setattr(ARGS,name,getattr(ARGS,name).resolve())
    ARGS.out.mkdir(parents=True,exist_ok=False)
    unittest.main(argv=[sys.argv[0]])
