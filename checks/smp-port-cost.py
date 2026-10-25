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
    core per round trip, with idle cores waiting in WFI, and in virtual time;
  - the same round trips between two EL0 tasks, whose sends keep the
    supervisor call;
  - an SGI that an EL1 task sends to its own core, from the send to the
    task's next instruction: IRQ entry, handler, the context-switch call at
    IRQ exit and the exception return;
  - an unpinned task that each tick on core 0 wakes while both cores are
    idle: how many of its wakes run on the tick's core, and the instructions
    both cores retire per wake; and the same wakes in a third image whose
    configuration gives the two cores different types
    (configCORES_SHARE_TYPE), where the other core's type takes precedence;
  - one uncontended read of the platform counter clock
    (platform/a733/counter_clock.c) from an EL1 task;
  - from a second image built with the SMP newlib runtime
    (runtime/newlib_smp.c), the C library state lookup __getreent from an EL1
    and an EL0 task.

With --baseline (a result.json from the base tree) it also requires that no
operation retires more instructions than in the baseline, that a cross-core
round trip retires at least --round-trip-reduction fewer instructions on both
cores (30% by default), and that an EL0 round trip retires at most 24 more
instructions per send than in the baseline, which pays for the task-register
and nesting test in front of the supervisor call
(docs/designs/cpu-a76-dvfs.md). It records,
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
              'el1_notify_give_take', 'el1_give_switch', 'cross_core_round_trip_instructions',
              'el1_self_sgi_round_trip', 'el1_counter_clock_now', 'el1_getreent', 'el0_getreent',
              'idle_wake_instructions')
# The newlib image mirrors the smp-runtime-model definitions (cmake/smp-runtime.cmake).
REENT_DEFINES = ['-DSUNSTONE_NEWLIB=1', '-DSUNSTONE_SMP_NEWLIB=1', '-D__DYNAMIC_REENT__=1',
                 '-DSMP_PORT_COST_REENT=1']
