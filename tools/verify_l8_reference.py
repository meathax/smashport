#!/usr/bin/env python3
"""Verify the private L8 archive against a selected MAME Smash T.V. driver.

This is a development-only metadata/reference check, not a source-retail
equivalence or gameplay differential. It never copies or changes ROM bytes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import zipfile
from pathlib import Path
from xml.etree import ElementTree


LOAD = re.compile(
    r'ROM_LOAD(?:16_BYTE)?\s*\(\s*"([^"]+)"\s*,\s*0x[0-9a-fA-F]+\s*,\s*'
    r'0x([0-9a-fA-F]+)\s*,\s*CRC\(([0-9a-fA-F]{8})\)\s*'
    r'SHA1\(([0-9a-fA-F]{40})\)\s*\)'
)


def verify(source: Path, rom_zip: Path, mame: Path | None) -> dict:
    driver = source.read_text(encoding='utf-8')
    section = re.search(r'ROM_START\(\s*smashtv\s*\)(.*?)ROM_END', driver, re.S)
    if not section:
        raise ValueError('No exact smashtv ROM_START section in driver')
    loads = LOAD.findall(section.group(1))
    if len(loads) != 18 or len({name for name, *_ in loads}) != 18:
        raise ValueError(f'Expected 18 distinct L8 ROM_LOAD entries, got {len(loads)}')
    rows = []
    with zipfile.ZipFile(rom_zip) as archive:
        by_name = {info.filename: info for info in archive.infolist()}
        for name, size_text, crc, sha1 in loads:
            if name not in by_name:
                raise ValueError(f'L8 archive missing {name}')
            info = by_name[name]
            data = archive.read(info)
            if len(data) != int(size_text, 16) or info.CRC != int(crc, 16):
                raise ValueError(f'L8 size/CRC mismatch for {name}')
            if hashlib.sha1(data).hexdigest() != sha1.lower():
                raise ValueError(f'L8 SHA1 mismatch for {name}')
            rows.append({'name': name, 'bytes': len(data), 'crc32': crc.lower(),
                         'sha1': sha1.lower()})
        extras = sorted(set(by_name) - {row['name'] for row in rows})
    result = {
        'result': 'PASS',
        'scope': 'L8 driver ROM metadata and optional MAME ROM verifier only; no gameplay or source-retail proof',
        'driver_path': str(source.resolve()),
        'driver_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'archive_sha256': hashlib.sha256(rom_zip.read_bytes()).hexdigest(),
        'required_loads_verified': len(rows),
        'extra_archive_entries': extras,
        'loads': rows,
    }
    if mame:
        version = subprocess.run([str(mame), '-version'], capture_output=True,
                                 text=True, timeout=20, check=True)
        if not version.stdout.strip().startswith('0.289'):
            raise ValueError(f'Reference MAME is not 0.289: {version.stdout.strip()}')
        checked = subprocess.run([str(mame), '-rompath', str(rom_zip.parent),
                                  '-verifyroms', 'smashtv'], capture_output=True,
                                 text=True, timeout=60)
        output = checked.stdout + checked.stderr
        if checked.returncode or 'romset smashtv is good' not in output:
            raise ValueError(f'MAME L8 verification failed: {output.strip()}')
        result['mame_version'] = version.stdout.strip()
        result['mame_executable_sha256'] = hashlib.sha256(mame.read_bytes()).hexdigest()
        result['mame_verifyroms'] = output.strip()
        listed = subprocess.run([str(mame), '-listxml', 'smashtv'],
                                capture_output=True, text=True, timeout=60, check=True)
        machine = ElementTree.fromstring(listed.stdout).find("./machine[@name='smashtv']")
        if machine is None or machine.find('display') is None:
            raise ValueError('MAME did not report the L8 display')
        result['mame_display'] = machine.find('display').attrib
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mame-source', type=Path, required=True)
    parser.add_argument('--rom-zip', type=Path, required=True)
    parser.add_argument('--mame', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error('Report already exists; refusing to overwrite it')
    report = verify(args.mame_source, args.rom_zip, args.mame)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"PASS: {report['required_loads_verified']} L8 ROM loads match MAME metadata; "
          f"MAME verifier: {report.get('mame_verifyroms', 'not run')}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
