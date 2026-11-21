"""Read the CMOS prefix of a Windows x64 STVMS003 development snapshot.

Layout is scoped to Cpu in include/stv/gsp/core.hpp and fields() in
src/gsp/state.cpp: 32 uint32 registers/flags plus one uint64 cycle count.
This does not validate the rest of a snapshot or restore any machine state.
"""
import struct


def read_cmos(data):
    if len(data)<149 or data[:8]!=b'STVMS003' or data[12] not in (0,1):
        raise ValueError('Unsupported/truncated machine snapshot')
    offset=13+136
    for expected,width in [(0x10000,2),(0x2000,2),(0x1000,4),(0x4000,2)]:
        if offset+8>len(data):raise ValueError('Truncated vector header')
        count,=struct.unpack_from('<Q',data,offset);offset+=8
        if count!=expected or offset+count*width>len(data):raise ValueError('Unexpected/truncated snapshot vector')
        if expected==0x4000:return struct.unpack_from('<16384H',data,offset)
        offset+=count*width
    raise ValueError('CMOS vector missing')
