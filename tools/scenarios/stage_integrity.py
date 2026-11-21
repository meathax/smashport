"""Validate passive list-health samples and censor contaminated pressure visits."""
import csv
import hashlib
import re


def read_integrity(directory, log, final):
    path = directory / 'list-integrity.csv'
    raw = path.read_bytes()
    rows = list(csv.reader(raw.decode('utf-8').splitlines()))
    markers = re.findall(r'^LIST_INTEGRITY samples=(\d+) first_invalid_frame=(\d+)$',
                         log, re.M)
    if len(markers) != 1:
        raise ValueError('Missing or ambiguous list-integrity terminal')
    samples, first = map(int, markers[0])
    for row in rows:
        if len(row) != 3:
            raise ValueError('Invalid list-integrity row')
        frame, root, node = int(row[0]), int(row[1], 16), int(row[2], 16)
        if not 2500 <= frame <= final or root not in (0x01000000, 0x01000020,
                0x01000040, 0x01000060, 0x010000a0) or not 0 <= node <= 0xffffffff:
            raise ValueError('Invalid list-integrity boundary')
    if len(rows) > 1 or first != (int(rows[0][0]) if rows else 0):
        raise ValueError('List-integrity first boundary differs')
    if samples != max(0, min(final, first or final) - 2500 + 1):
        raise ValueError('Incomplete list-integrity sampling')
    return dict(sha256=hashlib.sha256(raw).hexdigest(), samples=samples,
                first_invalid_frame=first or None)


def censor_pressure(run, first):
    """Keep only whole observations ending before the first invalid sample.

    A crossing visit's aggregate cannot be split into valid and invalid ticks.
    Retain excluded records separately, including partial visits and life deltas.
    """
    result, excluded = dict(run), []
    for key in ('observations', 'partial_visits', 'boundary_life_decreases'):
        result[key] = []
        for row in run[key]:
            if first is not None and row['frame'] >= first:
                excluded.append(dict(row, kind=key, reason='invalid_list_state'))
            else:
                result[key].append(row)
    return result, excluded
