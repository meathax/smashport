#!/usr/bin/env python3
"""Own two Windows exe peers and a seeded UDP shaper in an isolated capture.

This is development evidence, not two-PC, listening, or complete C6 gate proof.
No global TEMP files or parent environment are changed. A fresh output directory
is required; every process is reaped and artifacts are preserved on failure.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import heapq
import json
import os
from pathlib import Path
import random
import re
import select
import shutil
import socket
import subprocess
import sys
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from net_checkpoints import compare_periodic, checked as validate_periodic_report


def pin(path):
    path = Path(path).resolve()
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest}


def atomic_json(path, data):
    path = Path(path)
    temp = path.with_suffix(path.suffix + '.pending')
    temp.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8', newline='\n')
    temp.replace(path)


def validate_peer(data, role, frames, reference, fault, audio_audit=False, audio_trace=None):
    if not isinstance(data, dict):
        raise ValueError('peer report must be an object')
    if data.get('role') != role or data.get('end') != 'frames':
        raise ValueError('peer did not finish the requested frame run')
    for field in ['confirmed', 'frame', 'played', 'desyncs', 'resyncs', 'hash_checks']:
        value = data.get(field)
        if type(value) is not int or value < 0:
            raise ValueError('invalid peer counter: ' + field)
    if data['confirmed'] < frames or data['frame'] < data['confirmed'] or data['played'] != data['confirmed']:
        raise ValueError('incomplete confirmation or confirmed playback')
    if data.get('last_hash') != reference or not re.fullmatch('[0-9a-f]{8}', reference):
        raise ValueError('final confirmed-frame hash differs from offline reference')
    if data['hash_checks'] == 0:
        raise ValueError('no peer hash checks')
    for field in ['main_stopped', 'sound_fault', 'predictive_stopped']:
        if data.get(field) is not False:
            raise ValueError('missing or failed machine liveness: ' + field)
    if fault or 'fault_injection' in data:
        validate_fault_injection(data, role, fault, frames)
    if fault:
        validate_fault_observations(data, role, fault, frames)
    elif data['desyncs'] or data['resyncs'] or data.get('first_desync_frame') is not None or data.get('first_resync_base') is not None:
        raise ValueError('unexpected divergence or resynchronisation')
    if audio_audit:
        validate_audio_audit(data, frames)
    if audio_trace:
        validate_audio_trace(data, audio_trace)


def validate_output_timeline(data):
    """Bounded output-owner observations; never certify silence-free playback."""
    trace = data.get('audio_output_timeline')
    if not isinstance(trace, dict) or trace.get('schema') != 'smash.host.audio-output-timeline.v1' or trace.get('enabled') is not True:
        raise ValueError('missing requested audio output timeline')
    for field in ['samples_observed', 'history_discarded', 'events_discarded', 'maximum_owner_gap_us']:
        if type(trace.get(field)) is not int or trace[field] < 0:
            raise ValueError('invalid timeline counter: ' + field)
    if trace.get('history_limit') != 8192 or trace.get('event_limit') != 256 or trace.get('interval_us') != 10000:
        raise ValueError('unexpected audio output timeline bounds')
    for name, limit in [('samples', 8192), ('events', 256)]:
        rows = trace.get(name)
        if not isinstance(rows, list) or len(rows) > limit or (name == 'samples' and not rows):
            raise ValueError('missing or unbounded audio output timeline rows')
        previous = -1
        for row in rows:
            if not isinstance(row, dict):
                raise ValueError('invalid audio output timeline row')
            for field in ['time_us', 'owner_gap_us', 'frame', 'confirmed', 'played', 'pending', 'buffer_frames',
                          'output_samples', 'underruns', 'errors', 'recoveries', 'output_resets', 'source_drops',
                          'device_drops', 'network_resyncs', 'network_stalls', 'event_mask']:
                if type(row.get(field)) is not int or row[field] < 0:
                    raise ValueError('invalid audio output timeline field: ' + field)
            if row['time_us'] < previous or row['owner_gap_us'] > trace['maximum_owner_gap_us'] or row['event_mask'] > 2047:
                raise ValueError('invalid audio output timeline time/event')
            if name == 'events' and not row['event_mask']:
                raise ValueError('audio timeline event lacks a reason')
            if 'padding' not in row or (row['padding'] is not None and (type(row['padding']) is not int or row['padding'] < 0)):
                raise ValueError('invalid audio output device padding')
            if 'priming' in row:
                if type(row['priming']) is not bool:
                    raise ValueError('invalid audio priming state')
                for field in ['startup_primes', 'underrun_reprimes', 'priming_completions', 'priming_wait_ms', 'priming_target']:
                    if type(row.get(field)) is not int or row[field] < 0:
                        raise ValueError('invalid audio priming counter: ' + field)
            for field in ['online', 'paused', 'resyncing', 'available']:
                if type(row.get(field)) is not bool:
                    raise ValueError('invalid audio output timeline state: ' + field)
            if row['online'] and (row['played'] > row['confirmed'] or row['confirmed'] > row['frame']):
                raise ValueError('invalid timeline confirmation ownership')
            previous = row['time_us']
    if trace['samples_observed'] != trace['history_discarded'] + len(trace['samples']):
        raise ValueError('audio output timeline loss is not accounted')
    return dict(samples=trace['samples_observed'], retained=len(trace['samples']),
                events=len(trace['events']), events_discarded=trace['events_discarded'],
                maximum_owner_gap_ms=trace['maximum_owner_gap_us']/1000,
                underrun_events=sum(bool(row['event_mask'] & 1) for row in trace['events']),
                priming_instrumented='priming' in trace['samples'][0],
                priming_events=sum(bool(row['event_mask'] & 1024) for row in trace['events']),
                silence_free_proof=False)


def validate_fault_observations(data, role, requested, frames):
    """One isolated corruption can be noticed by either endpoint before repair."""
    if data.get('resyncs') != 1 or type(data.get('resyncs')) is not int:
        raise ValueError('injected divergence was not resynchronised exactly once')
    detections = data.get('desyncs')
    if type(detections) is not int or detections not in (0, 1):
        raise ValueError('extraneous or invalid divergence detections')
    if 'first_desync_frame' not in data or 'first_resync_base' not in data:
        raise ValueError('missing observed divergence/resync boundary')
    first = data['first_desync_frame']
    effective = effective_fault_frame(requested)
    if detections:
        if type(first) is not int or not effective <= first < frames or first % NET_HASH_EVERY:
            raise ValueError('divergence detection is outside the injected exchange interval')
    elif first is not None:
        raise ValueError('detection frame exists without a detection')
    base = data['first_resync_base']
    if role == 'join':
        if type(base) is not int or not effective < base < frames:
            raise ValueError('invalid observed joining resync boundary')
        if first is not None and first >= base:
            raise ValueError('divergence detection was not before the resync boundary')
    elif base is not None:
        raise ValueError('host cannot claim a client snapshot-load callback')
    return first, base


def validate_fault_pair(host, join, requested, frames):
    """Require actual mutation, pair-level detection, and one repair on each."""
    if not requested:
        raise ValueError('fault-pair validation requires an injected fault')
    for role, data in [('host', host), ('join', join)]:
        validate_fault_injection(data, role, requested, frames)
        validate_fault_observations(data, role, requested, frames)
    if host['desyncs'] + join['desyncs'] == 0:
        raise ValueError('injected divergence was not detected by either peer')
    base = join['first_resync_base']
    for role, data in [('host', host), ('join', join)]:
        first = data['first_desync_frame']
        if first is not None and first >= base:
            raise ValueError(role + ' divergence detection was not before the resync boundary')
    return dict(contract='smash.net.pair-fault-response.v1',
                effective_fault_frame=effective_fault_frame(requested),
                first_desync_frame={role: data['first_desync_frame'] for role, data in [('host', host), ('join', join)]},
                first_resync_base=base,
                detections=host['desyncs'] + join['desyncs'],
                resyncs_per_peer=1, actual_changed_injection=True)


AUDIO_ERROR_COUNTERS = [
    'outside_frame_accesses', 'unconfirmed_accesses', 'io_time_errors', 'invalid_accesses',
    'outside_frame_mixer_calls', 'unconfirmed_mixer_calls', 'mixer_time_errors',
    'outside_frame_outputs', 'unconfirmed_outputs', 'frame_sequence_errors',
    'frame_scope_errors', 'frame_completion_errors', 'pcm_accounting_errors', 'nonfinite_samples',
]
AUDIO_HASH_OFFSET = 'cbf29ce484222325'


def validate_audio_audit(data, frames):
    """Observe actual Board I/O, generated PCM, and queue ownership, not hearing."""
    audit = data.get('audio_audit')
    if not isinstance(audit, dict) or audit.get('schema') != 'smash.net.confirmed-audio.v1':
        raise ValueError('missing runtime confirmed-audio audit')
    if audit.get('enabled') is not True or audit.get('active') is not False:
        raise ValueError('audio audit disabled or playback scope left active')
    if (audit.get('frame_indexing') != 'zero_based_less_than_exclusive_confirmed' or
            audit.get('hash_algorithm') != 'fnv1a64-le-v1' or
            audit.get('submission_scope') != 'AudioOut_software_queue_not_device_rendering_or_listening'):
        raise ValueError('unknown audio ownership or hash contract')

    def counter(obj, field):
        value = obj.get(field)
        if type(value) is not int or value < 0:
            raise ValueError('invalid audio counter: ' + field)
        return value

    fields = ['sample_rate', 'comparison_limit', 'next_frame', 'last_frame', 'max_access_frame',
              'frames_started', 'frames_completed', 'io_accesses', 'io_reads', 'io_writes',
              'mixer_callbacks', 'pcm_generated', 'pcm_queued', 'pcm_discarded', 'output_frames',
              'submission_calls', 'device_frames', 'no_device_frames', 'violations'] + AUDIO_ERROR_COUNTERS
    for field in fields:
        counter(audit, field)
    for field in ['io_hash', 'pcm_hash', 'queued_pcm_hash']:
        if not isinstance(audit.get(field), str) or not re.fullmatch('[0-9a-f]{16}', audit[field]):
            raise ValueError('invalid audio stream hash: ' + field)
    if any(audit[field] for field in AUDIO_ERROR_COUNTERS + ['violations']):
        raise ValueError('runtime observed an audio ownership violation')
    played = counter(data, 'played')
    if played < frames or audit['comparison_limit'] != frames:
        raise ValueError('audio comparison interval does not cover the requested run')
    if any(audit[field] != played for field in
           ['frames_started', 'frames_completed', 'next_frame', 'output_frames']):
        raise ValueError('confirmed audio playback was not exactly once for every played frame')
    if audit['last_frame'] != played - 1 or audit['max_access_frame'] >= played:
        raise ValueError('invalid audio access or playback frame boundary')
    if audit['io_accesses'] == 0 or audit['io_reads'] + audit['io_writes'] != audit['io_accesses']:
        raise ValueError('no complete actual sound-board I/O observations')
    if not audit['sample_rate'] or not audit['mixer_callbacks'] or not audit['pcm_generated']:
        raise ValueError('no actual mixer PCM observations')
    if audit['pcm_queued'] + audit['pcm_discarded'] != audit['pcm_generated']:
        raise ValueError('generated PCM not completely accounted for')
    if audit['device_frames'] + audit['no_device_frames'] != played:
        raise ValueError('missing output-device decisions')
    if not audit['device_frames'] <= audit['submission_calls'] <= played:
        raise ValueError('queue submissions inconsistent with actual device availability')
    if not audit['device_frames'] and (audit['pcm_queued'] or audit['queued_pcm_hash'] != AUDIO_HASH_OFFSET):
        raise ValueError('PCM claimed as queued without an output device')
    if not audit['no_device_frames'] and (audit['pcm_discarded'] or audit['pcm_hash'] != audit['queued_pcm_hash']):
        raise ValueError('submitted PCM differs from generated PCM')
    if type(data.get('audio_endpoint_available')) is not bool:
        raise ValueError('missing actual output endpoint state')
    counter(data, 'audio_device_errors')
    if data.get('sound_fault') is not False or data.get('main_stopped') is not False:
        raise ValueError('sound or main machine stopped during audited playback')

    comparison = audit.get('comparison')
    if not isinstance(comparison, dict) or comparison.get('ready') is not True:
        raise ValueError('audio comparison prefix never completed')
    for field in ['frames', 'io_accesses', 'pcm_generated']:
        counter(comparison, field)
    if comparison['frames'] != frames:
        raise ValueError('audio comparison prefix has the wrong endpoint')
    for field in ['io_accesses', 'pcm_generated']:
        if not 0 < comparison[field] <= audit[field]:
            raise ValueError('invalid audio comparison event count: ' + field)
    for field in ['io_hash', 'pcm_hash']:
        if not isinstance(comparison.get(field), str) or not re.fullmatch('[0-9a-f]{16}', comparison[field]):
            raise ValueError('invalid audio comparison hash: ' + field)
    if played == frames and any(comparison[field] != audit[field] for field in
                                ['io_accesses', 'io_hash', 'pcm_generated', 'pcm_hash']):
        raise ValueError('complete audio comparison differs from the full observed stream')
    return audit


def validate_audio_pair(host, join, frames):
    """Both exact requested prefixes must match; overshoot is separately audited."""
    left = validate_audio_audit(host, frames)
    right = validate_audio_audit(join, frames)
    lc, rc = left['comparison'], right['comparison']
    if any(lc[field] != rc[field] for field in ['io_accesses', 'io_hash']):
        raise ValueError('actual confirmed sound-board I/O streams differ between peers')
    pcm_comparable = left['sample_rate'] == right['sample_rate']
    if pcm_comparable and any(lc[field] != rc[field] for field in ['pcm_generated', 'pcm_hash']):
        raise ValueError('generated PCM streams differ at the same sample rate')
    return {
        'result': 'PASS_CONFIRMED_AUDIO_OWNERSHIP',
        'frames': frames, 'io_accesses_per_peer': lc['io_accesses'], 'io_hash': lc['io_hash'],
        'pcm_compared': pcm_comparable,
        'pcm_comparison_scope': 'equal-rate exact generated samples' if pcm_comparable else 'different output rates; I/O identity only',
        'sample_rates': {'host': left['sample_rate'], 'join': right['sample_rate']},
        'pcm_generated_at_endpoint': {'host': lc['pcm_generated'], 'join': rc['pcm_generated']},
        'pcm_hashes_at_endpoint': {'host': lc['pcm_hash'], 'join': rc['pcm_hash']},
        'software_queue_samples': {'host': left['pcm_queued'], 'join': right['pcm_queued']},
        'discarded_without_device_samples': {'host': left['pcm_discarded'], 'join': right['pcm_discarded']},
        'audio_endpoint_available_at_exit': {'host': host['audio_endpoint_available'], 'join': join['audio_endpoint_available']},
        'physical_playback_or_listening_proof': False,
        'two_pc_proof': False, 'complete_C6_gate_proof': False,
    }


# Matches RollbackSession::kHashEvery and send()'s g % kHashEvery == 0.
# The actual peer report must advertise the same period and phase.
NET_HASH_EVERY = 30


def effective_fault_frame(requested):
    if type(requested) is not int or requested < 0:
        raise ValueError('fault frame must be a nonnegative integer; zero disables injection')
    effective = requested + (-requested % NET_HASH_EVERY) if requested else 0
    if effective > 0xffffffff:
        raise ValueError('fault frame exceeds the protocol frame range')
    return effective


def validate_fault_injection(data, role, requested, frames):
    fault = data.get('fault_injection')
    if not isinstance(fault, dict) or fault.get('schema') != 'smash.net.fault-injection.v1':
        raise ValueError('missing observed fault-injection report')
    if fault.get('mode') != 'single_exchanged_hash_frame_xor_8000':
        raise ValueError('unknown fault-injection contract')
    for name in ['requested_frame', 'effective_frame', 'hash_period', 'attempts', 'changed_words', 'changed_hashes']:
        if type(fault.get(name)) is not int or fault[name] < 0:
            raise ValueError('invalid fault-injection counter: ' + name)
    if fault['hash_period'] != NET_HASH_EVERY:
        raise ValueError('fault-injection hash exchange period differs from the protocol')
    wanted = requested if role == 'join' else 0
    if fault.get('enabled') is not bool(wanted) or fault['requested_frame'] != wanted:
        raise ValueError('wrong peer or requested frame for fault injection')
    effective = effective_fault_frame(wanted)
    if fault['effective_frame'] != effective or (wanted and effective >= frames):
        raise ValueError('fault injection is not at the requested exchanged-hash boundary')
    events = fault.get('events')
    if not isinstance(events, list) or len(events) != fault['attempts']:
        raise ValueError('missing observed fault-injection attempts')
    if not wanted:
        if events or any(fault[name] for name in ['attempts', 'changed_words', 'changed_hashes']):
            raise ValueError('unexpected fault injection on an uninjected peer')
        return fault
    if not fault['attempts'] or fault['changed_words'] != fault['attempts'] or fault['changed_hashes'] != fault['attempts']:
        raise ValueError('fault injection did not change observed RAM and its stored hash')
    for event in events:
        if not isinstance(event, dict) or type(event.get('frame')) is not int or event['frame'] != effective:
            raise ValueError('fault-injection event is outside the single selected hash frame')
        for field, digits in [('before_word', 4), ('after_word', 4), ('before_hash', 8), ('after_hash', 8)]:
            if not isinstance(event.get(field), str) or not re.fullmatch('[0-9a-f]{' + str(digits) + '}', event[field]):
                raise ValueError('invalid observed fault-injection value: ' + field)
        if int(event['before_word'], 16) ^ int(event['after_word'], 16) != 0x8000:
            raise ValueError('fault-injection event was a no-op or changed the wrong bits')
        if event['before_hash'] == event['after_hash']:
            raise ValueError('fault-injection event did not change the actual RAM hash')
    return fault


def parse_audio_trace(value):
    match = re.fullmatch(r'([0-9]+):([0-9]+)', value)
    if not match:
        raise argparse.ArgumentTypeError('audio trace must be FIRST:END with an exclusive end')
    first, end = map(int, match.groups())
    if end > 0xffffffff or not 0 < end - first <= 4096:
        raise argparse.ArgumentTypeError('audio trace must contain 1 to 4096 frames')
    return first, end


def validate_audio_trace(data, interval):
    trace = data.get('audio_trace')
    if not isinstance(trace, dict) or trace.get('enabled') is not True:
        raise ValueError('missing requested bounded audio trace')
    first, end = interval
    if (trace.get('first_frame'), trace.get('end_frame_exclusive')) != (first, end):
        raise ValueError('audio trace interval differs from the request')
    for field in ['first_frame', 'end_frame_exclusive']:
        if type(trace.get(field)) is not int:
            raise ValueError('invalid audio trace boundary')
    if trace.get('resync_trace_truncated') is not False:
        raise ValueError('bounded resync trace was truncated')
    rows = trace.get('records')
    expected = list(range(first, min(data['played'], end)))
    if not isinstance(rows, list) or len(rows) != len(expected):
        raise ValueError('incomplete per-frame audio trace')
    previous = None
    for frame, row in zip(expected, rows):
        if not isinstance(row, dict) or type(row.get('frame')) is not int or row['frame'] != frame:
            raise ValueError('audio trace repeats or skips a frame')
        for field in ['io_accesses', 'pcm_generated', 'mixer_callbacks']:
            if type(row.get(field)) is not int or row[field] < 0 or (previous and row[field] < previous[field]):
                raise ValueError('invalid or reversed cumulative audio trace count')
        for field in ['io_hash', 'pcm_hash']:
            if not isinstance(row.get(field), str) or not re.fullmatch('[0-9a-f]{16}', row[field]):
                raise ValueError('invalid actual audio trace hash')
        previous = row
    if rows and rows[-1]['frame'] == data['played'] - 1:
        audit = data['audio_audit']
        if any(rows[-1][field] != audit[field] for field in
               ['io_accesses', 'io_hash', 'pcm_generated', 'pcm_hash', 'mixer_callbacks']):
            raise ValueError('audio trace endpoint differs from actual full audit')
    if not isinstance(trace.get('resyncs'), list) or len(trace['resyncs']) > 8:
        raise ValueError('invalid bounded resync observations')
    for event in trace['resyncs']:
        if not isinstance(event, dict) or type(event.get('base')) is not int or type(event.get('loaded')) is not bool:
            raise ValueError('invalid resync observation')
        for state in ['before', 'after']:
            if not isinstance(event.get(state), dict):
                raise ValueError('missing actual before/after resync state')
    return trace


def compare_audio_trace(host, join, interval):
    left, right = validate_audio_trace(host, interval), validate_audio_trace(join, interval)
    fields = ['io_accesses', 'io_hash', 'mixer_callbacks']
    if host['audio_audit']['sample_rate'] == join['audio_audit']['sample_rate']:
        fields += ['pcm_generated', 'pcm_hash']
    first_difference = None
    for a, b in zip(left['records'], right['records']):
        different = [field for field in fields if a[field] != b[field]]
        if different:
            first_difference = {'frame': a['frame'], 'fields': different, 'host': a, 'join': b}
            break
    return {'scope': 'bounded actual cumulative event fingerprints, diagnostic only',
            'interval': list(interval), 'first_difference': first_difference,
            'host_resyncs': left['resyncs'], 'join_resyncs': right['resyncs']}


def wire_state(data):
    """Bounded diagnostic of the source-defined rollback header, no RAM data."""
    if len(data) < 7 or data[0] != ord('I'):
        return None
    first, count = struct.unpack_from('<IH', data, 1)
    at = 7 + count * 2
    if count > 64 or len(data) < at + 5:
        return None
    ack, hashes = struct.unpack_from('<IB', data, at)
    at += 5 + hashes * 8
    if hashes > 4 or len(data) != at + 18:
        return None
    pause, hold, frame, epoch, rx_epoch, rx_have = struct.unpack_from('<BBIIII', data, at)
    return dict(first_input=first, inputs=count, confirmed=ack, pause=pause, holding=hold,
                frame=frame, epoch=epoch, receiving_epoch=rx_epoch, snapshot_bytes_held=rx_have)


class Shaper:
    """Two sockets preserve the real peers' UDP endpoint ownership."""
    def __init__(self, host_port, proxy_port, latency_ms, jitter_ms, loss_percent, seed):
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.front.bind(('127.0.0.1', proxy_port))
            self.back.bind(('127.0.0.1', 0))
            self.front.setblocking(False)
            self.back.setblocking(False)
        except BaseException:
            self.close()
            raise
        self.host = ('127.0.0.1', host_port)
        self.client = None
        self.latency = latency_ms / 1000
        self.jitter = jitter_ms / 1000
        self.loss = loss_percent / 100
        self.rng = random.Random(seed)
        self.queue = []
        self.sequence = 0
        self.peer_wire_state = {}
        self.port_unreachable = 0
        self.stats = {name: {'received': 0, 'dropped': 0, 'delivered': 0} for name in ['host_to_join', 'join_to_host']}

    def close(self):
        self.front.close()
        self.back.close()

    def tick(self, timeout=0.005):
        ready, _, _ = select.select([self.front, self.back], [], [], timeout)
        for incoming in ready:
            while True:
                try:
                    data, source = incoming.recvfrom(65535)
                except BlockingIOError:
                    break
                except ConnectionResetError as error:
                    # Windows reports asynchronous ICMP feedback through recvfrom
                    # after a peer exits. This is UDP feedback, not a TCP session.
                    if getattr(error, 'winerror', None) != 10054:
                        raise
                    self.port_unreachable += 1
                    break
                if incoming is self.front:
                    if self.client is None:
                        self.client = source
                    if source != self.client:
                        continue
                    direction, outgoing, destination = 'join_to_host', self.back, self.host
                else:
                    if source != self.host or self.client is None:
                        continue
                    direction, outgoing, destination = 'host_to_join', self.front, self.client
                self.stats[direction]['received'] += 1
                state = wire_state(data)
                if state:
                    self.peer_wire_state[direction] = state
                if self.rng.random() < self.loss:
                    self.stats[direction]['dropped'] += 1
                    continue
                delay = max(0.0, self.latency + self.rng.uniform(-self.jitter, self.jitter))
                self.sequence += 1
                heapq.heappush(self.queue, (time.monotonic() + delay, self.sequence, outgoing, destination, data, direction))
        now = time.monotonic()
        while self.queue and self.queue[0][0] <= now:
            _, _, outgoing, destination, data, direction = heapq.heappop(self.queue)
            outgoing.sendto(data, destination)
            self.stats[direction]['delivered'] += 1


