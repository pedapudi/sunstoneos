#!/usr/bin/env python3
"""Count FreeRTOS SMP port instructions per operation on two QEMU cores.

The image is checks/smp-port-cost.c on platform/qemu-smp, built with the SMP
port patches, the task patch and the privileged-task patch from --root. It runs
under -icount shift=0, where INST_RETIRED is exact and the virtual counter
advances one nanosecond per instruction retired on either core, once with
cortex-a55 cores and once with cortex-a76 cores. It records:

  - a critical-section pair and a current-task read from an EL1 and an EL0 task;
  - a notification given to the calling task and taken with the count set;
  - a notification that wakes a higher-priority task on the same core, up to
    the woken task's first instruction, floating-point state included;
  - cross-core notification round trips, in instructions retired on each
    core per round trip, with idle cores waiting in WFI, and in virtual time.

With --baseline (a result.json from the base tree) it also requires that no
operation retires more instructions than in the baseline and that a cross-core
round trip retires at least 30% fewer instructions on both cores. It records,
without requiring them, the design targets of
docs/designs/smp-heterogeneous.md: at most 100 instructions for the EL1 pair,
180 for the EL0 pair and 800 for the same-core switch.
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
PATCHES = {PORT + '/port.c': ['freertos-smp-c.patch', 'freertos-smp-privileged.patch'],
           PORT + '/portASM.S': ['freertos-smp-asm.patch'], PORT + '/portmacro.h': ['freertos-smp-macro.patch'],
           'tasks.c': ['freertos-smp-tasks.patch']}
CPUS = ('cortex-a55', 'cortex-a76')
TARGETS = {'el1_critical_pair': 100, 'el0_critical_pair': 180, 'el1_give_switch': 800}
OPERATIONS = ('el1_critical_pair', 'el0_critical_pair', 'el1_current_task', 'el0_current_task',
              'el1_notify_give_take', 'el1_give_switch', 'cross_core_round_trip_instructions')
ROUND_TRIP_REDUCTION = 0.30


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True, help='new directory for logs and results')
    p.add_argument('--root', type=Path, default=ROOT, help='source tree that supplies patches and platform')
    p.add_argument('--baseline', type=Path, help='result.json of the base tree; enables the comparison')
    p.add_argument('--clang', type=Path, default=Path('/usr/bin/clang-21'))
    p.add_argument('--lld', type=Path, default=CACHE / 'host/usr/bin/ld.lld-21')
    p.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    p.add_argument('--freertos', type=Path, default=FREERTOS)
    a = p.parse_args()
    out, root = a.output.resolve(), a.root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(name, command, timeout=600):
        command = [str(c) for c in command]
        commands.append(command)
        r = subprocess.run(command, cwd=out, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        (out / (name + '.log')).write_text(r.stdout)
        if r.returncode:
            raise RuntimeError(f'{name} failed ({r.returncode}): {r.stdout[-4000:]}')
        return r.stdout

    kernel = out / 'kernel'
    kernel.mkdir()
    config = out / 'config'
    config.mkdir()
    (config / 'FreeRTOSConfig.h').write_text(
        f'#include "{root}/platform/qemu-smp/FreeRTOSConfig.h"\n'
        '#undef configUSE_PASSIVE_IDLE_HOOK\n#define configUSE_PASSIVE_IDLE_HOOK 1\n')
    for name, patches in PATCHES.items():
        source = a.freertos / name
        for patch in patches:
            target = kernel / (Path(name).name + '.tmp')
            run('patch-' + patch, ['patch', '--fuzz=0', '--reject-file=-', '--output', target, source,
                                   root / 'patches' / patch])
            source = kernel / Path(name).name
            target.replace(source)
    includes = [config, root / 'platform/qemu-smp', root / 'include', root / 'runtime/include', a.freertos / 'include',
                kernel]
    sources = [root / 'platform/qemu-smp/start.S', root / 'platform/qemu-smp/platform.c',
               root / 'runtime/memory.c', kernel / 'tasks.c', a.freertos / 'queue.c', a.freertos / 'list.c',
               kernel / 'port.c', kernel / 'portASM.S', ROOT / 'checks/smp-port-cost.c']
    objects = []
    for source in sources:
        obj = out / (Path(source).stem + '.o')
        run('compile-' + obj.stem, [a.clang, *FLAGS, *['-I' + str(i) for i in includes], '-c', source, '-o', obj])
        objects.append(obj)
    elf = out / 'smp-port-cost.elf'
    run('link', [a.lld, '-nostdlib', '-static', '--gc-sections', '-T', root / 'platform/qemu-smp/link.ld',
                 '-Map=' + str(out / 'smp-port-cost.map'), *objects, '-o', elf])
    runs = {}
    for cpu in CPUS:
        log = run(cpu, [a.qemu, *QEMU, '-cpu', cpu, '-device', f'loader,file={elf},cpu-num=0'])
        if 'PASS SMP port cost fixture' not in log or 'FAIL' in log:
            raise RuntimeError(cpu + ' run failed: ' + log[-2000:])
        m = {k: int(v, 16) for k, v in re.findall(r'MEASURE (\w+)=0x([0-9a-f]+)', log)}
        # Virtual time in nanoseconds; with shift=0 one instruction is one nanosecond.
        m['cross_core_round_trip_ns'] = (m['cross_core_virtual_ticks'] * 10**9 // m['counter_frequency']
                                         // m['cross_core_round_trips'])
        m['cross_core_round_trip_instructions'] = ((m['cross_core_core0_instructions'] +
                                                    m['cross_core_core1_instructions'])
                                                   // m['cross_core_round_trips'])
        runs[cpu] = m
    checked = {}
    if a.baseline:
        base = json.loads(a.baseline.read_text())['runs']
        for cpu, m in runs.items():
            for name in OPERATIONS:
                assert m[name] <= base[cpu][name], (cpu, name, m[name], base[cpu][name])
            ratio = m['cross_core_round_trip_instructions'] / base[cpu]['cross_core_round_trip_instructions']
            assert ratio <= 1 - ROUND_TRIP_REDUCTION, (cpu, 'cross-core round trip', ratio)
            checked[cpu] = {'baseline': {name: base[cpu][name] for name in OPERATIONS},
                            'round_trip_ratio': ratio,
                            'targets': {name: {'limit': limit, 'met': m[name] <= limit}
                                        for name, limit in TARGETS.items()}}
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'commands': commands, 'elf_sha256': digest(elf), 'qemu_sha256': digest(a.qemu),
        'clang_sha256': digest(a.clang), 'root': str(root),
        'inputs': {str(s): digest(s) for s in sources}, 'patches': {
            patch: digest(root / 'patches' / patch) for patches in PATCHES.values() for patch in patches},
        'runs': runs, 'baseline_comparison': checked,
        'scope': 'QEMU cortex-a55 and cortex-a76 cores with GICv3 and the FreeRTOS SMP port at EL1 and EL0, '
                 'without EL2 or EL3. Instruction counts are exact; virtual time is QEMU icount time, not A733 '
                 'cycles, and QEMU models no cache-line transfer between cores.'}, indent=2) + '\n')
    for cpu, m in runs.items():
        print(f'PASS {cpu}: critical pair EL1 {m["el1_critical_pair"]} EL0 {m["el0_critical_pair"]}, '
              f'current task EL1 {m["el1_current_task"]} EL0 {m["el0_current_task"]}, '
              f'notify {m["el1_notify_give_take"]}, same-core switch {m["el1_give_switch"]}, '
              f'cross-core round trip {m["cross_core_round_trip_instructions"]} instructions '
              f'({m["cross_core_round_trip_ns"]} ns virtual)')


if __name__ == '__main__':
    main()
