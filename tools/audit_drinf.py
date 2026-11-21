#!/usr/bin/env python3
"""Search ONLY user-supplied program ROM data for the DRINF-shaped table.
This is a data/signature audit, not execution or a proof of hardware read values.
CRC/size profiles identify supplied revision pairs; full SHA-256s are recorded.
"""
from __future__ import annotations
import argparse, hashlib, json, struct, zipfile
from pathlib import Path
PROFILES={8:(0x48cd793f,0x8e7fe463),6:(0xf1666017,0x908aca5d),5:(0x81f564b9,0xe5017d25),4:(0xa50ccb71,0xef0b0279),3:(0x33b626c3,0x5f6fbc25)}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('user_zip',type=Path);p.add_argument('--report',type=Path,required=True);a=p.parse_args();results=[]
    with zipfile.ZipFile(a.user_zip) as z:
        for rev,crcs in PROFILES.items():
            entries=[]
            for crc in crcs:
                matches=[e for e in z.infolist() if e.file_size==131072 and e.CRC==crc]
                if len(matches)!=1:raise ValueError(f'Expected one supplied L{rev} member for CRC {crc:08x}')
                entries.append(matches[0])
            even,odd=[z.read(e) for e in entries];program=bytearray(262144);program[::2]=even;program[1::2]=odd;matches=[]
            for pos in range(0,len(program)-32,2):
                zero,left,right,top=struct.unpack_from('<4I',program,pos)
                if zero==0 and 0x01000000<=left<0x01100000 and right==left+8 and top==left+16:
                    matches.append({'interleaved_program_byte_offset':hex(pos),'program_bit_address':hex(0xffe00000+8*pos),
                        'four_declared_longs':[hex(v) for v in (zero,left,right,top)],'long_at_gate4_index':hex(struct.unpack_from('<I',program,pos+16)[0]),'following_16_bytes':program[pos+16:pos+32].hex()})
            if not matches:raise ValueError(f'No DRINF-shaped candidate for supplied L{rev}')
            results.append({'profile':f'L{rev}','inputs':[{'name':e.filename,'crc32':f'{e.CRC:08x}','sha256':hashlib.sha256(z.read(e)).hexdigest()} for e in entries],
                'interleaved_program_sha256':hashlib.sha256(program).hexdigest(),'candidates':matches})
    report={'kind':'User ROM byte/signature audit; NOT runtime or hardware validation','zip_sha256':hashlib.sha256(a.user_zip.read_bytes()).hexdigest(),'profiles':results,
        'interpretation':'The four-long signature 0, RAM, RAM+8, RAM+16 matches the supplied HULKS DRINF shape. The next long is 0x18915610 in every candidate. Candidate identification is not a symbol map or program-flow proof. Do not assume which byte the PCB returns when that value is dereferenced; no zero/FF fallback was introduced.',
        'runtime_instructions_executed':False}
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
