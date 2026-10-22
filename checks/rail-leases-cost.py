#!/usr/bin/env python3
"""Count AArch64 instructions and cache lines for rail ledger and clock registry operations.

The check compiles checks/rail-leases-cost.c with the ledger, the clock
registry and the GPU resource manager of one source tree, using the
production code-generation flags (-march=armv8-a -mstrict-align -O2) tuned
for the Cortex-A55. Unicorn runs each operation once and counts the
instructions it executes outside the cost_* wrappers and the distinct 64-byte
lines of the ledger, registry and GPU manager objects it reads or writes. Pass --root to measure
another tree, such as the merge base, and --limits to enforce the budgets.
"""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_LR, UC_ARM64_REG_PC
if not __debug__:
    raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

HERE = Path(__file__).resolve().parent
CACHE = Path.home() / '.cache/sunstoneos'
CLANG = Path('/usr/bin/clang-21')
LLD = CACHE / 'host/usr/lib/llvm-21/bin/ld.lld'
BASE = 0x40000000
RETURN = 0x40ff0000
# Configurations: (clock leases, rail leases).
SMALL = (42, 16)
# The merge-base ledger has 128 slots and the new one 32, so the full
# configuration fills whichever table the tree has, less the measured cycle.
FULL_CLOCKS = 126


def build(root, out):
    flags = ['--target=aarch64-none-elf', '-march=armv8-a', '-mtune=cortex-a55', '-mstrict-align',
             '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-O2', '-std=gnu11',
             '-Wall', '-Wextra', '-Werror', '-I', str(root / 'include')]
    sources = [HERE / 'rail-leases-cost.c', root / 'drivers/power/a733_rail_leases.c',
               root / 'drivers/clock/a733_clock_leases.c', root / 'drivers/gpu/a733_gpu_resources.c']
    objects = []
    for i, source in enumerate(sources):
        obj = out / f'{i}.o'
        subprocess.run([CLANG, *flags, '-c', source, '-o', obj], check=True)
        objects.append(obj)
    elf = out / 'cost.elf'
    subprocess.run([LLD, '-static', '-nostdlib', '-e', 'cost_setup', f'--image-base={BASE:#x}', *objects,
                    '-o', elf], check=True)
    return elf


class Machine:
    def __init__(self, elf):
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.u.mem_map(BASE, 0x1000000)
        with elf.open('rb') as f:
            e = ELFFile(f)
            for segment in e.iter_segments():
                if segment['p_type'] == 'PT_LOAD':
                    self.u.mem_write(segment['p_vaddr'], segment.data())
            self.symbols = {s.name: (s['st_value'], s['st_size']) for s in e.get_section_by_name('.symtab').iter_symbols()}
        # The cost_* wrappers only call the operation; their instructions are
        # not counted.
        self.wrappers = [(v, v + n) for name, (v, n) in self.symbols.items() if name.startswith('cost_') and n]
        objects = ['cost_clocks', 'cost_gpu', 'cost_rails']
        self.ranges = [(self.symbols[n][0], self.symbols[n][0] + self.symbols[n][1]) for n in objects]
        self.count = 0
        self.lines = set()
        self.u.hook_add(UC_HOOK_CODE, self._code)
        self.u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self._memory)

    def _code(self, uc, address, size, data):
        if not any(lo <= address < hi for lo, hi in self.wrappers):
            self.count += 1

    def _memory(self, uc, access, address, size, value, data):
        for lo, hi in self.ranges:
            if lo <= address < hi:
                for line in range(address >> 6, (address + size - 1 >> 6) + 1):
                    self.lines.add(line)

    def word(self, name):
        address, size = self.symbols[name]
        return int.from_bytes(self.u.mem_read(address, size), 'little')

    def call(self, name, *args):
        self.count = 0
        self.lines = set()
        for i, value in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + i, value)
        self.u.reg_write(UC_ARM64_REG_SP, BASE + 0xfff000)
        self.u.reg_write(UC_ARM64_REG_LR, RETURN)
        self.u.emu_start(self.symbols[name][0], RETURN, count=2_000_000)
        assert self.u.reg_read(UC_ARM64_REG_PC) == RETURN, f'{name} did not return'
        result = self.u.reg_read(UC_ARM64_REG_X0) & 0xffffffff
        assert result == 0, f'{name} failed at step {result}'
        return {'instructions': self.count, 'lines': len(self.lines)}


def measure(root):
    with tempfile.TemporaryDirectory() as d:
        elf = build(root, Path(d))
        probe = Machine(elf)
        ledger = probe.word('cost_ledger_bytes')
        results = {'ledger_bytes': ledger, 'registry_bytes': probe.word('cost_registry_bytes')}
        # The full rail configuration leaves one slot for the measured cycle.
        rail_capacity = 32 if ledger < 4096 else 128
        for label, (clocks, rails) in [('small', SMALL), ('full', (FULL_CLOCKS, rail_capacity - 1))]:
            m = Machine(elf)
            m.call('cost_setup', clocks, rails)
            row = {'clock_leases': clocks, 'rail_leases': rails,
                   'rail_status': m.call('cost_rail_status'),
                   'clock_status': m.call('cost_clock_status'),
                   'acquire_poll_release': m.call('cost_cycle'),
                   'borrow': m.call('cost_borrow')}
            row['authority_per_transaction'] = m.call('cost_authority')
            row['end_borrow'] = m.call('cost_end_borrow')
            results[label] = row
        return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=HERE.parent)
    ap.add_argument('--limits', action='store_true', help='enforce the design budgets')
    ap.add_argument('--json', type=Path, help='also write the results here')
    a = ap.parse_args()
    results = measure(a.root.resolve())
    text = json.dumps(results, indent=1)
    print(text)
    if a.json:
        a.json.write_text(text + '\n')
    if a.limits:
        assert results['ledger_bytes'] <= 1632, results['ledger_bytes']
        for label in ['small', 'full']:
            row = results[label]
            assert row['rail_status']['instructions'] <= 60 and row['rail_status']['lines'] <= 3, row
            assert row['clock_status']['instructions'] <= 50, row
            assert row['acquire_poll_release']['instructions'] <= 450, row
            assert row['authority_per_transaction']['instructions'] <= 6, row
        # No scan: the cost does not grow with the number of holders.
        for op in ['rail_status', 'clock_status', 'acquire_poll_release', 'authority_per_transaction']:
            assert results['small'][op]['instructions'] == results['full'][op]['instructions'], op
        print('PASS rail and clock lease costs within the design budgets')


if __name__ == '__main__':
    main()
