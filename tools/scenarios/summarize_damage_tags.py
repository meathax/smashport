"""Summarize sampled player DEAD tags without claiming damaging-object identity."""
import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path

from summarize_stage_audit import verify_report


def valid_owner(row):
    process = row['process']
    return (0x01086790 <= process < 0x01086790 + 240 * 0x5a0
            and (process - 0x01086790) % 0x5a0 == 0
            and row['slot'] in (1, 2) and row['owner'] == row['slot'])


def summarize_rows(rows, first_invalid_frame=None):
    previous, pending = {}, {}
    events, excluded = [], []
    last_frame = -1
    for raw in rows:
        if set(raw) != {'frame', 'circuit', 'wave', 'state', 'slot', 'process', 'owner', 'dead', 'lives'}:
            raise ValueError('Unexpected life-tag schema')
        row = {key: int(value) for key, value in raw.items()}
        frame, slot = row['frame'], row['slot']
        if frame < 0 or frame < last_frame or slot not in (1, 2):
            raise ValueError('Unordered samples or invalid player slot')
        if (not 0 <= row['process'] <= 0xffffffff or not -1 <= row['owner'] <= 255
                or not -1 <= row['dead'] <= 65535 or not 0 <= row['lives'] <= 65535
                or any(not 0 <= row[key] <= 65535 for key in ('circuit', 'wave', 'state'))):
            raise ValueError('Sample outside captured field bounds')
        last_frame = frame
        old = previous.get(slot)
        if old is not None and frame <= old['frame']:
            raise ValueError('Duplicate or unordered player sample')
        previous[slot] = row
        if old is None:
            continue
        same = (row['state'] == old['state'] == 16
                and (row['circuit'], row['wave'], row['process'])
                == (old['circuit'], old['wave'], old['process'])
                and valid_owner(old) and valid_owner(row))
        prior_tag = pending.get(slot)
        if not same or row['dead'] <= 0 or row['dead'] != old['dead']:
            pending.pop(slot, None)
        # Require an observed zero -> tag change in the same player/room.
        # An inherited nonzero tag at capture/room entry is left unbound.
        if same and old['dead'] == 0 and row['dead'] > 0:
            pending[slot] = (frame, row['dead'])
        decrease = old['lives'] - row['lives']
        if decrease <= 0:
            continue
        event = dict(frame=frame, circuit=row['circuit'], wave=row['wave'],
                     player=slot, decrease=decrease, previous=old, current=row)
        if first_invalid_frame is not None and frame >= first_invalid_frame:
            event['reason'] = 'at_or_after_invalid_list'
            excluded.append(event)
        elif row['state'] != 16 or old['state'] != 16 or (
                row['circuit'], row['wave']) != (old['circuit'], old['wave']):
            event['reason'] = 'noncombat_or_room_boundary'
            excluded.append(event)
        else:
            tag = pending.get(slot)
            cleared = same and row['dead'] == 0 and old['dead'] > 0 and prior_tag
            if decrease == 1 and same and ((tag and tag[1] == row['dead']) or cleared):
                tag = prior_tag if cleared else tag
                event.update(classification='preceding_cleared_tag' if cleared else 'sampled_tag_association', tag=tag[1],
                             tag_first_sample=tag[0], tag_age_frames=frame-tag[0],
                             type_bits=(tag[1] >> 8) & 31,
                             same_sample=tag[0] == frame)
            else:
                event['classification'] = 'unassociated_decrease'
            events.append(event)
        pending.pop(slot, None)
    return dict(events=events, excluded=excluded)


def pin(path):
    return dict(path=path.as_posix(), sha256=hashlib.file_digest(path.open('rb'), 'sha256').hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists():
        raise ValueError('Use fresh diagnostic output')
    report = json.loads(args.report.read_text())
    verify_report(args.report, report)
    if not report.get('require_integrity'):
        raise ValueError('List-integrity qualification required')
    result = dict(schema=1, result='SAMPLED_DAMAGE_TAG_DIAGNOSTIC', gate_proof=False,
                  source_report=pin(args.report), analyzer=pin(Path(__file__)), jobs={},
                  limits=['Frame samples and process-player bytes do not prove continuous process lifetime or active PID.',
                          'DEAD tags identify sampled type bits, not the specific damaging object or collision geometry; cleared tags are explicitly preceding-frame associations.',
                          'Includes partial visits; do not substitute these counts for completed distinct-history balance ratios.',
                          'Life-tag files are pinned at analysis time; their creation-time hashes were not in the original report.'])
    summary = Counter()
    for name, job in report['jobs'].items():
        path = args.report.parent / name / 'life-tags.csv'
        data = summarize_rows(csv.DictReader(path.open()), job['list_integrity']['first_invalid_frame'])
        data['source'] = pin(path)
        for event in data['events']:
            summary[event['classification']] += event['decrease']
        result['jobs'][name] = data
    result['summary'] = dict(summary)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n', newline='\n')
    print(json.dumps(dict(result=result['result'], summary=result['summary'], jobs=len(result['jobs']))))


if __name__ == '__main__':
    main()
