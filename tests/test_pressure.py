import contextlib
import io
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
ROOT=next(p for p in Path(__file__).resolve().parents if (p/'tools/scenarios/pressure.py').is_file())
CANDIDATE=Path(__file__).with_name('pressure.py')
spec=importlib.util.spec_from_file_location('pressure_candidate',CANDIDATE if CANDIDATE.is_file() else ROOT/'tools/scenarios/pressure.py')
pressure=importlib.util.module_from_spec(spec)
spec.loader.exec_module(pressure)

END='native: frames=10000 cycles=100000 pc=ffe01f00 faults=121 hcount_left=0 input_left=0 input_mismatch=0 big_field=0\n'
def stat(p=1,f=2000,c=0,w=1,n=1000,d=1,a=10,k=20):
    return f'WAVESTAT{p} f={f} circuit={c} wave={w} frames={n} deaths={d} avg_enemies={a:.1f} peak={k}\n'

class PressureTest(unittest.TestCase):
    def test_exact_inclusive_ratio_boundary_without_rounding_waiver(self):
        # (18/3+1)/(11/3+1) is exactly3/2, formerly1.5000000000000002.
        reference=''.join(stat(f=2000+i*2000,d=d) for i,d in enumerate((4,4,3)))+END
        enhanced=''.join(stat(f=2000+i*2000,d=d) for i,d in enumerate((6,6,6)))+END
        row=self.compare(reference,enhanced)['rows'][0]
        self.assertEqual(row['smoothed_deaths_per_visit_ratio'],1.5)
        self.assertTrue(row['in_band'])
        outside=''.join(stat(f=2000+i*2000,d=d) for i,d in enumerate((6,6,7)))+END
        self.assertFalse(self.compare(reference,outside)['rows'][0]['in_band'])
        # The configured lower limit is0.67, not the distinct rational2/3.
        reference=stat(d=1)+stat(f=4000,d=0)+END
        enhanced=stat(d=0)+stat(f=4000,d=0)+END
        self.assertFalse(self.compare(reference,enhanced)['rows'][0]['in_band'])

    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup); self.root=Path(self.temp.name)
    def run_log(self,text,players=1,name='run.log'):
        path=self.root/name; path.write_text(text,encoding='utf-8'); return pressure.read_run(path,players)
    def compare(self,r,e,**kwargs):
        return pressure.compare(self.run_log(r,name='ref.log'),self.run_log(e,name='enh.log'),players=1,minimum_frames=1000,lower=.67,upper=1.5,**kwargs)
    def test_empty_rejects(self):
        with self.assertRaises(ValueError): self.run_log(END)
    def test_missing_terminal_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat())
    def test_duplicate_terminal_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat()+END+END)
    def test_stats_after_terminal_reject(self):
        with self.assertRaises(ValueError): self.run_log(stat()+END+stat(f=3000))
    def test_incomplete_end_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat(f=10001)+END)
    def test_malformed_stats_reject(self):
        with self.assertRaises(ValueError): self.run_log('WAVESTAT1 broken\n'+END)
    def test_out_of_order_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat()+stat(f=1999)+END)
    def test_duplicate_frame_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat()+stat()+END)
    def test_zero_frames_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat(n=0)+END)
    def test_impossible_average_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat(a=22,k=20)+END)
    def test_wrong_player_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat(p=2)+END)
    def test_missing_player_two_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat()+END,2)
    def test_both_players_retained(self):
        run=self.run_log(stat()+stat(p=2)+END,2); groups,_=pressure.aggregate(run,1000)
        self.assertEqual(set(groups),{(1,0,1),(2,0,1)})
    def test_revisits_preserved_and_weighted(self):
        run=self.run_log(stat(a=2,k=6)+stat(f=5000,n=3000,a=6,k=8)+END)
        group=pressure.aggregate(run,1000)[0][(1,0,1)]
        self.assertEqual((group['visits'],group['frames'],group['mean_enemies']),(2,4000,5))
    def test_all_short_reject(self):
        with self.assertRaises(ValueError): self.compare(stat(n=100)+END,stat(n=100)+END)
    def test_short_visits_explicit(self):
        result=self.compare(stat(n=100)+stat(f=3000)+END,stat()+END)
        self.assertEqual(len(result['excluded']['reference']),1)
    def test_unmatched_rooms_not_success(self):
        result=self.compare(stat()+stat(f=3000,w=2)+END,stat()+END)
        self.assertEqual(result['result'],'INCOMPLETE')
        self.assertEqual(result['unmatched']['reference_only'][0]['wave'],2)
    def test_required_missing_room_not_success(self):
        self.assertEqual(self.compare(stat()+END,stat()+END,required_rooms={(0,1),(0,2)})['result'],'INCOMPLETE')
    def test_band_deaths_flagged(self):
        self.assertEqual(self.compare(stat(d=0)+END,stat(d=5)+END)['result'],'OUT_OF_BAND')
    def test_band_frames_flagged(self):
        self.assertEqual(self.compare(stat()+END,stat(n=1900)+END)['result'],'OUT_OF_BAND')
    def test_unknown_seed_never_gate_proof(self):
        result=self.compare(stat()+END,stat()+END)
        self.assertEqual(result['result'],'OBSERVATIONS_IN_BAND'); self.assertFalse(result['gate_proof'])
    def test_inputs_mismatch_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat()+END.replace('input_mismatch=0','input_mismatch=1'))
    def test_unconsumed_hcount_rejects(self):
        with self.assertRaises(ValueError): self.run_log(stat()+END.replace('hcount_left=0','hcount_left=1'))
    def test_duplicate_required_rooms_reject(self):
        path=self.root/'rooms.json'; path.write_text('[[0,1],[0,1]]')
        with self.assertRaises(ValueError): pressure.read_rooms(path)
    def test_cli_incomplete_returns_three(self):
        r=self.root/'r.log'; e=self.root/'e.log'; r.write_text(stat()+stat(f=3000,w=2)+END); e.write_text(stat()+END)
        with contextlib.redirect_stdout(io.StringIO()): code=pressure.main([str(r),str(e),'--json',str(self.root/'out.json')])
        self.assertEqual(code,3); self.assertEqual(json.loads((self.root/'out.json').read_text())['result'],'INCOMPLETE')
    def test_cli_existing_output_preserved(self):
        path=self.root/'out.json'; path.write_text('original')
        with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit) as caught:
            pressure.main(['unused','unused','--json',str(path)])
        self.assertEqual(caught.exception.code,2); self.assertEqual(path.read_text(),'original')

