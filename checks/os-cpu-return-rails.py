#!/usr/bin/env python3
"""Execute the linked rail variant's admission up to its first rail acquisition.

Provider/thermal startup and CPU inspection are modeled API boundaries. The
application's actual plan, entry-voltage checks, pattern checks, all three
boot-clock checks and rail selection execute. The provider composition check
in cpu-rails-return.c exercises the actual write/settle/readback sequence.
"""
import argparse
from pathlib import Path
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X30, UC_ARM64_REG_PC, UC_ARM64_REG_SP
from harness import Image, save


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    image = Image(args.elf, {'bb_power', 'a733_platform_locks', 'a733_cpu_clock',
                             'a733_cpu_clock_report', 'a733_cpu_clock_domain'})
    unit = 'cpu-opp-step.c'
    cpu = image.address('cpu', unit)
    power = image.address('bb_power')
    cf, rf, df = (image.layouts[name] for name in ('a733_cpu_clock', 'a733_cpu_clock_report', 'a733_cpu_clock_domain'))
    assert df['hz'] == 24 and df['decoded'] == 32
    domain_bytes = 40
    boot = ((0xf8802600, 0x03010000, 494000000), (0xf8802700, 0x03000000, 1014000000),
            (0xf8802600, 0x03010105, 494000000))
    rows = []
    for case in ('entry800', 'wrong-dcdc3', 'wrong-dcdc5', 'little-pll', 'big-pll', 'dsu-pll',
                 'little-mux', 'big-mux', 'dsu-mux', 'pattern-enabled'):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x40000000, 0x4000000)
        uc.mem_map(0x02000000, 0x10000)
        uc.mem_map(0x03400000, 0x200000)
        uc.mem_map(0x08871000, 0x3000)
        for address, data in image.parts:
            uc.mem_write(address, data)
        def put32(address, value):
            uc.mem_write(address, struct.pack('<I', value))
        def put64(address, value):
            uc.mem_write(address, struct.pack('<Q', value))
        pf = image.layouts['bb_power']
        put64(power + pf['deadline'], 1500000)
        put32(power + pf['locks'] + image.layouts['a733_platform_locks']['initialized'], 1)
        put64(image.address('ullCriticalNesting'), 0)
        for d, (pll, mux, _) in enumerate(boot):
            put32(0x08871000 + d * 0x1000, pll)
            put32(0x0887101c + d * 0x1000, mux)
        state = {'now': 100, 'outcome': None, 'reason': None, 'instructions': 0, 'rail_mask': None}
        hooks = {}
        def stub(name, result=0, local=None):
            hooks[image.address(name, local)] = lambda: result
        for name in ('platform_puts', 'bb_power_wait_ready', 'bb_power_pause', 'bb_power_print',
                     'bb_line_start', 'bb_line_dec', 'bb_line_hex', 'bb_line_end'):
            stub(name)
        stub('xTaskGetCurrentTaskHandle', 0x1234)
        stub('xTaskGetSchedulerState', 2)
        stub('a733_clock_acquire', 0)
        stub('a733_thermal_init', 1)
        stub('a733_thermal_poll', 2)
        stub('a733_cpu_clock_init', 1)
        stub('sensor_set', 0, unit)
        def now():
            state['now'] += 1000
            return state['now']
        hooks[image.address('bb_power_now')] = now
        def inspect():
            d = uc.reg_read(UC_ARM64_REG_X0)
            base = cpu + cf['report']
            put32(base + rf['bin'], 4)
            put32(base + rf['binned'], 1)
            put32(base + rf['dcxo_hz'], 26000000)
            pll, mux, hz = boot[d]
            names = ('little', 'big', 'dsu')
            if case == names[d] + '-pll':
                pll ^= 0x100
            if case == names[d] + '-mux':
                mux ^= 1
            domain = base + rf['domain'] + d * domain_bytes
            put32(domain + df['pll'], pll)
            put32(domain + df['select'], mux)
            put32(domain + df['source'], 3)
            put32(domain + df['decoded'], 1)
            put64(domain + df['hz'], hz)
            put32(cpu + cf['rail_uv'], 800000 if case == 'wrong-dcdc5' else 1000000)
            put32(cpu + cf['rail_uv'] + 4, 1050000 if case == 'wrong-dcdc3' else 800000)
            if case == 'pattern-enabled':
                put32(cpu + cf['words'], 1 << 29)
            return 0
        hooks[image.address('inspect', unit)] = inspect
        def text(address):
            value = bytearray()
            while True:
                byte = bytes(uc.mem_read(address + len(value), 1))
                if byte == b'\0':
                    return value.decode()
                value += byte
        decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        def code(machine, address, size, unused):
            state['instructions'] += 1
            if address == image.address('refuse', unit):
                state.update(outcome='refused', reason=text(uc.reg_read(UC_ARM64_REG_X0)))
                machine.emu_stop()
                return
            if address in (image.address('bb_power_fail'), image.address('platform_exit')):
                state.update(outcome='unexpected-failure', reason=text(uc.reg_read(UC_ARM64_REG_X0)))
                machine.emu_stop()
                return
            if address == image.address('a733_rail_acquire'):
                state.update(outcome='rail-acquire', rail_mask=machine.reg_read(UC_ARM64_REG_X0 + 3))
                machine.emu_stop()
                return
            if address in hooks:
                machine.reg_write(UC_ARM64_REG_X0, hooks[address]())
                machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_X30))
                return
            ins = next(decoder.disasm(bytes(machine.mem_read(address, 4)), address))
            if ins.mnemonic == 'mrs':
                register, system = ins.op_str.split(', ')
                machine.reg_write(UC_ARM64_REG_X0 + int(register[1:]), 4 if system == 'currentel' else 0)
                machine.reg_write(UC_ARM64_REG_PC, address + 4)
            elif ins.mnemonic == 'msr':
                machine.reg_write(UC_ARM64_REG_PC, address + 4)
            elif ins.mnemonic in ('dmb', 'dsb', 'isb'):
                machine.reg_write(UC_ARM64_REG_PC, address + 4)
        writes = []
        def write(machine, access, address, size, value, unused):
            if address < 0x40000000:
                writes.append((address, size, value))
        uc.hook_add(UC_HOOK_CODE, code)
        uc.hook_add(UC_HOOK_MEM_WRITE, write)
        uc.reg_write(UC_ARM64_REG_SP, 0x41f80000)
        uc.reg_write(UC_ARM64_REG_X30, 0x40000000)
        uc.emu_start(image.address('client', unit), 0, count=100000)
        assert not writes, (case, writes)
        if case == 'entry800':
            assert state['outcome'] == 'rail-acquire' and state['rail_mask'] == 1 << 4, state
        else:
            assert state['outcome'] == 'refused', state
            assert state['reason'] == ('CPU supplies' if case.startswith('wrong-dcdc') else
                                       'pattern or LFM enabled' if case == 'pattern-enabled' else 'boot clock words'), state
        rows.append({'name': case, **state})
    save(args.out / 'results.json', {'physical_access': False, 'cases': rows})
    print('PASS linked CPU rail-return admission:', len(rows), 'cases')


if __name__ == '__main__':
    main()
