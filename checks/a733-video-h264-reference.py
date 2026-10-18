#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Trace selected pinned A733 library instructions; never emulate codec DMA.

The model supplies only caller metadata, an inert register window, a group
address callback and zero framebuffer offsets. Unknown services are refused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                UC_ARM64_REG_X3, UC_ARM64_REG_X30,
                                UC_ARM64_REG_SP, UC_ARM64_REG_PC)

OUTER, DEC, CTX, SPS, PPS = 0x100000, 0x104000, 0x110000, 0x122000, 0x123000
PIC, PICTURE, SHADOW = CTX+0xe78, 0x125000, 0x126000
MMIO, TOP, OPS, LEVEL = 0x130000, 0x131000, 0x132000, 0x133000
STOP, FBM, GROUP, MEMSET, STACK = 0x1ff000, 0x1ff010, 0x1ff020, 0x1ff030, 0x1fe000
PINS = {'libawh264.so': 'c977fe936a0bf1418f0a452c9769158b3ad2c8ff32aa972c1db4e21630f7fcc3',
        'libVE.so': '3af49a5eb24f6832518a858274fb206d60e490985bff02f5d5be36d053f65be4'}


class Reference:
    def __init__(self, path):
        assert hashlib.sha256(path.read_bytes()).hexdigest() == PINS[path.name]
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.u.mem_map(0, 0x40000); self.u.mem_map(0x100000, 0x100000)
        self.symbols, self.traps, self.trace, self.groups, self.offsets = {}, {}, [], [], []
        with path.open('rb') as f:
            elf = ELFFile(f)
            for segment in elf.iter_segments():
                if segment['p_type'] == 'PT_LOAD':
                    self.u.mem_write(segment['p_vaddr'], segment.data())
            for symbol in elf.get_section_by_name('.symtab').iter_symbols():
                if symbol['st_value']: self.symbols[symbol.name] = symbol['st_value']
            for name in ('.rela.dyn', '.rela.plt'):
                section = elf.get_section_by_name(name)
                table = elf.get_section(section['sh_link'])
                for relocation in section.iter_relocations():
                    symbol = table.get_symbol(relocation['r_info_sym'])
                    assert relocation['r_info_type'] in (257, 1025, 1026, 1027), relocation.entry
                    if relocation['r_info_type'] != 1027:
                        assert relocation['r_addend'] == 0, relocation.entry
                    else:
                        assert relocation['r_info_sym'] == 0, relocation.entry
                    value = relocation['r_addend'] if relocation['r_info_type'] == 1027 else symbol['st_value']
                    if symbol.name == 'GLOBAL_LOG_LEVEL': value = LEVEL
                    elif symbol.name == 'FbmGetBufferOffset': value = FBM
                    elif symbol.name == 'memset': value = MEMSET
                    elif not value and symbol.name:
                        value = 0x1f0000 + 16 * len(self.traps)
                        self.traps[value] = symbol.name
                    self.put(relocation['r_offset'], value, 'Q')
        self.put(LEVEL, 7)
        self.u.hook_add(UC_HOOK_CODE, self.code)
        self.u.hook_add(UC_HOOK_MEM_WRITE, self.write)

    def put(self, address, value, fmt='I'): self.u.mem_write(address, struct.pack('<'+fmt, value))
    def code(self, u, pc, size, data):
        assert pc not in self.traps, ('Unexpected service', self.traps.get(pc))
        if pc == STOP: u.emu_stop()
        elif pc in (FBM, GROUP, MEMSET):
            if pc == MEMSET:
                dest, value, count = [u.reg_read(reg) for reg in
                                     (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2)]
                assert value == 0 and count == 0x380 and STACK-0x1000 <= dest <= STACK-count
                u.mem_write(dest, bytes(count))
            if pc == GROUP:
                args = [u.reg_read(reg) for reg in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2)]
                assert args == [0, 0, 0], ('Unexpected group callback', args)
                self.groups.append(args)
            if pc == FBM:
                args = [u.reg_read(reg) for reg in (UC_ARM64_REG_X0, UC_ARM64_REG_X1)]
                assert args[0] == 0 and args[1] in (0, 1), ('Unexpected FBM offset', args)
                self.offsets.append(args)
            if pc != MEMSET: u.reg_write(UC_ARM64_REG_X0, 0 if pc == FBM else TOP)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_X30))
    def write(self, u, access, address, size, value, data):
        for base, label in ((MMIO, 'h264'), (TOP, 'top')):
            if base <= address < base+0x1000:
                assert size == 4
                self.trace.append([label, address-base, value & 0xffffffff])
    def call(self, name, *args):
        for reg, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3), args):
            self.u.reg_write(reg, value)
        self.u.reg_write(UC_ARM64_REG_SP, STACK); self.u.reg_write(UC_ARM64_REG_X30, STOP)
        self.u.emu_start(self.symbols[name], STOP, count=200000)
        assert self.u.reg_read(UC_ARM64_REG_PC) == STOP, name
        return self.u.reg_read(UC_ARM64_REG_X0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('libraries', type=Path); p.add_argument('fixture', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    metadata = json.loads((a.fixture/'result.json').read_text())
    results = []
    for version in (0x3332100021321, 0x3331000021320, 0x3052000012011):
        r = Reference(a.libraries/'libVE.so')
        for offset, value, fmt in ((0, 1, 'I'), (0x74, 3, 'I'), (0x48, TOP, 'Q'), (0x98, version, 'Q')):
            r.put(CTX+offset, value, fmt)
        assert r.call('VeGetGroupRegAddr', CTX, 2) == TOP+0x200
        results.append({'identity': hex(version), 'h264_offset': 0x200})
    r = Reference(a.libraries/'libawh264.so')
    for address, value in ((OUTER+0x190, DEC), (DEC+0x18, MMIO), (DEC+0x20, CTX),
                           (DEC+0x68, OUTER+0x68), (OUTER+0x100, OPS), (OPS+0x40, GROUP),
                           (DEC+0x1088, SHADOW), (CTX+0x100, SPS), (CTX+0x108, PPS),
                           (CTX+0xe390, PIC), (PIC+0xf0, PICTURE), (PIC+0xf8, PICTURE),
                           (PIC+0xd0, 0x10044000), (PIC+0xe0, 0x10044400),
                           (PICTURE+0x90, 0x10010000), (PICTURE+0x98, 0x10014000),
                           (CTX+0xe440, 0x10020000), (CTX+0xe448, 0x10048000)):
        r.put(address, value, 'Q')
    for address, value in ((SPS+0x10, 3), (SPS+0x14, 2), (SPS+0x58, 66),
                           (PPS+0x1c, 1), (PPS+0x20, 1), (CTX+0x18, 3),
                           (CTX+0x38, 1), (CTX+0x3c, 1), (CTX+0x44, 1),
                           (CTX+0x48, 2), (CTX+0x74, 26), (CTX+0x78, 1),
                           (CTX+0xe58, 1)):
        r.put(address, value)
    for address in (SPS+3, SPS+4): r.put(address, 1, 'B')
    r.put(CTX+0x1e, 3, 'B')
    r.put(CTX+0x20, 2, 'B')  # Parser's internal H264_I_TYPE, distinct raw slice field.
    # Actual primary-only branch in H264ConfigureFrameInfoRegister. No scaler.
    r.put(CTX+0xe6e8, 1, 'B'); r.put(CTX+0xe6e4, 2)
    for offset, value in ((0xe518, 512), (0xe51c, 64), (0xe520, 32)):
        r.put(CTX+offset, value)
    # VBV pointers are model metadata, not mapped DMA or actual codec behavior.
    r.put(CTX+0xe3a8, 0x10001000, 'Q'); r.put(CTX+0xe3b0, 0x10001000, 'Q')
    r.put(CTX+0xe3b8, 0x10001fff, 'Q')
    # H264SetVbvParams stores (physical & 0x0ffffff0) + (physical >> 28).
    r.put(CTX+0xe3d8, 0x1001); r.put(CTX+0xe3dc, 0x10001fff); r.put(CTX+0xe3e0, 7)
    r.call('H264InitFuncCtrlRegister', OUTER)
    r.call('H264ConfigureAvcRegister', OUTER, CTX, 1, metadata['files']['slice.bin']['bytes']*8)
    r.call('H264ConfigureSliceRegister', OUTER, CTX, 0)
    program = list(r.trace)
    (a.output/'trace.json').write_text(json.dumps({'pins': PINS, 'routes': results,
        'slice_program': program, 'group_calls': r.groups, 'fbm_offset_calls': r.offsets,
        'limitations': 'Inert MMIO; no entropy engine, DMA, producer drain or physical identity emulated.'}, indent=2)+'\n')
    last = {offset: value for group, offset, value in program if group == 'h264'}
    expected = {0: 0xd0201, 4: 0, 8: 0x1220, 0xc: 0x100, 0x1c: 0x100001a,
                0x20: 0x407, 0x24: 8, 0x2c: 0, 0x30: 0x70001001,
                0x34: 0, 0x38: metadata['files']['slice.bin']['bytes']*8,
                0x3c: 0x10001fff, 0x4c: 0, 0x50: 0x10020000, 0x54: 0x10048000, 0x60: 0}
    for offset, value in expected.items(): assert last[offset] == value, (hex(offset), last[offset], value)
    top = {offset: value for group, offset, value in program if group == 'top'}
    assert top[0xec] & 0x70 == 0x20 and top[0xc4] == 512 and top[0xc8] == 0x200040
    assert last[0x20] & 0x300 == 0
    assert not any(group == 'h264' and offset in (0x40, 0x44, 0x48) for group, offset, _ in program)
    assert r.groups == [[0, 0, 0]] and r.offsets == [[0, 1], [0, 0]]
    print('PASS: pinned A733 instruction routing, selected register and planar format fields.')


if __name__ == '__main__': main()
