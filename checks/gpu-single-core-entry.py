#!/usr/bin/env python3
"""Trace the frozen GPU boot entry through platform initialization without task execution."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess

from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


shared = module('gpu_entry_shared', ROOT / 'checks/gpu-single-core.py')


class EntryMachine(shared.Machine):
    def __init__(self, image, raw, stop):
        super().__init__(image, raw)
        self.stop_at = stop
        self.entry_trace = []
        self.accesses = {'read': 0, 'write': 0}
        self.boundaries = {image.address(n): n for n in
                          ('sd_single_core_entry', 'sd_single_core_monitor_init',
                           '_start', 'kernel_main', 'platform_init')}
        self.cache_helper = image.address('invalidate_data_cache')
        self.events.add(self.cache_helper)
        self.events.update(self.boundaries)
        self.events.update(a for a, op in image.ops.items() if op == 'eret')
        self.sysregs.update(elr_el3=0, spsr_el3=0, sctlr_el2=0,
                            cntv_ctl_el0=0, vbar_el3=0)
        self.cpu.hook_add(UC_HOOK_MEM_READ, self.memory_access, user_data='read')
        self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.memory_access, user_data='write')

    def memory_access(self, cpu, access, address, size, value, kind):
        if self.el != 4 or not self.sysregs['sctlr_el1'] & 1:
            return
        for va in (address, address + size - 1):
            par = self.translate(va, kind == 'write')
            assert not par & 1, (kind, hex(va), hex(cpu.reg_read(UC_ARM64_REG_PC)))
            assert (par & 0x0000fffffffff000) | (va & 4095) == va
        self.accesses[kind] += 1

    def hook(self, cpu, address, size, unused):
        if address in self.boundaries:
            self.entry_trace.append({'function': self.boundaries[address], 'el': self.el,
                                     'sp': cpu.reg_read(UC_ARM64_REG_SP)})
        if address == self.cache_helper:
            # Cache geometry and set/way effects remain the established hardware
            # model boundary. This check proves control flow, not publication.
            self.ret()
            return
        text = self.e.ops.get(address, '')
        extra = re.fullmatch(r'msr\s+(spsr_el3|sctlr_el2|cntv_ctl_el0|vbar_el3),\s*x(\d+|zr)', text, re.I)
        if extra:
            self.sysregs[extra[1].lower()] = (0 if extra[2].lower() == 'zr' else
                                             cpu.reg_read(UC_ARM64_REG_X0 + int(extra[2])))
            cpu.reg_write(UC_ARM64_REG_PC, address + 4)
            return
        if text == 'eret':
            assert self.el == 12 and self.sysregs['spsr_el3'] == 0x3c5
            assert self.sysregs['scr_el3'] == 0x505
            assert self.sysregs['elr_el3'] == self.e.address('_start')
            self.el = 4
            cpu.reg_write(UC_ARM64_REG_PSTATE, self.sysregs['spsr_el3'])
            cpu.reg_write(UC_ARM64_REG_PC, self.sysregs['elr_el3'])
            return
        return super().hook(cpu, address, size, unused)


def run(args):
    native = module('gpu_entry_image', ROOT / 'checks/storage-read-native.py')
    image = native.Image(args.image)
    image.path = args.image
    image.ops = {}
    for line in subprocess.check_output(
            ['/usr/lib/llvm-21/bin/llvm-objdump', '-d', '--no-show-raw-insn', str(args.image)],
            text=True).splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+(.+)$', line)
        if match:
            image.ops[int(match[1], 16)] = match[2].strip()
    # Stop at the actual return address of kernel_main's platform_init call.
    start = image.address('kernel_main')
    calls = [a for a, op in image.ops.items() if start <= a < start + 128 and
             re.fullmatch(r'bl\s+0x[0-9a-f]+ <platform_init>', op)]
    assert len(calls) == 1, 'Expected one early platform_init call in kernel_main.'
    prepared = json.loads((args.bundle / 'prepared.json').read_text())
    raw = bytearray((args.bundle / 'session-template.bin').read_bytes())
    struct.pack_into('<3Q', raw, 24, prepared['epoch'], prepared['generation'], prepared['lease'])
    raw[48:80] = bytes.fromhex(prepared['build_id'])
    machine = EntryMachine(image, bytes(raw), calls[0] + 4)
    machine.cpu.emu_start(image.address('sd_single_core_entry'), 0, count=10000000)
    assert machine.finished and not machine.parked
    assert [x['function'] for x in machine.entry_trace] == [
        'sd_single_core_entry', 'sd_single_core_monitor_init', '_start', 'kernel_main', 'platform_init']
    assert machine.entry_trace[1]['sp'] == image.address('__monitor_stack_top')
    assert machine.entry_trace[3]['sp'] == image.address('__exception_stack_top')
    assert machine.sysregs['sctlr_el1'] & 0x1005 == 0x1005
    assert machine.accesses['read'] and machine.accesses['write']
    result = dict(passed=True, physical_access=False,
                  image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),
                  session_sha256=hashlib.sha256(raw).hexdigest(),
                  checkpoints=machine.entry_trace, mapped_accesses=machine.accesses,
                  stop_address=calls[0] + 4,
                  scope='Continuous actual entry, monitor admission, modeled ERET, actual EL1 BSS clear and platform initialization on linked stacks. EL1 data accesses after MMU enable checked against actual descriptors. Cache maintenance, instruction visibility, registers, counter and GIC behavior remain modeled. No scheduler, peripheral service or physical execution.')
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'bundle', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    run(parser.parse_args())
