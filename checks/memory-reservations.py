#!/usr/bin/env python3
"""Check reserved allocation ownership, local pool preparation and opt-in native image layout."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    'include/device_memory_pool.h', 'runtime/device_memory_pool.c',
    'include/a733_memory_reservations.h', 'platform/a733/memory_reservations.c',
    'platform/a733/link.ld', 'checks/memory-reservations.c', 'checks/memory-reservations.py',
    'include/a733_dma.h', 'include/a733_dma_controller.h', 'include/a733_dma_native.h',
    'include/dma_memory.h', 'include/aarch64_device_memory.h', 'include/a733_mapping_check.h',
    'arch/aarch64/device_memory.c', 'include/a733_memory_critical.h',
    'include/a733_memory_uncached.h', 'platform/a733/memory_uncached.c',
    'include/a733_device_memory_layout.h', 'checks/memory-reservations-local.c',
    'include/a733_memory_local.h',
]
# Local-layout preparation cases: pool range, extra definitions, and the
# translations expected: one write translation per page of either pool, or,
# with the 2 MiB stride that images with 2 MiB block descriptors link, one
# per 2 MiB block that holds a pool byte.
LARGE_GRAPH = ['-DSUNSTONE_NPU_LARGE_GRAPH', '-DSUNSTONE_NPU_SINGLE_CORE', '-DSUNSTONE_NPU_RUNTIME_INPUT',
               '-DSUNSTONE_NPU_INTERRUPT', '-DSUNSTONE_NPU_SUPERVISED']
BLOCKS = ['-DTEST_POOL_STRIDE=0x200000']
LOCAL_CASES = {
    'unaligned-2mib': (0x41500000, 0x41700000, [], 512 + 512),
    'large-graph': (0x41600000, 0x41a00000, LARGE_GRAPH, 1024 + 512),
    'unaligned-2mib-blocks': (0x41500000, 0x41700000, BLOCKS, 2 + 1),
    'aligned-2mib-blocks': (0x41a00000, 0x41c00000, BLOCKS, 1 + 1),
    'large-graph-blocks': (0x41600000, 0x41a00000, LARGE_GRAPH + BLOCKS, 2 + 1),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf(path):
    with path.open('rb') as stream:
        image = ELFFile(stream)
        sections = {s.name: {'address': s['sh_addr'], 'size': s['sh_size'],
                    'type': s['sh_type'], 'flags': s['sh_flags'],
                    'sha256': hashlib.sha256(s.data()).hexdigest()}
                    for s in image.iter_sections() if s['sh_flags'] & 2 and s['sh_size']}
        symbols = {s.name: s['st_value'] for s in image.get_section_by_name('.symtab').iter_symbols()}
        loads = [(s['p_vaddr'], s['p_filesz'], s['p_memsz'], s['p_flags'])
                 for s in image.iter_segments() if s['p_type'] == 'PT_LOAD']
    return sections, symbols, loads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=('all', 'host', 'native', 'link', 'capacity'), default='all')
    parser.add_argument('--baseline-revision', default='a44566a42e3c2b21dda3f1eae2744bb13d4a06aa')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = {}
    for name in SOURCES:
        target = out / 'inputs' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, target)
        inputs[name] = digest(target)
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    commands = []
    def run(name, command, success=True):
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (name + '.log')).write_text(result.stdout)
        if success and result.returncode:
            raise RuntimeError(f'{name} failed: {result.stdout}')
        if not success and not result.returncode:
            raise RuntimeError(f'{name} unexpectedly succeeded')
        return result.stdout
    clang = shutil.which('clang-21')
    lld = Path.home() / '.cache/sunstoneos/host/usr/bin/ld.lld-21'
    common = [clang, '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Iinclude']
    runtime = ['runtime/device_memory_pool.c']
    results = {}
    try:
        if args.mode in ('all', 'host'):
            binary = out / 'ownership'
            run('host-build', common + ['-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-DDEVICE_MEMORY_POOL_TRACE', 'checks/memory-reservations.c'] + runtime + ['-o', str(binary)])
            # Record executable identity before executing the candidate.
            (out / 'executed.json').write_text(json.dumps({'path': str(binary), 'sha256': digest(binary)}, indent=2) + '\n')
            results['host'] = run('ownership', [str(binary)]).strip()
            results['local'] = {}
            for name, (start, end, extra, expected) in LOCAL_CASES.items():
                local = out / ('local-' + name)
                symbols = {'__device_pool_start': start, '__device_pool_end': end, '__heap_end': 0x41400000,
                           '__uncached_pool_start': 0x41c00000, '__uncached_pool_end': 0x41e00000}
                run('local-build-' + name, common + ['-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-no-pie', '-DPREPARED_POOLS', f'-DPOOL_START={start:#x}', f'-DPOOL_END={end:#x}', *extra,
                    'checks/memory-reservations-local.c', 'platform/a733/memory_reservations.c',
                    'platform/a733/memory_uncached.c'] + runtime + ['-o', str(local)] +
                    [f'-Wl,--defsym={k}={v:#x}' for k, v in symbols.items()])
                text = run('local-' + name, [str(local)])
                counts = dict(item.split('=') for item in text.split('\n')[0].split()[1:])
                assert int(counts['total']) == expected, counts
                fault = run('local-fault-' + name, [str(local), hex(end - 1 >> 21)])
                assert 'refuses its pool' in fault, fault
                results['local'][name] = {'translations': {k: int(v) for k, v in counts.items()},
                                          'expected': expected}
        if args.mode in ('all', 'native'):
            native = runtime + ['platform/a733/memory_reservations.c', 'platform/a733/memory_uncached.c',
                                'arch/aarch64/device_memory.c']
            objects = []
            freestanding = common + ['--target=aarch64-none-elf', '-ffreestanding',
                                     '-fno-builtin', '-mstrict-align', '-mgeneral-regs-only']
            # Without the kernel headers a native ledger compiles only as an
            # explicit one-thread fixture.
            unlocked = run('native-without-kernel', freestanding + ['-fsyntax-only', runtime[0]], success=False)
            assert 'DEVICE_MEMORY_POOL_SINGLE_THREAD' in unlocked, unlocked
            for source in native:
                obj = out / (Path(source).stem + '.o')
                run(obj.stem, freestanding + ['-DDEVICE_MEMORY_POOL_SINGLE_THREAD', '-fstack-usage',
                    '-c', source, '-o', str(obj)])
                objects.append({'source': source, 'object': str(obj), 'sha256': digest(obj)})
            results['native_objects'] = objects
        if args.mode in ('all', 'link', 'capacity'):
            fixture = out / 'layout.S'
            fixture.write_text('''.section .text.boot.monitor,"ax"
.global monitor_entry
monitor_entry: ret
.section .rodata,"a"
.byte 17, 23, 42
.section .data,"aw"
.quad 0x12345678
.section .monitor_state,"aw"
.quad 1
.section .bss,"aw",@nobits
.balign 4096
.skip 8192
''')
            obj = out / 'layout.o'
            run('layout-compile', [clang, '--target=aarch64-none-elf', '-c', str(fixture), '-o', str(obj)])
            baseline = out / 'baseline.ld'
            baseline.write_bytes(subprocess.check_output(['git', 'show', args.baseline_revision + ':platform/a733/link.ld'], cwd=ROOT))
            def link(name, script, pool=None, heap=4194304, success=True):
                cmd = [str(lld), '-T', str(script), str(obj), '--defsym=__runtime_heap_size=' + str(heap),
                       '-Map=' + str(out / (name + '.map')), '-o', str(out / (name + '.elf'))]
                if pool is not None:
                    cmd.append('--defsym=__device_pool_size=' + str(pool))
                return run(name, cmd, success)
            current = ROOT / 'platform/a733/link.ld'
            if args.mode == 'capacity':
                link('capacity', current, 2097152)
                _, symbols, _ = elf(out / 'capacity.elf')
                assert symbols['__device_pool_end'] - symbols['__device_pool_start'] == 2097152
                assert symbols['__device_pool_end'] <= 0x41f00000
                assert 'registry capacity' in link('too-large', current, 2097152 + 4096, success=False)
                results['capacity'] = {'accepted_bytes': 2097152, 'rejected_bytes': 2097152 + 4096}
            else:
                link('baseline', baseline)
                link('default', current)
                old_sections, old_symbols, old_loads = elf(out / 'baseline.elf')
                sections, symbols, loads = elf(out / 'default.elf')
                assert sections == old_sections, (old_sections, sections)
                assert old_loads == loads, (old_loads, loads)
                assert all(symbols[k] == v for k, v in old_symbols.items()), 'existing symbol moved'
                link('reserved', current, 65536)
                reserved_sections, reserved_symbols, _ = elf(out / 'reserved.elf')
                start, end = reserved_symbols['__device_pool_start'], reserved_symbols['__device_pool_end']
                assert not (start & 4095) and end - start == 65536
                assert start >= reserved_symbols['__heap_end'] and end <= 0x41f00000
                assert reserved_sections.pop('.device_pool')['type'] == 'SHT_NOBITS'
                assert reserved_sections == old_sections, 'existing allocated sections changed'
                assert 'complete pages' in link('partial-page', current, 4097, success=False)
                assert 'registry capacity' in link('too-large', current, 2097152 + 4096, success=False)
                assert 'reservation' in link('outside-image', current, 1048576, heap=15 * 1024 * 1024, success=False)
                results['link'] = {'default_matches_baseline': args.baseline_revision,
                                   'start': start, 'end': end, 'pool_bytes': end - start,
                                   'default_loads': loads, 'baseline_linker_sha256': digest(baseline)}
        assert inputs == {name: digest(ROOT / name) for name in SOURCES}, 'inputs changed during check'
        results['status'] = 'pass'
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