def partial(**kw):return stat(**kw).replace('WAVESTAT','WAVEPART',1)
def boundary(player=1,frame=2000,fc=0,fw=1,tc=0,tw=2,units=1):
    return f'WAVEBOUND{player} f={frame} from_circuit={fc} from_wave={fw} to_circuit={tc} to_wave={tw} life_decrease={units}\n'

class Boundaries(unittest.TestCase):
    setUp=PressureTest.setUp
    run_log=PressureTest.run_log
    compare=PressureTest.compare
    def test_completed_stat_at_terminal_frame_is_valid(self):
        self.assertEqual(self.run_log(stat(f=10000)+END)['observations'][0]['frame'],10000)
    def test_partial_at_terminal_retained_not_averaged(self):
        r=self.run_log(stat()+partial(f=10000,w=2,n=3000)+END)
        self.assertEqual(len(r['partial_visits']),1)
        grouped,_=pressure.aggregate(r,1000)
        self.assertEqual(len(grouped),1)
        self.assertNotIn((1,0,2),grouped)
    def test_partial_only_cannot_be_completed_room_proof(self):
        with self.assertRaises(ValueError):self.run_log(partial(f=10000)+END)
    def test_partial_p2_does_not_fill_missing_completed_p2(self):
        with self.assertRaises(ValueError):self.run_log(stat()+partial(p=2,f=10000)+END,players=2)
    def test_boundary_drop_makes_comparison_incomplete(self):
        log=stat()+boundary(units=2)+stat(f=4000,w=2)+END
        result=self.compare(log,log)
        self.assertEqual(result['result'],'INCOMPLETE')
        self.assertEqual(result['unattributed_boundary_life_decreases'],dict(reference=2,enhanced=2))
        self.assertEqual(result['rows'][0]['reference']['deaths'],1)
    def test_boundary_without_any_counter_loss_rejected(self):
        with self.assertRaises(ValueError):self.run_log(stat()+boundary(units=0)+END)
    def test_boundary_with_unchanged_room_rejected(self):
        with self.assertRaises(ValueError):self.run_log(stat()+boundary(tw=1)+END)
    def test_boundary_after_terminal_rejected(self):
        with self.assertRaises(ValueError):self.run_log(stat()+END+boundary())
    def test_boundary_out_of_order_rejected(self):
        with self.assertRaises(ValueError):self.run_log(stat()+boundary(frame=1999)+END)
    def test_boundary_duplicate_rejected(self):
        with self.assertRaises(ValueError):self.run_log(stat()+boundary()+boundary()+END)
    def test_boundary_player_and_circuit_domain_rejected(self):
        for line in (boundary(player=2),boundary(tc=3),boundary(units=65536)):
            with self.subTest(line=line),self.assertRaises(ValueError):self.run_log(stat()+line+END)
    def test_partial_after_terminal_rejected(self):
        with self.assertRaises(ValueError):self.run_log(stat()+END+partial(f=10000))
    def test_partial_duplicate_frame_rejected(self):
        with self.assertRaises(ValueError):self.run_log(stat()+partial(f=2000)+END)
    def test_partial_counts_preserved_in_comparison(self):
        a=stat()+partial(f=9000)+END
        result=self.compare(a,a)
        self.assertEqual(result['partial_visit_counts'],dict(reference=1,enhanced=1))
        self.assertEqual(result['result'],'OBSERVATIONS_IN_BAND')
        self.assertFalse(result['gate_proof'])
    def test_both_players_keep_boundary_and_partial_records(self):
        text=stat()+boundary()+stat(p=2)+boundary(player=2)+partial(f=9000)+partial(p=2,f=9000)+END
        r=self.run_log(text,players=2)
        self.assertEqual([x['player'] for x in r['boundary_life_decreases']],[1,2])
        self.assertEqual([x['player'] for x in r['partial_visits']],[1,2])


if __name__=='__main__': unittest.main()
