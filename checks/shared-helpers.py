#!/usr/bin/env python3
"""Shared range, deadline, MMIO, mapping-check and task-context helpers on host and AArch64/AArch32.

The host build runs checks/shared-helpers.c under ASan and UBSan at -O0 and
-O2. The AArch64 builds compile the task-context and MMIO probes against the
pinned FreeRTOS headers, once with the single-core SRE port and once with the
project's patched SMP port; the AArch32 build compiles the MMIO probes. The
disassembly must keep each helper's barrier placement and the task-context
order: entry checks, IRQ mask, counter reads, DAIF restore, scheduler calls.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (enforces assertions before any decision)

ROOT = Path(__file__).resolve().parents[1]
FREERTOS = harness.CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
SOURCES = ['checks/shared-helpers.c', 'checks/shared-helpers.py', 'include/a733_range.h',
           'include/a733_mmio.h', 'include/a733_mapping_check.h', 'include/a733_task_context.h',
           'include/aarch64_device_memory.h', 'patches/freertos-smp-macro.patch',
           'platform/a733-smp-cached/FreeRTOSConfig.h', 'include/FreeRTOSConfig.h']
WARN = ['-Wall', '-Wextra', '-Werror']
TASK_API = ['-DINCLUDE_xTaskGetSchedulerState=1', '-DINCLUDE_xTaskGetCurrentTaskHandle=1']


def run(command, log, out, **kw):
    r = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kw)
    (out / f'{log}.log').write_text(' '.join(map(str, command)) + '\n' + r.stdout + r.stderr)
    assert r.returncode == 0, f'{log} failed ({r.returncode}):\n{r.stdout[-2000:]}{r.stderr[-4000:]}'
    return r.stdout


def functions(objdump, obj, out, name):
    text = run([objdump, '-d', '--no-show-raw-insn', obj], name, out)
    result, current = {}, None
    for line in text.splitlines():
        m = re.match(r'^[0-9a-f]+ <(\w+)>:$', line)
        if m:
            current = result.setdefault(m.group(1), [])
            continue
        m = re.match(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$', line)
        if m and current is not None:
            current.append((m.group(1).lower(), m.group(2).strip().lower()))
    return result


def check_mmio(probes, arch):
    def shape(name):
        return ['dsb sy' if i == ('dsb', 'sy') else 'ldr' if i[0].startswith('ldr')
                else 'str' if i[0].startswith('str') else 'ret' if i[0] in ('ret', 'bx') else i[0]
                for i in probes[name]]
    for width in (8, 16, 32, 64):
        assert shape(f'probe_read{width}_both') == ['dsb sy', 'ldr', 'dsb sy', 'ret'], (arch, width)
        assert shape(f'probe_read{width}_after') == ['ldr', 'dsb sy', 'ret'], (arch, width)
        assert shape(f'probe_write{width}_both') == ['dsb sy', 'str', 'dsb sy', 'ret'], (arch, width)
        assert shape(f'probe_write{width}_after') == ['str', 'dsb sy', 'ret'], (arch, width)
    assert shape('probe_read32_plain') == ['ldr', 'ret'], arch
    assert shape('probe_write32_plain') == ['str', 'ret'], arch
    assert shape('probe_dsb_sy') == ['dsb sy', 'ret'], arch


def index(code, predicate, what):
    for i, instruction in enumerate(code):
        if predicate(instruction):
            return i
    raise AssertionError(f'missing {what}')


def check_task(probes, smp):
    mask = lambda i: i[0] == 'msr' and i[1].startswith('daifset, #0x2')
    restore = lambda i: i[0] == 'msr' and re.match(r'daif, x\d+$', i[1])
    tpidr = lambda i: i[0] == 'mrs' and i[1].endswith('tpidr_el1')
    call = lambda i: i[0] == 'bl'
    owner = probes['probe_task_owner']
    el = index(owner, lambda i: i[0] == 'mrs' and i[1].endswith('currentel'), 'CurrentEL read')
    daif = index(owner, lambda i: i[0] == 'mrs' and i[1].endswith('daif'), 'DAIF read')
    el_test = index(owner, lambda i: i[0] == 'cmp' and i[1].endswith('#0x4'), 'EL1 comparison')
    irq_test = index(owner, lambda i: i[0] == 'tbnz' and '#0x7' in i[1], 'entry IRQ-mask test')
    masked = index(owner, mask, 'IRQ mask')
    restored = index(owner, restore, 'DAIF restore')
    first_call = index(owner, call, 'scheduler call')
    assert el < masked and daif < masked and el_test < masked and irq_test < masked < restored < first_call
    assert sum(1 for i in owner if mask(i)) == 1
    if smp:
        assert masked < index(owner, tpidr, 'TPIDR_EL1 read') < restored
    any_task = probes['probe_task_any']
    assert not any(i[0] == 'mrs' and i[1].endswith('currentel') for i in any_task), 'any-task form reads CurrentEL'
    daif = index(any_task, lambda i: i[0] == 'mrs' and i[1].endswith('daif'), 'DAIF read')
    masked = index(any_task, mask, 'IRQ mask')
    restored = index(any_task, restore, 'DAIF restore')
    assert daif < masked < restored < index(any_task, call, 'scheduler call')
    if smp:
        assert masked < index(any_task, tpidr, 'TPIDR_EL1 read') < restored


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--freertos', type=Path, default=FREERTOS)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    clang = harness.CLANG
    objdump = clang.with_name('llvm-objdump-21') if clang.name == 'clang-21' else clang.with_name('llvm-objdump')
    source = ROOT / 'checks/shared-helpers.c'
    results = {}

    for level in ('-O0', '-O2'):
        exe = out / f'host{level}'
        run([clang, '-std=c11', level, '-g', *WARN, *harness.SANITIZE, f'-I{ROOT / "include"}',
             source, '-o', exe], f'host-build{level}', out)
        stdout = run([exe], f'host-run{level}', out, env=harness.environment())
        assert stdout.startswith('PASS shared helpers: '), stdout
        results[f'host{level}'] = stdout.strip()

    common = ['--target=aarch64-none-elf', '-march=armv8-a', '-ffreestanding', '-O2', *WARN, *TASK_API]
    sre = out / 'aarch64-sre.o'
    run([clang, *common, f'-I{ROOT / "include"}', f'-I{a.freertos / "include"}',
         f'-I{a.freertos / "portable/GCC/ARM_AARCH64_SRE"}', '-c', source, '-o', sre], 'aarch64-sre-build', out)
    probes = functions(objdump, sre, out, 'aarch64-sre-objdump')
    check_mmio(probes, 'aarch64')
    check_task(probes, smp=False)
    results['aarch64-sre'] = 'barrier placement and single-core task-context order'

    port = out / 'smp-port'
    port.mkdir()
    run(['patch', '--fuzz=0', '--reject-file=-', '--output', port / 'portmacro.h',
         a.freertos / 'portable/GCC/ARM_CR82/portmacro.h', ROOT / 'patches/freertos-smp-macro.patch'],
        'smp-port-patch', out)
    smp = out / 'aarch64-smp.o'
    run([clang, *common, f'-I{ROOT / "platform/a733-smp-cached"}', f'-I{ROOT / "include"}',
         f'-I{a.freertos / "include"}', f'-I{port}', '-c', source, '-o', smp], 'aarch64-smp-build', out)
    probes = functions(objdump, smp, out, 'aarch64-smp-objdump')
    check_mmio(probes, 'aarch64')
    check_task(probes, smp=True)
    results['aarch64-smp'] = 'barrier placement and per-core task-context order'

    a32 = out / 'aarch32.o'
    run([clang, '--target=arm-none-eabi', '-march=armv8-a', '-marm', '-ffreestanding', '-O2', *WARN,
         f'-I{ROOT / "include"}', '-c', source, '-o', a32], 'aarch32-build', out)
    check_mmio(functions(objdump, a32, out, 'aarch32-objdump'), 'aarch32')
    results['aarch32'] = 'barrier placement'

    inputs = {s: hashlib.sha256((ROOT / s).read_bytes()).hexdigest() for s in SOURCES}
    (out / 'result.json').write_text(json.dumps(dict(
        passed=True, physical_access=False, results=results, inputs=inputs,
        scope='Host predicates with sanitizers; AArch64/AArch32 instruction order of the barrier and '
              'task-context helpers. No board, emulator or FreeRTOS scheduler execution.'), indent=2) + '\n')
    print('PASS shared helpers: host -O0/-O2 sanitized; AArch64 SRE/SMP and AArch32 instruction order')


if __name__ == '__main__':
    main()
