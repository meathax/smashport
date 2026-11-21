#!/usr/bin/env python3
"""Audit the translated Windows game's PE headers/imports; --legacy-lab checks the retired diagnostic."""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
from pathlib import Path

# D3D11 is the host GPU-effects owner; D3D12 remains the CRT presentation owner.
EXPECTED_DLLS = {'kernel32.dll', 'user32.dll', 'gdi32.dll', 'comdlg32.dll', 'ole32.dll', 'shell32.dll', 'ws2_32.dll', 'd3d11.dll', 'd3d12.dll', 'dxgi.dll', 'd3dcompiler_47.dll'}
LEGACY_DLLS = {'kernel32.dll', 'user32.dll', 'gdi32.dll'}

def audit(path: Path, legacy_lab: bool = False) -> dict:
    data = path.read_bytes()
    def unpack(fmt: str, offset: int):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise ValueError('Truncated PE structure')
        return struct.unpack_from(fmt, data, offset)
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Not an MZ executable')
    pe, = unpack('<I', 0x3c)
    if data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Missing PE signature')
    machine, sections, timestamp = unpack('<HHI', pe+4)
    optional_size, = unpack('<H', pe+20)
    op = pe+24
    magic, = unpack('<H', op)
    subsystem, dll_characteristics = unpack('<HH', op+68)
    directories, = unpack('<I', op+108)
    if machine != 0x8664 or magic != 0x20b or optional_size < 240 or directories < 16:
        raise ValueError('Expected Windows x64 PE32+ header')
    section_table = op+optional_size
    mappings = []
    for i in range(sections):
        _, virtual_size, virtual_address, raw_size, raw_pointer = unpack('<8sIIII', section_table+i*40)
        mappings.append((virtual_address, virtual_size, raw_size, raw_pointer))
    def at(rva: int, size: int = 1) -> int:
        for va, vs, rs, rp in mappings:
            delta = rva-va
            if 0 <= delta and delta+size <= rs:
                offset = rp+delta
                if offset+size <= len(data):
                    return offset
        raise ValueError(f'RVA has no file backing: {rva:#x}')
    def string(rva: int) -> str:
        start = at(rva)
        end = data.find(b'\0', start, min(start+4096, len(data)))
        if end < 0:
            raise ValueError('Unterminated PE string')
        return data[start:end].decode('ascii', errors='strict')
    import_rva, import_size = unpack('<II', op+112+8)
    imports = {}
    for i in range(import_size//20):
        original_thunk, _, _, name, first_thunk = unpack('<IIIII', at(import_rva+i*20, 20))
        if not any((original_thunk, name, first_thunk)):
            break
        dll = string(name).lower()
        symbols = []
        for j in range(4096):
            thunk, = unpack('<Q', at((original_thunk or first_thunk)+j*8, 8))
            if not thunk:
                break
            symbols.append(f'ordinal:{thunk & 0xffff}' if thunk >> 63 else string(thunk+2))
        else:
            raise ValueError('Import thunk table exceeded audit bound')
        imports[dll] = symbols
    else:
        raise ValueError('Import descriptor terminator missing')
    delay_imports = unpack('<II', op+112+13*8)
    clr = unpack('<II', op+112+14*8)
    expected = LEGACY_DLLS if legacy_lab else EXPECTED_DLLS
    # The GPU-resident D3D11 host no longer links the older D3D12/DXGI path.
    # Accept only these two exact production import graphs, not arbitrary subsets.
    resident = EXPECTED_DLLS - {'d3d12.dll', 'dxgi.dll'}
    valid_imports = set(imports) == expected or (not legacy_lab and set(imports) == resident)
    if not valid_imports or any(delay_imports) or any(clr) or subsystem != 2:
        raise ValueError(f'Import graph/subsystem differs from the expected native target: {sorted(imports)}')
    if not (dll_characteristics & 0x20 and dll_characteristics & 0x40 and dll_characteristics & 0x100):
        raise ValueError('Expected ASLR/high-entropy/NX PE flags are absent')
    return {'result': 'PASS', 'artifact': path.name, 'bytes': len(data),
            'sha256': hashlib.sha256(data).hexdigest(), 'machine': 'x86_64', 'format': 'PE32+',
            'subsystem': 'Windows GUI', 'timestamp': timestamp,
            'nx': True, 'aslr': True, 'high_entropy_va': True, 'static_imports': imports,
            'delay_imports': False, 'clr_header': False,
            'dynamic_load_from_source_review': 'Optional system XInput and local pinned librashader/dxcompiler GPU runtime; package inventory verifies runtime hashes',
            'scope': 'PE headers/imports only. Not Windows execution or a proof of every machine-code path.',
            'windows_execution': 'NOT RUN'}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--legacy-lab', action='store_true', help='audit the retired source diagnostic instead of the translated game')
    args = parser.parse_args()
    try:
        result = audit(args.executable, legacy_lab=args.legacy_lab)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, str(error)+'\n')
    text = json.dumps(result, indent=2)+'\n'
    if args.report:
        args.report.write_text(text, encoding='utf-8')
    print(text, end='')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
