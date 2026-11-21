"""Fail-closed exe-peer receipts and real two-way UDP proxy regression checks."""
import copy
import importlib.util
import os
from pathlib import Path
import socket
import struct
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('net_exe_peer', ROOT / 'tools/net_exe_peer.py')
peer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(peer)


def fault_fixture(requested=0):
    events = [dict(frame=peer.effective_fault_frame(requested), before_word='bddd', after_word='3ddd',
                   before_hash='5c1c1d78', after_hash='8f4d9d78')] if requested else []
    return dict(schema='smash.net.fault-injection.v1', mode='single_exchanged_hash_frame_xor_8000',
                enabled=bool(requested), requested_frame=requested,
                effective_frame=peer.effective_fault_frame(requested), hash_period=30,
                attempts=len(events), changed_words=len(events), changed_hashes=len(events), events=events)


class FaultReceiptTests(unittest.TestCase):
    def test_selected_boundary_and_zero_disable(self):
        for requested, effective in [(0, 0), (899, 900), (900, 900), (901, 930), (1000, 1020)]:
            with self.subTest(requested=requested):
                self.assertEqual(peer.effective_fault_frame(requested), effective)
        for invalid in [-1, True, '900', 0xffffffff]:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                peer.effective_fault_frame(invalid)

    def test_actual_changes_and_replay_events(self):
        fault = fault_fixture(900)
        peer.validate_fault_injection(dict(fault_injection=fault), 'join', 900, 3000)
        fault['events'] += copy.deepcopy(fault['events'])
        fault.update(attempts=2, changed_words=2, changed_hashes=2)
        peer.validate_fault_injection(dict(fault_injection=fault), 'join', 900, 3000)
        peer.validate_fault_injection(dict(fault_injection=fault_fixture()), 'host', 900, 3000)
        peer.validate_fault_injection(dict(fault_injection=fault_fixture()), 'join', 0, 3000)

    def test_missing_fault_evidence_rejects(self):
        for value in [None, {}, []]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                peer.validate_fault_injection(dict(fault_injection=value), 'join', 900, 3000)

    def test_no_change_or_no_attempt_rejects(self):
        for field in ['attempts', 'changed_words', 'changed_hashes']:
            fault = fault_fixture(900); fault[field] = 0
            with self.subTest(field=field), self.assertRaises(ValueError):
                peer.validate_fault_injection(dict(fault_injection=fault), 'join', 900, 3000)
        fault = fault_fixture(900); fault['events'] = []
        with self.assertRaises(ValueError):
            peer.validate_fault_injection(dict(fault_injection=fault), 'join', 900, 3000)

    def test_wrong_peer_request_or_exchange_phase_rejects(self):
        for field, value in [('enabled', False), ('requested_frame', 901), ('effective_frame', 901),
                             ('hash_period', 29), ('mode', 'or_8000')]:
            fault = fault_fixture(900); fault[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                peer.validate_fault_injection(dict(fault_injection=fault), 'join', 900, 3000)
        with self.assertRaises(ValueError):
            peer.validate_fault_injection(dict(fault_injection=fault_fixture(900)), 'host', 900, 3000)
        with self.assertRaises(ValueError):
            peer.validate_fault_injection(dict(fault_injection=fault_fixture(2999)), 'join', 2999, 3000)

    def test_boolean_and_missing_counters_reject(self):
        for field in ['requested_frame', 'effective_frame', 'hash_period', 'attempts', 'changed_words', 'changed_hashes']:
            for value in [None, True, -1]:
                fault = fault_fixture(900); fault[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    peer.validate_fault_injection(dict(fault_injection=fault), 'join', 900, 3000)

    def test_actual_word_and_hash_evidence_required(self):
        for field, value in [('frame', 901), ('frame', True), ('before_word', 'bddd0'),
                             ('after_word', 'bddd'), ('after_word', '3ddc'),
                             ('after_hash', '5c1c1d78'), ('before_hash', None)]:
            fault = fault_fixture(900); fault['events'][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                peer.validate_fault_injection(dict(fault_injection=fault), 'join', 900, 3000)

    def test_successful_injection_still_requires_resync_and_final_reference(self):
        data = dict(role='join', end='frames', confirmed=3000, frame=3000, played=3000,
                    last_hash='aabb0011', desyncs=0, resyncs=0, hash_checks=99,
                    main_stopped=False, sound_fault=False, predictive_stopped=False,
                    fault_injection=fault_fixture(900))
        with self.assertRaisesRegex(ValueError, 'resynchronised'):
            peer.validate_peer(data, 'join', 3000, 'aabb0011', 900)
        data.update(desyncs=1, resyncs=1, first_desync_frame=900, first_resync_base=910)
        peer.validate_peer(data, 'join', 3000, 'aabb0011', 900)
        data['last_hash'] = 'ffffffff'
        with self.assertRaisesRegex(ValueError, 'offline reference'):
            peer.validate_peer(data, 'join', 3000, 'aabb0011', 900)


def fault_pair_fixture(host_detects=1, join_detects=1):
    return [
        dict(role=role, desyncs=detected, resyncs=1,
             first_desync_frame=900 if detected else None,
             first_resync_base=910 if role == 'join' else None,
             fault_injection=fault_fixture(900 if role == 'join' else 0))
        for role, detected in [('host', host_detects), ('join', join_detects)]
    ]


class FaultPairTests(unittest.TestCase):
    def test_host_only_join_only_and_both_are_protocol_valid(self):
        for detections in [(1, 0), (0, 1), (1, 1)]:
            host, join = fault_pair_fixture(*detections)
            with self.subTest(detections=detections):
                result = peer.validate_fault_pair(host, join, 900, 3000)
                self.assertEqual(result['detections'], sum(detections))
                self.assertEqual(result['first_resync_base'], 910)
                self.assertTrue(result['actual_changed_injection'])

    def test_neither_detection_rejects_even_when_both_resynchronised(self):
        with self.assertRaisesRegex(ValueError, 'either peer'):
            peer.validate_fault_pair(*fault_pair_fixture(0, 0), 900, 3000)

    def test_extra_detection_or_resync_rejects(self):
        for role in range(2):
            for field, value in [('desyncs', 2), ('desyncs', True), ('resyncs', 0), ('resyncs', 2), ('resyncs', True)]:
                pair = fault_pair_fixture()
                pair[role][field] = value
                with self.subTest(role=role, field=field, value=value), self.assertRaises(ValueError):
                    peer.validate_fault_pair(*pair, 900, 3000)

    def test_detection_must_be_on_exchange_grid_after_fault_before_repair(self):
        for role in range(2):
            for frame in [None, True, 870, 901, 910, 930, 3000]:
                pair = fault_pair_fixture()
                pair[role]['first_desync_frame'] = frame
                with self.subTest(role=role, frame=frame), self.assertRaises(ValueError):
                    peer.validate_fault_pair(*pair, 900, 3000)

    def test_zero_detections_require_null_frame(self):
        for role in range(2):
            pair = fault_pair_fixture()
            pair[role]['desyncs'] = 0
            with self.subTest(role=role), self.assertRaisesRegex(ValueError, 'without a detection'):
                peer.validate_fault_pair(*pair, 900, 3000)

    def test_missing_observations_reject(self):
        for role in range(2):
            for field in ['first_desync_frame', 'first_resync_base']:
                pair = fault_pair_fixture()
                del pair[role][field]
                with self.subTest(role=role, field=field), self.assertRaisesRegex(ValueError, 'missing observed'):
                    peer.validate_fault_pair(*pair, 900, 3000)

    def test_invalid_or_unobserved_resync_boundary_rejects(self):
        for boundary in [None, True, -1, 900, 3000, '910']:
            pair = fault_pair_fixture()
            pair[1]['first_resync_base'] = boundary
            with self.subTest(boundary=boundary), self.assertRaises(ValueError):
                peer.validate_fault_pair(*pair, 900, 3000)
        pair = fault_pair_fixture()
        pair[0]['first_resync_base'] = 910
        with self.assertRaisesRegex(ValueError, 'host cannot claim'):
            peer.validate_fault_pair(*pair, 900, 3000)

    def test_pair_still_requires_actual_changed_injection(self):
        pair = fault_pair_fixture()
        pair[1]['fault_injection']['events'][0]['after_word'] = 'bddd'
        with self.assertRaises(ValueError):
            peer.validate_fault_pair(*pair, 900, 3000)
        with self.assertRaises(ValueError):
            peer.validate_fault_pair(*fault_pair_fixture(), 0, 3000)


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.data = dict(role='join', end='frames', confirmed=3000, frame=3000, played=3000,
                         last_hash='aabb0011', desyncs=0, resyncs=0, hash_checks=99,
                         main_stopped=False, sound_fault=False, predictive_stopped=False)

    def test_complete(self):
        peer.validate_peer(self.data, 'join', 3000, 'aabb0011', 0)

    def test_machine_liveness_is_required_without_audio_audit(self):
        for field in ['main_stopped', 'sound_fault', 'predictive_stopped']:
            for value in [True, None, 0, "false"]:
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, "machine liveness"):
                    peer.validate_peer(dict(self.data, **{field: value}), 'join', 3000, 'aabb0011', 0)
            missing = dict(self.data)
            del missing[field]
            with self.subTest(missing=field), self.assertRaisesRegex(ValueError, "machine liveness"):
                peer.validate_peer(missing, 'join', 3000, 'aabb0011', 0)

    def test_incomplete_and_invalid_counter(self):
        for field, value in [('confirmed', 2999), ('played', 2999), ('frame', 2999),
                             ('confirmed', True), ('hash_checks', 0), ('frame', -1)]:
            with self.subTest(field=field, value=value):
                data = dict(self.data, **{field: value})
                with self.assertRaises(ValueError):
                    peer.validate_peer(data, 'join', 3000, 'aabb0011', 0)

    def test_terminal_and_hash(self):
        for field, value in [('role', 'host'), ('end', 'quit'), ('last_hash', 'aabb0012')]:
            with self.subTest(field=field):
                with self.assertRaises(ValueError):
                    peer.validate_peer(dict(self.data, **{field: value}), 'join', 3000, 'aabb0011', 0)

    def test_missing_and_boolean_counters(self):
        for field in ['confirmed', 'frame', 'played', 'desyncs', 'resyncs', 'hash_checks']:
            for value in [None, True, '3000']:
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    peer.validate_peer(dict(self.data, **{field: value}), 'join', 3000, 'aabb0011', 0)

    def test_uninjected_divergence(self):
        for field in ['desyncs', 'resyncs']:
            with self.assertRaises(ValueError):
                peer.validate_peer(dict(self.data, **{field: 1}), 'join', 3000, 'aabb0011', 0)

    def test_injection_needs_exact_resync_and_observed_boundaries(self):
        data = dict(self.data, fault_injection=fault_fixture(1000), first_desync_frame=1020, first_resync_base=1030)
        for resyncs in [0, 2]:
            with self.assertRaises(ValueError):
                peer.validate_peer(dict(data, desyncs=1, resyncs=resyncs), 'join', 3000, 'aabb0011', 1000)
        peer.validate_peer(dict(data, desyncs=1, resyncs=1), 'join', 3000, 'aabb0011', 1000)
        peer.validate_peer(dict(data, desyncs=0, resyncs=1, first_desync_frame=None), 'join', 3000, 'aabb0011', 1000)
        peer.validate_peer(dict(self.data, role='host', resyncs=1, first_desync_frame=None, first_resync_base=None,
                                fault_injection=fault_fixture()), 'host', 3000, 'aabb0011', 1000)

    def test_atomic_write_preserves_other_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'report.json'
            untouched = Path(directory) / 'old.json'; untouched.write_bytes(b'old evidence')
            peer.atomic_json(path, self.data)
            self.assertFalse(path.with_suffix('.json.pending').exists())
            self.assertEqual(untouched.read_bytes(), b'old evidence')

    def test_child_environment_isolated(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            rom = path / 'rom.zip'; rom.write_bytes(b'private fixture')
            args = SimpleNamespace(rom=rom, profile='reference', inputs=path/'recorded.inputs', frames=3000,
                                   port=27015, proxy_port=27016, fault=1000)
            before = copy.deepcopy(dict(os.environ))
            child = peer.child_environment(path, 'join', args)
            self.assertEqual(dict(os.environ), before)
            self.assertEqual(child['STV_NET'], 'join:127.0.0.1:27016')
            self.assertEqual(child['STV_NET_FAULT'], '1000')
            self.assertEqual(Path(child['TEMP']), path / 'join/temp')
            self.assertEqual((Path(child['LOCALAPPDATA'])/'SmashTV/smashtv.zip').read_bytes(), rom.read_bytes())


def audio_fixture(played=3000, device=True):
    audit = {name: 0 for name in peer.AUDIO_ERROR_COUNTERS}
    audit.update(
        schema='smash.net.confirmed-audio.v1', enabled=True, active=False,
        frame_indexing='zero_based_less_than_exclusive_confirmed', hash_algorithm='fnv1a64-le-v1',
        submission_scope='AudioOut_software_queue_not_device_rendering_or_listening',
        sample_rate=48000, comparison_limit=3000, next_frame=played, last_frame=played-1,
        max_access_frame=played-1, frames_started=played, frames_completed=played,
        io_accesses=played*3, io_reads=played*2, io_writes=played, io_hash='1'*16,
        mixer_callbacks=played*10, pcm_generated=played*100, pcm_hash='2'*16,
        pcm_queued=played*100 if device else 0, pcm_discarded=0 if device else played*100,
        queued_pcm_hash='2'*16 if device else peer.AUDIO_HASH_OFFSET,
        output_frames=played, submission_calls=played if device else 0,
        device_frames=played if device else 0, no_device_frames=0 if device else played,
        violations=0,
        comparison=dict(ready=True, frames=3000, io_accesses=9000, io_hash='1'*16,
                        pcm_generated=300000, pcm_hash='2'*16))
    return dict(role='join', end='frames', confirmed=played, frame=played, played=played,
                last_hash='aabb0011', desyncs=0, resyncs=0, hash_checks=99,
                audio_endpoint_available=device, audio_device_errors=0 if device else 1,
                sound_fault=False, main_stopped=False, predictive_stopped=False, audio_audit=audit)


class AudioReceiptTests(unittest.TestCase):
    def test_complete_actual_audio_ownership(self):
        data = audio_fixture()
        peer.validate_peer(data, 'join', 3000, 'aabb0011', 0, True)
        result = peer.validate_audio_pair(data, data, 3000)
        self.assertTrue(result['pcm_compared'])
        self.assertFalse(result['physical_playback_or_listening_proof'])
        self.assertFalse(result['complete_C6_gate_proof'])
        self.assertFalse(result['two_pc_proof'])

    def test_missing_endpoint_is_generation_only(self):
        data = audio_fixture(device=False)
        result = peer.validate_audio_pair(data, data, 3000)
        self.assertEqual(result['software_queue_samples'], dict(host=0, join=0))
        self.assertEqual(result['discarded_without_device_samples'], dict(host=300000, join=300000))
        self.assertFalse(result['audio_endpoint_available_at_exit']['host'])
        self.assertFalse(result['physical_playback_or_listening_proof'])

    def test_requested_audit_cannot_be_missing_or_disabled(self):
        for value in [None, {}, dict(audio_fixture()['audio_audit'], enabled=False),
                      dict(audio_fixture()['audio_audit'], active=True)]:
            with self.subTest(value=value):
                data = audio_fixture(); data['audio_audit'] = value
                with self.assertRaises(ValueError):
                    peer.validate_peer(data, 'join', 3000, 'aabb0011', 0, True)

    def test_every_actual_event_violation_rejects_even_if_total_claims_zero(self):
        for field in peer.AUDIO_ERROR_COUNTERS + ['violations']:
            with self.subTest(field=field):
                data = audio_fixture(); data['audio_audit'][field] = 1
                with self.assertRaisesRegex(ValueError, 'violation'):
                    peer.validate_audio_audit(data, 3000)

    def test_missing_boolean_or_negative_audio_counter_rejects(self):
        fixture = audio_fixture()
        fields = [name for name, value in fixture['audio_audit'].items() if type(value) is int]
        for field in fields:
            for value in [None, True, -1, '0']:
                with self.subTest(field=field, value=value):
                    data = copy.deepcopy(fixture); data['audio_audit'][field] = value
                    with self.assertRaises(ValueError):
                        peer.validate_audio_audit(data, 3000)

    def test_non_exact_frame_counts_reject(self):
        for field, value in [('frames_started', 2999), ('frames_completed', 2999), ('next_frame', 2999),
                             ('last_frame', 3000), ('output_frames', 3001), ('max_access_frame', 3000),
                             ('comparison_limit', 2999)]:
            with self.subTest(field=field):
                data = audio_fixture(); data['audio_audit'][field] = value
                with self.assertRaises(ValueError):
                    peer.validate_audio_audit(data, 3000)

    def test_missing_io_or_pcm_accounting_rejects(self):
        for field, value in [('io_accesses', 0), ('io_writes', 2999), ('mixer_callbacks', 0),
                             ('sample_rate', 0), ('pcm_generated', 0), ('pcm_queued', 299999),
                             ('device_frames', 2999), ('submission_calls', 2999)]:
            with self.subTest(field=field):
                data = audio_fixture(); data['audio_audit'][field] = value
                with self.assertRaises(ValueError):
                    peer.validate_audio_audit(data, 3000)

    def test_no_device_cannot_claim_queued_samples(self):
        data = audio_fixture(device=False)
        data['audio_audit'].update(pcm_queued=1, pcm_discarded=299999)
        with self.assertRaisesRegex(ValueError, 'without an output device'):
            peer.validate_audio_audit(data, 3000)

    def test_submitted_pcm_identity_is_required(self):
        data = audio_fixture(); data['audio_audit']['queued_pcm_hash'] = '3'*16
        with self.assertRaisesRegex(ValueError, 'submitted PCM'):
            peer.validate_audio_audit(data, 3000)

    def test_exact_requested_prefix_and_hash_contract_required(self):
        for field, value in [('ready', False), ('frames', 2999), ('io_accesses', 0),
                             ('io_hash', 'x'*16), ('pcm_generated', 1), ('pcm_hash', '3'*16)]:
            with self.subTest(field=field):
                data = audio_fixture(); data['audio_audit']['comparison'][field] = value
                with self.assertRaises(ValueError):
                    peer.validate_audio_audit(data, 3000)
        for field in ['schema', 'frame_indexing', 'hash_algorithm', 'submission_scope', 'io_hash']:
            data = audio_fixture(); data['audio_audit'][field] = 'unknown'
            with self.subTest(field=field), self.assertRaises(ValueError):
                peer.validate_audio_audit(data, 3000)

    def test_confirmed_batch_overshoot_compares_same_prefix(self):
        left, right = audio_fixture(3003), audio_fixture(3007)
        self.assertTrue(peer.validate_audio_pair(left, right, 3000)['pcm_compared'])
        right['audio_audit']['outside_frame_accesses'] = 1
        with self.assertRaisesRegex(ValueError, 'violation'):
            peer.validate_audio_pair(left, right, 3000)

    def test_cross_peer_io_mismatch_rejects(self):
        left, right = audio_fixture(), audio_fixture()
        right['audio_audit']['io_hash'] = right['audio_audit']['comparison']['io_hash'] = '3'*16
        with self.assertRaisesRegex(ValueError, 'I/O streams differ'):
            peer.validate_audio_pair(left, right, 3000)

    def test_cross_peer_same_rate_pcm_mismatch_rejects(self):
        left, right = audio_fixture(), audio_fixture()
        right['audio_audit']['pcm_hash'] = right['audio_audit']['queued_pcm_hash'] = '3'*16
        right['audio_audit']['comparison']['pcm_hash'] = '3'*16
        with self.assertRaisesRegex(ValueError, 'PCM streams differ'):
            peer.validate_audio_pair(left, right, 3000)

    def test_different_rates_report_io_only_pcm_comparison(self):
        left, right = audio_fixture(), audio_fixture()
        right['audio_audit']['sample_rate'] = 44100
        right['audio_audit']['pcm_hash'] = right['audio_audit']['queued_pcm_hash'] = '3'*16
        right['audio_audit']['comparison']['pcm_hash'] = '3'*16
        result = peer.validate_audio_pair(left, right, 3000)
        self.assertFalse(result['pcm_compared'])
        self.assertIn('different output rates', result['pcm_comparison_scope'])

    def test_machine_fault_or_missing_endpoint_state_rejects(self):
        for field, value in [('sound_fault', True), ('main_stopped', True),
                             ('sound_fault', None), ('audio_endpoint_available', None),
                             ('audio_device_errors', True)]:
            data = audio_fixture(); data[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                peer.validate_audio_audit(data, 3000)

    def test_audit_does_not_weaken_forced_resync_requirement(self):
        data = audio_fixture()
        data['fault_injection'] = fault_fixture(1000)
        with self.assertRaisesRegex(ValueError, 'resynchronised'):
            peer.validate_peer(data, 'join', 3000, 'aabb0011', 1000, True)
        data.update(desyncs=1, resyncs=1, first_desync_frame=1020, first_resync_base=1030)
        peer.validate_peer(data, 'join', 3000, 'aabb0011', 1000, True)

    def test_audio_environment_requires_explicit_request(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            rom = path/'rom.zip'; rom.write_bytes(b'private fixture')
            args = SimpleNamespace(rom=rom, profile='reference', inputs=path/'recorded.inputs', frames=3000,
                                   port=27015, proxy_port=27016, fault=0, audio_audit=False)
            with patch.dict(os.environ, {'STV_NET_AUDIO_AUDIT': '1'}):
                no_audit = peer.child_environment(path, 'join', args)
                self.assertNotIn('STV_NET_AUDIO_AUDIT', no_audit)
                args.audio_audit = True
                audited = peer.child_environment(path, 'host', args)
                self.assertEqual(audited['STV_NET_AUDIO_AUDIT'], '1')
                self.assertEqual(os.environ['STV_NET_AUDIO_AUDIT'], '1')


def trace_fixture():
    data = audio_fixture()
    rows = [dict(frame=f, io_accesses=(f+1)*3, io_hash=f'{f+1:016x}',
                 pcm_generated=(f+1)*100, pcm_hash=f'{f+10:016x}',
                 mixer_callbacks=(f+1)*10) for f in range(3)]
    data['audio_trace'] = dict(enabled=True, first_frame=0, end_frame_exclusive=3,
                               resync_trace_truncated=False, records=rows, resyncs=[])
    return data


class AudioTraceTests(unittest.TestCase):
    def test_bounded_interval_parser(self):
        self.assertEqual(peer.parse_audio_trace('0:4096'), (0, 4096))
        self.assertEqual(peer.parse_audio_trace('5900:6400'), (5900, 6400))
        for invalid in ['0:4097', '1:1', '2:1', '-1:3', '0,3', '0:4294967296', 'bad']:
            with self.subTest(invalid=invalid), self.assertRaises(peer.argparse.ArgumentTypeError):
                peer.parse_audio_trace(invalid)

    def test_actual_ordered_trace(self):
        data = trace_fixture()
        peer.validate_audio_trace(data, (0, 3))
        self.assertIsNone(peer.compare_audio_trace(data, data, (0, 3))['first_difference'])

    def test_reports_first_observed_divergent_frame(self):
        left, right = trace_fixture(), trace_fixture()
        right['audio_trace']['records'][1]['io_hash'] = '3'*16
        found = peer.compare_audio_trace(left, right, (0, 3))['first_difference']
        self.assertEqual(found['frame'], 1)
        self.assertEqual(found['fields'], ['io_hash'])

    def test_missing_incomplete_or_truncated_trace_rejects(self):
        for field, value in [('enabled', False), ('first_frame', 1), ('end_frame_exclusive', True),
                             ('resync_trace_truncated', True), ('records', []), ('resyncs', None)]:
            data = trace_fixture(); data['audio_trace'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                peer.validate_audio_trace(data, (0, 3))

    def test_repeated_skipped_reversed_and_malformed_records_reject(self):
        for field, value in [('frame', 0), ('frame', True), ('io_accesses', 0),
                             ('pcm_generated', True), ('io_hash', 'bad')]:
            data = trace_fixture(); data['audio_trace']['records'][1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                peer.validate_audio_trace(data, (0, 3))

    def test_completed_trace_must_match_full_actual_endpoint(self):
        data = trace_fixture(); data['played'] = 3
        with self.assertRaisesRegex(ValueError, 'endpoint differs'):
            peer.validate_audio_trace(data, (0, 3))
        data['audio_audit'].update({k: v for k, v in data['audio_trace']['records'][-1].items() if k != 'frame'})
        peer.validate_audio_trace(data, (0, 3))


class ShaperTests(unittest.TestCase):
    def test_source_defined_wire_state(self):
        data = b'I' + struct.pack('<IH', 2000, 1) + struct.pack('<H', 0) + struct.pack('<IB', 2000, 0)
        data += struct.pack('<BBIIII', 0, 1, 2001, 0, 1, 32768)
        state = peer.wire_state(data)
        self.assertEqual(state['snapshot_bytes_held'], 32768)
        self.assertEqual(state['confirmed'], 2000)
        self.assertEqual(state['receiving_epoch'], 1)
        self.assertIsNone(peer.wire_state(data[:-1]))
        self.assertIsNone(peer.wire_state(data + b'extra'))

    def test_windows_udp_icmp_feedback_is_counted(self):
        from unittest.mock import patch
        proxy = peer.Shaper(27015, 0, 0, 0, 0, 1)
        original = proxy.front
        error = ConnectionResetError('UDP peer port closed'); error.winerror = 10054
        class Feedback:
            def recvfrom(self, size):
                raise error
            def close(self):
                original.close()
        proxy.front = Feedback()
        try:
            with patch.object(peer.select, 'select', return_value=([proxy.front], [], [])):
                proxy.tick()
            self.assertEqual(proxy.port_unreachable, 1)
        finally:
            proxy.close()

    def test_two_way_endpoint_ownership(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as join:
            host.bind(('127.0.0.1', 0)); join.bind(('127.0.0.1', 0)); host.settimeout(1); join.settimeout(1)
            proxy = peer.Shaper(host.getsockname()[1], 0, 0, 0, 0, 1)
            try:
                join.sendto(b'hello', proxy.front.getsockname()); proxy.tick()
                data, upstream = host.recvfrom(100); self.assertEqual(data, b'hello')
                host.sendto(b'welcome', upstream); proxy.tick()
                data, downstream = join.recvfrom(100); self.assertEqual(data, b'welcome')
                self.assertEqual(downstream, proxy.front.getsockname())
                self.assertEqual(proxy.stats['join_to_host']['delivered'], 1)
                self.assertEqual(proxy.stats['host_to_join']['delivered'], 1)
            finally:
                proxy.close()

    def test_loss_is_applied_in_both_directions(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as join:
            host.bind(('127.0.0.1', 0)); join.bind(('127.0.0.1', 0))
            proxy = peer.Shaper(host.getsockname()[1], 0, 0, 0, 100, 1)
            try:
                join.sendto(b'hello', proxy.front.getsockname()); proxy.tick()
                host.sendto(b'welcome', proxy.back.getsockname()); proxy.tick()
                for stats in proxy.stats.values():
                    self.assertEqual(stats, dict(received=1, dropped=1, delivered=0))
                self.assertFalse(proxy.queue)
            finally:
                proxy.close()

    def test_latency_defers_delivery(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as host, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as join:
            host.bind(('127.0.0.1', 0)); join.bind(('127.0.0.1', 0)); host.setblocking(False)
            proxy = peer.Shaper(host.getsockname()[1], 0, 20, 0, 0, 1)
            try:
                join.sendto(b'hello', proxy.front.getsockname()); proxy.tick(0)
                with self.assertRaises(BlockingIOError):
                    host.recvfrom(100)
                deadline = time.monotonic() + 1
                while proxy.queue and time.monotonic() < deadline:
                    proxy.tick()
                self.assertEqual(host.recvfrom(100)[0], b'hello')
            finally:
                proxy.close()


if __name__ == '__main__':
    unittest.main()
