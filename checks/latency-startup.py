#!/usr/bin/env python3
"""Execute linked latency readiness and fault branches with modeled OS API calls.

The check replaces public OS API responses and the measured workload only.
Readiness loops, failure selection and workload progress execute from the ELF.
It establishes no physical clock, thermal, scheduler or recovery evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from harness import machine as harness_machine
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR)

CHECK_INPUTS = ['apps/latency/native.c', 'apps/latency/workload.c', 'include/sunstone_app.h']
SCRATCH, SCRATCH_BYTES = 0x81000000, 0x10000
RETURN = SCRATCH + 0x100
CLOCK = b'CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 '
CASES = ('delayed-thermal', 'delayed-clock', 'ready-at-entry', 'live-fails-during-warmup',
         'measurement-thermal-failure', 'measurement-live-failure', 'measurement-progress')


class Image:
    def __init__(self, path):
        self.path = path
        self.sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
        self.symbols = {}
        with path.open('rb') as stream:
            elf = ELFFile(stream)
            assert elf['e_machine'] == 'EM_AARCH64'
            self.parts = [(int(p['p_vaddr']), int(p['p_memsz']), p.data())
                          for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
            for symbol in elf.get_section_by_name('.symtab').iter_symbols():
                if symbol['st_info']['type'] == 'STT_FUNC' and symbol['st_shndx'] != 'SHN_UNDEF':
                    self.symbols.setdefault(symbol.name, set()).add(int(symbol['st_value']))
        assert all(address + size <= SCRATCH or address >= SCRATCH + SCRATCH_BYTES
                   for address, size, _ in self.parts), 'Scratch overlaps the linked image.'

    def address(self, name):
        values = self.symbols.get(name, set())
        assert len(values) == 1, (name, values)
        return next(iter(values))


class Machine:
    def __init__(self, image, clock_after=0, thermal_until=0, live_failure_at=None):
        self.image = image
        self.cpu = harness_machine()
        self.tick = self.progress = self.delays = self.workloads = 0
        self.clock_after, self.thermal_until = clock_after, thermal_until
        self.live_failure_at = live_failure_at
        self.failure = self.failure_return_address = self.stop = None
        self.activity = []
        self.calls = []
        # Merge page ranges because ELF load segments can share their last page.
        ranges = []
        for address, size, _ in image.parts:
            low, high = address & ~4095, (address + size + 4095) & ~4095
            if size:
                ranges.append((low, high))
        merged = []
        for low, high in sorted(ranges):
            if merged and low <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], high)
            else:
                merged.append([low, high])
        for low, high in merged:
            self.cpu.mem_map(low, high - low)
        self.cpu.mem_map(SCRATCH, SCRATCH_BYTES)
        for address, _, data in image.parts:
            self.cpu.mem_write(address, data)
        names = ('ss_live', 'ss_clock_ready', 'ss_throttled', 'ss_progress', 'ss_fail',
                 'ss_clock_info', 'ss_activity_begin', 'ss_activity_end', 'ss_finish',
                 'vTaskDelay', 'latency_workload_run')
        # Hooks cover only function entry instructions; application logic executes.
        self.hooks = {image.address(name): name for name in names}
        for address in (*self.hooks, RETURN):
            self.cpu.hook_add(UC_HOOK_CODE, self.code, begin=address, end=address)

    def returned(self, value=0):
        self.cpu.reg_write(UC_ARM64_REG_X0, value & ((1 << 64) - 1))
        self.cpu.reg_write(UC_ARM64_REG_PC, self.cpu.reg_read(UC_ARM64_REG_LR))

    def code(self, cpu, address, size, unused):
        if address == RETURN:
            self.stop = 'returned'
            cpu.emu_stop()
            return
        name = self.hooks[address]
        self.calls.append(dict(name=name, tick=self.tick))
        arg0, arg1 = cpu.reg_read(UC_ARM64_REG_X0), cpu.reg_read(UC_ARM64_REG_X1)
        if name == 'ss_live':
            self.returned(-1 if self.live_failure_at is not None and self.tick >= self.live_failure_at else 1)
        elif name == 'ss_clock_ready':
            self.returned(self.tick >= self.clock_after)
        elif name == 'ss_throttled':
            self.returned(self.tick < self.thermal_until)
        elif name == 'ss_progress':
            self.progress += 1
            self.returned()
        elif name == 'vTaskDelay':
            assert arg0 == 1, arg0
            self.delays += 1
            self.tick += 1
            self.returned()
        elif name == 'ss_clock_info':
            assert arg1 > len(CLOCK)
            cpu.mem_write(arg0, CLOCK + b'\0')
            self.returned(len(CLOCK))
        elif name == 'ss_activity_begin':
            self.activity.append('begin')
            self.returned()
        elif name == 'ss_activity_end':
            assert arg0 == 1
            self.activity.append('end')
            self.returned()
        elif name == 'latency_workload_run':
            assert self.tick >= max(self.clock_after, self.thermal_until), 'Measurement began before readiness.'
            assert self.activity == ['begin']
            self.workloads += 1
            self.returned()
        elif name == 'ss_fail':
            self.failure, self.failure_return_address = arg0, cpu.reg_read(UC_ARM64_REG_LR)
            self.stop = 'failure'
            cpu.emu_stop()
        elif name == 'ss_finish':
            assert self.workloads == 1 and self.activity == ['begin', 'end'] and arg1 > 0
            self.stop = 'finished'
            cpu.emu_stop()

    def call(self, name):
        self.cpu.reg_write(UC_ARM64_REG_SP, SCRATCH + SCRATCH_BYTES - 16)
        self.cpu.reg_write(UC_ARM64_REG_LR, RETURN)
        self.cpu.reg_write(UC_ARM64_REG_X0, SCRATCH + 4096)
        self.cpu.emu_start(self.image.address(name), 0, count=100000)
        assert self.stop is not None, ('Execution budget exhausted', name, hex(self.cpu.reg_read(UC_ARM64_REG_PC)))

    def report(self):
        return dict(passed=True, stop=self.stop, failure=self.failure,
                    failure_return_address=self.failure_return_address,
                    ticks=self.tick, progress_calls=self.progress, delay_calls=self.delays,
                    workload_calls=self.workloads, activity=self.activity, calls=self.calls)


def run_case(image, name, baseline=False):
    if name in ('delayed-thermal', 'delayed-clock', 'ready-at-entry'):
        clock, thermal = {'delayed-thermal': (2, 4), 'delayed-clock': (4, 2),
                          'ready-at-entry': (0, 0)}[name]
        machine = Machine(image, clock_after=clock, thermal_until=thermal)
        machine.call('latency_main')
        if baseline and name != 'ready-at-entry':
            assert machine.failure == 0xb01 and machine.delays == machine.workloads == 0
        else:
            assert machine.stop == 'finished' and machine.failure is None
            assert machine.progress == machine.delays == max(clock, thermal)
        return machine.report()
    if name == 'live-fails-during-warmup':
        machine = Machine(image, clock_after=4, thermal_until=0, live_failure_at=1)
        machine.call('latency_main')
        assert machine.failure == 0xb01 and machine.workloads == 0
        assert machine.delays == machine.progress == 1
        return machine.report()
    machine = Machine(image, thermal_until=1 if name == 'measurement-thermal-failure' else 0,
                      live_failure_at=0 if name == 'measurement-live-failure' else None)
    machine.call('latency_workload_progress')
    if name == 'measurement-thermal-failure':
        assert machine.failure == (0xb01 if baseline else 0xb05)
    elif name == 'measurement-live-failure':
        assert machine.failure == 0xb01
    else:
        assert machine.stop == 'returned' and machine.failure is None and machine.progress == 1
    assert machine.delays == machine.workloads == 0
    return machine.report()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true', help='Require the old warmup failure and shared fault code.')
    parser.add_argument('--case', action='append', choices=CASES, help='Run selected cases after a fixture repair.')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = Image(args.image)
    # Resolve every native entry point before running a case.
    for name in ('latency_main', 'latency_workload_progress'):
        image.address(name)
    report = dict(passed=False, physical_access=False, modeled_os_api=True, native_workload=False,
                  image=str(args.image.resolve()), image_sha256=image.sha256, baseline=args.baseline, cases={})
    destination = args.output / 'result.json'
    for name in args.case or CASES:
        report['cases'][name] = run_case(image, name, args.baseline)
        destination.write_text(json.dumps(report, indent=2) + '\n')
    report['passed'] = True
    destination.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(passed=True, cases=len(report['cases']), physical_access=False, baseline=args.baseline)))


if __name__ == '__main__':
    main()
