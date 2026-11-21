#!/usr/bin/env python3
"""Read-only source inventory and conservative literal-include resolver.
This is NOT a macro preprocessor, assembler, full dependency audit or retail-match proof.
Historical absolute DOS paths are resolved only against files actually present.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

INCLUDE = re.compile(r'^\s*\.?include\s+(?:["\']([^"\']+)["\']|([^;\s]+))', re.I)
TEXT_SUFFIXES = {'.asm', '.equ', '.inc', '.hdr', '.rbo', '.old', '.glo', '.tbl', '.cmd', '.ini'}
LINK_OBJECTS = ('NDSP1 RAM UTIL PALL MPROC BAKGND BGNDTBL BGNDPAL IMGTBL IMGPAL MAIN ROBO '
                'SCORE SHOTS HULKS COLL2 SOUNDS PATCH TEXT DEMO WAVES ROBOATT HSTD ADJUST '
                'AUDIT TEST CHASER ENDWAVE ROBODOOR POW1 POW2 TRIG ICONS TANK FATMAN ROBOPAL '
                'MUTOID MINE LZRORB WLMAN TVMNTR RBOSS ROBOTS GAL COBRA SNAKE DIAG MENU').split()

def inventory(root: Path, exclude: Path | None = None) -> dict:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError('Source root is not a directory')
    paths = sorted(p for p in root.rglob('*') if p.is_file() and '.git' not in p.relative_to(root).parts
                   and not p.is_symlink() and (exclude is None or p.resolve() != exclude.resolve()))
    entries, by_name, by_relative = [], {}, {}
    for path in paths:
        relative = path.relative_to(root).as_posix()
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        entries.append({'path': relative, 'bytes': path.stat().st_size, 'sha256': digest})
        by_name.setdefault(path.name.casefold(), []).append(relative)
        by_relative[relative.casefold()] = relative
    include_records = []
    for path in paths:
        if path.suffix.casefold() not in TEXT_SUFFIXES or path.stat().st_size > 8 * 1024 * 1024:
            continue
        source = path.relative_to(root).as_posix()
        for line_number, line in enumerate(path.read_text(encoding='latin1').splitlines(), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            literal = match.group(1) or match.group(2)
            normalized = literal.replace('\\', '/')
            candidates = []
            for key in [(path.parent.relative_to(root) / normalized).as_posix().casefold(), normalized.casefold()]:
                if key in by_relative and by_relative[key] not in candidates:
                    candidates.append(by_relative[key])
            method = 'exact_relative_case_insensitive'
            if not candidates:
                candidates = by_name.get(normalized.rsplit('/', 1)[-1].casefold(), [])
                method = 'historical_path_basename_candidates'
            include_records.append({'source': source, 'line': line_number, 'literal': literal,
                                    'method': method, 'candidates': candidates,
                                    'status': 'unique_candidate_NOT_semantic_proof' if len(candidates) == 1 else
                                              'AMBIGUOUS' if candidates else 'NOT_FOUND_in_this_tree'})
    commit = None
    try:
        check = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True,
                               text=True, timeout=10, check=True)
        if re.fullmatch(r'[0-9a-f]{40}', check.stdout.strip()):
            commit = check.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    linked = []
    for name in LINK_OBJECTS:
        candidates = [e['path'] for e in entries if Path(e['path']).stem.casefold() == name.casefold()]
        linked.append({'object': name, 'same_stem_candidates': candidates,
                       'status': 'CANDIDATES_ONLY' if candidates else 'NOT_FOUND_in_this_tree'})
    return {'format': 'STV_SOURCE_INVENTORY_1', 'root': str(root), 'git_commit': commit,
            'full_source_audit': 'NOT_COMPLETE', 'retail_revision_match': 'UNVERIFIED',
            'scope': 'File hashes and literal include candidates only; no macros, conditionals or link semantics resolved.',
            'files': entries, 'literal_includes': include_records, 'known_link_object_candidates': linked}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error('Report already exists; refusing to overwrite it')
    try:
        report = inventory(args.root, args.report)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
    print(f'Inventoried {len(report["files"])} files; full source audit and retail match remain UNVERIFIED.')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