def child_environment(directory, role, args):
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith('STV_')}
    data = directory / role / 'local/SmashTV'
    temp = directory / role / 'temp'
    data.mkdir(parents=True)
    temp.mkdir(parents=True)
    # Historical player builds read this copy from the isolated test profile.
    shutil.copyfile(args.rom, data / 'smashtv.zip')
    (data / 'pc-settings.cfg').write_text('version=1\nmuted=1\n', encoding='ascii')
    (data / 'profile.cfg').write_text(args.profile + '\n', encoding='ascii')
    env.update(STV_HOST_ROM_ZIP=str(args.rom.resolve()), LOCALAPPDATA=str(data.parent), TEMP=str(temp), TMP=str(temp),
               STV_PROFILE=args.profile, STV_HOST_NOPAUSE='1', STV_NET_INPUTS=str(args.inputs), STV_NET_FRAMES=str(args.frames))
    env['STV_NET'] = f'host:{args.port}' if role == 'host' else f'join:127.0.0.1:{args.proxy_port}'
    if role == 'join' and args.fault:
        env['STV_NET_FAULT'] = str(args.fault)
    if getattr(args, 'audio_audit', False):
        env['STV_NET_AUDIO_AUDIT'] = '1'
    if getattr(args, 'checkpoint_audit', False):
        env['STV_NET_CHECKPOINT_AUDIT'] = '1'
    if getattr(args, 'audio_output_timeline', False):
        env['STV_AUDIO_TELEMETRY'] = '1'
    if getattr(args, 'audio_trace', None):
        env['STV_NET_AUDIO_TRACE'] = ','.join(map(str, args.audio_trace))
    return env


