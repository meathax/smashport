#!/usr/bin/env python3
"""Independent record-line parser for the original wave/entry DATA tables.
Does not import or invoke the generator's assembler parser. No CPU execution.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('generated_report', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    generated = json.loads(args.generated_report.read_text())
    symbols: dict[str, int] = {}
    def value(word: str) -> int:
        word = word.strip().upper()
        if word in symbols:
            return symbols[word]
        if re.fullmatch(r'[0-9A-F]+H', word):
            return int(word[:-1], 16)
        if re.fullmatch(r'[01]+B', word):
            return int(word[:-1], 2)
        if re.fullmatch(r'>[0-9A-F]+', word):
            return int(word[1:], 16)
        if re.fullmatch(r'[0-9]+', word):
            return int(word, 10)
        raise ValueError(f'Unsupported source literal {word!r}')
    for line in (args.source/'ROBO.EQU').read_text('latin1').splitlines():
        match = re.match(r'^\s*(\w+)\s+\.EQU\s+([^;]+)', line, re.I)
        if match:
            try:
                symbols[match[1].upper()] = value(match[2])
            except ValueError:
                pass  # unused pointer/field expressions are not this oracle's scope
    all_lines = (args.source/'ROBO.ASM').read_text('latin1').splitlines()
    circuit = -1
    rows: list[list[int]] = []
    waves: list[dict] = []
    wave = 1
    start_line = 0
    for number, raw in enumerate(all_lines, 1):
        line = raw.split(';', 1)[0].strip()
        if line in ['WAVE_INFO:', 'W2_INFO:', 'W3_INFO:']:
            assert not rows
            circuit += 1
            wave = 1
            continue
        if circuit < 0:
            continue
        if re.fullmatch(r'\.IF\s+DEBUG', line, re.I):
            break
        if not line:
            continue
        match = re.fullmatch(r'\.WORD\s+(.+)', line, re.I)
        if not match:
            raise ValueError(f'Unexpected wave directive at {number}: {line}')
        record = [value(word) for word in match[1].split(',')]
        if record == [0]:
            assert 0 < len(rows) <= 8
            waves.append({'circuit': circuit, 'wave': wave, 'line': start_line, 'records': rows})
            rows = []
            wave += 1
        else:
            assert len(record) == 6
            if not rows:
                start_line = number
            rows.append(record)
    assert not rows and waves == generated['waves']
    att = (args.source/'ROBOATT.ASM').read_text('latin1').splitlines()
    movement_results = []
    for expected in generated['movements']:
        label = expected['label']
        start = next(i for i, line in enumerate(att) if re.match(r'^' + label + r'\b', line))
        words = []
        for raw in att[start:]:
            line = raw.split(';', 1)[0]
            match = re.search(r'\.WORD\s+(.+)', line, re.I)
            if not match:
                continue
            stop = False
            for token in match[1].split(','):
                n = value(token)
                if n == 65535:
                    stop = True
                    break
                words.append(n)
            if stop:
                break
        assert len(words) % 2 == 0
        steps = [words[i:i+2] for i in range(0, len(words), 2)]
        actual = {'label': label, 'line': start+1, 'steps': steps, 'ticks': sum(x[1] for x in steps)}
        assert actual == expected
        movement_results.append({'label': label, 'steps': len(steps), 'ticks': actual['ticks']})
    for filename, expected_hash in generated['inputs'].items():
        assert hashlib.sha256((args.source/filename).read_bytes()).hexdigest() == expected_hash
    report = {'result': 'PASS', 'wave_tables': len(waves), 'wave_records': sum(len(w['records']) for w in waves),
              'waves_per_circuit': [sum(w['circuit'] == c for w in waves) for c in range(3)],
              'movement_tables': movement_results, 'source_sha256': generated['inputs'],
              'scope': 'Independent parsing of supplied source DATA; not gameplay/arcade equivalence'}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
