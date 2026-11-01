#!/usr/bin/env python3
"""Derive UVC descriptor fields from the pinned public C270 lsusb report.

The output omits audio, vendor extension controls and unrelated descriptors.
It is a descriptor projection, not a raw USB capture.
"""
import hashlib
import pathlib
import re
import struct
import sys

raw = pathlib.Path(sys.argv[1]).read_bytes()
expected = sys.argv[2]
assert hashlib.sha256(raw).hexdigest() == expected, 'source hash mismatch'
text = raw.decode()
blocks = re.split(r'(?m)^\s+(?:Interface Descriptor:|VideoControl Interface Descriptor:|VideoStreaming Interface Descriptor:|Endpoint Descriptor:)\n', text)
out = bytearray()
sub = 0
for block in blocks[1:]:
    def field(name):
        m = re.search(r'^\s+' + re.escape(name) + r'\s+(0x[0-9a-fA-F]+|\d+)', block, re.M)
        return int(m[1], 0) if m and m[1].startswith('0x') else int(m[1]) if m else 0
    typ = field('bDescriptorType')
    if typ == 4:
        cls, sub = field('bInterfaceClass'), field('bInterfaceSubClass')
        if cls == 14:
            out += bytes([9,4,field('bInterfaceNumber'),field('bAlternateSetting'),field('bNumEndpoints'),cls,sub,field('bInterfaceProtocol'),0])
        else:
            sub = 0
    elif typ == 36 and sub == 1 and field('bDescriptorSubtype') == 1:
        version = re.search(r'bcdUVC\s+(\d+)\.(\d+)', block)
        value = int(version[1])*256+int(version[2],16)
        out += bytes([5,36,1])+struct.pack('<H', value)
    elif typ == 36 and sub == 2:
        subtype = field('bDescriptorSubtype')
        if subtype == 4:
            d = bytearray(27); d[:5] = bytes([27,36,4,field('bFormatIndex'),field('bNumFrameDescriptors')])
            d[5:21] = bytes.fromhex('5955593200001000800000aa00389b71');d[21]=field('bBitsPerPixel');d[22]=field('bDefaultFrameIndex');out += d
        elif subtype == 6:
            out += bytes([11,36,6,field('bFormatIndex'),field('bNumFrameDescriptors'),field('bFlags'),field('bDefaultFrameIndex'),0,0,0,0])
        elif subtype in (5,7):
            intervals = [int(x) for x in re.findall(r'dwFrameInterval\(\s*\d+\)\s+(\d+)',block)]
            assert len(intervals)==field('bFrameIntervalType')
            d=bytearray(26+4*len(intervals));d[:5]=bytes([len(d),36,subtype,field('bFrameIndex'),field('bmCapabilities')])
            struct.pack_into('<HHIIII',d,5,field('wWidth'),field('wHeight'),field('dwMinBitRate'),field('dwMaxBitRate'),field('dwMaxVideoFrameBufferSize'),field('dwDefaultFrameInterval'))
            d[25]=len(intervals)
            for i,value in enumerate(intervals):struct.pack_into('<I',d,26+4*i,value)
            out+=d
    elif typ == 5 and sub == 2:
        out += struct.pack('<BBBBHB',7,5,field('bEndpointAddress'),field('bmAttributes'),field('wMaxPacketSize'),field('bInterval'))
print('/* Derived descriptor facts; see docs/usb-uvc-sources.json. */')
print('static const unsigned char c270_descriptors[] = {')
for i in range(0,len(out),16):print('  '+','.join(f'0x{x:02x}' for x in out[i:i+16])+',')
print('};')
