#!/usr/bin/env python3
"""Independent Python byte parser vs native image geometry/pixels/palette hashes.
No use of the native parser or the source importer implementation.
"""
from __future__ import annotations
import argparse,csv,hashlib,pathlib,struct,json,zipfile

def run(source:pathlib.Path,report:pathlib.Path,archive:pathlib.Path|None)->dict:
    rows={ (r['file'],int(r['image_index'])):r for r in csv.DictReader(report.open(),delimiter='\t') }
    compared=padding=0
    for file in sorted((source/'IMG').glob('*.IMG')):
        raw=file.read_bytes();count,next_palette,directory=struct.unpack_from('<HHI',raw)
        assert 0<=directory<len(raw)
        for index in range(count):
            header=directory+index*42
            name=raw[header:header+16].split(b'\0')[0].decode('ascii')
            ax,ay,w,h,selector,offset=struct.unpack_from('<hhHHHI',raw,header+16)
            stride=4*((w+3)//4)
            pixels=b''.join(raw[offset+y*stride:offset+y*stride+w] for y in range(h))
            assert len(pixels)==w*h
            for y in range(h):
                pad=raw[offset+y*stride+w:offset+(y+1)*stride]
                assert all(x==0 for x in pad),f'Nonzero source row padding {file.name}/{name}'
                padding+=len(pad)
            palette=(selector&255)-3
            assert 0<=palette<next_palette-3
            pheader=directory+count*42+palette*26
            colors=struct.unpack_from('<H',raw,pheader+12)[0]
            poffset=struct.unpack_from('<I',raw,pheader+14)[0]
            words=raw[poffset:poffset+colors*2]
            assert all(x<colors for x in pixels)
            row=rows.pop((file.name.upper(),index))
            assert row['image']==name
            expected={'width':w,'height':h,'anchor_x':ax,'anchor_y':ay,'selector':selector}
            for k,v in expected.items():assert int(row[k])==v,(file.name,name,k)
            assert row['pixel_sha256']==hashlib.sha256(pixels).hexdigest()
            assert row['palette_sha256']==hashlib.sha256(words).hexdigest()
            compared+=1
    assert not rows,rows
    preserved=0
    if archive:
        with zipfile.ZipFile(archive) as z:
            for entry in z.infolist():
                if entry.is_dir():continue
                relative=pathlib.PurePosixPath(entry.filename)
                assert relative.parts[0]=='smashtv-main'
                path=source.joinpath(*relative.parts[1:])
                assert path.read_bytes()==z.read(entry),f'Original modified: {path}'
                preserved+=1
    result={'result':'PASS','images_compared':compared,'zero_padding_bytes_checked':padding,'original_files_byte_identical_to_upload':preserved,
      'scope':'Independent editor-byte interpretation against native hash/geometry output. Not a retail ROM or hardware screenshot comparison.'}
    print(json.dumps(result,indent=2));return result
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('native_tsv',type=pathlib.Path);p.add_argument('--archive',type=pathlib.Path);a=p.parse_args();run(a.source,a.native_tsv,a.archive)
