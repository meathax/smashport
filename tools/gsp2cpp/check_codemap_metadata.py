"""Real-ROM regressions for branch joins and weak-stage transfer reporting."""
from pathlib import Path
import argparse, json, os, subprocess, sys

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / "tools/gsp2cpp/codemap.py").is_file())
JOIN = 'fff50100'
WEAK_DATA = ('ffe62340', 'ffe62380', 'ffe62440', 'ffe62680', 'ffe626c0', 'ffe62700',
             'ffe6de40', 'ffe6e2d0', 'ffe70b40', 'fff90000', 'fff9cac0', 'fffb55a0')
MASK_RETURNS = ('ffe6f500', 'ffe7b910', 'ffe9b3e0', 'ffeb63b0', 'ffebb560', 'fff3a690')
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generator', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    output = (ROOT / args.out).resolve()
    assert output.is_relative_to((ROOT / 'build').resolve()) and not output.exists()
    output.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['PYTHONPATH'] = str(ROOT / 'tools/gsp2cpp')
    run = subprocess.run([sys.executable, '-B', str(args.generator.resolve()), '--out', str(output)],
                         cwd=ROOT, env=env, capture_output=True, text=True, encoding='utf-8', timeout=120)
    print(run.stdout, end='')
    print(run.stderr, end='', file=sys.stderr)
    if run.returncode:
        raise SystemExit(run.returncode)
    data = json.loads(output.read_bytes().decode('utf-8'))
    original = json.loads((ROOT / 'generated/private/l8_codemap.json').read_bytes().decode('utf-8'))
    missing = [pc for pc in (JOIN,) + WEAK_DATA + MASK_RETURNS if pc not in data['indirect_sites']]
    assert {k: v for k, v in data.items() if k != 'indirect_sites'} == {k: v for k, v in original.items() if k != 'indirect_sites'}
    result = {'regression': 'branch-selected callback, restored returns and every weak-stage transfer must remain visible',
              'CIRCLE_join_reported': JOIN in data['indirect_sites'], 'weak_data_transfers_reported': sum(a in data['indirect_sites'] for a in WEAK_DATA),
              'required_MMFM_returns_reported': sum(a in data['indirect_sites'] for a in MASK_RETURNS),
              'missing': missing, 'indirect_sites': len(data['indirect_sites']),
              'instruction_vectors_and_conflicts_unchanged': True, 'result': 'FAIL' if missing else 'PASS'}
    print(json.dumps(result))
    raise SystemExit(1 if missing else 0)


if __name__ == '__main__':
    main()
