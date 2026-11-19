"""Verify an audited private ZIP and run an isolated offline player smoke test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audit', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    args = parser.parse_args()
    audit = json.loads(args.audit.read_text(encoding='utf-8-sig'))
    if audit['result'] != 'PASS':
        raise ValueError('Package audit is not PASS')
    package = Path(audit['zip'])
    if hashlib.sha256(package.read_bytes()).hexdigest() != audit['zip_sha256']:
        raise ValueError('ZIP differs from audit')
    expected = {f['path']: f for f in audit['files']}
    with zipfile.ZipFile(package) as archive:
        files = [f for f in archive.infolist() if not f.is_dir()]
        if len(files) != len(expected) or {f.filename for f in files} != set(expected):
            raise ValueError('ZIP contents differ from audited inventory')
        for f in files:
            parts = Path(f.filename).parts
            if Path(f.filename).is_absolute() or '..' in parts or ':' in f.filename:
                raise ValueError('Unsafe ZIP path')
            data = archive.read(f)
            record = expected[f.filename]
            if len(data) != record['bytes'] or hashlib.sha256(data).hexdigest() != record['sha256']:
                raise ValueError('ZIP member differs: ' + f.filename)
        args.out.mkdir(parents=True, exist_ok=False)
        player = args.out / 'player'
        archive.extractall(player)
    data = args.out / 'userdata' / 'SmashTV'
    data.mkdir(parents=True)
    # Historical player builds read this copy from the isolated test profile.
    shutil.copyfile(args.rom, data / 'smashtv.zip')
    # Exercise the real audio pipeline with silent device output in the isolated profile.
    (data / 'pc-settings.cfg').write_text('version=1\nmuted=1\nvolume=0\n', encoding='ascii')
    temp = args.out / 'temp'
    temp.mkdir()
    report = args.out / 'host.json'
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('STV_')}
    env.update(STV_HOST_ROM_ZIP=str(args.rom.resolve()), LOCALAPPDATA=str(data.parent.resolve()), TEMP=str(temp.resolve()), TMP=str(temp.resolve()),
               STV_HOST_FRAMES='3300', STV_HOST_NOPAUSE='1', STV_HOST_REPORT=str(report.resolve()),
               STV_HOST_INPUTS=str(args.inputs.resolve()))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    with (args.out / 'process.log').open('w') as log:
        proc = subprocess.run([str((player / 'SmashTV.exe').resolve())], cwd=player, env=env,
                              startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, timeout=150)
    host = json.loads(report.read_text(encoding='utf-8-sig'))
    ok = proc.returncode == 0 and host.get('frames') == 3300 and host.get('timed_out') is False and host.get('sound_fault') == ''
    result = {'result': 'PASS_OFFLINE_PACKAGE_SMOKE' if ok else 'FAIL', 'package_audit': str(args.audit.resolve()),
              'exe_sha256': audit['exe_sha256'], 'exit_code': proc.returncode, 'host': host,
              'scope': 'ZIP hashes and isolated offline execution; not visual, listening or full-game qualification'}
    (args.out / 'report.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(result['result'], report)
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
