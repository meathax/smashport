"""Classify retail WAVE writes; this does not certify room clearance."""
import json
from pathlib import Path

from rebaseline import require


def classify(path: Path, inventory, final_frame: int):
    rows = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
    require(len(rows) >= 2, 'Missing event header/end')
    header, end = rows[0], rows[-1]
    require(header.get('schema') == 'stv.mame.route_writes.v1' and
            header.get('phase') == 'write_tap_before_store' and
            header.get('pc_semantics') == 'raw_cpu_state_not_instruction_start', 'Wrong event contract')
    events = rows[1:-1]
    require(end == dict(kind='end', frame=final_frame, events=len(events)), 'Incomplete event stream')
    binding = inventory['transition_binding']
    require(binding['source_statements_verified'] and binding['nonrelocated_bytes_identical'],
            'Unverified source writer')
    require([i['pc'] for i in binding['instructions']][2:4] == ['ffec9570', 'ffec95a0'],
            'Unexpected retail writer/next PC')
    observed, excluded = [], []
    previous = 0
    fields = ('frame', 'address', 'data', 'mask', 'old_wave', 'merged_wave', 'pc_raw',
              'a0_raw', 'a1_raw', 'circuit', 'gamstate', 'p1_lives', 'p2_lives')
    for event in events:
        require(event.get('kind') == 'wave_write' and
                all(type(event.get(k)) is int and 0 <= event[k] <= 0xffffffff for k in fields),
                'Malformed wave event')
        require(previous <= event['frame'] <= final_frame, 'Unordered/out-of-range event')
        previous = event['frame']
        require(event['address'] == 0x010eb630, 'Wrong observed address')
        if event['pc_raw'] != 0xffec95a0 or event['gamstate'] != 16:
            excluded.append(event)
            continue
        require(event['mask'] == 0xffff and event['data'] == event['merged_wave'] ==
                (event['a0_raw'] & 0xffff), 'Writer value/mask mismatch')
        matches = []
        for record in inventory['physical_records']:
            if event['circuit'] not in record['table_users'] or event['old_wave'] != record['physical_wave']:
                continue
            for option in record['options']:
                if (event['a1_raw'] == int(record['address'], 16) + 192 + option['slot'] * 32 and
                        event['data'] == option['destination_field']):
                    matches.append(dict(record=record['address'], **option))
        require(len(matches) == 1, 'Retail exit writer does not resolve uniquely to source table')
        # These are raw counters, not proof of player-object presence. Retail
        # REDREG also writes WAVE while both counters are zero.
        observed.append(dict(event=event, option=matches[0], clearance_proved=False))
    return dict(exit_writes=observed, excluded_writes=excluded, clearance_proved=False)
