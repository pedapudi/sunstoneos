#!/usr/bin/env python3
"""Count instructions with the CPU PMU module on two QEMU cores in the FreeRTOS SMP port.

The image is checks/cpu-pmu-smp.c with platform/a733/cpu_pmu.c on
platform/qemu-smp, built with the SMP port patches and the privileged-task
patch, so every task runs at EL1. It runs under -icount shift=0, where
INST_RETIRED is exact, once with cortex-a55 cores and once with cortex-a76
cores. Each run requires, on both cores:

  - an empty region (back-to-back begin and end) of at most 24 instructions;
  - a loop of 2K instructions to count exactly 2K more than the same region
    with no iterations;
  - a nonzero cycle difference, the core number, MIDR_EL1, CNTFRQ_EL0, the
    counters used and the supported-event mask that QEMU's PMCEID registers give;

and on core 0 the nested-region, cleared-enable, two-core-mask and moved-task
refusals.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
FREERTOS = CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
FLAGS = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-ffreestanding', '-fno-builtin',
         '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
         '-ffunction-sections', '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-Werror']
QEMU = ['-machine', 'virt,gic-version=3,secure=off,virtualization=off', '-smp', '2', '-m', '128M',
        '-nographic', '-monitor', 'none', '-semihosting-config', 'enable=on,target=native', '-icount', 'shift=0']
PORT = 'portable/GCC/ARM_CR82'
PATCHES = {'port.c': ['freertos-smp-c.patch', 'freertos-smp-privileged.patch'],
           'portASM.S': ['freertos-smp-asm.patch'], 'portmacro.h': ['freertos-smp-macro.patch']}
CPUS = {'cortex-a55': 0x410fd050, 'cortex-a76': 0x410fd0b0}  # MIDR without variant and revision
K_ITERATIONS = 1000
EMPTY_BUDGET = 24
STATUS = {'nested_status': 7, 'disabled_status': 8, 'two_core_mask_status': 2, 'moved_status': 6}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True, help='new directory for logs and results')
    p.add_argument('--clang', type=Path, default=Path('/usr/bin/clang-21'))
    p.add_argument('--lld', type=Path, default=CACHE / 'host/usr/bin/ld.lld-21')
    p.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    p.add_argument('--freertos', type=Path, default=FREERTOS)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(name, command, timeout=600):
        command = [str(c) for c in command]
        commands.append(command)
        r = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        (out / (name + '.log')).write_text(r.stdout)
        if r.returncode:
            raise RuntimeError(f'{name} failed ({r.returncode}): {r.stdout[-4000:]}')
        return r.stdout

    port = out / 'port'
    port.mkdir()
    for name, patches in PATCHES.items():
        source = a.freertos / PORT / name
        for patch in patches:
            target = port / (name + '.tmp')
            run('patch-' + patch, ['patch', '--fuzz=0', '--reject-file=-', '--output', target, source,
                                   ROOT / 'patches' / patch])
            source = port / name
            target.replace(source)
    includes = [ROOT / 'platform/qemu-smp', ROOT / 'include', ROOT / 'runtime/include', a.freertos / 'include', port]
    sources = [ROOT / 'platform/qemu-smp/start.S', ROOT / 'platform/qemu-smp/platform.c', ROOT / 'runtime/memory.c',
               a.freertos / 'tasks.c', a.freertos / 'queue.c', a.freertos / 'list.c', port / 'port.c',
               port / 'portASM.S', ROOT / 'platform/a733/cpu_pmu.c', ROOT / 'checks/cpu-pmu-smp.c']
    objects = []
    for source in sources:
        obj = out / (Path(source).stem + '.o')
        run('compile-' + obj.stem, [a.clang, *FLAGS, *['-I' + str(i) for i in includes], '-c', source, '-o', obj])
        objects.append(obj)
    elf = out / 'cpu-pmu-smp.elf'
    run('link', [a.lld, '-nostdlib', '-static', '--gc-sections', '-T', ROOT / 'platform/qemu-smp/link.ld',
                 '-Map=' + str(out / 'cpu-pmu-smp.map'), *objects, '-o', elf])
    results = {}
    for cpu, midr in CPUS.items():
        log = run(cpu, [a.qemu, *QEMU, '-cpu', cpu, '-device', f'loader,file={elf},cpu-num=0'])
        if 'PASS cpu-pmu SMP fixture' not in log or 'FAIL' in log:
            raise RuntimeError(cpu + ' run failed: ' + log[-2000:])
        m = {k: int(v, 16) for k, v in re.findall(r'MEASURE (\w+)=0x([0-9a-f]+)', log)}
        for core in (0, 1):
            g = lambda name: m[f'{name}_core{core}']
            assert g('empty_instructions') <= EMPTY_BUDGET, (cpu, core, g('empty_instructions'))
            assert g('loopk_instructions') - g('loop0_instructions') == 2 * K_ITERATIONS, (cpu, core, m)
            assert g('loopk_cycles') > 0 and g('loopk_ticks') >= 0, (cpu, core, m)
            assert g('record_core') == core, (cpu, core, m)
            assert g('record_midr') == g('expect_midr') and g('record_midr') & 0xff0ffff0 == midr, (cpu, core, m)
            assert g('record_cntfrq') == g('expect_cntfrq'), (cpu, core, m)
            assert g('record_counters') == g('expect_counters') == 6, (cpu, core, m)
            assert g('record_supported') == g('expect_supported'), (cpu, core, m)
            assert g('record_supported') & 1, 'INST_RETIRED must be implemented'
            assert g('record_overflow') == 0, (cpu, core, m)
        assert m['reconfigured_empty_instructions_core0'] == m['empty_instructions_core0']
        for name, value in STATUS.items():
            assert m[name + '_core0'] == value, (cpu, name, m)
        results[cpu] = m
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'commands': commands, 'elf_sha256': digest(elf), 'qemu_sha256': digest(a.qemu),
        'clang_sha256': digest(a.clang), 'inputs': {str(s.relative_to(ROOT)): digest(s) for s in sources
                                                    if ROOT in s.parents},
        'k_iterations': K_ITERATIONS, 'empty_budget': EMPTY_BUDGET, 'runs': results,
        'scope': 'QEMU cortex-a55 and cortex-a76 cores with GICv3 and the FreeRTOS SMP port at EL1, '
                 'without EL2 or EL3; instruction counts are exact, cycles are QEMU virtual time. '
                 'No A733 core, MDCR_EL2 setting or event other than those QEMU implements.'},
        indent=2) + '\n')
    for cpu, m in results.items():
        print(f'PASS {cpu}: empty region {m["empty_instructions_core0"]}/{m["empty_instructions_core1"]} '
              f'instructions on cores 0/1, loop of {2 * K_ITERATIONS} exact, supported mask '
              f'{m["record_supported_core0"]:#x}, refusals {", ".join(STATUS)}')


if __name__ == '__main__':
    main()
