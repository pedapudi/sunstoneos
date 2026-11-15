#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare native JPEG preparation to pinned A733 AArch64 library instructions.

Takes a compiled host shared library, original libawmjpeg.so, a baseline JPEG,
and a fresh evidence directory. Requires pyelftools and Unicorn. Runs no devices.
"""
import argparse
import ctypes as C
import hashlib
import json
import pathlib
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC

class Huffman(C.Structure):
    _fields_ = [('counts', C.c_uint8 * 16), ('symbols', C.c_uint8 * 256),
                ('count', C.c_uint16), ('present', C.c_uint8)]
class Jpeg(C.Structure):
    _fields_ = [('width', C.c_uint16), ('height', C.c_uint16), ('restart', C.c_uint16),
                ('quant', (C.c_uint8 * 64) * 2), ('present', C.c_uint8 * 2),
                ('huff', (Huffman * 2) * 2), ('q', C.c_uint8 * 3),
                ('dc', C.c_uint8 * 3), ('ac', C.c_uint8 * 3),
                ('offset', C.c_uint32), ('length', C.c_uint32), ('bytes', C.c_uint32)]
class Tables(C.Structure):
    _fields_ = [('quant', C.c_uint32 * 128), ('huff', C.c_uint32 * 512),
                ('size', C.c_uint32), ('restart', C.c_uint32), ('format', C.c_uint8)]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('native', type=pathlib.Path)
p.add_argument('reference', type=pathlib.Path)
p.add_argument('fixture', type=pathlib.Path)
p.add_argument('output', type=pathlib.Path)
p.add_argument('--executor-trace', type=pathlib.Path, help='Check only the new executor path; preparation supplies its reference state.')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
assert hashlib.sha256(a.reference.read_bytes()).hexdigest() == 'c6c0fff04dcddc633544831430727b445a3f1fe4873c881a16aeaed958513e65'
lib = C.CDLL(str(a.native.resolve()))
lib.a733_video_jpeg_parse.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(Jpeg)]
lib.a733_video_jpeg_prepare.argtypes = [C.POINTER(Jpeg), C.POINTER(Tables)]

def parse(data):
    j = Jpeg()
    C.memset(C.byref(j), 0xa5, C.sizeof(j))
    before = bytes(j)
    result = lib.a733_video_jpeg_parse(data, len(data), C.byref(j))
    if not result:
        assert bytes(j) == before, 'refusal modified output'
    return result, j

# Load only ELF segments, apply internal relocations and explicit host substitutes.
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(0, 0x20000)
u.mem_map(0x100000, 0x40000)
CTX, DEC, INPUT, SCRATCH, MMIO, STACK = 0x100000, 0x102000, 0x104000, 0x120000, 0x122000, 0x130000
STOP, MEMSET, PHYS = 0x13f000, 0x13f010, 0x13f020
GROUP, WAIT, ALIGN = 0x13f030, 0x13f040, 0x13f050
symbols = {}
with a.reference.open('rb') as f:
    elf = ELFFile(f)
    for seg in elf.iter_segments():
        if seg['p_type'] == 'PT_LOAD': u.mem_write(seg['p_vaddr'], seg.data())
    for sym in elf.get_section_by_name('.symtab').iter_symbols():
        if sym['st_value']: symbols[sym.name] = sym['st_value']
    for name in ('.rela.dyn', '.rela.plt'):
        section = elf.get_section_by_name(name)
        table = elf.get_section(section['sh_link'])
        for rel in section.iter_relocations():
            sym = table.get_symbol(rel['r_info_sym'])
            value = rel['r_addend'] if rel['r_info_type'] == 1027 else sym['st_value']
            if sym.name == 'memset': value = MEMSET
            if sym.name == 'FbmGetAlignValue': value = ALIGN
            u.mem_write(rel['r_offset'], struct.pack('<Q', value))

def put(off, value, fmt='I', base=CTX):
    u.mem_write(base + off, struct.pack('<' + fmt, value))

trace = []
def on_write(uc, access, address, size, value, _):
    if MMIO <= address < MMIO + 0x1000:
        trace.append([address - MMIO, size, value])

def hook(uc, address, size, _):
    if address == STOP: uc.emu_stop()
    elif address == MEMSET:
        dest, val, length = [uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2)]
        assert length <= 0x10000
        uc.mem_write(dest, bytes([val & 255]) * length)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
    elif address in (GROUP, WAIT, ALIGN):
        val = MMIO + 0x100 * uc.reg_read(UC_ARM64_REG_X1) if address == GROUP else (32 if address == ALIGN else 1)
        uc.reg_write(UC_ARM64_REG_X0, val)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
    elif address == PHYS:
        uc.reg_write(UC_ARM64_REG_X0, uc.reg_read(UC_ARM64_REG_X0) - INPUT + 0x81000000)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
u.hook_add(UC_HOOK_CODE, hook)
u.hook_add(UC_HOOK_MEM_WRITE, on_write)

def call(name, x0=CTX, x1=DEC):
    u.reg_write(UC_ARM64_REG_X0, x0)
    u.reg_write(UC_ARM64_REG_X1, x1)
    u.reg_write(UC_ARM64_REG_SP, STACK)
    u.reg_write(UC_ARM64_REG_X30, STOP)
    u.emu_start(symbols[name], STOP, count=100000)
    assert u.reg_read(UC_ARM64_REG_PC) == STOP, name + ' did not return'
    return u.reg_read(UC_ARM64_REG_X0)

def compare(data, label):
    ok, j = parse(data)
    assert ok, label + ': native parser refused fixture'
    t = Tables()
    assert lib.a733_video_jpeg_prepare(C.byref(j), C.byref(t)) == 1
    u.mem_write(CTX, bytes(0x2000))
    u.mem_write(DEC, bytes(0x2000))
    u.mem_write(INPUT, data)
    u.mem_write(SCRATCH, bytes(0x800))
    put(0x10, INPUT, 'Q'); put(0x18, len(data))
    put(0x818, SCRATCH, 'Q')
    put(0xe8, DEC + 0x200, 'Q', DEC)
    put(0x240, PHYS, 'Q', DEC)
    u.mem_write(symbols['uMjpegRegisterBaseAddr'], struct.pack('<Q', MMIO + 0x100))
    pos = 2
    while pos < len(data):
        assert data[pos] == 0xff
        marker = data[pos + 1]; pos += 2
        length = int.from_bytes(data[pos:pos+2], 'big')
        put(0x30, INPUT + pos, 'Q'); put(0x38, len(data) - pos)
        names = {0xdb:'JpegGetDqt', 0xc4:'JpegGetDht', 0xc0:'JpegGetSof', 0xdd:'JpegGetDri', 0xda:'JpegGetSos'}
        if marker in names: assert call(names[marker]) == 1, names[marker]
        pos += length
        if marker == 0xda: break
    assert call('SetJpegFormat') == 1
    trace.clear()
    assert call('InitJpegHw') == 1
    q = [v for off, size, v in trace if off == 0x180]
    h = [v for off, size, v in trace if off == 0x1e4]
    assert q == list(t.quant), label + ': quantization mismatch'
    assert h == list(t.huff), label + ': Huffman mismatch'
    for off, size, value in [(0x1b8,4,t.size), (0x1c0,4,t.restart), (0x11b,1,t.format)]:
        assert [v for o,s,v in trace if o == off and s == size] == [value]
    (a.output / (label + '-trace.json')).write_text(json.dumps(trace))
    (a.output / (label + '-native.bin')).write_bytes(bytes(t))
    return j

fixture = a.fixture.read_bytes()
j = compare(fixture, 'baseline')
if a.executor_trace:
    # Reference planar YU12, alignment 32, no scale or rotation. The library's
    # GetBufferSize and JpegMallocFrmBuffer paths define these context fields.
    stride = (j.width + 31) & ~31
    rows = (j.height + 31) & ~31
    cb = stride * rows // 4
    put(0x18,65536)
    put(0x7e0,0x82000000); put(0x7e4,0x83000000)
    put(0x7f0,0x0003332100021321,'Q')
    put(0x800,cb); put(0x804,cb); put(0x808,stride); put(0x80c,stride//2)
    put(0x834,2); put(0x838,1,'B')
    put(0x100,DEC+0x300,'Q',DEC); put(0x108,DEC,'Q',DEC)
    put(0x340,GROUP,'Q',DEC); put(0x328,WAIT,'Q',DEC)
    u.mem_write(MMIO,bytes(0x1000))
    trace.clear()
    assert call('InitJpegHw') == 1
    assert call('JpegHwDec',CTX,DEC) == 1
    reference = trace[:]
    raw=a.executor_trace.read_bytes()
    native=list(struct.iter_unpack('<III',raw))
    # Compare the complete quant/Huffman streams and byte strobes. Other
    # read-modify-write operations are combined in the native controller;
    # compare their final prepared register bytes before the trigger.
    def before_trigger(trace):
        return trace[:next(i for i,(off,size,value) in enumerate(trace) if off==0x118)]
    rr, nn=before_trigger(reference),before_trigger(native)
    for off in (0x180,0x1e4):
        assert [x[2] for x in rr if x[0]==off]==[x[2] for x in nn if x[0]==off]
    def image(trace):
        result=bytearray(0x1000)
        for off,size,value in trace:
            if off in (0x180,0x1e4):continue
            result[off:off+size]=value.to_bytes(size,'little')
        return result
    assert image(rr)==image(nn), [(hex(i),x,y) for i,(x,y) in enumerate(zip(image(rr),image(nn))) if x!=y]
    assert [x for x in native if x[0]==0x118]==[(0x118,1,14)]
    assert native[-2:]==[(0x114,4,0x180),(0x11c,4,1)]
    (a.output/'executor-reference.json').write_text(json.dumps(reference))
    (a.output/'executor-native.json').write_text(json.dumps(native))
    print('PASS: complete native JPEG programming matches pinned A733 instruction output before trigger; strobe and conservative completion handling pass. No hardware execution.')
    raise SystemExit(0)

# A second valid marker profile uses restart markers and swapped table selectors.
# Entropy here is synthetic: this tests preparation, not numerical decoding.
def segment(marker, payload):
    return bytes([255, marker]) + (len(payload)+2).to_bytes(2,'big') + payload
head = b'\xff\xd8'
head += segment(0xdb, bytes([0])+bytes(range(1,65))+bytes([1])+bytes(range(64,0,-1)))
for cls in range(2):
    for idx in range(2):
        counts = bytes([0, 1, 1] + [0]*13)
        vals = bytes([idx, 2+idx]) if not cls else bytes([0, 0x11+idx])
        head += segment(0xc4, bytes([cls*16+idx])+counts+vals)
head += segment(0xc0, bytes([8,0,32,0,48,3,1,0x22,1,2,0x11,0,3,0x11,0]))
head += segment(0xdd, bytes([0,2]))
head += segment(0xda, bytes([3,1,0x11,2,0,3,0,0,63,0]))
synthetic = head + b'\x12\xff\xd0\x34\xff\xd1\x56\xff\xd9'
compare(synthetic, 'restart-and-table-selection')
# Every truncation, framing errors and unsupported profile must refuse atomically.
for cut in range(len(fixture)): assert not parse(fixture[:cut])[0], ('truncation', cut)
for bad in [fixture+b'\x00', synthetic.replace(b'\xff\xd1',b'\xff\xd3'),
            synthetic.replace(b'\xff\xc0',b'\xff\xc2'),
            synthetic.replace(b'\xff\xd0',b'\xff\x00')]:
    assert not parse(bad)[0]
# Oversubscribed/all-one Huffman trees and zero quantization values.
for offset, value in [(head.index(b'\xff\xc4')+5, 2), (head.index(b'\xff\xdb')+5, 0)]:
    bad = bytearray(synthetic); bad[offset] = value
    assert not parse(bytes(bad))[0]
t = Tables(); C.memset(C.byref(t),0xa5,C.sizeof(t)); before=bytes(t)
j.quant[j.q[0]][0] = 0
assert not lib.a733_video_jpeg_prepare(C.byref(j),C.byref(t)) and bytes(t)==before
manifest = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in [a.native,a.reference,a.fixture,pathlib.Path(__file__)]}
(a.output/'inputs.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('PASS: native JPEG tables/geometry match A733 package instructions; malformed-input refusal passes. No hardware execution.')
