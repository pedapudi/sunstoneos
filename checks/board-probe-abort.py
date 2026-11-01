#!/usr/bin/env python3
"""Execute the shared EL3 first-fault and deadline cutoff path without USB.

The AArch64 fixture models MMIO and exception entry. It establishes ordering
and recursive-fault suppression, not physical GPIO or reset behavior.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import harness
from unicorn import UC_HOOK_MEM_WRITE

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['platform/a733/probe.c', 'platform/a733/boot.h',
                'include/sunstone_probe.h', 'include/sunstone_crumb.h', 'runtime/memory.c']
spec = importlib.util.spec_from_file_location('abort_machine', ROOT/'checks/memtest-machine.py')
machine = importlib.util.module_from_spec(spec)
spec.loader.exec_module(machine)

FIXTURE = r'''
#include "sunstone_probe.h"
SUNSTONE_PROBE_IMAGE(0x42000100,256,0,0,20);
volatile unsigned calls,recurse;
void clean_data_cache(void) {}
uint64_t sunstone_probe_quiesce(void) {return SUNSTONE_PROBE_READY;}
uint64_t sunstone_probe_restore_interrupts(void) {return SUNSTONE_PROBE_READY;}
_Noreturn void sunstone_probe_smc(unsigned status)
{(void)status;for(;;)__asm__ volatile("wfe");}
_Noreturn void sunstone_probe_rom(void) {for(;;)__asm__ volatile("wfe");}
#ifdef STRONG_ABORT
__attribute__((section(".text.probe_recovery"),noinline))
void sunstone_probe_abort_devices(void)
{
    ++calls;
    volatile unsigned *dat=(void *)0x07025010;
    *dat=*dat&~0x100u;
    __asm__ volatile("dsb sy":::"memory");
    if(recurse)sunstone_probe_fault(0xee,0xdead,0xbeef,0xcafe);
}
#endif
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out/'fixture.c').write_text(FIXTURE)
    (out/'image.ld').write_text('ENTRY(sunstone_probe_fault)\nSECTIONS { . = 0x41000000; '
        '.text : { *(.text*) } .rodata : { *(.rodata*) *(.probe_marker) } '
        '.data : { *(.data*) *(.monitor_state) } .bss : { *(.bss*) *(COMMON) } }\n')
    cases = []
    for strong in (False, True):
        image = out/('strong.elf' if strong else 'weak.elf')
        command = [str(harness.CLANG), '--target=aarch64-none-elf', '-mcpu=cortex-a55',
            '-g', '-Os', '-mgeneral-regs-only', '-ffreestanding', '-fno-builtin',
            '--ld-path='+str(harness.LLD), '-I'+str(ROOT/'include'),
            '-I'+str(ROOT/'platform/a733'),
            '-isystem', str(harness.CACHE/'host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include'),
            '-Wl,-T,'+str(out/'image.ld'),
            str(ROOT/'platform/a733/probe.c'), str(ROOT/'runtime/memory.c'),
            str(out/'fixture.c'), '-nostdlib',
            '-o', str(image)]
        if strong:
            command.append('-DSTRONG_ABORT=1')
        result = subprocess.run(command, env=harness.environment(), capture_output=True)
        (out/(image.stem+'-build.log')).write_bytes(result.stdout+result.stderr)
        if result.returncode:
            raise SystemExit(result.stderr.decode(errors='replace'))
        for name in (('weak-default',) if not strong else ('fault', 'deadline', 'recursive', 'already-latched')):
            model = machine.Machine(image, step=1)
            model.sys.update(currentel=12, sctlr_el3=0, sctlr_el1=0)
            model.write(0x07025010, 0xa5a5a5a5, 4)
            if name == 'recursive':
                model.write(model.address('recurse'), 1, 4)
            events = []

            def write(uc, access, address, size, value, data):
                if address in (0x07090110, 0x07025010):
                    events.append((address, value))
                if address == 0x07025010:
                    assert model.read(0x07090110, 4) >> 24 == 0xfa

            model.uc.hook_add(UC_HOOK_MEM_WRITE, write)
            sites = harness.instruction_sites(model.e.parts,
                lambda word: word in (0xd503205f, 0xd503207f))
            harness.hook_addresses(model.uc, sites, lambda uc, address, size, data: uc.emu_stop())
            if name == 'already-latched':
                model.write(0x07090110, 0xfa030077, 4)
                model.write(0x07090114, 0x1234, 4)
                events.clear()
            if name == 'deadline':
                model.call('sunstone_probe_timeout', count=10000)
                expected = (0xfa030002, 0, None, 0)
            else:
                model.call('sunstone_probe_fault', 0x33, 0x44, 0x55, 0x66, count=10000)
                expected = (0xfa030033, 0x44, 0x55, 0x66)
            if name == 'already-latched':
                assert model.read(0x07090110, 4) == 0xfa030077
                assert model.read(0x07090114, 4) == 0x1234
            else:
                for address, value in zip((0x07090110, 0x07090114, 0x07090118, 0x0709011c), expected):
                    if value is not None:
                        assert model.read(address, 4) == value, (name, hex(address))
            called = strong and name != 'already-latched'
            assert model.read(model.address('calls'), 4) == int(called), name
            assert model.read(0x07025010, 4) == (0xa5a5a4a5 if called else 0xa5a5a5a5)
            if called:
                assert [address for address, _ in events] == [0x07090110, 0x07025010]
            cases.append(name)
            print('PASS', name)
    (out/'result.json').write_text(json.dumps({'passed': True, 'hardware_access': False,
        'cases': cases}, indent=2)+'\n')


if __name__ == '__main__':
    main()
