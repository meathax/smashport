import struct
import sys
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/scenarios'))
import capture_full_exits as audit


class FullExitCaptureTests(unittest.TestCase):
    def test_bounded_window_cannot_exceed_qualified_recording(self):
        for requested,expected in [(None,77700),(10000,10000),(77700,77700)]:
            self.assertEqual(audit.bounded_final(77700,requested),expected)
        for requested in (0,-1,77701,True,1.5):
            with self.subTest(requested=requested),self.assertRaises(audit.r.ProofError):
                audit.bounded_final(77700,requested)

    def test_target_replay_uses_sealed_recording_and_rejects_wrong_provenance(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'recorder.exe';exe.write_bytes(b'fixture')
            inputs=Path(folder)/'recorded.inputs';inputs.write_text('0 :IN1 1 1\n')
            report=dict(result='COMPLETE_TARGETED_PRESSURE_OBSERVATIONS_GATE_OPEN',
                owned_live_pids=[],verified_at='current',pins=[audit.r.pin(exe)],
                cases=[dict(name='target',profile='reference',players=1,final_frame=60)],
                steps=[dict(name='target',state='PASS',exit_code=0,
                    command=[str(exe),'rom','native.bin','60','--inputs','prefix','--bot',str(inputs)],
                    proof=dict(endpoint=dict(first_frame=0,last_frame=60,frames=61),
                        artifacts={'recorded.inputs':audit.r.pin(inputs)}))])
            def select(row):return audit.targeted_replay(row,'target',1,'current',exe)
            self.assertEqual(select(report),(inputs,60))
            for change in ('live','stale','enhanced','players','failed','short','duplicate','prefix','exe'):
                row=deepcopy(report)
                if change=='live':row['owned_live_pids']=[1]
                elif change=='stale':row['verified_at']='old'
                elif change=='enhanced':row['cases'][0]['profile']='enhanced'
                elif change=='players':row['cases'][0]['players']=2
                elif change=='failed':row['steps'][0]['exit_code']=1
                elif change=='short':row['steps'][0]['proof']['endpoint']['frames']=60
                elif change=='duplicate':row['cases']*=2
                elif change=='prefix':row['steps'][0]['command'][-1]='prefix'
                elif change=='exe':row['pins'][0]['sha256']='0'*64
                with self.subTest(change=change),self.assertRaises(audit.r.ProofError):select(row)
            inputs.write_text('changed')
            with self.assertRaises(audit.r.ProofError):select(report)

    def test_child_registration_survives_report_saves(self):
        class Child:
            pid=12345
            code=None
            def poll(self):return self.code
        with tempfile.TemporaryDirectory() as folder:
            report=audit.r.Report(Path(folder),dict(steps=[]))
            child=Child();row=dict(name='native',state='RUNNING')
            audit.register_child(report,child,row)
            report.save()
            self.assertEqual(report.data['owned_live_pids'],[child.pid])
            self.assertEqual(row['pid'],child.pid)
            child.code=0;report.save()
            self.assertEqual(report.data['owned_live_pids'],[])

    def test_complete_regions_and_endpoint_are_required(self):
        def record(kind,frame,payload):return struct.pack('<BIH',kind,frame,len(payload))+payload
        initial=b''.join(record(k,0,struct.pack('<H',i)+bytes(256))
            for k in (1,2,3) for i in range(audit.r.lsfile.SIZES[k]//256))
        summary=record(9,0,bytes(8));end=record(8,0,b'')
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'full.bin'
            p.write_bytes(b'STVLS001'+initial+summary+end)
            self.assertEqual(audit.full_endpoint(p,0)['frames'],1)
            for body in [initial+end,initial+summary+summary+end,
                    initial[265:]+summary+end,
                    record(1,0,bytes(6))+summary+end,
                    initial+summary+end+record(9,1,bytes(8))+record(8,1,b'')]:
                p.write_bytes(b'STVLS001'+body)
                with self.assertRaises(audit.r.ProofError):audit.full_endpoint(p,0)


if __name__=='__main__':unittest.main()
