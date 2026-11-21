#!/usr/bin/env python3
"""Synthetic smoke checks for the read-only source inventory helper; no original source used."""
from __future__ import annotations
import importlib.util
import json
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audit_source', root / 'tools/audit_source.py')
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory)
    for name in ('sub', 'defs', 'duplicate'):
        (source/name).mkdir()
    (source/'sub/X.ASM').write_text(' include "C:\\OLD\\MPROC.EQU"\n include "AMB.EQU"\n include "MISSING.EQU"\n; include "ignored"\n')
    (source/'defs/MPROC.EQU').write_text('fixture not original')
    (source/'defs/AMB.EQU').write_text('one')
    (source/'duplicate/AMB.EQU').write_text('two')
    result = module.inventory(source)
    assert len(result['files']) == 4
    assert len(result['literal_includes']) == 3
    assert [record['status'] for record in result['literal_includes']] == [
        'unique_candidate_NOT_semantic_proof', 'AMBIGUOUS', 'NOT_FOUND_in_this_tree']
    assert result['git_commit'] is None
    assert result['full_source_audit'] == 'NOT_COMPLETE'
print(json.dumps({'result': 'PASS', 'cases': ['inventory count', 'literal include filtering',
      'historical DOS basename candidate', 'ambiguous include candidates', 'missing include',
      'no invented git pin', 'partial-audit status'], 'original_source_used': False}, indent=2))
