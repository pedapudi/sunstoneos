#!/usr/bin/env python3
"""Measure and check the device-memory ledger on two QEMU cores with the FreeRTOS SMP port.

The image is checks/memory-architecture-smp.c on platform/qemu-smp, with the
SMP port patches plus the privileged-task patch, so every task runs at EL1.
Three runs of the current pool:

  measure   -icount shift=0: PMU instructions retired for describe, one xHCI
            register read, the camera memory check, the critical-section pair,
            the hold of a worst-case acquire, a release and a retain, and a
            local-pool getter's first-call hold and later call;
            nested-masking assertions; a short cross-core run.
  contend   multithreaded TCG: 200,000 rounds of release and retain on core 1
            against long acquires on core 0; entries attempted while the other
            core held an acquire are counted. Then 100,000 rounds in which
            core 0 releases a lease and gives its slot to another owner and
            extent while core 1 describes the released lease.
  trace     -d int,exec,nochain,in_asm: each measured call once between
            semihosting markers; the executed blocks give the SVC, DSB, ISB and
            DMB instructions per call.

Each --compare REVISION:LABEL builds the same image against the pool source
and headers at REVISION and runs measure and trace for the per-access calls.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-ffreestanding', '-fno-builtin',
         '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
         '-ffunction-sections', '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-Werror']
QEMU = ['-machine', 'virt,gic-version=3,secure=off,virtualization=off', '-cpu', 'cortex-a55', '-smp', '2',
        '-m', '128M', '-nographic', '-monitor', 'none', '-semihosting-config', 'enable=on,target=native']
PORT = 'portable/GCC/ARM_CR82'
PATCHES = {'port.c': ['freertos-smp-c.patch', 'freertos-smp-privileged.patch'],
           'portASM.S': ['freertos-smp-asm.patch'], 'portmacro.h': ['freertos-smp-macro.patch']}
OVERLAY = ['runtime/device_memory_pool.c', 'include/device_memory_pool.h', 'include/a733_memory_critical.h']


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Runner:
    def __init__(self, out, args):
        self.out, self.args, self.commands = out, args, []

    def run(self, name, command, timeout=600, check=True):
        command = [str(c) for c in command]
        self.commands.append(command)
        (self.out / 'commands.json').write_text(json.dumps(self.commands, indent=2) + '\n')
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
        (self.out / (name + '.log')).write_text(result.stdout)
        if check and result.returncode:
            raise RuntimeError(f'{name} failed ({result.returncode}): {result.stdout[-4000:]}')
        return result.stdout

    def compile(self, source, obj, includes, extra=()):
        self.run('compile-' + obj.stem, [self.args.clang, *FLAGS, *extra, *['-I' + str(i) for i in includes],
                                         '-c', source, '-o', obj])
        return obj


def mode_image(elf, mode, target):
    """Copy the ELF with fixture_mode set to `mode`."""
    data = bytearray(elf.read_bytes())
    with elf.open('rb') as stream:
        image = ELFFile(stream)
        symbol = image.get_section_by_name('.symtab').get_symbol_by_name('fixture_mode')[0]
        address = symbol['st_value']
        for section in image.iter_sections():
            if section['sh_type'] == 'SHT_PROGBITS' and section['sh_addr'] <= address < section['sh_addr'] + section['sh_size']:
                offset = section['sh_offset'] + address - section['sh_addr']
                break
        else:
            raise RuntimeError('fixture_mode is not in a loaded section')
    assert data[offset:offset + 4] == (0xaabbccdd).to_bytes(4, 'little')
    data[offset:offset + 4] = mode.to_bytes(4, 'little')
    target.write_bytes(data)
    return target


def measurements(output):
    found = {}
    for line in output.splitlines():
        match = re.match(r'MEASURE (\w+)=0x([0-9a-f]+)', line.strip())
        if match:
            found[match[1]] = int(match[2], 16)
    return found


# Encodings with every operand bit masked: SVC #imm, and DSB, DMB, ISB with any option.
BARRIERS = {'svc': (0xffe0001f, 0xd4000001), 'dsb': (0xfffff0ff, 0xd503309f),
            'dmb': (0xfffff0ff, 0xd50330bf), 'isb': (0xfffff0ff, 0xd50330df)}


def trace_filter(elf):
    """-dfilter ranges: all code except the boot wait loops, which would fill the log."""
    with elf.open('rb') as stream:
        image = ELFFile(stream)
        symbols = {s.name: s for s in image.get_section_by_name('.symtab').iter_symbols()}
        text = image.get_section_by_name('.text')
        skip = sorted([(symbols[n]['st_value'], symbols[n]['st_value'] + symbols[n]['st_size'])
                       for n in ('platform_init', 'platform_secondary', 'xPortStartScheduler')] +
                      [(symbols['_start']['st_value'], symbols['_freertos_vector_table']['st_value'])])
    ranges, at = [], text['sh_addr']
    for start, end in skip + [(text['sh_addr'] + text['sh_size'], None)]:
        if start > at:
            ranges.append(f'0x{at:x}+0x{start - at:x}')
        at = max(at, end or 0)
    return ','.join(ranges)


def trace_counts(log):
    """Instructions, SVC, DSB, ISB and DMB that CPU 0 executes between successive
    semihosting markers, from the translated blocks (in_asm) and their executions."""
    blocks, sections, counts, words, address = {}, [], None, None, None
    exec_line = re.compile(r'^Trace 0: \S+ \[[0-9a-f]+/([0-9a-f]+)/')
    with log.open(errors='replace') as stream:
        for line in stream:
            if line.startswith('IN:'):
                words, address = [], None
                continue
            if words is not None:
                if address is None and line.startswith('0x'):
                    address = int(line.split(':')[0], 16)
                    continue
                if line.startswith('OBJD-T:'):
                    data = bytes.fromhex(line.split(':', 1)[1].strip())
                    words += [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
                    continue
                blocks[address] = words
                words = None
            if 'Taking exception 16 [Semihosting call] on CPU 0' in line:
                counts = {'instructions': 0, **{name: 0 for name in BARRIERS}}
                sections.append(counts)
                continue
            match = exec_line.match(line)
            if match and counts is not None:
                for word in blocks[int(match[1], 16)]:
                    counts['instructions'] += 1
                    for name, (mask, value) in BARRIERS.items():
                        if word & mask == value:
                            counts[name] += 1
    return sections


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True)
    for name in ('clang', 'lld', 'qemu', 'freertos'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--compare', action='append', default=[], help='REVISION:LABEL of an earlier pool')
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    r = Runner(out, a)
    port = out / 'port'
    port.mkdir()
    for name, patches in PATCHES.items():
        source = a.freertos / PORT / name
        for patch in patches:
            target = port / (name + '.tmp')
            r.run('patch-' + patch, ['patch', '--fuzz=0', '--reject-file=-', '--output', target, source,
                                     ROOT / 'patches' / patch])
            source = port / name
            target.replace(source)
    common_includes = [ROOT / 'platform/qemu-smp', ROOT / 'include', ROOT / 'runtime/include',
                       a.freertos / 'include', port]
    objects = out / 'objects'
    objects.mkdir()
    common = [r.compile(src, objects / (Path(src).stem + '.o'), common_includes) for src in
              [ROOT / 'platform/qemu-smp/start.S', ROOT / 'platform/qemu-smp/platform.c',
               ROOT / 'runtime/memory.c', ROOT / 'arch/aarch64/device_memory.c',
               a.freertos / 'tasks.c', a.freertos / 'queue.c', a.freertos / 'list.c',
               port / 'port.c', port / 'portASM.S']]
    variants = [('current', None)] + [tuple(reversed(item.split(':', 1))) for item in a.compare]
    results = {}
    for label, revision in variants:
        work = out / label
        work.mkdir()
        includes = common_includes
        pool_source = ROOT / 'runtime/device_memory_pool.c'
        extra = ['-DFIXTURE_LEDGER'] if revision is None else []
        if revision is not None:
            overlay = work / 'overlay'
            for name in OVERLAY:
                shown = subprocess.run(['git', 'show', revision + ':' + name], cwd=ROOT, capture_output=True)
                if shown.returncode == 0:  # The base revision has no lock header.
                    (overlay / name).parent.mkdir(parents=True, exist_ok=True)
                    (overlay / name).write_bytes(shown.stdout)
            pool_source = overlay / 'runtime/device_memory_pool.c'
            includes = [overlay / 'include'] + common_includes
        variant_objects = [
            r.compile(pool_source, work / 'device_memory_pool.o', includes),
            r.compile(ROOT / 'checks/memory-architecture-smp.c', work / 'fixture.o', includes, extra),
            r.compile(ROOT / 'checks/memory-architecture-smp-xhci.c', work / 'xhci.o', includes,
                      ['-Wno-unused-function']),
            r.compile(ROOT / 'checks/memory-architecture-smp-camera.c', work / 'camera.o', includes,
                      ['-DSUNSTONE_CAMERA_MODEL', '-Wno-unused-function']),
        ]
        elf = work / 'memory-architecture-smp.elf'
        r.run('link-' + label, [a.lld, '-nostdlib', '-static', '--gc-sections',
              '--wrap=vTaskEnterCritical', '--wrap=vTaskExitCritical',
              '-T', ROOT / 'platform/qemu-smp/link.ld', '-Map=' + str(work / 'memory-architecture-smp.map'),
              *common, *variant_objects, '-o', elf])
        result = {'revision': revision or 'working tree', 'elf_sha256': digest(elf)}
        measure = r.run(label + '-measure', [a.qemu, *QEMU, '-icount', 'shift=0', '-device',
                        f'loader,file={mode_image(elf, 0, work / "measure.elf")},cpu-num=0'])
        if 'PASS memory-architecture SMP fixture' not in measure or 'FAIL' in measure:
            raise RuntimeError(label + ' measure run failed: ' + measure[-2000:])
        result['measure'] = measurements(measure)
        trace_log = work / 'trace.log'
        r.run(label + '-trace', [a.qemu, *QEMU, '-icount', 'shift=0', '-d', 'int,exec,nochain,in_asm',
              '-dfilter', trace_filter(elf), '-D', trace_log,
              '-device', f'loader,file={mode_image(elf, 2, work / "trace.elf")},cpu-num=0'])
        sections = trace_counts(trace_log)
        # One section follows each marker; the one after the first end marker
        # holds the ledger set-up between the two groups.
        names = ['describe', 'xhci_rd', 'camera_memory'] + ([None, 'acquire', 'release', 'retain', 'getter']
                                                            if revision is None else [])
        assert len(sections) >= len(names) + 1, sections
        result['trace'] = {name: counts for name, counts in zip(names, sections) if name}
        if revision is None:
            contend = r.run(label + '-contend', [a.qemu, *QEMU, '-accel', 'tcg,thread=multi', '-device',
                            f'loader,file={mode_image(elf, 1, work / "contend.elf")},cpu-num=0'], timeout=900)
            if 'PASS memory-architecture SMP fixture' not in contend or 'FAIL' in contend:
                raise RuntimeError('contend run failed: ' + contend[-2000:])
            result['contend'] = measurements(contend)
            assert result['contend']['contended_release_entries'] > 0, result['contend']
            assert result['contend']['contended_retain_entries'] > 0, result['contend']
            assert result['contend']['stale_describe_accepted'] > 0, result['contend']
            assert result['contend']['stale_describe_refused'] > 0, result['contend']
            for call in ('describe', 'xhci_rd', 'camera_memory', 'getter'):
                assert all(result['trace'][call][n] == 0 for n in ('svc', 'dsb', 'isb')), result['trace']
        results[label] = result
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'qemu_sha256': digest(a.qemu), 'clang_sha256': digest(a.clang),
        'variants': results,
        'scope': 'QEMU cortex-a55 cores with GICv3 and the FreeRTOS SMP port; instruction and barrier '
                 'counts, not cycles. No A733 core, cache, interconnect or board qualification.'},
        indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
