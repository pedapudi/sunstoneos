#!/usr/bin/env python3
"""Execute the actual finite observer with modeled RTOS calls, counter and I2C effects."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP,
                                 UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE, UC_ARM64_REG_CPACR_EL1)
import harness

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append',
                        choices=('code100', 'disabled', 'address-nack', 'active-deadline'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = harness.Image(args.image, ('a733_pmic', 'a733_rail_token', 'a733_pmic_request'))
    unit = '/apps/power-supply-observe/main.c'
    spec = importlib.util.spec_from_file_location('supply_decode', ROOT / 'apps/power-supply-observe/decode.py')
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    results = []
    for variant in (args.case or ('code100', 'disabled', 'address-nack', 'active-deadline')):
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(0x02000000, 0x06000000)
        cpu.mem_map(0x40000000, 0x04000000)
        for address, data in image.parts:
            if data:
                cpu.mem_write(address, data)
        cpu.reg_write(UC_ARM64_REG_PSTATE, 5)
        cpu.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
        for name in ('ullCriticalNesting', 'ullPortInterruptNesting'):
            cpu.mem_write(image.address(name), bytes(8))
        state = {'time': 1000000, 'exit': None, 'phase': 'idle', 'data': None, 'reg': None,
                 'transactions': [], 'writes': [], 'holders': {}, 'injections': 0,
                 'reaped': 0, 'released': 0, 'task': None}
        regs = {0x07090160: 0x983fb0f7, 0x02002000: 0xf8675f00,
                0x0701000c: 0, 0x07010010: 0x04000000, 0x0701019c: 0x10001,
                0x07025000: 0x22, 0x07025014: 0x11, 0x07025024: 0x5,
                0x0708300c: 0x40, 0x07083010: 0xf8, 0x07083014: 0x28, 0x07083020: 0x30,
                0x07083200: 0x00f81000, 0x07083210: 0x80c0, 0x07083218: 0x100010,
                0x0340011c: 0, 0x0340021c: 0, 0x0340031c: 0}
        values = {0xff: 0, 0x0e: 3, 0x0f: 0, 0x10: 0xff, 0x11: 1, 0x21: 0xff,
                  0x12: 23, 0x19: 0xe4, 0x1a: 0xe4, 0x2e: 13, 0x2f: 13, 0x31: 13}
        if variant == 'disabled':
            values.update({0x10: 0, 0x11: 0, 0x21: 0})
        for address, value in regs.items():
            cpu.mem_write(address, struct.pack('<I', value))
        names = ('platform_init', 'xTaskCreateStatic', 'vTaskStartScheduler', 'platform_panic',
                 'xTaskGetCurrentTaskHandle', 'xTaskGetSchedulerState', 'xQueueCreateMutexStatic',
                 'xQueueGetMutexHolder', 'xQueueSemaphoreTake', 'xQueueGenericSend', 'vTaskDelay',
                 'platform_exit', 'a733_pmic_reap', 'a733_rail_release')
        hooks = {image.address(name): name for name in names}
        instructions = harness.instruction_sites(image.parts,
            lambda word: word & 0xffffffe0 in (0xd5384240, 0xd53be000, 0xd53be020))

        def read(address, size=8):
            return int.from_bytes(cpu.mem_read(address, size), 'little')

        def write(address, value, size=8):
            cpu.mem_write(address, int(value).to_bytes(size, 'little'))

        def ret(value=0):
            cpu.reg_write(UC_ARM64_REG_X0, value)
            cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))

        def code(uc, address, size, cookie):
            word = read(address, 4)
            kind = word & 0xffffffe0
            if kind in (0xd5384240, 0xd53be000, 0xd53be020):
                value = 4 if kind == 0xd5384240 else 24000000 if kind == 0xd53be000 else state['time'] * 24
                if kind == 0xd53be020:
                    state['time'] += 1
                cpu.reg_write(UC_ARM64_REG_X0 + (word & 31), value)
                cpu.reg_write(UC_ARM64_REG_PC, address + 4)
                return
            name = hooks[address]
            a = [cpu.reg_read(UC_ARM64_REG_X0 + i) for i in range(8)]
            if name == 'platform_init':
                ret()
            elif name == 'xTaskCreateStatic':
                assert a[0] == image.address('probe', unit=unit) and a[2:5] == [4096, 0, 3]
                state['task'] = a[0]
                ret(0x43000000)
            elif name == 'vTaskStartScheduler':
                cpu.reg_write(UC_ARM64_REG_PC, state['task'])
                cpu.reg_write(UC_ARM64_REG_X0, 0)
            elif name == 'xTaskGetCurrentTaskHandle':
                ret(0x43000000)
            elif name == 'xTaskGetSchedulerState':
                ret(2)
            elif name == 'xQueueCreateMutexStatic':
                queue = 0x43010000 + len(state['holders']) * 0x100
                state['holders'][queue] = 0
                ret(queue)
            elif name == 'xQueueGetMutexHolder':
                ret(state['holders'][a[0]])
            elif name == 'xQueueSemaphoreTake':
                assert not state['holders'][a[0]]
                state['holders'][a[0]] = 0x43000000
                ret(1)
            elif name == 'xQueueGenericSend':
                assert state['holders'][a[0]] == 0x43000000
                state['holders'][a[0]] = 0
                ret(1)
            elif name == 'vTaskDelay':
                state['time'] += 1000 * a[0]
                ret()
            elif name == 'a733_pmic_reap':
                state['reaped'] += 1  # Execute the actual function after observing the call.
            elif name == 'a733_rail_release':
                assert state['reaped'] == 6
                state['released'] += 1
            elif name == 'platform_exit':
                state['exit'] = a[0]
                cpu.emu_stop()
            else:
                raise AssertionError('unexpected panic')

        def mmio_write(uc, access, address, size, value, cookie):
            assert size == 4 and address in (0x07025000, 0x07083008, 0x0708300c), hex(address)
            assert any(state['holders'].values())
            state['writes'].append([address, value])
            if address == 0x07083008:
                state['data'] = value
            if address != 0x0708300c:
                return
            phase = state['phase']
            if value == 0x68:
                status = 8 if phase == 'idle' else 0x10
                state['phase'] = 'start' if phase == 'idle' else 'restart'
            elif value == 0x58:
                assert phase == 'received'
                status = 0xf8
                state['phase'] = 'idle'
            elif value == 0x48:
                if phase == 'start':
                    assert state['data'] == 0x6c
                    status = 0x18
                    if variant == 'address-nack' and len(state['transactions']) == 18:
                        status = 0x20
                        state['injections'] += 1
                    state['phase'] = 'address'
                elif phase == 'address':
                    state['reg'] = state['data']
                    state['transactions'].append(state['reg'])
                    status, state['phase'] = 0x28, 'register'
                elif phase == 'restart':
                    assert state['data'] == 0x6d
                    status, state['phase'] = 0x40, 'read-address'
                elif phase == 'read-address':
                    status, state['phase'] = 0x58, 'received'
                    write(0x07083008, values[state['reg']], 4)
                    if variant == 'active-deadline' and state['reg'] == 0x2e:
                        state['time'] = 2500001
                        state['injections'] += 1
                else:
                    raise AssertionError('regulator data write or invalid I2C phase: ' + phase)
            else:
                raise AssertionError(hex(value))
            state['control'] = 0x40 if status == 0xf8 else 0x48
            write(0x07083010, status, 4)

        def mmio_read(uc, access, address, size, value, cookie):
            assert size == 4
            if address == 0x0708300c and 'control' in state:
                write(address, state['control'], 4)

        harness.hook_addresses(cpu, set(hooks) | set(instructions), code)
        cpu.hook_add(UC_HOOK_MEM_WRITE, mmio_write, begin=0x02000000, end=0x07ffffff)
        cpu.hook_add(UC_HOOK_MEM_READ, mmio_read, begin=0x02000000, end=0x07ffffff)
        cpu.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
        cpu.reg_write(UC_ARM64_REG_LR, 0x43fff000)
        cpu.emu_start(image.address('kernel_main'), 0, count=12000000)
        log = bytes(cpu.mem_read(0x42000100, read(0x42000018))).decode()
        (out / (variant + '.log')).write_text(log)
        layout = image.layouts['a733_pmic']
        provider = image.address('provider', unit=unit)
        retained = read(provider + layout['retained'], 4)
        held = read(provider + layout['held'], 4)
        complete = variant in ('code100', 'disabled')
        assert state['exit'] == (0 if complete else 1), (variant, log)
        if complete:
            report = decoder.decode(log)
            assert state['injections'] == 0 and state['reaped'] == 6 and state['released'] == 1
            assert not retained and not held and not any(state['holders'].values())
            assert state['time'] - 1000000 < 1500000
            expected = []
            for enable, selector in ((0x10, 0x12), (0x10, 0x19), (0x11, 0x1a),
                                     (0x21, 0x2e), (0x21, 0x2f), (0x21, 0x31)):
                expected.extend((0xff, 0x0e, 0x0f, enable, selector, 0xff))
            assert state['transactions'] == expected
            assert [r['enabled'] for r in report['observations']] == [int(variant != 'disabled')] * 6
            for r in report['observations'][1:3]:
                assert r['selector'] == 0xe4 and not r['decoded'] and not r['programmed microvolts']
            for broken, reason in ((log.replace('SUPPLY_END=bldo5', ''), 'incomplete supply block'),
                                   (log.replace('SUPPLY_BEGIN=bldo5', 'SUPPLY_BEGIN=dcdc1'), 'missing supply block'),
                                   (log.replace('rail=0x0000000000000013', 'rail=0x0000000000000000'), 'invalid supply rail identity'),
                                   (log.replace('completed supplies=0x0000000000000006', ''), 'invalid supply completion')):
                harness.refused(lambda: decoder.decode(broken), reason)
        else:
            assert state['injections'] == 1 and state['reaped'] == 3 and state['released'] == 0
            assert held and retained and any(state['holders'].values())
            assert read(provider + layout['failed'], 4)
            assert read(image.address('rail', unit=unit) + image.layouts['a733_rail_token']['id'], 8)
            assert read(image.address('request', unit=unit) + image.layouts['a733_pmic_request']['id'], 8)
            assert 'COMPLETE native supply observations' not in log
            harness.refused(lambda: decoder.decode(log), 'complete supply observation required')
        results.append({'name': variant, 'passed': True, 'exit': state['exit'],
                        'reaped': state['reaped'], 'released': state['released'],
                        'injections': state['injections'], 'provider_retained': retained,
                        'transactions': state['transactions'], 'elapsed_modeled_us': state['time'] - 1000000,
                        'mmio_writes': state['writes']})
    (out / 'results.json').write_text(json.dumps({
        'passed': True, 'physical_access': False,
        'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
        'checker_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'decoder_sha256': hashlib.sha256((ROOT / 'apps/power-supply-observe/decode.py').read_bytes()).hexdigest(),
        'cases': results,
        'limits': 'RTOS queue/scheduler calls, architectural counter and I2C effects are modeled. '
                  'Actual client/provider/locks/rail/pin/clock code executes. No physical or return qualification.'}, indent=2) + '\n')
    print(f'PASS: {len(results)} linked observer cases and decoder refusals')


if __name__ == '__main__':
    main()
