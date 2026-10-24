#!/usr/bin/env python3
"""Focused actual-ELF consumer checks; device transitions are explicit models."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP

EXPECTED = 36 << 48 | 56 << 32 | 104 << 16 | 183
NAMES = ['bb_power', 'a733_gpu_platform', 'a733_gpu_power', 'a733_pmic_consumer',
         'a733_power_domains', 'a733_power_domain', 'a733_power_domain_row']


def run(image, case):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0x41000000, 0x1200000)
    u.mem_map(0x1800000, 0x1000)
    for address, data in image.parts:
        u.mem_write(address, data)
    u.reg_write(UC_ARM64_REG_SP, 0x421f0000)
    L = image.layouts
    gpu, bb = image.address('gpu'), image.address('bb_power')
    power = gpu + L['a733_gpu_platform']['power']
    row = power + L['a733_gpu_power']['row']
    consumer = gpu + L['a733_gpu_platform']['pmic']
    domains = bb + L['bb_power']['domains']
    domain5 = domains + L['a733_power_domains']['domain'] + 2 * 32
    deadline = 10000
    state = {'case': case, 'calls': [], 'gpu_reads': [], 'gpu_writes': [], 'clock_reads': 0,
             'time': 100, 'messages': [], 'exit_status': None, 'failure': None}

    def put(address, value, size=4):
        u.mem_write(address, value.to_bytes(size, 'little'))

    def field(base, typ, name, value, size=4):
        put(base + L[typ][name], value, size)

    def ret(value=1):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def text(address):
        value = bytearray()
        while (byte := u.mem_read(address + len(value), 1)[0]):
            value.append(byte)
        return value.decode()

    field(bb, 'bb_power', 'deadline', deadline, 8)
    field(row, 'a733_power_domain_row', 'domains', domains, 8)
    field(row, 'a733_power_domain_row', 'domain', 5)
    field(consumer, 'a733_pmic_consumer', 'rail', 32, 8)
    field(domain5, 'a733_power_domain', 'id', 5)
    raw = EXPECTED if case != 'unexpected-id' else EXPECTED + 1
    if case == 'legacy-id':
        raw = 36 << 16 | 56 << 8
        put(0x1800018, 104 << 8 | 183)
    put(0x1800020, raw, 8)

    def call(name):
        state['calls'].append(name)
        if name == 'bb_power_now':
            state['clock_reads'] += 1
            state['time'] += 1
            if case == 'read-crosses-deadline' and state['gpu_reads']:
                state['time'] = deadline
            if case == 'clock-regresses' and state['gpu_reads']:
                state['time'] = 99
            ret(state['time'])
        elif name == 'a733_gpu_platform_poll':
            if case == 'on-fault':
                ret((1 << 64) - 1)
                return
            field(gpu, 'a733_gpu_platform', 'phase', 3)
            field(gpu, 'a733_gpu_platform', 'token', 17, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 1)
            ret()
        elif name == 'aarch64_device_translate':
            address = u.reg_read(UC_ARM64_REG_X0)
            assert address in (0x1800020, 0x1800018)
            assert u.reg_read(UC_ARM64_REG_X1) == 0
            # The compiled mapping predicate consumes this native translation
            # result; wrong physical address must refuse before the MMIO read.
            target = u.reg_read(UC_ARM64_REG_X2)
            u.mem_write(target, struct.pack('<QII', address + (4096 if case == 'bad-mapping' else 0), 0, 0))
            ret()
        elif name == 'a733_gpu_platform_off_poll':
            if case == 'off-fault':
                ret((1 << 64) - 1)
                return
            field(gpu, 'a733_gpu_platform', 'phase', 6)
            field(gpu, 'a733_gpu_platform', 'token', 17 if case == 'retained-token' else 0, 8)
            field(gpu, 'a733_gpu_platform', 'delegated', 0)
            field(power, 'a733_gpu_power', 'phase', 4)
            field(row, 'a733_power_domain_row', 'state', 3)
            field(row, 'a733_power_domain_row', 'counted', case == 'counted-row')
            field(domain5, 'a733_power_domain', 'state', 1)
            ret()
        elif name == 'bb_power_park':
            field(bb, 'bb_power', 'parked', 0 if case == 'unparked' else 1)
            ret()
        elif name == 'bb_power_fail':
            state['failure'] = text(u.reg_read(UC_ARM64_REG_X0))
            u.emu_stop()
        elif name == 'platform_puts':
            state['messages'].append(text(u.reg_read(UC_ARM64_REG_X0)))
            ret()
        elif name == 'platform_exit':
            state['exit_status'] = u.reg_read(UC_ARM64_REG_X0)
            u.emu_stop()
        else:
            ret()

    modeled = ['bb_power_wait_ready', 'bb_power_now', 'bb_power_pause',
               'a733_gpu_platform_begin', 'a733_gpu_platform_poll', 'aarch64_device_translate',
               'a733_gpu_platform_off_begin', 'a733_gpu_platform_off_poll', 'bb_power_park',
               'a733_power_service_idle', 'bb_power_fail', 'platform_puts', 'platform_exit']
    hooks = {image.address(name): name for name in modeled}
    u.hook_add(UC_HOOK_CODE, lambda uc, address, size, arg: call(hooks[address]) if address in hooks else None)

    def read(uc, access, address, size, value, arg):
        state['gpu_reads'].append([hex(address), size])
        assert (address, size) in [(0x1800020, 8), (0x1800020, 4), (0x1800018, 4)]

    def write(uc, access, address, size, value, arg):
        state['gpu_writes'].append([address, size, value])
        raise AssertionError('GPU write in never-booted identity consumer')

    u.hook_add(UC_HOOK_MEM_READ, read, begin=0x1800000, end=0x1800fff)
    u.hook_add(UC_HOOK_MEM_WRITE, write, begin=0x1800000, end=0x1800fff)
    u.emu_start(image.address('gpu_power_id_client'), 0, count=100000)
    output = ''.join(state['messages'])
    if case in ['normal', 'legacy-id', 'unexpected-id']:
        assert state['exit_status'] == 0 and state['failure'] is None
        assert 'COMPLETE GPU power identity: OFF and provider parked' in output
        assert f'matches={0 if case == "unexpected-id" else 1}' in output
        assert state['calls'].index('a733_gpu_platform_off_poll') < state['calls'].index('bb_power_park') < state['calls'].index('platform_puts')
        assert state['gpu_reads'] == ([['0x1800020', 8], ['0x1800020', 4], ['0x1800018', 4]] if case == 'legacy-id' else [['0x1800020', 8]])
    else:
        reasons = {'on-fault': 'GPU ON poll', 'bad-mapping': 'GPU identity mapping',
                   'read-crosses-deadline': 'GPU identity deadline/clock', 'clock-regresses': 'GPU identity deadline/clock',
                   'off-fault': 'GPU OFF poll', 'retained-token': 'GPU OFF ownership proof',
                   'counted-row': 'GPU OFF ownership proof', 'unparked': 'GPU provider park proof'}
        assert state['failure'] == reasons[case], state
        assert state['exit_status'] is None and 'COMPLETE' not in output
        if case in ['on-fault', 'bad-mapping']:
            assert not state['gpu_reads']
        if case in ['on-fault', 'bad-mapping', 'read-crosses-deadline', 'clock-regresses']:
            assert 'a733_gpu_platform_off_begin' not in state['calls']
    assert not state['gpu_writes']
    return state


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    image = harness.Image(a.image, layout_names=NAMES)
    cases = ['normal', 'legacy-id', 'unexpected-id', 'on-fault', 'bad-mapping',
             'read-crosses-deadline', 'clock-regresses', 'off-fault', 'retained-token', 'counted-row', 'unparked']
    result = {'physical_access': False, 'scope': 'actual ELF client and ID access; modeled provider/power transitions',
              'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
              'cases': [run(image, case) for case in cases]}
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(cases)} focused client cases passed; no physical access')


if __name__ == '__main__':
    main()
