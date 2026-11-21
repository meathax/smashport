#!/usr/bin/env python3
"""Build a deterministic source-art pack from the supplied editor assets.
No downloads, ROM execution or proprietary SDK. Original source is never changed.
"""
from __future__ import annotations
import argparse, hashlib, json, pathlib, struct, zlib

def build(source:pathlib.Path, output:pathlib.Path)->dict:
    image_dir=source/'IMG'
    files=sorted((p for p in image_dir.iterdir() if p.suffix.upper() in {'.IMG','.BDD','.BDB'}),key=lambda p:p.name.upper())
    if not files or len(files)>128: raise ValueError('Expected 1..128 original image/background files')
    payload=bytearray(); table=bytearray(); manifest=[]
    offset=64+len(files)*64
    for p in files:
        name=p.name.upper().encode('ascii'); data=p.read_bytes()
        if len(name)>=48: raise ValueError('Filename too long')
        table+=struct.pack('<48sQII',name,offset,len(data),zlib.crc32(data))
        payload+=data;offset+=len(data)
        manifest.append({'file':'IMG/'+p.name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    body=table+payload
    result=struct.pack('<8sIIQQ32s',b'STVSRC2\0',2,len(files),64+len(files)*64,64+len(body),hashlib.sha256(body).digest())+body
    output.parent.mkdir(parents=True,exist_ok=True); output.write_bytes(result)
    report={'format':'STVSRC2','revision':'user-supplied historical source snapshot; retail match unverified','rights':'Original assets retain their owners\' copyrights; private user-provided inputs, no redistribution permission inferred.','output_sha256':hashlib.sha256(result).hexdigest(),'inputs':manifest}
    output.with_suffix('.manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    return report
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('source',type=pathlib.Path);ap.add_argument('output',type=pathlib.Path);args=ap.parse_args()
    report=build(args.source,args.output);print(f'{len(report["inputs"])} original files -> {args.output}; SHA256 {report["output_sha256"]}')
