#!/usr/bin/env python3
"""Extract numeric GET_CUR probe facts from a pinned Linux usbmon capture."""
import hashlib
import pathlib
import struct
import sys
raw = pathlib.Path(sys.argv[1]).read_bytes()
assert hashlib.sha256(raw).hexdigest() == 'bd1ab6f2775fae9ad8b8732598c42e89f4a6b50915a1203302e7d41fb4f635e7'
assert struct.unpack_from('<I',raw)[0]==0xa1b2c3d4
assert struct.unpack_from('<I',raw,20)[0]==220
pending={}
offset=24
facts=[]
while offset<len(raw):
    _,_,size,_=struct.unpack_from('<IIII',raw,offset)
    packet=raw[offset+16:offset+16+size];offset+=16+size
    assert len(packet)>=64
    if packet[8]==ord('S'):
        pending[packet[:8]]=packet[40:48]
    elif packet[8]==ord('C'):
        setup=pending.pop(packet[:8],b'')
        if setup==bytes.fromhex('a181000103001a00'):
            data=packet[64:]
            assert len(data)==26 and struct.unpack_from('<i',packet,28)[0]==0
            facts.append((data[2],data[3],*struct.unpack_from('<I',data,4),*struct.unpack_from('<H',data,12),*struct.unpack_from('<II',data,18)))
assert len(facts)==9
print('/* Numeric streaming probe facts from Wireshark issue 8414; no image bytes. */')
print('static const struct { unsigned format,frame,interval,quality,maximum,payload; } captured_probes[] = {')
for fact in facts:print('  {'+','.join(str(x) for x in fact)+'},')
print('};')
