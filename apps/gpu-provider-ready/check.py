#!/usr/bin/env python3
"""Execute frozen constructors and readiness proofs with modeled architecture/service progress."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
import unicorn.arm64_const as A

spec = importlib.util.spec_from_file_location('gpu_identity_build', ROOT / 'apps/gpu-power-id/build.py')
old_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(old_build)
NAMES = ['bb_power', 'a733_gpu_platform', 'a733_gpu_power', 'a733_power_domain_row',
         'a733_power_domains', 'a733_rail_leases', 'a733_pmic']
REGS = [getattr(A, f'UC_ARM64_REG_X{i}') for i in range(31)]


def machine(image):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    # Any device access fails: only private/result RAM is mapped.
    u.mem_map(0x41000000, 0x1200000)
    for address, data in image.parts:
        u.mem_write(address, data)
    u.reg_write(A.UC_ARM64_REG_SP, 0x421f0000)
    return u


def returning(u, value=1):
    u.reg_write(A.UC_ARM64_REG_X0, value)
    u.reg_write(A.UC_ARM64_REG_PC, u.reg_read(A.UC_ARM64_REG_LR))


def string(u, address):
    return bytes(u.mem_read(address, 200)).split(b'\0')[0].decode()


def constructors(image):
    u = machine(image)
    u.mem_write(0x42000040, struct.pack('<Q', 24000000))
    record = {'boundary': None, 'instructions': 0, 'translations': [], 'time': 24001000}
    hooks = {image.address(n): n for n in ['platform_init', 'aarch64_device_translate', 'vTaskStartScheduler', 'platform_panic']}

    def step(uc, address, size, _):
        record['instructions'] += 1
        name = hooks.get(address)
        if name == 'platform_init':
            returning(uc)
            return
        if name == 'aarch64_device_translate':
            value, write, out = [uc.reg_read(r) for r in REGS[:3]]
            assert value in [0x07061000, 0x07064000, 0x07065000, 0x02002000, 0x07010000]
            record['translations'].append([hex(value), write])
            uc.mem_write(out, struct.pack('<QII', value, 0, 2))
            returning(uc)
            return
        if name in ['vTaskStartScheduler', 'platform_panic']:
            record['boundary'] = name
            if name == 'platform_panic':
                record['panic'] = string(uc, uc.reg_read(REGS[0]))
            uc.emu_stop()
            return
        word = int.from_bytes(uc.mem_read(address, 4), 'little')
        register = word & ~31
        values = {0xd5384240: 4, 0xd53be000: 24000000, 0xd53b4220: 0x3c0, 0xd53800a0: 0, 0xd5384600: 0xb0}
        if register == 0xd53be020:
            record['time'] += 24
            values[register] = record['time']
        if register in values:
            if word & 31 != 31:
                uc.reg_write(REGS[word & 31], values[register])
            uc.reg_write(A.UC_ARM64_REG_PC, address + 4)

    u.hook_add(UC_HOOK_CODE, step)
    u.emu_start(image.address('kernel_main'), 0, count=100000)
    assert record['boundary'] == 'vTaskStartScheduler', record
    return bytes(u.mem_read(0x41000000, 0x1200000)), record


def client(image, layouts, template, case):
    u = machine(image)
    u.mem_write(0x41000000, template)
    L = {name: value[1] for name, value in layouts.items()}
    bb, gpu = image.address('bb_power'), image.address('gpu')
    rails = bb + L['bb_power']['rails']
    row = gpu + L['a733_gpu_platform']['power'] + L['a733_gpu_power']['row']

    def put(address, value, size=4):
        u.mem_write(address, value.to_bytes(size, 'little'))

    authority = rails + L['a733_rail_leases']['authority']
    put(authority, 15 & ~(4 if case == 'dcdc2-unverified' else 8 if case == 'dcdc4-unverified' else 0))
    pmic = bb + L['bb_power']['pmic']
    put(pmic + L['a733_pmic']['active'], 8)
    # This immutable handle may remain after the ledger entry was released.
    domains = bb + L['bb_power']['domains']
    put(domains + L['a733_power_domains']['supply_token'], 32, 8)
    if case == 'request-used':
        put(row + L['a733_power_domain_row']['request'], 3)
    if case == 'token-used':
        put(gpu + L['a733_gpu_platform']['token'], 7, 8)
    if case == 'domain-access':
        put(bb + L['bb_power']['accessed'], 1)
    record = {'case': case, 'failure': None, 'exit_status': None, 'messages': [], 'clock_reads': 0, 'park_called': False}
    hooks = {image.address(n): n for n in ['bb_power_now', 'bb_power_park', 'a733_power_service_idle',
                                         'bb_power_fail', 'platform_puts', 'platform_exit']}

    def step(uc, address, size, _):
        name = hooks.get(address)
        if name == 'bb_power_now':
            record['clock_reads'] += 1
            now = 1000001 + record['clock_reads']
            if record['clock_reads'] == 2:
                if case == 'late':
                    now = 2500000
                if case == 'backward':
                    now = 1
            returning(uc, now)
        elif name == 'bb_power_park':
            record['park_called'] = True
            put(bb + L['bb_power']['parked'], case != 'not-parked')
            if case == 'rail-retained':
                put(rails + L['a733_rail_leases']['slots'], 32, 8)
            if case == 'late-request':
                put(row + L['a733_power_domain_row']['request'], 3)
            returning(uc)
        elif name == 'a733_power_service_idle':
            returning(uc)
        elif name == 'bb_power_fail':
            record['failure'] = string(uc, uc.reg_read(REGS[0]))
            uc.emu_stop()
        elif name == 'platform_puts':
            record['messages'].append(string(uc, uc.reg_read(REGS[0])))
            returning(uc)
        elif name == 'platform_exit':
            record['exit_status'] = uc.reg_read(REGS[0])
            uc.emu_stop()

    u.hook_add(UC_HOOK_CODE, step)
    u.emu_start(image.address('gpu_provider_ready_client'), 0, count=50000)
    if case == 'normal':
        assert record['exit_status'] == 0 and record['failure'] is None
        assert record['park_called'] and record['clock_reads'] == 2
        assert 'COMPLETE GPU provider readiness:' in ''.join(record['messages'])
    else:
        expected = {'dcdc2-unverified': 'supplies unverified', 'dcdc4-unverified': 'supplies unverified',
                    'request-used': 'ownership', 'token-used': 'ownership', 'domain-access': 'ownership',
                    'rail-retained': 'rail retained', 'late-request': 'ownership', 'not-parked': 'provider not parked',
                    'late': 'deadline', 'backward': 'deadline'}
        assert record['failure'] == 'GPU readiness ' + expected[case], record
        assert record['exit_status'] is None and not record['messages']
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image = harness.Image(args.image, layout_names=[])
    layouts = old_build.shared_layouts(args.image, NAMES)
    template, initialized = constructors(image)
    cases = ['normal', 'dcdc2-unverified', 'dcdc4-unverified', 'request-used', 'token-used',
             'domain-access', 'rail-retained', 'late-request', 'not-parked', 'late', 'backward']
    result = {'physical_access': False, 'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
              'scope': 'actual constructors/client; architecture, service progress and park modeled; device MMIO unmapped',
              'constructor': initialized, 'cases': [client(image, layouts, template, case) for case in cases]}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('Actual constructor chain and eleven readiness cases passed; no device MMIO mapped')


if __name__ == '__main__':
    main()
