#!/usr/bin/env python3
"""Execute application-table publication and the forced-walk interval in linked OS images."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
sys.path.insert(0, str(ROOT / 'tools'))
import a733_tcr
GATE_RUNS = [['--image', '{elf:ret-walks-uncached}', '--image', '{elf:ret-walks-cached}', '--output', '{output}']]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def machine(image):
    u = harness.machine()
    u.mem_map(0x40000000, 0x08000000)
    for address, data in image.parts:
        u.mem_write(address, data)
    u.reg_write(UC_ARM64_REG_SP, 0x47e00000)
    u.reg_write(UC_ARM64_REG_LR, 0x47fff000)
    return u


def publication(image, tcr, variant='normal'):
    u = machine(image)
    arena = image.address('arena_pages', unit='apps/os/large.c')
    active = image.address('os_space_active')
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    dirty, cleaned = set(), set()
    trace = {'stores': 0, 'maintenance': 0, 'published': False, 'barrier': None}
    hooks = {image.address(name): name for name in (
        'os_kernel_l1_head', 'os_kernel_l2_slot1', 'app_asid_pool_init', 'app_asid_alloc',
        'umem_cell_bytes', 'umem_init', 'umem_alloc', 'umem_for_each', 'os_npu_pool_prepare', 'board_fail_note')}

    def ret(value=0):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def memory(cpu, access, address, size, value, cookie):
        if arena <= address < arena + 13 * 4096:
            assert not trace['published'], 'table store after publication'
            for a in range(address & ~63, (address + size + 63) & ~63, 64):
                dirty.add(a)
                cleaned.discard(a)
            trace['stores'] += size
        if address == active and value:
            assert trace['stores'] >= 8192 and trace['published'], 'table stores lack a completion barrier'
            u.emu_stop()

    def code(cpu, address, size, cookie):
        if address == 0x47fff000:
            raise AssertionError('space preparation returned without publishing the space')
        name = hooks.get(address)
        arg = u.reg_read(UC_ARM64_REG_X0)
        if name:
            if name == 'os_kernel_l1_head':
                data = [0] * 16
                data[1] = 0x421f1003
                data[2] = 0x80000000 | (3 << 53) | (1 << 11) | (1 << 10) | (3 << 8) | 5
                u.mem_write(arg, struct.pack('<16Q', *data))
            elif name == 'os_kernel_l2_slot1':
                u.mem_write(arg, bytes(4096))
            elif name == 'umem_for_each':
                # One partial upper slot forces real root and child stores.
                u.mem_write(image.address('os_large_entry_storage'), struct.pack('<QQHB5x', 0x80000000, 0x200000, 1, 3))
                u.mem_write(u.reg_read(UC_ARM64_REG_X0 + 3), struct.pack('<I', 1))
            elif name == 'board_fail_note':
                raise AssertionError('space preparation refused')
            ret(1 if name in ('app_asid_alloc', 'os_npu_pool_prepare', 'umem_alloc', 'umem_for_each') else
                128 if name == 'umem_cell_bytes' else 0)
            return
        op = next(decoder.disasm(bytes(u.mem_read(address, 4)), address))
        args = op.op_str.lower().split(', ')
        if op.mnemonic == 'mrs':
            values = {'ttbr0_el1': 0x421f0000, 'tcr_el1': tcr, 'id_aa64mmfr0_el1': 0x101122}
            assert args[1] in values, args
            u.reg_write(UC_ARM64_REG_X0 + int(args[0][1:]), values[args[1]])
        elif op.mnemonic == 'dc':
            assert args[0] == 'civac', args
            trace['maintenance'] += 1
            if variant != 'missing-clean':
                cleaned.add(u.reg_read(UC_ARM64_REG_X0 + int(args[1][1:])) & ~63)
        elif op.mnemonic == 'dsb':
            if dirty and variant != 'missing-barrier':
                assert args[0] in ('ish', 'sy'), args
                if not tcr & 0xf00:
                    assert dirty <= cleaned, 'uncached walker sees unclean descriptors'
                trace['published'] = True
                trace['barrier'] = args[0]
        else:
            return
        u.reg_write(UC_ARM64_REG_PC, address + 4)

    u.hook_add(UC_HOOK_MEM_WRITE, memory)
    u.hook_add(UC_HOOK_CODE, code)
    u.emu_start(image.address('os_large_prepare'), 0, count=1000000)
    assert trace['published']
    if tcr & 0xf00:
        assert trace['maintenance'] == 0 and trace['barrier'] == 'ish', trace
    else:
        assert trace['maintenance'] == 13 * 4096 // 64 and trace['barrier'] == 'sy', trace
    return trace


def loop(image):
    u = machine(image)
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    counts = dict(cycles=0, ticks=0, data=0, instruction=0, bus=0)
    loads = []
    result = 0x47f00000

    def code(cpu, address, size, cookie):
        if address == 0x47fff000:
            u.emu_stop()
            return
        counts['cycles'] += 100
        counts['ticks'] += 1
        op = next(decoder.disasm(bytes(u.mem_read(address, 4)), address))
        if op.mnemonic == 'tlbi':
            assert op.op_str == 'vmalle1'
            counts['instruction'] += 1
            u.reg_write(UC_ARM64_REG_PC, address + 4)
        elif op.mnemonic == 'mrs':
            dest, reg = op.op_str.split(', ')
            key = {'pmccntr_el0': 'cycles', 'cntpct_el0': 'ticks', 'pmevcntr4_el0': 'data',
                   'pmevcntr5_el0': 'instruction', 'pmevcntr3_el0': 'bus'}[reg]
            u.reg_write(UC_ARM64_REG_X0 + int(dest[1:]), counts[key])
            u.reg_write(UC_ARM64_REG_PC, address + 4)

    def read(cpu, access, address, size, value, cookie):
        assert 0x40000000 <= address < 0x48000000 and not 0x42000000 <= address < 0x42200000
        assert size == 8
        counts['data'] += 1
        counts['bus'] += 1
        loads.append(address)

    u.hook_add(UC_HOOK_CODE, code)
    u.hook_add(UC_HOOK_MEM_READ, read)
    u.reg_write(UC_ARM64_REG_X0, result)
    u.reg_write(UC_ARM64_REG_X0 + 1, 1024)
    u.emu_start(image.address('table_walk_loop'), 0, count=100000)
    values = struct.unpack('<6Q', bytes(u.mem_read(result, 48)))
    assert len(loads) == 1024 and values[2:5] == (1024, 1024, 1024), values
    assert len({(a - 0x40000000) >> 21 for a in loads}) == 63
    return dict(iterations=len(loads), mapped_blocks=63, measured_data_walks=values[2])


def decoder_cases(tcr, out):
    host = load('table_walk_host', ROOT / 'apps/table-walks/host.py')
    frozen = out / f'decoder-{tcr:x}.py'
    host.freeze(frozen)
    host = load('frozen_table_walk_host', frozen)
    clock = b'CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=900 transitions=3 dsu_mhz=1274 core=6 pattern=0'
    def payload(value, cycles=1024 * 700):
        return (host.HEAD.pack(b'SSTWALK1', value, 0x414fd0b1, 6, 1024, 24000000) +
                clock.ljust(256, b'\0') * 2 + host.SAMPLE.pack(cycles, round(cycles * 24 / 2002), 1024, 1024, 0, 0) * 6)
    a = host.decode(payload(a733_tcr.UNCACHED_WALK))
    b = host.decode(payload(a733_tcr.CACHED_WALK, 1024 * 400))
    assert host.compare(a, b)['gain']
    assert not host.compare(a, host.decode(payload(a733_tcr.CACHED_WALK)))['gain']
    assert not host.compare(a, host.decode(payload(a733_tcr.CACHED_WALK, 1024 * 800)))['gain']
    for offset, size, reason in ((8, 8, 'translation-control profile'), (24, 8, 'CPU'), (host.HEAD.size, 1, 'operating points'),
                                (host.HEAD.size + 512 + 16, 8, 'walk counts')):
        bad = bytearray(payload(tcr))
        bad[offset:offset + size] = bytes(size)
        harness.refused(lambda: host.decode(bytes(bad)), reason, ValueError)
    harness.refused(lambda: host.decode(payload(tcr)[:-1]), 'length', ValueError)
    bad = bytearray(payload(tcr))
    struct.pack_into('<Q', bad, 8, tcr ^ (1 << 12))
    harness.refused(lambda: host.decode(bytes(bad)), 'translation-control profile', ValueError)
    return 10


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, action='append', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    reports = []
    for path in args.image:
        image, tcr = harness.Image(path, layout_names=()), a733_tcr.for_image(path)
        report = dict(image=str(path), tcr=hex(tcr), publication=publication(image, tcr), loop=loop(image))
        harness.refused(lambda: publication(image, tcr, 'missing-barrier'), 'completion barrier', AssertionError)
        if not tcr & 0xf00:
            harness.refused(lambda: publication(image, tcr, 'missing-clean'), 'unclean descriptors', AssertionError)
        report['decoder_cases'] = decoder_cases(tcr, args.output)
        reports.append(report)
    harness.save(args.output / 'result.json', dict(passed=True, physical_access=False, images=reports,
        scope='Linked instruction execution; cache visibility and PMU events are modeled. No physical latency or coherence claim.'))
    print('PASS linked application publication, barrier omissions, forced-walk addresses and decoder acceptance')


if __name__ == '__main__':
    main()
