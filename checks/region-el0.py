#!/usr/bin/env python3
"""Run the large application region on QEMU: real tables, ASIDs, EL0 routines and fault records.

The fixture also runs the unified memory ledger on the same tables: run-time growth, prepare and hand-off with the real
maintenance, the policy conversion of a chunk through every root, and the isolation of two applications' ledger buffers
and of the ledger's own pages.

The fixture boots at EL1 with the probe platform's kernel tables and the upper map, takes a 1 GiB
region from the ledger, builds three address spaces and runs the EL0 routines of the board image
(apps/sd-read-probe/region.c and el0.S). It checks which EL0 accesses fault and with what exception
class, fault class and address, that the streaming routine folds the same bytes as EL1, and that the
kernel table and vectors are restored. QEMU models the translation regime, ASIDs and exception
levels; it does not model the PMU events, DRAM timing or the board's firmware.
"""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
HOST = Path.home() / '.cache/sunstoneos/host/usr'


def run(command, log, **kwargs):
    r = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    log.write_text(r.stdout + r.stderr)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit(f'failed: {command[0]}')
    return r.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=HOST / 'bin/qemu-system-aarch64')
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    wrapper = out / 'ld.lld'
    wrapper.write_text(f'#!/bin/sh\nexec {HOST}/lib/llvm-21/bin/lld -flavor gnu "$@"\n')
    wrapper.chmod(0o755)
    flags = ['clang-21', '--target=aarch64-none-elf', '-O2', '-g', '-march=armv8-a', '-mstrict-align', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-Wall', '-Wextra', '-Werror', f'-I{ROOT}/include',
             f'-I{ROOT}/apps/sd-read-probe', f'-isystem{HOST.parent}/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include', '-c']
    objects = []
    for source, options in ((ROOT / 'checks/region-el0.c', ['-mgeneral-regs-only']),
                            (ROOT / 'checks/region-el0.S', []),
                            (ROOT / 'apps/sd-read-probe/region.c', ['-mgeneral-regs-only', '-DREGION_PROGRESS()=((void)0)']),
                            (ROOT / 'apps/sd-read-probe/el0.S', []),
                            (ROOT / 'kernel/appmem/app_region.c', ['-mgeneral-regs-only']),
                            (ROOT / 'kernel/umem/umem.c', ['-mgeneral-regs-only']),
                            (ROOT / 'kernel/umem/umem_policy.c', ['-mgeneral-regs-only']),
                            (ROOT / 'arch/aarch64/umem_cpu.c', ['-mgeneral-regs-only']),
                            (ROOT / 'arch/aarch64/device_memory.c', ['-mgeneral-regs-only'])):
        obj = out / (source.name + '.o')
        run(flags + options + [source, '-o', obj], out / (source.name + '.log'))
        objects.append(obj)
    elf = out / 'region-el0.elf'
    run([wrapper, '-nostdlib', '-static', '-T', ROOT / 'checks/region-el0.ld', *objects, '-o', elf], out / 'link.log')
    result = subprocess.run([str(args.qemu), '-machine', 'virt', '-cpu', 'max', '-m', '4G', '-nographic', '-semihosting',
                             '-kernel', str(elf)], capture_output=True, text=True, timeout=300)
    (out / 'qemu.log').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    if 'REGION-EL0 PASS' not in result.stdout or result.returncode:
        print(result.stderr)
        raise SystemExit('region EL0 fixture failed')
    harness.save(out / 'result.json', {'physical_access': False, 'qemu': True})


if __name__ == '__main__':
    main()
