import copy
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import net_exe_peer as peer

def fixture():
    row = dict(time_us=0, owner_gap_us=0, event_mask=0, frame=40, confirmed=36, played=36,
               padding=100, pending=0, buffer_frames=4800, output_samples=100,
               underruns=0, errors=0, recoveries=0, output_resets=0, source_drops=0,
               device_drops=0, network_resyncs=0, network_stalls=0,
               online=True, paused=False, resyncing=False, available=True)
    return dict(audio_output_timeline=dict(schema='smash.host.audio-output-timeline.v1',
                enabled=True, interval_us=10000, history_limit=8192, event_limit=256,
                samples_observed=1, history_discarded=0, events_discarded=0,
                maximum_owner_gap_us=0, samples=[row], events=[]))

class OutputTimelineTests(unittest.TestCase):
    def test_requires_requested_actual_trace(self):
        for data in [{}, {'audio_output_timeline': {}}, {'audio_output_timeline': None}]:
            with self.assertRaises(ValueError):
                peer.validate_output_timeline(data)
        data=fixture(); data['audio_output_timeline']['enabled']=False
        with self.assertRaises(ValueError): peer.validate_output_timeline(data)

    def test_gaps_are_diagnostic_not_silence_free_proof(self):
        data=fixture(); trace=data['audio_output_timeline']
        row=trace['samples'][0]
        row.update(underruns=1, event_mask=1, resyncing=True, padding=0)
        trace['events']=[copy.deepcopy(row)]
        result=peer.validate_output_timeline(data)
        self.assertEqual(result['underrun_events'],1)
        self.assertIs(result['silence_free_proof'],False)

    def test_unknown_padding_and_monotonic_time(self):
        data=fixture(); trace=data['audio_output_timeline']
        trace['samples'][0]['padding']=None
        peer.validate_output_timeline(data)
        trace['samples'].append(copy.deepcopy(trace['samples'][0]))
        trace['samples_observed']=2
        trace['samples'][0]['time_us']=10
        with self.assertRaises(ValueError): peer.validate_output_timeline(data)

    def test_loss_and_bounds_are_explicit(self):
        for field,value in [('history_limit',8193),('event_limit',257),
                            ('samples_observed',2),('history_discarded',1),
                            ('events_discarded',-1),('maximum_owner_gap_us',True)]:
            data=fixture(); data['audio_output_timeline'][field]=value
            with self.subTest(field=field), self.assertRaises(ValueError):
                peer.validate_output_timeline(data)

    def test_confirmation_and_event_states_are_not_guessed(self):
        for field,value in [('played',37),('confirmed',41),('available',1),
                            ('event_mask',2048),('owner_gap_us',1),('errors',-1)]:
            data=fixture(); data['audio_output_timeline']['samples'][0][field]=value
            with self.subTest(field=field), self.assertRaises(ValueError):
                peer.validate_output_timeline(data)

    def test_event_must_have_reason(self):
        data=fixture(); trace=data['audio_output_timeline']
        trace['events']=[copy.deepcopy(trace['samples'][0])]
        with self.assertRaises(ValueError): peer.validate_output_timeline(data)

    def test_child_environment_requires_explicit_timeline_request(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory); rom=path/'rom.zip'; rom.write_bytes(b'private fixture')
            args=SimpleNamespace(rom=rom,profile='enhanced',inputs=path/'recorded.inputs',
                                 frames=3000,port=27015,proxy_port=27016,fault=0)
            with patch.dict(os.environ,{'STV_AUDIO_TELEMETRY':'1'}):
                no_trace=peer.child_environment(path,'join',args)
                self.assertNotIn('STV_AUDIO_TELEMETRY',no_trace)
                args.audio_output_timeline=True
                traced=peer.child_environment(path,'host',args)
                self.assertEqual(traced['STV_AUDIO_TELEMETRY'],'1')
                self.assertEqual(os.environ['STV_AUDIO_TELEMETRY'],'1')

if __name__ == '__main__':
    unittest.main()
