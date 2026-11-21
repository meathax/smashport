import sys
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/scenarios'))
import prepare_recorded_exit_replay as a
import validated_exit_replay as validated
import json


class RecordedExitReplayTests(unittest.TestCase):
    def test_preserves_endpoint_input_and_rejects_invalid_source_or_missing_room(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            exe = root/'recorder.exe'; exe.write_bytes(b'fixture')
            inputs = root/'recorded.inputs'
            inputs.write_bytes(b'0 :IN1 1 1\n60 :IN1 1 0\n61 :IN1 4 1\n')
            log = root/'case.log'
            log.write_text('WAVESTAT1 f=50 circuit=1 wave=13 frames=40 deaths=1 avg_enemies=2.0 peak=4\n'
                'native: frames=100 cycles=1000 pc=ff00 faults=0 hcount_left=0 input_left=0 input_mismatch=0 big_field=0\n')
            report = dict(result='COMPLETE_OBSERVATIONS_GATE_OPEN', owned_live_pids=[],
                verified_at='runtime', jobs=[['case', 'reference', 1, 2, 100]],
                pins=[a.r.pin(exe)], steps=[dict(name='case', state='PASS', exit_code=0,
                    command=[str(exe), 'rom', 'native.bin', '100', '--bot', str(inputs)],
                    proof=dict(endpoint=dict(first_frame=0,last_frame=100,frames=101),
                        artifacts={'recorded.inputs':a.r.pin(inputs),'case.log':a.r.pin(log)}))])
            def select(r, final=60, targets=((1,13),)):
                return a.select(r,'case',1,final,'runtime',exe,targets)
            data, observed, _, _ = select(report)
            self.assertEqual(data,b'0 :IN1 1 1\n60 :IN1 1 0\n')
            self.assertEqual(observed[0]['frame'],50)
            source = root/'source.json'
            source.write_text(json.dumps(report))
            prefix = root/'prefix.inputs'; prefix.write_bytes(data)
            plan = dict(schema='stv.recorded_exit_replay.v1', result='PREPARED_NOT_EXECUTED',
                verified_at='runtime', players=1, final_frame=60, targets=[[1,13]],
                source_case='case', source_observations=observed, inputs=a.r.pin(prefix),
                pins=[a.r.pin(source),a.r.pin(exe),a.r.pin(inputs),a.r.pin(log)])
            self.assertEqual(validated.validate(plan,1,'runtime',exe),(prefix,60))
            for key in ('players','runtime','endpoint','observations','pins','targets'):
                bad=deepcopy(plan)
                if key=='players':bad['players']=True
                elif key=='runtime':bad['verified_at']='stale'
                elif key=='endpoint':bad['final_frame']=True
                elif key=='observations':bad['source_observations']=[]
                elif key=='pins':bad['pins']*=2
                elif key=='targets':bad['targets']=[[1,True]]
                with self.subTest(plan=key),self.assertRaises(a.r.ProofError):
                    validated.validate(bad,1,'runtime',exe)
            # Even a rehashed forged schedule must fail the original-prefix check.
            prefix.write_bytes(b'0 :IN1 4 1\n')
            forged=deepcopy(plan);forged['inputs']=a.r.pin(prefix)
            with self.assertRaises(a.r.ProofError):validated.validate(forged,1,'runtime',exe)
            prefix.write_bytes(data)
            mapped=deepcopy(report)
            mapped['pins']={str(exe):a.r.pin(exe)}
            self.assertEqual(select(mapped)[0],data)
            for final in (0, 50, 101, True):
                with self.subTest(final=final),self.assertRaises(a.r.ProofError):select(report,final)
            with self.assertRaises(a.r.ProofError):select(report,targets=((2,13),))
            for key in ('live','stale','duplicate','enhanced','players','failed','short','wrong_input'):
                row=deepcopy(report)
                if key=='live':row['owned_live_pids']=[123]
                elif key=='stale':row['verified_at']='old'
                elif key=='duplicate':row['jobs']*=2
                elif key=='enhanced':row['jobs'][0][1]='enhanced'
                elif key=='players':row['jobs'][0][2]=2
                elif key=='failed':row['steps'][0]['exit_code']=1
                elif key=='short':row['steps'][0]['proof']['endpoint']['frames']=100
                elif key=='wrong_input':row['steps'][0]['command'][-1]='prefix.inputs'
                with self.subTest(key=key),self.assertRaises(a.r.ProofError):select(row)
            inputs.write_bytes(b'altered')
            with self.assertRaises(a.r.ProofError):select(report)


if __name__=='__main__':unittest.main()
