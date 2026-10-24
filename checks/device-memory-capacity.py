#!/usr/bin/env python3
"""Check pool placement and class alignment, the EL1 map in both layouts, and pool preparation.

The extended-layout fixture runs on eight QEMU cores with the DRAM decode check
failing (the map below 0x80000000) and passing (DRAM up to 0x340000000). The
local-layout fixture runs on one core with the production cached linker script,
with and without the uncached pool and the read-only GPU output view. Execution
traces between semihosting markers count instructions, AT, DC, TLBI and
barriers per traced call. --images reads the symbol tables of built images.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402  Refuses to run with assertions disabled.


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


# Writers of the cached EL1 and EL3 tables outside their builder. The golden
# comparison sees the builder's tables; a later writer may only set the
# outer-shareable field of level-1 entry 0 (MMIO), which leaves every pool
# entry, and so the per-block pool check, unchanged.
BUILDERS = {'platform/a733-smp-cached/memory.c'}
LATER_WRITE = re.compile(r'a733_cached_el[13]_l1\[0\]\|=UINT64_C\(2\)<<8;')


DC_CRM = {4, 6, 10, 11, 12, 13, 14}
COUNTED = ['at', 'dc', 'tlbi', *harness.BARRIERS]


def classify(word):
    """Instruction classes counted per traced call: AT, DC, TLBI and barriers."""
    found = harness.barriers(word)
    if word & 0xfff80000 == 0xd5080000:  # SYS, op0 = 1
        crn, crm = (word >> 12) & 15, (word >> 8) & 15
        if crn == 7 and crm in (8, 9):
            found.append('at')
        elif crn == 7 and crm in DC_CRM:
            found.append('dc')
        elif crn == 8:
            found.append('tlbi')
    return found


def labelled(output, log):
    """Each traced call's counts, keyed by the marker printed before it."""
    lines = [l.strip() for l in output.splitlines() if re.match(r'(TRACE|MEASURE|PASS|FAIL)', l.strip())]
    sections = harness.trace_sections(log, classify, COUNTED)
    if len(sections) != len(lines) + 1:  # The last call is the exit.
        raise RuntimeError(f'{len(sections)} semihosting calls for {len(lines)} lines')
    return {line: counts for line, counts in zip(lines, sections) if line.startswith('TRACE')}


def minus(a, b):
    return {k: a[k] - b[k] for k in a}


POOL_PREPARATIONS = ('a733_memory_reservations_prepare', 'a733_memory_uncached_prepare')


def branches(parts):
    """(site, target) of every B and BL word in the executable parts."""
    words = {}
    for base, data in parts:
        for index, (word,) in enumerate(struct.iter_unpack('<I', data[:len(data) & ~3])):
            words[base + 4 * index] = word
    found = []
    for site in harness.instruction_sites(parts, lambda word: word & 0x7c000000 == 0x14000000):
        offset = words[site] & 0x3ffffff
        found.append((site, site + 4 * (offset - ((offset & 0x2000000) << 1))))
    return found


def scan_images(directories):
    """Symbol-table and call facts of every built image: which decode check
    and pool stride definitions it links, and where it calls the pool
    preparations. An image that links local pools calls each linked
    preparation; an extended image calls them only from
    a733_device_memory_prepare(), whose binding state orders them."""
    images = {}
    for directory in directories:
        for path in sorted(Path(directory).rglob('*.elf')):
            with path.open('rb') as f:
                try:
                    elf = ELFFile(f)
                except Exception:
                    continue
                if elf['e_machine'] != 'EM_AARCH64' or not elf.get_section_by_name('.symtab'):
                    continue
                symbols = {}
                for sym in elf.get_section_by_name('.symtab').iter_symbols():
                    if sym['st_shndx'] != 'SHN_UNDEF' and sym.name:
                        symbols[sym.name] = sym
                parts = [(seg['p_vaddr'], seg.data()) for seg in elf.iter_segments()
                         if seg['p_type'] == 'PT_LOAD' and seg['p_flags'] & 1]
            builder = 'a733_cached_memory_prepare' in symbols
            extended = 'a733_device_memory_prepare' in symbols
            # The pool sources define the preparations strongly; the weak
            # definitions of a733_memory_local.h stand for absent pools.
            linked = {symbols[n]['st_value']: n for n in POOL_PREPARATIONS if n in symbols}
            strong = {a for a, n in linked.items() if symbols[n]['st_info']['bind'] == 'STB_GLOBAL'}
            decode = symbols.get('a733_dram_decode_check')
            stride = symbols.get('a733_local_pool_stride')
            record = {'cached_builder': builder, 'extended': extended, 'local_pools': bool(strong) and not extended,
                      'decode_check': None if decode is None else decode['st_info']['bind'],
                      'pool_stride': None if stride is None else stride['st_info']['bind']}
            if builder and record['decode_check'] is None:
                raise RuntimeError(f'{path}: cached builder without a decode check')
            calls = [(site, target) for site, target in branches(parts) if target in linked]
            if extended:
                start = symbols['a733_device_memory_prepare']['st_value']
                end = start + symbols['a733_device_memory_prepare']['st_size']
                outside = sorted({f'{linked[t]} at {s:#x}' for s, t in calls if not start <= s < end})
                if outside:
                    raise RuntimeError(f'{path}: extended image calls a pool preparation outside '
                                       f'a733_device_memory_prepare: {", ".join(outside)}')
                record['preparation_calls'] = len(calls)
            elif strong:
                # A linked local preparation is called: the getters no longer prepare.
                called = {t for _, t in calls}
                if not strong <= called:
                    raise RuntimeError(f'{path}: a linked local pool preparation is never called')
                record['preparation_calls'] = len(calls)
            images[str(path)] = record
    return images


