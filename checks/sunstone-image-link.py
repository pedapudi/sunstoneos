#!/usr/bin/env python3
"""Validate the application manifest and generated tables in a native OS image."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import re
import subprocess
import sys
GATE_WAIVED = (
    'helper module of a runnable check (not run on its own) | '
    'run by cmake/sunstone-image.cmake at every OS image link')

if not __debug__:
    raise RuntimeError('checks require assertions')


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


HOOKS = ('board_application_task_privileged', 'board_application_pins', 'board_application_start',
         'a733_smp_diagnostic_start', 'a733_smp_contract_valid', 'vApplicationIRQHandler')
ROUTE_BYTES, APPLICATION_BYTES = 16, 32


class Elf:
    def __init__(self, data):
        assert data[:4] == b'\x7fELF' and data[4] == 2 and data[5] == 1, 'AArch64 little-endian ELF64 required'
        self.data = data
        shoff, = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<3H', data, 0x3a)
        self.sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
        names = self.sections[shstrndx]
        self.names = [self.string(names[4], s[0]) for s in self.sections]
        self.symbols = []
        for index, s in enumerate(self.sections):
            if s[1] != 2:  # SHT_SYMTAB
                continue
            strings = self.sections[s[6]]
            for offset in range(s[4], s[4] + s[5], 24):
                name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', data, offset)
                self.symbols.append({'name': self.string(strings[4], name), 'bind': info >> 4,
                                     'type': info & 15, 'shndx': shndx, 'value': value, 'size': size})

    def string(self, base, offset):
        end = self.data.index(b'\0', base + offset)
        return self.data[base + offset:end].decode()

    def read(self, address, size):
        for (name, kind, flags, addr, offset, length, *_rest) in self.sections:
            if kind == 1 and addr <= address and address + size <= addr + length:  # SHT_PROGBITS
                return self.data[offset + address - addr:offset + address - addr + size]
        raise AssertionError('address %#x is not in initialized image data' % address)

    def cstring(self, address):
        for (name, kind, flags, addr, offset, length, *_rest) in self.sections:
            if kind == 1 and addr <= address < addr + length:
                start = offset + address - addr
                return self.data[start:self.data.index(b'\0', start)].decode()
        raise AssertionError('string %#x is not in image data' % address)



def application_manifest(elf):
    decoder = module('sunstone_manifest', Path(__file__).resolve().parents[1] / 'tools/sunstone-manifest.py')
    sections = [section for name, section in zip(elf.names, elf.sections) if name == 'sunstone_manifest']
    assert len(sections) == 1, 'image must contain one application manifest section'
    section = sections[0]
    raw = elf.data[section[4]:section[4] + section[5]]
    manifests = decoder.unpack(raw)
    assert len(manifests) == 1, 'image must contain one application'
    for name, expected in (('__start_sunstone_manifest', section[3]),
                           ('__stop_sunstone_manifest', section[3] + section[5])):
        symbols = [symbol for symbol in elf.symbols if symbol['name'] == name and symbol['shndx'] != 0]
        assert len(symbols) == 1 and symbols[0]['value'] == expected, name
    return {'sha256': hashlib.sha256(raw).hexdigest(), **manifests[0]}


def check(image, manifest=None, application=None):
    image = Path(image)
    elf = Elf(image.read_bytes())
    # Returning launchers save mutable image RAM. C++ metadata and linker
    # orphans must not introduce writable sections into the executable block.
    # The one exception is the sealed session (apps/os/sealed.c): a NOLOAD section last in the CODE block, written only by
    # EL3 with translation off, which EL1 and EL0 map read-only.
    sealed = [s for name, s in zip(elf.names, elf.sections) if name == '.sealed']
    assert len(sealed) == 1 and sealed[0][1] == 8 and sealed[0][2] & 3 == 3 and \
        0x41000000 <= sealed[0][3] < sealed[0][3] + sealed[0][5] <= 0x41200000, 'the sealed session must be NOLOAD inside CODE'
    writable = [s for name, s in zip(elf.names, elf.sections) if s[2] & 3 == 3 and s[5] and s[3] < 0x42000000 and name != '.sealed']
    assert all(0x41200000 <= s[3] < s[3] + s[5] <= 0x42000000 for s in writable), \
        'allocated writable section outside image RAM'
    defined = [s for s in elf.symbols if s['name'] and s['shndx'] not in (0, 0xfff1)]

    def one(name):
        symbols = [s for s in defined if s['name'] == name and s['bind'] == 1]
        assert len(symbols) == 1, 'missing or ambiguous symbol: ' + name
        return symbols[0]

    def read(name):
        symbol = one(name)
        return elf.read(symbol['value'], symbol['size'])

    for name in HOOKS:
        assert one(name)['type'] == 2, name
    functions = {s['value'] for s in defined if s['type'] == 2}
    routes = []
    raw = read('a733_irq_routes')
    count = struct.unpack('<I', read('a733_irq_route_count'))[0]
    assert len(raw) == count * ROUTE_BYTES and count <= 64, 'interrupt route geometry'
    for offset in range(0, len(raw), ROUTE_BYTES):
        intid, core, priority, handler = struct.unpack_from('<HBB4xQ', raw, offset)
        assert 32 <= intid < 1020 and core < 8 and priority >= 0xd8 and handler in functions, 'invalid interrupt route'
        routes.append({'intid': intid, 'core': core, 'priority': priority})
    assert len({r['intid'] for r in routes}) == len(routes), 'duplicate interrupt routes'
    starts = []
    raw = read('os_applications')
    assert len(raw) % APPLICATION_BYTES == 0, 'start table geometry'
    for offset in range(0, len(raw), APPLICATION_BYTES):
        name, *hooks = struct.unpack_from('<4Q', raw, offset)
        assert all(hook in functions for hook in hooks), 'unlinked start hook'
        starts.append({'name': elf.cstring(name)})
    names = [s['name'] for s in starts]
    assert len(set(names)) == len(names), 'duplicate start entries'
    sections = [s for name, s in zip(elf.names, elf.sections) if name == 'sunstone_manifest']
    if application is None:
        application = application_manifest(elf)['name'] if sections and sections[0][5] else 'none'
    names_defined = {symbol['name'] for symbol in defined}
    watchdog_seconds = None
    if application != 'none' and 'os_cpu_return_watchdog' in names_defined:
        # The EL3 admission decodes the session with apps/os/session.c, which refuses a manifest whose run deadline
        # ends later than the lifetime minus return_reserve_us; the board then stops before the application runs
        # (a733_smp_stop(1, A733_REFUSAL(SESSION_DECODE, ...))). The image's lifetime is the compiled
        # A733_BOARD_SUPERVISED_SECONDS of its watchdog marker; the build derives it from the same manifest
        # (tools/session_lifetime.py), and a marker that differs from that derivation is a stale or mixed build.
        lifetime = module('session_lifetime', Path(__file__).resolve().parents[1] / 'tools/session_lifetime.py')
        marker = struct.unpack_from('<5Q', read('os_cpu_return_watchdog'))
        seconds = marker[4]
        manifest_fields = application_manifest(elf)
        try:
            lifetime.check(manifest_fields['run_deadline_us'], manifest_fields['return_reserve_us'], seconds)
        except lifetime.LifetimeError as error:
            raise AssertionError('session lifetime (%s): %s' % (error.term, error))
        derived = lifetime.derive({'run_deadline_us': manifest_fields['run_deadline_us'],
                                   'return_reserve_us': manifest_fields['return_reserve_us']})
        assert seconds == derived, \
            'session lifetime: the watchdog marker says %d s but the manifest derives %d s; rebuild the image' % (seconds, derived)
        watchdog_seconds = seconds
    features = {'audio': 'os_audio_stream_open', 'radio': 'os_radio_start', 'headset': 'os_headset_start',
                'npu': 'os_npu_start', 'gpu_queue': 'os_gpu_queue_start',
                'cpu_clock': 'os_cpu_clock_step', 'perf_modes': 'os_perf_boot_mode'}
    result = {'interrupts': routes, 'applications': starts, 'application': application, 'watchdog_seconds': watchdog_seconds,
              **{name: symbol in names_defined for name, symbol in features.items()},
              'build_id': read('a733_smp_build_id').hex(),
              'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest()}
    service_data = read('os_services')
    assert len(service_data) % 16 == 0 and len(service_data) >= 7 * 16, 'supervision table geometry'
    services = []
    for offset in range(0, len(service_data), 16):
        name, window = struct.unpack_from('<QQ', service_data, offset)
        assert window, 'zero supervision progress window'
        services.append({'name': elf.cstring(name), 'window_us': window})
    result['services'] = services
    if application == 'none':
        assert not sections or not sections[0][5], 'application manifest in an empty OS image'
        assert not any(n['name'] == 'none' for n in starts), 'empty image starts an application'
        result['manifest'] = None
    else:
        parsed = application_manifest(elf)
        assert parsed['name'] == application, 'application name differs from registration'
        assert len(sections) == 1, 'application section absent'
        section = sections[0]
        if manifest is not None:
            assert elf.data[section[4]:section[4] + section[5]] == Path(manifest).read_bytes(), 'compiled manifest differs from generated bytes'
        assert services[5] == {'name': application, 'window_us': parsed['progress_window_us']}, 'application supervision differs from manifest'
        assert names.count(application) == 1, 'application start entry absent'
        regions = parsed['regions']
        allocated = [(s[3], s[3] + s[5]) for s in elf.sections if s[2] & 2 and s[5]]
        for region in regions:
            start, end = region['address'], region['address'] + region['bytes']
            assert all(end <= low or high <= start for low, high in allocated), 'application region overlaps OS image'
        result['manifest'] = parsed
    return result


MEMORY_STRICT = ('memcpy', 'memmove', 'memset')
MEMORY_UNALIGNED = ('ss_memcpy_unaligned', 'ss_memmove_unaligned', 'ss_memset_unaligned')


def memory_routines(objects, nm, image):
    """Which objects call which version of the memory routines (cmake/codegen-align.cmake): the strict ones of runtime/memory.c
    and memmove.c, safe with the MMU off and on Device memory, or the unaligned ones of runtime/memory_unaligned.c, for code that
    runs under Normal write-back memory with SCTLR_EL1.A clear. An object that calls both is listed under both."""
    strict, unaligned = [], []
    for path in objects:
        output = subprocess.run([nm, '--format=posix', '--extern-only', '--undefined-only', str(path)], check=True,
                                capture_output=True, text=True).stdout
        undefined = {line.split()[0] for line in output.splitlines() if len(line.split()) >= 2 and not line.endswith(':')}
        name = str(path).split('.dir/', 1)[-1]
        if undefined & set(MEMORY_STRICT):
            strict.append(name)
        if undefined & set(MEMORY_UNALIGNED):
            unaligned.append(name)
    linked = subprocess.run([nm, '--defined-only', str(image)], capture_output=True, text=True).stdout
    for routine in MEMORY_STRICT + MEMORY_UNALIGNED:
        assert (' T ' + routine) in linked or (' t ' + routine) in linked or routine in MEMORY_STRICT, \
            'the image does not link ' + routine
    return {'strict_callers': sorted(strict), 'unaligned_callers': sorted(unaligned),
            'both': sorted(set(strict) & set(unaligned))}


def application_symbols(elf, objects, libraries, runtime_libraries, nm, large_expected=None):
    """Applications may use their closure, public calls, FreeRTOS and C/C++."""
    def symbols(paths, defined):
        if not paths:
            return set()
        command = [nm, '--format=posix', '--extern-only',
                   '--defined-only' if defined else '--undefined-only', *map(str, paths)]
        output = subprocess.run(command, check=True, capture_output=True, text=True).stdout
        return {line.split()[0] for line in output.splitlines()
                if len(line.split()) >= 2 and not line.endswith(':')}
    assert objects, 'application object inventory is empty'
    own = symbols(objects + libraries, True)
    external = symbols(objects + libraries, False) - own
    allowed = symbols(runtime_libraries, True)
    linked = {symbol['name'] for symbol in elf.symbols if symbol['shndx'] != 0}
    kit = Path(__file__).resolve().parents[1] / 'include'
    header = ''.join((kit / name).read_text() for name in
                     ('sunstone_app.h', 'gpu_queue.h', 'sunstone_latency.h', 'sunstone_dma.h', 'sunstone_storage.h',
                      'sunstone_npu_queue.h'))
    public_calls = set(re.findall(r'\b((?:ss_|gpu_queue_)[a-z0-9_]+)\s*\([^;{}]*\)\s*;', header))
    # Object inventories include references in discarded sections. A public
    # call remains valid when no live caller keeps its implementation linked.
    allowed |= public_calls
    # The unaligned memory routines are what an application's memcpy, memmove and memset call (runtime/memory_unaligned.c,
    # include/sunstone_relaxed_memory.h): the same contract as the C library's routines, and stateless.
    allowed |= {'ss_memcpy_unaligned', 'ss_memmove_unaligned', 'ss_memset_unaligned'}
    # An explicit shared-GPU descriptor names its own generated manifest.
    # Admit that immutable object only, using the selected image manifest's
    # exact bytes and section bounds rather than a symbol-name suffix.
    manifests = [(n, section) for n, section in zip(elf.names, elf.sections)
                 if n == 'sunstone_manifest' and section[5]]
    if manifests:
        assert len(manifests) == 1, 'ambiguous generated application manifest'
        section = manifests[0][1]
        selected = application_manifest(elf)['name'].replace('-', '_') + '_manifest'
        manifest_objects = [symbol for symbol in elf.symbols if symbol['name'] == selected and symbol['shndx']]
        assert len(manifest_objects) == 1 and manifest_objects[0]['bind'] == 1 and manifest_objects[0]['type'] == 1 and \
            (manifest_objects[0]['value'], manifest_objects[0]['size']) == (section[3], section[5]) and \
            section[2] & 7 == 2, 'generated manifest object differs from the immutable application section'
        allowed.add(selected)
    allowed |= {name for name in linked if re.match(r'^(?:[xv]|ux|ul|uc|pv|pc)[A-Z]', name)
                or name == 'eTaskGetState'}
    # FreeRTOS configASSERT is a board-supplied kernel callback.
    allowed.update(('vApplicationAssert', 'platform_panic', 'ullPortYieldRequired'))
    forbidden = sorted(external - allowed)
    assert not forbidden, 'application references private OS symbols: ' + ', '.join(forbidden)
    # Application registration and program bytes enter immutable image
    # sections with exact bounds. Neither section may contain OS-owned data.
    admitted = {}
    for name, alignment in (('sunstone_gpu_program', 4), ('sunstone_region_initializers', 8), ('sunstone_large_regions', 8)):
        chunks = []
        for path in objects:
            obj = Elf(Path(path).read_bytes())
            chunks += [obj.data[s[4]:s[4] + s[5]] for n, s in zip(obj.names, obj.sections) if n == name]
        sections = [s for n, s in zip(elf.names, elf.sections) if n == name]
        assert len(sections) <= 1, 'multiple application sections: ' + name
        raw = b'' if not sections else elf.data[sections[0][4]:sections[0][4] + sections[0][5]]
        if name == 'sunstone_large_regions' and large_expected is not None:
            # The records come from the generated manifest object, which is the image's, not the application's.
            assert raw == large_expected, 'large region records differ from the generated bytes'
        else:
            assert raw == b''.join(chunks), 'section contains data outside application objects: ' + name
        bounds = []
        for marker in ('__start_' + name, '__stop_' + name):
            values = [s['value'] for s in elf.symbols if s['name'] == marker and s['shndx']]
            assert len(values) == 1, 'missing application section bound: ' + marker
            bounds.append(values[0])
        assert bounds[0] % alignment == 0 and bounds[1] - bounds[0] == len(raw), 'section bounds differ: ' + name
        if raw:
            section = sections[0]
            assert section[2] & 2 and not section[2] & 1, 'application section must be immutable: ' + name
            assert section[3] == bounds[0], 'section start differs: ' + name
        admitted[name] = raw
    large = admitted['sunstone_large_regions']
    assert len(large) % 320 == 0, 'large region records extent'
    large_records = [dict(zip(('index', 'flags', 'bytes', 'source_bytes', 'first_sector', 'sha256', 'path'),
                              struct.unpack_from('<IIQQQ32s256s', large, 320 * i))) for i in range(len(large) // 320)]
    for i, record in enumerate(large_records):
        flags = record['flags']
        assert record['index'] == i and not flags & ~15 and record['bytes'] % 0x200000 == 0 and \
            ((flags & 4 and not record['source_bytes'] and not any(record['sha256'])) or
             (not flags & 4 and 0 < record['source_bytes'] <= record['bytes'])), 'large region record %d' % i
        if flags & 8:
            # The heap (SS_LARGE_HEAP): unfilled and read-write, and the last record.
            assert flags == 13 and i == len(large_records) - 1, 'large region heap record %d' % i
    initializers = admitted['sunstone_region_initializers']
    assert len(initializers) % 16 == 0 and len(initializers) <= 8 * 16, 'region initializer extent'
    regions = application_manifest(elf)['regions']
    seen = set()
    for index, reserved, size in struct.iter_unpack('<IIQ', initializers):
        assert not reserved and index < len(regions) and index not in seen, 'region initializer index'
        region = regions[index]
        assert region['kind'] == 2 and region['access'] & 1 and region['access'] & 8 and not region['access'] & 2, 'region initializer access'
        assert size > 0 and size % 8 == 0 and size <= region['bytes'], 'region initializer prefix'
        seen.add(index)
    return {'objects': len(objects), 'libraries': len(libraries), 'external_symbols': len(external),
            'gpu_program_bytes': len(admitted['sunstone_gpu_program']),
            'region_initializers': len(initializers) // 16, 'large_regions': len(large_records)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--application', required=True)
    parser.add_argument('--build-id', type=Path, required=True)
    parser.add_argument('--lifetime', choices=('supervised', 'return'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--application-objects', type=Path, nargs='*', default=[])
    parser.add_argument('--application-library', type=Path, action='append', default=[])
    parser.add_argument('--runtime-library', type=Path, action='append', default=[])
    parser.add_argument('--nm', default='nm')
    parser.add_argument('--memory-objects', type=Path, nargs='*', action='append', default=[],
                        help='objects of the OS core, its device objects and the application, for the report of which memory routines each links')
    parser.add_argument('--session-seconds', type=int, help='the session lifetime the build compiled (A733_BOARD_SUPERVISED_SECONDS)')
    args = parser.parse_args()
    try:
        result = check(args.image, args.manifest, args.application)
        assert result['build_id'] == args.build_id.read_text().strip(), 'linked build identity differs from generated identity'
        if args.lifetime in ('supervised', 'return') and result.get('manifest'):
            # OS session admission (apps/os/session.c) refuses a manifest whose run deadline ends later than the session
            # lifetime minus the return reserve; the EL3 monitor then stops the board before the image runs. The build passes
            # the lifetime it compiled (derived from the manifest by tools/session_lifetime.py); check() compares a returning
            # image's own watchdog marker with it.
            lifetime = module('session_lifetime', Path(__file__).resolve().parents[1] / 'tools/session_lifetime.py')
            manifest = result['manifest']
            seconds = args.session_seconds
            if seconds is None:
                header = (Path(__file__).resolve().parents[1] / 'include/a733_board_application.h').read_text()
                seconds = int(re.search(r'#define A733_BOARD_SUPERVISED_SECONDS (\d+)u', header).group(1))
            try:
                lifetime.check(manifest['run_deadline_us'], manifest['return_reserve_us'], seconds)
            except lifetime.LifetimeError as error:
                raise AssertionError('session lifetime (%s): %s' % (error.term, error))
            if result.get('watchdog_seconds') is not None:
                assert result['watchdog_seconds'] == seconds, \
                    'session lifetime: the watchdog marker says %d s but the build compiled %d s' % (result['watchdog_seconds'], seconds)
        owner = subprocess.run([args.nm, '--defined-only', str(args.image)], capture_output=True, text=True).stdout
        if ' ss_storage_enable' in owner:
            # The SD owner reads the CCU and drives SMHC0. The application's EL0 task creates it on first use, so the scheduler
            # must be told to create it at EL1: board_application_task_privileged has to reach os_sd_owner_task_privileged.
            # (The first board run faulted reading 0x02002500 at EL0 in the owner task.)
            asm = subprocess.run(['llvm-objdump-21', '-d', '--disassemble-symbols=os_application_task_privileged', str(args.image)],
                                 capture_output=True, text=True).stdout
            assert 'os_sd_owner_task_privileged' in asm, 'the SD owner task is not created at EL1 (os_application_task_privileged does not reach os_sd_owner_task_privileged)'
        if args.application != 'none':
            result['symbol_boundary'] = application_symbols(Elf(args.image.read_bytes()),
                args.application_objects, args.application_library, args.runtime_library,
                args.nm, (args.manifest.parent / 'large.bin').read_bytes())
        objects = [path for group in args.memory_objects for path in group]
        if objects:
            result['memory_routines'] = memory_routines(objects, args.nm, args.image)
    except (AssertionError, ValueError) as error:
        print('FAIL OS image link:', error, file=sys.stderr)
        return 1
    result.update(passed=True, application=args.application, target=args.image.stem,
                  lifetime=args.lifetime, build_id=args.build_id.read_text().strip(),
                  image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS OS image link:', args.application, len(result['interrupts']), 'interrupts,',
          len(result['applications']), 'start entries')
    return 0


if __name__ == '__main__':
    sys.exit(main())