# As in that target, C compiles to general registers only, so the supervisor
# call that returns the state cannot overwrite a task's vector registers.
REENT_C_FLAGS = ['-mgeneral-regs-only']
ROUND_TRIP_REDUCTION = 0.30
# Instructions per send, two sends per round trip. The task-register and
# nesting test adds 5 instructions to the EL0 send; lock-wait interleaving
# under icount amplifies a longer send path (five no-ops in the base send
# added 10 instructions per EL0 send and 24 per EL1 send, the bound used).
EL0_SEND_ALLOWANCE = 24


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True, help='new directory for logs and results')
    p.add_argument('--root', type=Path, default=ROOT, help='source tree that supplies patches and platform')
    p.add_argument('--baseline', type=Path, help='result.json of the base tree; enables the comparison')
    p.add_argument('--round-trip-reduction', type=float, default=ROUND_TRIP_REDUCTION,
                   help='required fractional reduction of the EL1 cross-core round trip')
    p.add_argument('--clang', type=Path, default=Path('/usr/bin/clang-21'))
    p.add_argument('--lld', type=Path, default=CACHE / 'host/usr/bin/ld.lld-21')
    p.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    p.add_argument('--freertos', type=Path, default=FREERTOS)
    p.add_argument('--newlib-smp', type=Path, default=CACHE / 'host/newlib-smp-bd24e79bebf17c01',
                   help='newlib built by tools/build-newlib-smp.py')
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
        '#undef configUSE_PASSIVE_IDLE_HOOK\n#define configUSE_PASSIVE_IDLE_HOOK 1\n'
        '#ifdef SMP_PORT_COST_TWO_TYPES\n#define configCORES_SHARE_TYPE(a, b) ((a) == (b))\n#endif\n')
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
    kernel_sources = [root / 'platform/qemu-smp/start.S', root / 'platform/qemu-smp/platform.c',
                      root / 'runtime/memory.c', kernel / 'tasks.c', a.freertos / 'queue.c', a.freertos / 'list.c',
                      kernel / 'port.c', kernel / 'portASM.S', ROOT / 'checks/smp-port-cost.c']
    libc = a.newlib_smp / 'aarch64-none-elf'

    def build(name, sources, flags, c_flags, libraries, script='link.ld'):
        objects = []
        for source in sources:
            obj = out / (name + '-' + Path(source).stem + '.o')
            language = c_flags if Path(source).suffix == '.c' else []
            run('compile-' + obj.stem, [a.clang, *FLAGS, *flags, *language, *['-I' + str(i) for i in includes],
                                        '-c', source, '-o', obj])
            objects.append(obj)
        elf = out / (name + '.elf')
        run('link-' + name, [a.lld, '-nostdlib', '-static', '--gc-sections', '-T', root / 'platform/qemu-smp' / script,
                             '-Map=' + str(out / (name + '.map')), *objects, *libraries, '-o', elf])
        return elf

    sources = kernel_sources + [root / 'platform/a733/counter_clock.c']
    elf = build('smp-port-cost', sources, [], [], [])
    types_elf = build('smp-port-cost-types', sources, ['-DSMP_PORT_COST_TWO_TYPES=1'], [], [])
    reent_sources = kernel_sources + [root / 'runtime/newlib.c', root / 'runtime/newlib_smp.c',
                                      root / 'runtime/newlib_syscalls_smp.c']
    reent_elf = build('smp-port-cost-reent', reent_sources, [*REENT_DEFINES, '-isystem', libc / 'include'],
                      REENT_C_FLAGS, [libc / 'lib/libc.a'], 'runtime.ld')
    runs = {}
    for cpu in CPUS:
        log = run(cpu, [a.qemu, *QEMU, '-cpu', cpu, '-device', f'loader,file={elf},cpu-num=0'])
        if 'PASS SMP port cost fixture' not in log or 'FAIL' in log:
            raise RuntimeError(cpu + ' run failed: ' + log[-2000:])
        m = {k: int(v, 16) for k, v in re.findall(r'MEASURE (\w+)=0x([0-9a-f]+)', log)}
        log = run(cpu + '-types', [a.qemu, *QEMU, '-cpu', cpu, '-device', f'loader,file={types_elf},cpu-num=0'])
        if 'PASS SMP port cost fixture' not in log or 'FAIL' in log:
            raise RuntimeError(cpu + ' two-type run failed: ' + log[-2000:])
        types = {k: int(v, 16) for k, v in re.findall(r'MEASURE (\w+)=0x([0-9a-f]+)', log)}
        m['two_types_idle_wakes_on_tick_core'] = types['idle_wakes_on_tick_core']
        log = run(cpu + '-reent', [a.qemu, *QEMU, '-cpu', cpu, '-device', f'loader,file={reent_elf},cpu-num=0'])
        if 'PASS SMP port cost fixture' not in log or 'FAIL' in log:
            raise RuntimeError(cpu + ' newlib run failed: ' + log[-2000:])
        m.update({k: int(v, 16) for k, v in re.findall(r'MEASURE (\w+)=0x([0-9a-f]+)', log)})
        # Virtual time in nanoseconds; with shift=0 one instruction is one nanosecond.
        m['cross_core_round_trip_ns'] = (m['cross_core_virtual_ticks'] * 10**9 // m['counter_frequency']
                                         // m['cross_core_round_trips'])
        m['cross_core_round_trip_instructions'] = ((m['cross_core_core0_instructions'] +
                                                    m['cross_core_core1_instructions'])
                                                   // m['cross_core_round_trips'])
        m['idle_wake_instructions'] = ((m['idle_wake_core0_instructions'] + m['idle_wake_core1_instructions'])
                                       // m['idle_wakes'])
        m['el0_cross_core_round_trip_instructions'] = ((m['el0_cross_core_core0_instructions'] +
                                                        m['el0_cross_core_core1_instructions'])
                                                       // m['cross_core_round_trips'])
        runs[cpu] = m
    checked = {}
    if a.baseline:
        base = json.loads(a.baseline.read_text())['runs']
        for cpu, m in runs.items():
            # A baseline from an older fixture lacks the operations added since.
            for name in (n for n in OPERATIONS if n in base[cpu]):
                assert m[name] <= base[cpu][name], (cpu, name, m[name], base[cpu][name])
            ratio = m['cross_core_round_trip_instructions'] / base[cpu]['cross_core_round_trip_instructions']
            assert ratio <= 1 - a.round_trip_reduction, (cpu, 'cross-core round trip', ratio)
            el0 = 'el0_cross_core_round_trip_instructions'
            assert m[el0] <= base[cpu][el0] + 2 * EL0_SEND_ALLOWANCE, (cpu, el0, m[el0], base[cpu][el0])
            checked[cpu] = {'baseline': {name: base[cpu][name] for name in OPERATIONS + (el0,) if name in base[cpu]},
                            'round_trip_ratio': ratio,
                            'targets': {name: {'limit': limit, 'met': m[name] <= limit}
                                        for name, limit in TARGETS.items()}}
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'commands': commands, 'elf_sha256': digest(elf), 'reent_elf_sha256': digest(reent_elf),
        'types_elf_sha256': digest(types_elf),
        'newlib_libc_sha256': digest(libc / 'lib/libc.a'), 'qemu_sha256': digest(a.qemu),
        'clang_sha256': digest(a.clang), 'root': str(root),
        'inputs': {str(s): digest(s) for s in dict.fromkeys(sources + reent_sources)}, 'patches': {
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
              f'({m["cross_core_round_trip_ns"]} ns virtual), EL0 round trip '
              f'{m["el0_cross_core_round_trip_instructions"]}, self SGI {m["el1_self_sgi_round_trip"]}, '
              f'idle wake {m["idle_wake_instructions"]} with {m["idle_wakes_on_tick_core"]} of '
              f'{m["idle_wakes"]} on the tick core ({m["two_types_idle_wakes_on_tick_core"]} with two core types), '
              f'counter clock {m["el1_counter_clock_now"]}, __getreent EL1 {m["el1_getreent"]} '
              f'EL0 {m["el0_getreent"]}')


if __name__ == '__main__':
    main()
