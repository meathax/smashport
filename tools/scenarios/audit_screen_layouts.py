"""Audit complete recorded scene streams, including end-to-next-game handoff.

This checks layout ownership, not all-game fidelity or individual room clearance.
The two-frame transition allowance accounts for the completed scanout latch.
"""
import argparse
import hashlib
import json
from pathlib import Path

PAGES = {0xffea54f0: 'ending', 0xffea5550: 'title', 0xffea55b0: 'credits',
         0xffea5610: 'high_scores', 0xffea5670: 'initials'}


def audit(path, final):
    seen = set()
    counts = {}
    errors = []
    previous = None
    stable = 0
    ending = False
    restart = False
    with path.open(encoding='utf-8') as stream:
        assert json.loads(next(stream))['schema'] == 'stv.scene.frames.v1'
        for frame, line in enumerate(stream):
            row = json.loads(line)
            assert row['frame'] == frame
            key = (row['gamstate'], row['background'])
            stable = stable + 1 if key == previous else 0
            previous = key
            page = PAGES.get(row['background'])
            live = row['gamstate'] in (0x10, 0x18, 0x50)
            if page == 'ending':
                ending = True
            if stable < 2:
                continue
            expected = 410 if page or not live else 547
            # POST uses its own uninitialised RAM, never a live scene contract.
            if row['gamstate'] not in (0, 0x10, 0x18, 0x30, 0x40, 0x50):
                continue
            if row['view_width'] != expected or row['outside_nonzero_indices']:
                if len(errors) < 10:
                    errors.append(dict(frame=frame, expected=expected, observed=row))
            label = page or ('world' if live else 'native')
            counts[label] = counts.get(label, 0) + 1
            if label == 'world':
                seen.add(row['circuit'])
                if ending and row['circuit'] == 0 and row['wave'] == 1:
                    restart = True
    assert frame == final
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                frames=final+1, errors=errors, scenes=counts,
                world_circuits=sorted(seen), ending_to_new_game=restart)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    before = audit(args.root/'coop/scenes.jsonl', 240000)
    coop = audit(args.root/'fixed-coop/scenes.jsonl', 240000)
    initials = audit(args.root/'fixed-initials/scenes.jsonl', 70000)
    suite = json.loads((args.root/'fixed-scenes/status.json').read_text(encoding='utf-8'))
    ok = bool(before['errors']) and not coop['errors'] and not initials['errors']
    ok &= coop['world_circuits'] == [0, 1, 2] and coop['ending_to_new_game']
    ok &= all(initials['scenes'].get(k, 0) for k in ('initials', 'high_scores', 'title', 'credits'))
    ok &= coop['scenes'].get('ending', 0) > 1000
    ok &= suite['status'] == 'PASS_CAPTURE_AND_NATIVE_SCENE_IDENTITY'
    report = dict(result='PASS' if ok else 'FAIL', before=before, coop=coop,
                  initials=initials, scene_suite_cases=len(suite['cases']),
                  limits=['Layout ownership and complete capture endpoints; not whole-game fidelity.',
                          'PNG visual review and compiled geometry checks are separately recorded.'])
    args.out.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(result=report['result'], report=str(args.out))))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