def later_table_writers():
    files = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '*.c', '*.cc', '*.h', '*.S', '*.patch'],
                           check=True, capture_output=True, text=True).stdout.split()
    write = re.compile(r'a733_cached_el[13]_l[123]\s*\[[^]]*\]\s*([|&^+-]?=)(?!=)')
    found = []
    for name in files:
        if name in BUILDERS or name.startswith('checks/'):
            continue
        for number, line in enumerate((ROOT / name).read_text(errors='replace').splitlines(), 1):
            if name.endswith('.patch') and line.startswith('-'):
                continue
            if write.search(line):
                if not LATER_WRITE.search(line.replace(' ', '')):
                    raise RuntimeError(f'table write outside the builder touches more than l1[0] SH: {name}:{number}')
                found.append(f'{name}:{number}')
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reuse-production', type=Path)
    p.add_argument('--images', type=Path, action='append', default=[],
                   help='build directory whose ELF images are scanned')
    for name in ('clang', 'ld', 'qemu', 'freertos'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    writers = later_table_writers()
    objects = out / 'objects'
    objects.mkdir()
    commands = []
    dependencies = {ROOT / 'checks/device-memory-capacity.py',
                    ROOT / 'platform/a733-smp-cached/link-devices.ld'}
    environment = os.environ.copy()
    environment['PATH'] = str(a.clang.resolve().parent) + os.pathsep + environment.get('PATH', '')
    qemu_lib = a.qemu.resolve().parent.parent / 'lib/x86_64-linux-gnu'
    if qemu_lib.exists():
        environment['LD_LIBRARY_PATH'] = str(qemu_lib) + ':' + environment.get('LD_LIBRARY_PATH', '')

    def run(name, command, timeout=60):
        commands.append(command)
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        result = subprocess.run(command, cwd=ROOT, env=environment, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        (out / (name + '.log')).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f'{name} failed: {result.stdout[-4000:]}')

    def compile_source(source, name, host=False, extended=True, extra=()):
        source = ROOT / source
        obj = objects / (name + '.o')
        cmd = [str(a.clang.resolve()), '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-I' + str(ROOT / 'platform/a733-smp-cached'), '-I' + str(ROOT / 'include')]
        if source.suffix == '.c':
            cmd += ['-std=c11']
        if host:
            cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        else:
            cmd += ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align',
                    '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
                    '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections',
                    '-I' + str(ROOT / 'runtime/include'), '-I' + str(a.freertos.resolve() / 'include'),
                    '-I' + str(a.port.resolve()), '-DSUNSTONE_SECURE_MONITOR=1',
                    '-DSUNSTONE_A733_SMP_DIAGNOSTIC=1']
            if source.suffix == '.c':
                cmd += ['-mgeneral-regs-only']
        if extended:
            cmd += ['-DSUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1']
        cmd += list(extra) + ['-MMD', '-MF', str(obj.with_suffix('.d')), '-c', str(source), '-o', str(obj)]
        run('compile-' + name, cmd)
        dependencies.add(source)
        data = obj.with_suffix('.d').read_text().replace('\\\n', ' ')
        dependencies.update(Path(x).resolve() for x in shlex.split(data.split(':', 1)[1]))
        return obj

    # Build the actual large image once. Reuse its native objects in the
    # architectural fixture; only the fixture entry/control units differ.
    production = a.reuse_production.resolve() if a.reuse_production else out / 'production'
    if a.reuse_production:
        saved = json.loads((production / 'a733-smp-cached-devices/manifest.json').read_text())
        if saved['compiler_sha256'] != digest(a.clang.resolve()) or saved['linker_sha256'] != digest(a.ld.resolve()):
            raise RuntimeError('reused production toolchain differs')
        if digest(production / 'sunstone-a733-smp-cached-devices.elf') != saved['final_elf']:
            raise RuntimeError('reused production ELF identity differs')
        for name, expected in saved['inputs'].items():
            if digest(Path(name)) != expected:
                raise RuntimeError('reused production input differs: ' + name)
    else:
        run('configure-production', ['cmake', '-S', str(ROOT / 'platform/a733-smp-cached-devices'),
            '-B', str(production), '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=SUNSTONE_LLD', '-DFREERTOS_DIR=' + str(a.freertos.resolve()),
            '-DSUNSTONE_LLD=' + str(a.ld.resolve()), '-DCMAKE_C_COMPILER=' + str(a.clang.resolve()),
            '-DCMAKE_ASM_COMPILER=' + str(a.clang.resolve())])
    configured_compilers = {}
    for language in ('C', 'ASM'):
        records = list((production / 'CMakeFiles').glob('*/CMake' + language + 'Compiler.cmake'))
        if len(records) != 1:
            raise RuntimeError('missing or ambiguous configured compiler record')
        text = records[0].read_text()
        match = re.search(r'set\(CMAKE_' + language + r'_COMPILER "([^"\n]+)"\)', text)
        if not match or Path(match[1]).resolve() != a.clang.resolve():
            raise RuntimeError('production compiler differs from requested fixture compiler')
        actual = Path(match[1]).resolve()
        if digest(actual) != digest(a.clang.resolve()):
            raise RuntimeError('production compiler identity changed')
        configured_compilers[language] = {'path': str(actual), 'sha256': digest(actual)}
        dependencies.add(records[0])
    (out / 'configured-compilers.json').write_text(json.dumps(configured_compilers, indent=2) + '\n')
    if not a.reuse_production:
        run('build-production', ['cmake', '--build', str(production), '--target',
            'sunstone-a733-smp-cached-devices', '-j4'])
    a.port = production / 'freertos-a733-smp-cached'
    produced = {}
    for entry in json.loads((production / 'compile_commands.json').read_text()):
        command = shlex.split(entry['command'])
        obj = Path(entry['directory']) / command[command.index('-o') + 1]
        if 'sunstone-a733-smp-cached-devices.dir' not in str(obj):
            continue
        produced[Path(entry['file']).resolve()] = obj
        dep = Path(str(obj) + '.d')
        if dep.exists():
            data = dep.read_text().replace('\\\n', ' ')
            dependencies.update(Path(x).resolve() for x in shlex.split(data.split(':', 1)[1]))
        elif Path(entry['file']).suffix != '.S':
            raise RuntimeError('missing production C dependency file: ' + str(dep))
    production_manifest = production / 'a733-smp-cached-devices/manifest.json'
    dependencies.update([production_manifest, production / 'compile_commands.json',
        ROOT / 'platform/a733-smp-cached-devices/CMakeLists.txt',
        ROOT / 'cmake/a733-smp-cached-devices.cmake'])
    dependencies.update(Path(x) for x in json.loads(production_manifest.read_text())['inputs'])
    # The host links only pure layout predicates; admission still takes its real v2 branch.
    host = [compile_source('checks/device-memory-capacity.c', 'host-check', True, False,
                ['-DCAPACITY_HOST=1', '-DDEVICE_MEMORY_POOL_TRACE']),
            compile_source('runtime/device_memory_pool.c', 'host-pool', True, False, ['-DDEVICE_MEMORY_POOL_TRACE']),
            compile_source('platform/a733/device_memory_layout.c', 'host-layout', True, False),
            compile_source('platform/a733-smp-cached/admission.c', 'host-admission', True)]
    executable = out / 'capacity-host'
    run('link-host', [str(a.clang.resolve()), '-fsanitize=address,undefined', *map(str, host), '-o', str(executable)])
    run('host', [str(executable)])
    native_sources = ['checks/device-memory-capacity.c', 'checks/device-memory-capacity.S',
                      'runtime/device_memory_pool.c', 'runtime/memory.c',
                      'platform/a733/device_memory_layout.c', 'platform/a733/memory_reservations.c',
                      'platform/a733/memory_uncached.c', 'platform/a733-smp-cached/memory.c',
                      'platform/a733-smp-cached/admission.c', 'platform/a733-smp-cached/platform.c',
                      'platform/a733/cpu_start.c', 'platform/a733/cpu_coherence.c',
                      'platform/a733/cpu_coherence.S', 'arch/aarch64/device_memory.c']
    native = [produced[(ROOT / source).resolve()] if (ROOT / source).resolve() in produced else
              compile_source(source, 'native-' + str(i)) for i, source in enumerate(native_sources)]
    # The measured image calls the translation helper directly. A second
    # image wraps the helper to count AT instructions; the wrapper's own
    # instructions stay out of the measured image.
    elf_path = out / 'device-memory-capacity.elf'
    counting_path = out / 'device-memory-capacity-counting.elf'
    for name, path, extra in (('link-native', elf_path, []),
                              ('link-counting', counting_path, ['--wrap=aarch64_device_translate'])):
        run(name, [str(a.ld.resolve()), '-static', '--gc-sections', *extra, '-T',
            str(ROOT / 'platform/a733-smp-cached/link-devices.ld'), '-Map=' + str(path.with_suffix('.map')),
            *map(str, native), '-o', str(path)])
    # Compile real startup callers and direct/transitive reservation consumers once.
    closure = ['platform/a733/gpu_reserved.c',
               'platform/a733/gpu_compute_reserved.c', 'platform/a733/npu_reserved.c',
               'drivers/crypto/a733_ce_native.c', 'drivers/crypto/a733_ce_session.c',
               'drivers/crypto/a733_ce_entropy.c', 'drivers/crypto/a733_ce_hash.c',
               'drivers/crypto/a733_ce_cipher.c']
    for i, source in enumerate(closure):
        compile_source(source, 'consumer-' + str(i))
    # Changed conditional native files also compile with the ordinary profile.
    for i, source in enumerate(['platform/a733/memory_reservations.c', 'platform/a733/memory_uncached.c',
            'platform/a733-smp-cached/memory.c', 'platform/a733-smp-cached/admission.c',
            'platform/a733-smp-cached/boot.c', 'platform/a733-smp-cached/continue.c']):
        compile_source(source, 'compact-' + str(i), extended=False)
    extended_layout = {'__device_memory_profile': 1, '__device_pool_start': 0x48000000,
                       '__device_pool_end': 0x78000000, '__uncached_pool_start': 0x78000000,
                       '__uncached_pool_end': 0x79000000}

    def load_segments(image, prefix, expected):
        with image.open('rb') as f:
            elf = ELFFile(f)
            symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            assert all(symbols[k] == v for k, v in expected.items())
            assert symbols['__bss_end'] < 0x42000000 and symbols['__tables_end'] <= 0x42200000
            # Translation-table memory: five pages and the sum line.
            assert symbols['__tables_end'] - symbols['__tables_start'] == 5 * 4096 + 64
            for name in ('.device_pool', '.uncached_pool'):
                section = elf.get_section_by_name(name)  # An absent local pool is empty.
                assert section['sh_type'] == 'SHT_NOBITS' or (not expected and not section['sh_size'])
            segments = []
            for i, seg in enumerate(elf.iter_segments()):
                if seg['p_type'] != 'PT_LOAD' or not seg['p_filesz']:
                    continue
                assert 0x41000000 <= seg['p_paddr'] < seg['p_paddr'] + seg['p_filesz'] <= 0x42000000
                path = out / (prefix + 'segment-' + str(i) + '.bin')
                path.write_bytes(seg.data())
                segments.append((path, int(seg['p_paddr'])))
        return symbols, segments, {k: symbols[k] for k in expected}
    images = {'measured': load_segments(elf_path, '', extended_layout),
              'counting': load_segments(counting_path, 'counting-', extended_layout)}
    (out / 'layout.json').write_text(json.dumps(images['measured'][2], indent=2) + '\n')
    # Local layout: the production cached linker script with its pool sizes,
    # one image per pool set; the fixture runs on one core.
    local_sources = ['checks/device-memory-capacity-local.c', 'checks/device-memory-capacity.S',
                     'runtime/device_memory_pool.c', 'runtime/memory.c', 'platform/a733/memory_reservations.c',
                     'platform/a733/memory_uncached.c', 'platform/a733-smp-cached/memory.c',
                     'platform/a733/cpu_start.c', 'platform/a733/cpu_coherence.c',
                     'platform/a733/cpu_coherence.S', 'arch/aarch64/device_memory.c']
    local_variants = {'local-uncached': (2097152, 2097152, []),
                      'local-write-back-only': (65536, 0, []),
                      'local-read-only-view': (2097152, 2097152, ['-DA733_GPU_OUTPUT_EL0_READ_ONLY=1'])}
    local_script = ROOT / 'platform/a733-smp-cached/link.ld'
    dependencies.add(local_script)
    for name, (wb_bytes, nc_bytes, extra) in local_variants.items():
        objs = [compile_source(source, name + '-' + str(i), extended=False,
                               extra=['-DDEVICE_MEMORY_POOL_SINGLE_THREAD', *extra])
                for i, source in enumerate(local_sources)]
        path = out / (name + '.elf')
        # GNU ld evaluates DEFINED() in the script only for symbols defined
        # before it, so the pool sizes come first.
        run('link-' + name, [str(a.ld.resolve()), '-static', '--gc-sections',
            '--defsym=__device_pool_size=' + str(wb_bytes), '--defsym=__uncached_pool_size=' + str(nc_bytes),
            '-T', str(local_script),
            '-Map=' + str(path.with_suffix('.map')), *map(str, objs), '-o', str(path)])
        images[name] = load_segments(path, name + '-', {})
    snapshots = {}
    for i, path in enumerate(sorted(dependencies)):
        target = out / 'inputs' / str(i) / path.name
        target.parent.mkdir(parents=True)
        shutil.copy2(path, target)
        snapshots[str(path)] = {'sha256': digest(target), 'snapshot': str(target.relative_to(out))}
    (out / 'inputs.json').write_text(json.dumps(snapshots, indent=2) + '\n')
    rom = out / 'reset-rom.bin'
    rom.write_bytes(struct.pack('<IIQ', 0x58000040, 0xd61f0000, 0x41000000))

    def boot(label, image, mode, cores, trace):
        symbols, segments, _ = images[image]
        loaders = []
        controls = 0
        for original, address in segments:
            data = bytearray(original.read_bytes())
            path = original
            if address <= symbols['fixture_mode'] < address + len(data):
                offset = symbols['fixture_mode'] - address
                assert offset + 4 <= len(data)
                struct.pack_into('<I', data, offset, mode)
                path = out / (label + '-' + original.name)
                path.write_bytes(data)
                controls += 1
            loaders += ['-device', f"loader,file={path},addr={address},force-raw=on"]
        assert controls == 1
        log = out / ('trace-' + label + '.log')
        tracing = ['-d', 'int,exec,nochain,in_asm', '-D', str(log)] if trace else []
        run('native-' + label, [str(a.qemu.resolve()), '-machine', 'virt,secure=on,virtualization=on',
            '-cpu', 'cortex-a53', '-smp', str(cores), *tracing, '-icount', 'shift=0', '-m', '1024M',
            '-nographic', '-monitor', 'none', '-serial', 'none', '-semihosting-config', 'enable=on,target=native',
            '-bios', str(rom), *loaders], timeout=900)
        output = (out / ('native-' + label + '.log')).read_text()
        found = {}
        for line in output.splitlines():
            if line.startswith('MEASURE '):
                key, value = line[8:].split('=')
                found[key] = int(value, 16)
        return found, labelled(output, log) if trace else None

    # Extended layout: full publication, an interior mapping fault and a
    # changed admitted deadline with the decode check failing, publication
    # with it passing, and the counting image. Modes 5 and 6 trace the table
    # builder with the check passing and failing. -icount makes the PMU
    # instruction counter exact.
    measurements, traces = {}, {}
    cases = (('publication', 0, 'measured', 8, False), ('interior-fault', 1, 'measured', 8, False),
             ('deadline-binding', 2, 'measured', 8, False), ('translation-count', 3, 'counting', 8, False),
             ('publication-decode', 4, 'measured', 8, False),
             ('builder-trace-decode', 5, 'measured', 1, True), ('builder-trace', 6, 'measured', 1, True))
    cases += tuple((name + ('-decode' if mode == 0 else ''), mode, name, 1, True)
                   for name in local_variants for mode in (0, 1))
    for label, mode, image, cores, trace in cases:
        measurements[label], traced = boot(label, image, mode, cores, trace)
        if traced is not None:
            traces[label] = traced
    assert measurements['translation-count']['prepare_translations'] == 392
    assert measurements['publication']['prepare_instructions'] <= 25000
    budgets = {}

    def builder(label):
        """The table builder's counts with the decode check's own counts removed."""
        t = traces[label]
        total = t['TRACE prepare 0000000000000000' if 'TRACE prepare 0000000000000000' in t else 'TRACE prepare']
        decode = t.get('TRACE decode 0000000000000000', t.get('TRACE decode'))
        after = t.get('TRACE decode end 0000000000000000', t.get('TRACE decode end'))
        return {k: total[k] + after[k] for k in total}, decode
    for passed, failed in [('builder-trace-decode', 'builder-trace')] + \
            [(name + '-decode', name) for name in local_variants]:
        with_map, _ = builder(passed)
        without_map, _ = builder(failed)
        added = minus(with_map, without_map)
        # The new entries cost stores only: no AT, cache maintenance, TLBI or
        # barrier, and at most 2,000 instructions above the decode-fail path.
        assert all(added[k] == 0 for k in ('at', 'dc', 'tlbi', 'dsb', 'dmb', 'isb')), added
        assert 0 <= added['instructions'] <= 2000, added
        assert with_map['at'] == 0 and with_map['tlbi'] == 0, with_map
        budgets['builder ' + failed] = {'with_map': with_map, 'without_map': without_map, 'added': added}
    for name in local_variants:
        for label in (name, name + '-decode'):
            t = traces[label]
            empty = t['TRACE empty']
            local = minus(t['TRACE local prepare'], empty)
            getter = minus(t['TRACE getter'], empty)
            el1 = minus(t['TRACE translate'], empty)
            el0 = minus(t['TRACE translate el0'], empty)
            # One write AT per 2 MiB block that holds a pool byte.
            assert local['at'] == measurements[label]['local_blocks'], (label, local)
            # The getter, its call and the result move.
            assert getter['instructions'] <= 8 and all(getter[k] == 0 for k in ('dsb', 'dmb', 'isb', 'at')), getter
            # The two translation functions share one sequence; the traced
            # sections also hold one caller instruction that may differ.
            assert abs(el0['instructions'] - el1['instructions']) <= 1, (el0, el1)
            assert {k: el0[k] for k in el0 if k != 'instructions'} == \
                {k: el1[k] for k in el1 if k != 'instructions'}, (el0, el1)
            assert el0['isb'] == 1 and el0['dsb'] == 0 and el0['at'] == 1, el0
            assert measurements[label]['tables_bytes'] == 5 * 4096 + 64
            budgets[label] = {'local_prepare': local, 'getter': getter, 'translate': el1,
                              'translate_el0': el0, **measurements[label]}
    (out / 'budgets.json').write_text(json.dumps(budgets, indent=2) + '\n')
    scanned = scan_images(a.images)
    (out / 'result.json').write_text(json.dumps({'passed': True,
        'elf_sha256': digest(elf_path), 'host_sha256': digest(executable),
        'qemu_sha256': digest(a.qemu.resolve()), 'clang_sha256': digest(a.clang.resolve()),
        'ld_sha256': digest(a.ld.resolve()), 'counting_elf_sha256': digest(counting_path),
        'local_elf_sha256': {name: digest(out / (name + '.elf')) for name in local_variants},
        'native_cases': len(cases), 'cores': 8,
        'later_table_writers': writers,
        'measurements': measurements, 'budgets': budgets, 'images': scanned,
        'loaded_segments': {p.name: digest(p) for p in out.glob('*segment-*.bin')},
        'rom_sha256': digest(rom),
        'production_elf': str(production / 'sunstone-a733-smp-cached-devices.elf'),
        'production_manifest': str(production_manifest), 'physical_memory_qualified': False}, indent=2) + '\n')
    print(json.dumps({'budgets': budgets, 'images': len(scanned)}, indent=2))

if __name__ == '__main__':
    main()