def capture(args):
    if os.name != 'nt':
        raise ValueError('Windows exe peers require Windows')
    checkpoint_audit = getattr(args, 'checkpoint_audit', False)
    for name in ['build_directory', 'inputs', 'rom', 'out']:
        setattr(args, name, Path(getattr(args, name)).resolve())
    if args.frames < 60 or args.port == args.proxy_port or not 0 <= args.fault < args.frames:
        raise ValueError('invalid frames, ports or fault frame')
    if args.audio_trace and (not args.audio_audit or args.audio_trace[0] >= args.frames):
        raise ValueError('bounded audio trace needs --audio-audit and an interval intersecting the requested run')
    if args.fault and effective_fault_frame(args.fault) >= args.frames:
        raise ValueError('fault frame rounds to an exchanged hash outside the requested run')
    if not 0 <= args.loss_percent <= 100 or min(args.latency_ms, args.jitter_ms) < 0:
        raise ValueError('invalid shaping conditions')
    cache = (args.build_directory / 'CMakeCache.txt').read_text(encoding='utf-8')
    if args.profile == 'reference' and 'STV_PLAYER_RELEASE:BOOL=ON' in cache:
        raise ValueError('fixed-player executable cannot run reference geometry')
    exe = args.build_directory / 'SmashTV.exe'
    reference_exe = args.build_directory / 'stv_net_tests.exe'
    files = [exe, reference_exe, args.rom, args.inputs, Path(__file__)]
    if args.audio_audit or args.fault or checkpoint_audit or getattr(args, 'audio_output_timeline', False):
        files += [ROOT / 'src/host/win_main.cpp']
    if checkpoint_audit:
        files += [ROOT / 'tools/net_checkpoints.py', ROOT / 'tests/test_net_rollback.cpp',
                  ROOT / 'include/stv/net/rollback.hpp']
    if args.audio_audit:
        files += [ROOT / 'include/stv/host/net_audio_audit.hpp']
    if args.fault:
        files += [ROOT / 'include/stv/host/net_fault.hpp', ROOT / 'include/stv/net/rollback.hpp']
    if args.audio_trace:
        files += [ROOT / 'include/stv/game/cosched.hpp']
    if getattr(args, 'audio_output_timeline', False):
        files += [ROOT / 'include/stv/host/audio_telemetry.hpp', ROOT / 'src/host/wasapi.hpp', ROOT / 'src/host/wasapi.cpp']
    pins = [pin(path) for path in files]
    args.out.mkdir(parents=True, exist_ok=False)
    report_path = args.out / 'report.json'
    report = {'result': 'RUNNING', 'gate_proof': False, 'C6': 'OPEN',
              'verified_at': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'working_tree_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
              'started_at_utc': datetime.now(timezone.utc).isoformat(), 'frames': args.frames, 'fault': args.fault,
              'profile': args.profile, 'pins': pins, 'audio_audit_requested': args.audio_audit, 'audio_trace_requested': args.audio_trace,
              'checkpoint_audit_requested': checkpoint_audit,
              'audio_output_timeline_requested': getattr(args, 'audio_output_timeline', False),
              'conditions': {'one_way_latency_ms': args.latency_ms, 'jitter_plus_minus_ms': args.jitter_ms,
                             'loss_percent_per_direction': args.loss_percent, 'seed': args.seed},
              'owned_live_pids': [], 'processes': {},
              'limits': ['one machine, not physical two-PC proof', 'final confirmed hashes and playback counts, not full per-frame oracle identity',
                         'physical device rendering and listening are not established by software queue acceptance',
                         'not a frozen release soak']}
    atomic_json(report_path, report)
    children, logs = {}, []
    shaper = None
    started = time.monotonic()
    try:
        command = [str(reference_exe), str(args.rom), str(args.inputs), str(args.frames), '--ref-report']
        if args.profile == 'enhanced':
            command.append('--enhanced')
        ref_log = (args.out / 'reference.log').open('wb'); logs.append(ref_log)
        ref = subprocess.Popen(command, stdout=ref_log, stderr=subprocess.STDOUT)
        children['reference'] = ref
        report['processes']['reference'] = {'pid': ref.pid, 'exit_code': None}
        report['owned_live_pids'] = [ref.pid]
        report['reference_command'] = command
        atomic_json(report_path, report)
        ref.wait(timeout=args.timeout or args.frames / 30 + 120)
        ref_log.flush()
        report['processes']['reference']['exit_code'] = ref.returncode
        report['owned_live_pids'] = []
        reference = json.loads((args.out / 'reference.log').read_text(encoding='utf-8'))
        expected_profile = 'enhanced_l8_16_9' if args.profile == 'enhanced' else 'reference_l8'
        if ref.returncode or reference.get('profile') != expected_profile or reference.get('frames') != args.frames or not re.fullmatch('[0-9a-f]{8}', reference.get('last_hash', '')):
            raise ValueError('offline reference command failed')
        if checkpoint_audit:
            validate_periodic_report(reference, args.frames, 'offline')
        report['reference_hash'] = reference['last_hash']
        report['reference_actual'] = reference
        shaper = Shaper(args.port, args.proxy_port, args.latency_ms, args.jitter_ms, args.loss_percent, args.seed)
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        for role in ['host', 'join']:
            log = (args.out / (role + '.log')).open('wb'); logs.append(log)
            child = subprocess.Popen([str(exe)], cwd=exe.parent, env=child_environment(args.out, role, args),
                                     stdout=log, stderr=subprocess.STDOUT, startupinfo=startup)
            children[role] = child
            report['processes'][role] = {'pid': child.pid, 'exit_code': None}
            report['owned_live_pids'].append(child.pid)
            atomic_json(report_path, report)
            until = time.monotonic() + 0.5
            while time.monotonic() < until:
                shaper.tick()
        deadline = time.monotonic() + (args.timeout or args.frames / 50 + 120)
        checkpoint = time.monotonic()
        while any(child.poll() is None for child in children.values()):
            if time.monotonic() > deadline:
                raise TimeoutError('owned exe peers exceeded the monotonic deadline')
            shaper.tick()
            if time.monotonic() - checkpoint >= 10:
                report['owned_live_pids'] = [child.pid for child in children.values() if child.poll() is None]
                report['elapsed_seconds'] = round(time.monotonic() - started, 3)
                report['shaper'] = shaper.stats
                report['peer_wire_state'] = shaper.peer_wire_state
                atomic_json(report_path, report)
                checkpoint = time.monotonic()
        # Preserve both peers' actual reports before judging any failure.
        for role, child in children.items():
            report['processes'][role]['exit_code'] = child.wait()
            if role == 'reference':
                continue
            p = args.out / role / 'temp' / ('smashtv_net_' + role + '.json')
            if p.is_file():
                report[role] = {'report': pin(p)}
                try:
                    report[role]['actual'] = json.loads(p.read_text(encoding='utf-8'))
                except (ValueError, UnicodeError) as error:
                    report[role]['parse_error'] = str(error)
        for role, child in children.items():
            if child.returncode:
                raise ValueError(f'{role} exited {child.returncode}')
            if role != 'reference':
                if 'actual' not in report.get(role, {}):
                    raise ValueError(role + ' has no complete peer report')
                validate_peer(report[role]['actual'], role, args.frames, report['reference_hash'],
                              args.fault, args.audio_audit, args.audio_trace)
        if args.fault:
            report['fault_response'] = validate_fault_pair(report['host']['actual'], report['join']['actual'], args.fault, args.frames)
        if getattr(args, 'audio_output_timeline', False):
            report['audio_output_summary'] = {role: validate_output_timeline(report[role]['actual']) for role in ['host', 'join']}
        if args.audio_trace:
            report['audio_trace_comparison'] = compare_audio_trace(report['host']['actual'], report['join']['actual'], args.audio_trace)
        if args.audio_audit:
            report['audio_ownership'] = validate_audio_pair(report['host']['actual'], report['join']['actual'], args.frames)
        if checkpoint_audit:
            report['periodic_ram'] = compare_periodic(reference, report['host']['actual'], report['join']['actual'],
                                                      args.frames, args.fault)
        if any(pin(path) != saved for path, saved in zip(files, pins)):
            raise ValueError('source/exe/ROM/input changed during the capture')
        report['result'] = 'PASS_EXE_PEER_CASE_C6_OPEN'
    except Exception as error:
        report['result'] = 'FAIL'
        report['error'] = str(error)
    except KeyboardInterrupt:
        report['result'] = 'INTERRUPTED'
        report['error'] = 'capture interrupted; owned children reaped'
    finally:
        for role, child in children.items():
            if child.poll() is None:
                child.terminate()
            try:
                code = child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill(); code = child.wait(timeout=10)
            report['processes'][role]['exit_code'] = code
        for log in logs:
            log.close()
        if shaper:
            report['shaper'] = shaper.stats
            report['peer_wire_state'] = shaper.peer_wire_state
            report['udp_port_unreachable_notifications'] = shaper.port_unreachable
            shaper.close()
        report['owned_live_pids'] = []
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        report['finished_at_utc'] = datetime.now(timezone.utc).isoformat()
        atomic_json(report_path, report)
    print(json.dumps({'result': report['result'], 'frames': args.frames, 'report': str(report_path), 'error': report.get('error')}))
    return 0 if report['result'].startswith('PASS_') else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-directory', type=Path, default=ROOT / 'build/msvc')
    parser.add_argument('--inputs', type=Path, default=ROOT / 'tests/scenarios/two_player_full_game_440k.inputs')
    parser.add_argument('--rom', type=Path, default=ROOT / 'original/user-supplied-roms/smashtv.zip')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=3000)
    parser.add_argument('--port', type=int, default=27015)
    parser.add_argument('--proxy-port', type=int, default=27016)
    parser.add_argument('--fault', type=int, default=0,
                        help='zero disables injection; positive frame selects the first exchanged hash at or after it')
    parser.add_argument('--audio-audit', action='store_true',
                        help='require actual confirmed sound I/O and PCM ownership on both peers; not a listening gate')
    parser.add_argument('--checkpoint-audit', action='store_true',
                        help='compare every protocol-period confirmed RAM hash with the offline run')
    parser.add_argument('--audio-trace', type=parse_audio_trace,
                        help='bounded actual per-frame/resync diagnostic interval FIRST:END, maximum4096 frames')
    parser.add_argument('--audio-output-timeline', action='store_true',
                        help='capture bounded device/protocol timing; gaps remain counted and reported')
    parser.add_argument('--profile', choices=['reference', 'enhanced'], default='reference')
    parser.add_argument('--latency-ms', type=float, default=0)
    parser.add_argument('--jitter-ms', type=float, default=0)
    parser.add_argument('--loss-percent', type=float, default=0)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=0)
    return capture(parser.parse_args())


if __name__ == '__main__':
    raise SystemExit(main())
