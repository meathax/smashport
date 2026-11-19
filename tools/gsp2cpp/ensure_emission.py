"""Refresh private L8 emission transactionally, or reject stale build inputs."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = 'emission_manifest.json'
REQUIRED = {'l8_dispatch.cpp', 'l8_chunk_000.cpp', 'l8_translation_index.csv', 'enhanced_stage_balance.inc'}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inputs(root):
    paths = [root/'generated/private/l8_codemap.json',
             root/'original/user-supplied-roms/smashtv.zip']
    paths += sorted((root/'tools/gsp2cpp').glob('*.py'))
    paths += sorted((root/'data').glob('enhanced*.json'))
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f'Missing L8 emission prerequisite: {path}')
    if not (root/'tools/gsp2cpp/emit.py').is_file():
        raise RuntimeError('Missing L8 emitter')
    return {p.relative_to(root).as_posix(): digest(p) for p in paths}


def outputs(directory):
    return {p.name: digest(p) for p in sorted(directory.iterdir())
            if p.is_file() and (p.name.startswith('l8_') and p.suffix in ('.cpp', '.csv')
                                or p.name == 'enhanced_stage_balance.inc')}


@contextmanager
def locked(directory):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory/'.emission.lock').open('a+b') as stream:
        stream.seek(0)
        if not stream.read(1):
            stream.write(b'\0'); stream.flush()
        stream.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError('Another L8 emission check/refresh owns the output directory') from error
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == 'nt':
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


def archive(root, path):
    destination = root/'build/l8-emission-history'/digest(path)/path.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if digest(destination) != digest(path):
            raise RuntimeError('L8 emission archive collision')
    else:
        shutil.copy2(path, destination)
    return destination


def ensure(root, refresh=False):
    root = root.resolve()
    directory = root/'generated/private/l8'
    with locked(directory):
        current = inputs(root)
        path = directory/MANIFEST
        try:
            saved = json.loads(path.read_text())
        except (OSError, ValueError):
            saved = {}
        actual = outputs(directory)
        if (saved.get('schema') == 1 and saved.get('inputs') == current
                and REQUIRED <= actual.keys() and saved.get('outputs') == actual and saved.get('chunk') == 192):
            return dict(result='CURRENT', changed=[])
        if not refresh:
            raise RuntimeError('Stale or unsealed private L8 code. Run: python tools/gsp2cpp/ensure_emission.py --refresh')
        build = root/'build'
        build.mkdir(exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix='l8-emission-', dir=build)).resolve()
        if scratch.parent != build.resolve():
            raise RuntimeError('L8 emission staging escaped the build directory')
        success = False
        try:
            command = [sys.executable, str(root/'tools/gsp2cpp/emit.py'), '--chunk', '192',
                       '--codemap', str(root/'generated/private/l8_codemap.json'), '--out', str(scratch)]
            with (scratch/'generation.log').open('w') as log:
                child = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=300)
            if child.returncode:
                raise RuntimeError(f'L8 emission failed ({child.returncode}); retained {scratch}/generation.log')
            created = outputs(scratch)
            if not REQUIRED <= created.keys():
                raise RuntimeError('L8 emitter omitted required outputs')
            rows = (scratch/'l8_translation_index.csv').read_text().splitlines()[1:]
            count = len(json.loads((root/'generated/private/l8_codemap.json').read_text())['instructions'])
            # Instructions contain unquoted commas: only split the final status.
            if len(rows) != count or any(row.rsplit(',', 1)[-1] != 'translated' for row in rows):
                raise RuntimeError('Incomplete L8 emission or blocked instruction forms')
            if inputs(root) != current:
                raise RuntimeError('L8 emission inputs changed during generation')
            stale = set(outputs(directory)) - created.keys()
            for name in stale:
                candidate = directory/name
                if (not re.fullmatch(r'l8_chunk_\d{3}\.cpp', name)
                        or not candidate.read_bytes().startswith(b'// GENERATED by tools/gsp2cpp/emit.py')):
                    raise RuntimeError(f'Refusing to remove unowned private source: {candidate}')
            changed = []
            for name, sha in created.items():
                target = directory/name
                if target.exists() and digest(target) == sha:
                    continue  # Preserve unchanged C++ mtimes and incremental builds.
                if target.exists():
                    archive(root, target)
                os.replace(scratch/name, target)
                changed.append(name)
            for name in stale:
                candidate = (directory/name).resolve()
                if candidate.parent != directory.resolve():
                    raise RuntimeError('Stale L8 source escaped its output directory')
                archive(root, candidate)
                candidate.unlink()
            if path.exists():
                archive(root, path)
            manifest = dict(schema=1, chunk=192, inputs=current, outputs=created)
            temporary = directory/(MANIFEST+'.tmp')
            temporary.write_text(json.dumps(manifest, indent=2)+'\n', newline='\n')
            os.replace(temporary, path)
            if outputs(directory) != created:
                raise RuntimeError('Published L8 emission differs from verified staging')
            success = True
            return dict(result='REFRESHED', changed=changed, removed=sorted(stale), instructions=count)
        finally:
            # Only successful, newly created staging is disposable. Failed runs
            # remain for diagnosis; prior private sources are archived by hash.
            if success:
                if scratch.parent != build.resolve():
                    raise RuntimeError('L8 staging cleanup escaped the build directory')
                shutil.rmtree(scratch)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh', action='store_true')
    args = parser.parse_args()
    try:
        print(json.dumps(ensure(ROOT, args.refresh)))
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
