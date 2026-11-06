#!/usr/bin/env python3
"""Check the memory layout record against the files that still hold its numbers as text.

tools/memory_layout.py holds the layout of the images built today
(docs/designs/application-memory-layout.md). This check shows that

- the record equals the numbers recorded here for the images that exist, so a
  change to the record is a deliberate change of this check;
- the linker scripts, the table builder and the application identity check hold
  the same numbers as the record;
- the host tools that import the record use its values;
- the ranges of the record are ordered, aligned and disjoint where they must be;
- optional --image arguments: a linked image places its sections and symbols
  inside the record's ranges.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import memory_layout

MIB = 1 << 20
# The numbers of the images built before the record existed.
TODAY = {
    'dram': (0x40000000, 0x340000000),
    'el0': (0x40000000, 0x48000000),
    'image.entry': 0x41000000,
    'image.code': (0x41000000, 0x41200000),
    'image.ram': (0x41200000, 0x42000000),
    'image.image_ram': (0x41000000, 0x42200000),
    'image.bss_limit': 0x42000000,
    'image.writable_ceiling': 0x41c00000,
    'image.uncached_block': (0x41c00000, 0x41e00000),
    'image.session': 0x41f00000,
    'monitor.range': (0x42000000, 0x42200000),
    'monitor.result': (0x42000000, 0x42004000),
    'monitor.contract': (0x42004000, 0x42005000),
    'monitor.bss_first': 0x42005000,
    'monitor.tables': 0x421f0000,
    'window': (0x44000000, 0x47000000),
    'runner_dram': (0x40000000, 0x100000000),
    'fel_bytes_per_second': 550000,
    'boot_slot_load': (0x41000000, 0x47000000),
}


def fail(message):
    raise SystemExit('memory-layout: ' + message)


def require(condition, message):
    if not condition:
        fail(message)


def lookup(record, dotted):
    for part in dotted.split('.'):
        record = record[part]
    return record


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def number(text):
    return int(text.replace('_', ''), 0)


def linker_script(path, record):
    """MEMORY regions and ASSERT limits of a cached-profile linker script."""
    text = Path(path).read_text()
    regions = {m[0]: (number(m[1]), number(m[1]) + number(m[2]) * (MIB if m[3] == 'M' else 1))
               for m in re.findall(r'(\w+) \(\w+\) : ORIGIN = (0x[0-9a-f]+), LENGTH = (\d+)([M]?)', text)}
    require(regions.get('CODE') == tuple(record['image']['code']), '%s: CODE differs from the record' % path)
    require(regions.get('RAM') == tuple(record['image']['ram']), '%s: RAM differs from the record' % path)
    require(regions.get('MONITOR') == tuple(record['monitor']['range']), '%s: MONITOR differs from the record' % path)
    return text


def check_device_ranges():
    """The NPU's reservation is taken from the ledger when the application is admitted and published for the return: the terminal
    reads the published list, the EL3 tables map every DRAM block that the decode check passed, and nothing names a pool at link
    or build time."""
    terminal = (ROOT / 'apps/os/return-terminal.patch').read_text()
    require('os_device_ranges_head' in terminal and '.Lranges_next' in terminal and '__os_npu_pool' not in terminal,
            'apps/os/return-terminal.patch: the return does not clean the published device ranges')
    memory = (ROOT / 'platform/a733-smp-cached/memory.c').read_text()
    require('a733_cached_el3_l1[g]' in memory and 'dram_map[r]' in memory and '__os_npu_pool' not in memory,
            'platform/a733-smp-cached/memory.c: the EL3 tables do not map all DRAM for the published ranges')
    for path in ('cmake/sunstone-os-core.cmake', 'apps/os/npu.h', 'apps/os/npu.c', 'apps/os/large.c', 'apps/os/admission.c'):
        text = (ROOT / path).read_text()
        for name in ('OS_NPU_POOL', 'SUNSTONE_OS_NPU_POOL', '__os_npu_pool', 'SUNSTONE_OS_NPU_LINKS', 'os-npu-meta', 'OS_NPU_META'):
            require(name not in text, '%s still names %s' % (path, name))
    require(memory_layout.device_range_valid(0x60000000, 0x400000) and memory_layout.device_range_valid(0x100000000, 0x200000) and
            not memory_layout.device_range_valid(0x41f00000, 0x1000) and not memory_layout.device_range_valid(0x60000001, 64) and
            not memory_layout.device_range_valid(0x33ffffe00, 0x400) and not memory_layout.device_range_valid(0x60000000, 0),
            'tools/memory_layout.py device_range_valid')


def check_sources(record):
    cached = linker_script(ROOT / 'platform/a733-smp-cached/link.ld', record)
    require('ASSERT(__bss_end <= 0x%x' % record['image']['bss_limit'] in cached, 'linker script bss limit differs')
    linker_script(ROOT / 'apps/board-single-core/link.ld', record)
    for symbol, key in (('.result', 'result'), ('.contract', 'contract')):
        match = re.search(r'\%s (0x[0-9a-f]+) \(NOLOAD\)' % symbol, cached)
        require(match and number(match[1]) == record['monitor'][key][0], 'linker script %s address differs' % symbol)
    require(re.search(r'\.tables (0x[0-9a-f]+) \(NOLOAD\)', cached) and
            number(re.search(r'\.tables (0x[0-9a-f]+) \(NOLOAD\)', cached)[1]) == record['monitor']['tables'],
            'linker script .tables address differs')
    block = record['image']['uncached_block']
    require('0x%x : .' % block[0] in re.sub(r'\s+', ' ', cached), 'linker script uncached block address differs')
    require('__uncached_pool_end == 0x%x' % block[1] in cached, 'linker script uncached block end differs')
    identity = (ROOT / 'apps/os/identity.c').read_text()
    low, high = record['window']
    require('UINT64_C(0x%x)' % low in identity and 'UINT64_C(0x%x)' % high in identity,
            'apps/os/identity.c window differs from the record')
    platform = (ROOT / 'platform/a733/platform.c').read_text()
    el0 = record['el0']
    require('UINT64_C(0x%x)' % el0[1] in platform, 'platform.c upper memory start differs from the EL0 end')
    require('for (unsigned i = 0; i < %d; ++i)' % ((el0[1] - el0[0]) // (2 * MIB)) in platform,
            'platform.c maps a different number of EL0 blocks than the record')


def check_consumers(record):
    manifest = load_module('layout_manifest', ROOT / 'tools/sunstone-manifest.py')
    require(tuple(manifest.WINDOW) == tuple(record['window']), 'manifest tool window differs')
    slots = load_module('layout_boot_slots', ROOT / 'tools/boot-slots.py')
    require((slots.LOAD_BASE, slots.LOAD_LIMIT) == tuple(record['boot_slot_load']), 'boot-slot planner load range differs')
    images = load_module('layout_board_images', ROOT / 'tools/board_images.py')
    require(tuple(images.RUNNER_DRAM) == tuple(record['runner_dram']), 'board image DRAM range differs')
    try:
        returning = load_module('layout_cpu_return', ROOT / 'apps/os/cpu-return-host.py')
    except ImportError as error:
        print('memory-layout: cpu-return-host not loaded (%s); skipped' % error)
        return
    require(returning.IMAGE_RAM == memory_layout.extent(record['image']['image_ram']), 'CPU-return image RAM differs')
    # A placement request exists only when someone asks for one: a template whose NPU record holds stale pool words produces a
    # session with zero words, and an explicit request sets exactly them.
    import struct
    host_records = load_module('layout_os_host', ROOT / 'apps/os/host.py')
    stale = bytearray(host_records.SERVICE_BYTES[host_records.NPU])
    struct.pack_into('<QQ', stale, host_records.NPU_POOL_REQUEST_OFFSET, 0x47400000, 0x800000)
    stale[0] = 0x5a; stale[200] = 0xa5
    services = {host_records.NPU: bytes(stale)}
    clean = host_records.set_npu_pool_request(services, 0, 0)[host_records.NPU]
    require(struct.unpack_from('<QQ', clean, host_records.NPU_POOL_REQUEST_OFFSET) == (0, 0) and clean[0] == 0x5a and clean[200] == 0xa5 and
            len(clean) == len(stale), 'a template with stale pool words does not produce a session with zero words')
    asked = host_records.set_npu_pool_request(services, 0x180000000, 0x4000000)[host_records.NPU]
    require(struct.unpack_from('<QQ', asked, host_records.NPU_POOL_REQUEST_OFFSET) == (0x180000000, 0x4000000), 'an explicit request is not written')
    require(struct.unpack_from('<QQ', services[host_records.NPU], host_records.NPU_POOL_REQUEST_OFFSET) == (0x47400000, 0x800000),
            'the services were changed in place')
    for bad in ((0x180000000, 0), (0, 0x4000000), (-1, 5)):
        try:
            host_records.set_npu_pool_request(services, *bad)
        except ValueError:
            continue
        require(False, 'a half-formed request %r is accepted' % (bad,))
    require(host_records.parse_npu_pool('0x180000000:0x4000000') == (0x180000000, 0x4000000) and host_records.parse_npu_pool(None) == (0, 0),
            'the request text does not parse')
    require('set_npu_pool_request' in (ROOT / 'apps/os/prepare.py').read_text() and
            'npu_pool=records.parse_npu_pool' in (ROOT / 'tools/board_images.py').read_text(),
            'the session preparation does not set the request from its explicit argument')
    # What the run publishes: the list head and first page, the terminal walk's counters and the placement guard.
    import struct
    page = struct.pack('<2Q', 0, 2) + struct.pack('<2Q', 0x180000000, 0x4000000) + struct.pack('<2Q', 0x80000000, 0x4000000)
    page += bytes(4096 - len(page))
    walk = b''.join(struct.pack('<2Q', 1000 + core, 1000 + core + 24000) for core in range(8))
    guard = struct.pack('<8Q', 0x180000000, 0x4000000, 0x80000000, 0x4000000, 1, 0, 0, 0xa5a5a5a5a5a5a5a5)
    out = returning.decode_published({'device-ranges-head': struct.pack('<Q', 0x70000000), 'device-ranges-page': page,
                                      'return-walk': walk, 'npu-guard': guard}, {})
    require(out['device_ranges']['ranges'] == [{'base': 0x180000000, 'bytes': 0x4000000}, {'base': 0x80000000, 'bytes': 0x4000000}] and
            out['return_walk'][3]['ticks'] == 24000 and abs(out['return_walk'][3]['microseconds'] - 1000.0) < 1e-6 and
            out['npu_guard']['differing_words'] == 0 and out['npu_guard']['checked'] == 1, 'the published ranges and the walk do not decode')
    registered = returning.decode_published({'npu-register': struct.pack('<8Q', 80, 13815360, 8000, 4000, 36000, 0, 0, 0)}, {})['npu_register']
    require(registered['graphs'] == 80 and registered['per_graph_us'] == {'copy': 100.0, 'decode': 50.0, 'layout': 450.0},
            'the graph registration record does not decode')
    prepared = returning.decode_published({'npu-prepare': struct.pack('<16Q', 80, 13815360, 15000, 30, 400000, 300000, 20000, 1000, 40000,
                                                                       20000, 5000, 801000, 3200000000, 24000000, 0x30d01805, 0x412fd050)}, {})['npu_prepare']
    require(abs(prepared['cpu_mhz'] - 3200.0) < 1e-6 and prepared['data_cache_on'] and prepared['mmu_on'] and prepared['core_part'] == 0xd05
            and prepared['unattributed_us'] == 801000 - 15000 - 30 - 400000 - 300000 - 20000 - 1000 - 40000 - 20000 - 5000,
            'the admission timing record does not decode')
    require(returning.decode_published({'npu-register': bytes(64)}, {})['npu_register']['per_graph_us'] is None,
            'an empty registration record does not decode')
    require(returning.decode_published({'device-ranges-head': bytes(8)}, {})['device_ranges'] == {'head': 0, 'ranges': []},
            'an empty range list does not decode')
    for name, bad in (('a range below the image', struct.pack('<2Q', 0, 1) + struct.pack('<2Q', 0x41000000, 4096)),
                      ('an unaligned range', struct.pack('<2Q', 0, 1) + struct.pack('<2Q', 0x60000001, 4096)),
                      ('a count over a page', struct.pack('<2Q', 0, 256))):
        try:
            returning.decode_published({'device-ranges-head': struct.pack('<Q', 0x70000000),
                                        'device-ranges-page': bad + bytes(4096 - len(bad))}, {})
        except ValueError:
            continue
        require(False, name + ' decodes')
    returning.require_listable_head(0x48000000)
    for name, head in (('a head at 12 GiB', 0x301380000), ('a head just above 4 GiB', 1 << 32), ('an unaligned head', 0x48000010),
                       ('a head below the image', 0x41000000)):
        try:
            returning.require_listable_head(head)
        except ValueError as error:
            require(('4 GiB' in str(error)) == (head >= 1 << 32), name + ' is refused for the wrong reason: ' + str(error))
            continue
        require(False, name + ' is accepted')
    # The published-range head: an OS image must carry it, a probe may omit it, and nothing else is accepted.
    ram = (0x41200000, 0x42000000)
    head = {'os_device_ranges_head': (0x41800000, 8)}
    os_image = {'os_large_prepare': (0x41000000, 64)}
    require(returning.device_ranges_geometry({**os_image, **head}, ram) == {'address': 0x41800000, 'bytes': 8}, 'an OS image with its list head is refused')
    require(returning.device_ranges_geometry({}, ram) is None, 'a probe without a list head is refused')
    for name, symbols in (('an OS image without the list head', os_image),
                          ('a list head outside image RAM', {**os_image, 'os_device_ranges_head': (0x42100000, 8)}),
                          ('a list head of the wrong size', {**os_image, 'os_device_ranges_head': (0x41800000, 16)}),
                          ('a list head in a probe', head)):
        try:
            returning.device_ranges_geometry(symbols, ram)
        except ValueError:
            continue
        require(False, name + ' is accepted')


def check_shape(record):
    image, monitor = record['image'], record['monitor']
    code, ram, shared = image['code'], image['ram'], image['image_ram']
    block = 2 * MIB
    for name, pair in (('code', code), ('ram', ram), ('image_ram', shared), ('monitor', monitor['range']),
                       ('uncached block', image['uncached_block']), ('window', record['window']),
                       ('el0', record['el0']), ('runner_dram', record['runner_dram'])):
        require(pair[0] < pair[1] and pair[0] % block == 0 and pair[1] % block == 0, name + ' is not 2 MiB aligned')
    require(code[1] == ram[0] and ram[1] == monitor['range'][0] and shared == (code[0], monitor['range'][1]),
            'code, RAM and monitor are not one contiguous image range')
    require(image['entry'] == code[0], 'entry is not the first code byte')
    require(ram[0] <= image['writable_ceiling'] <= image['bss_limit'] <= ram[1], 'writable ceiling lies outside RAM')
    require(image['uncached_block'][0] == image['writable_ceiling'] and image['uncached_block'][1] <= image['session'], 'uncached block overlaps the session')
    require(ram[0] <= image['session'] < ram[1], 'session lies outside RAM')
    result, contract = monitor['result'], monitor['contract']
    require(result[1] == contract[0] and contract[1] <= monitor['bss_first'] < monitor['tables'] < monitor['range'][1],
            'monitor records overlap')
    window = record['window']
    require(record['el0'][0] <= window[0] and window[1] <= record['el0'][1], 'window lies outside EL0 reach')
    require(shared[1] <= window[0], 'window overlaps the image range')
    require(record['boot_slot_load'] == (code[0], window[1]), 'boot slot range is not the image range through the window')
    require(record['runner_dram'][0] == record['dram'][0] and record['runner_dram'][1] <= 1 << 32,
            'runner range lies outside FEL reach (a 32-bit address)')


def check_placement(record):
    """The manifest tool places regions in the window, then in the slot 1 area, and refuses the rest."""
    import json, tempfile
    manifest = load_module('layout_placement', ROOT / 'tools/sunstone-manifest.py')

    def placed(regions):
        base = {'name': 'placement', 'devices': {'required': [], 'optional': []},
                'cpu': {'cluster': 'a55', 'workers': 0, 'priority': 2, 'stack_bytes': 4096},
                'regions': regions, 'deadlines': {'return_reserve_us': 1000000, 'progress_window_us': 1000000},
                'output_limit': 64}
        with tempfile.NamedTemporaryFile('w', suffix='.json') as handle:
            json.dump(base, handle)
            handle.flush()
            m = manifest.load(handle.name)
        manifest.assign(m)
        return [r['address'] for r in m['regions']]

    window, area = record['window'], record['slot1_area']
    work = lambda name, mib, **extra: dict(name=name, kind='work', access=['read', 'write'], bytes=mib << 20, **extra)
    require(placed([work('a', 1)]) == [window[0]], 'a small region is not at the window start')
    require(placed([work('a', 40), work('b', 20)]) == [window[0], area[0]],
            'a region that does not fit the rest of the window is not at the slot 1 area start')
    require(placed([work('a', 64)]) == [area[0]], 'a region larger than the window is not at the slot 1 area start')
    require(placed([work('a', 64, address=area[0] + (16 << 20))]) == [area[0] + (16 << 20)], 'a pinned slot 1 region moved')
    # The upper memory (step 6b): first fit continues there when the slot 1 area is full, a pinned region may lie there, and
    # nothing may cross the unmapped hole below it or run past the installed memory.
    upper = record['upper_area']
    area_mib = (area[1] - area[0]) >> 20
    require(placed([work('a', area_mib - 8), work('b', 100)]) == [area[0], upper[0]],
            'a region that does not fit the slot 1 area is not at the start of the upper memory')
    require(placed([work('a', area_mib - 8), work('b', 100), work('c', 100)]) == [area[0], upper[0], upper[0] + (100 << 20)],
            'the second region of the upper memory does not follow the first')
    require(placed([work('a', 64, address=upper[0] + (8 << 30))]) == [upper[0] + (8 << 30)], 'a pinned upper region moved')
    require(placed([work('a', 64, address=upper[1] - (64 << 20))]) == [upper[1] - (64 << 20)],
            'a pinned region at the end of the installed memory was refused')
    require(placed([work('a', 64, address=upper[0] + (8 << 20)), work('b', 8)]) == [upper[0] + (8 << 20), window[0]],
            'an unpinned region does not take the window beside a pinned upper region')
    require(upper[0] == 0x80000000 and upper[1] == record['dram'][1], 'the upper memory is not from 0x80000000 to the installed memory')
    for bad in (dict(address=window[1] - (2 << 20)), dict(address=area[0] + 4096), dict(address=area[1] - (2 << 20)),
                dict(address=upper[0] - (32 << 20)), dict(address=upper[1] - (32 << 20)), dict(address=upper[0] + 4096)):
        try:
            placed([work('a', 64, **bad)])
        except SystemExit:
            continue
        fail('a region at %s was accepted' % bad)


def check_pools(record, images):
    """Memory that a device pool owns is never in the ledger's usable ranges.

    The ledger's usable ranges are the 2 MiB blocks of level-1 slot 1 above the EL0 window and the level-1 slots from
    0x80000000 that the kernel table maps as the plain EL1-only block (apps/os/large.c). The images that link their
    device pools into upper memory (platform/a733-smp-cached/link-devices.ld: DEVICE_WB and DEVICE_NC) map those
    pools with their own attributes, and the table builder leaves the slot 1 range out of its list for them."""
    area = record['slot1_area']
    text = (ROOT / 'platform/a733-smp-cached/link-devices.ld').read_text()
    pools = {m[0]: (number(m[1]), number(m[1]) + number(m[2]) * MIB)
             for m in re.findall(r'(DEVICE_\w+) \(rw\) : ORIGIN = (0x[0-9a-f]+), LENGTH = (\d+)M', text)}
    require(set(pools) == {'DEVICE_WB', 'DEVICE_NC'}, 'link-devices.ld pools are not DEVICE_WB and DEVICE_NC')
    overlapping = [name for name, (low, high) in pools.items() if low < area[1] and area[0] < high]
    require(overlapping, 'the slot 1 area does not contain the extended device pools, so this check has nothing to guard')
    memory = (ROOT / 'platform/a733-smp-cached/memory.c').read_text()
    table = memory[memory.index('static const struct a733_dram_range dram_map[]'):]
    table = table[:table.index('};')]
    guarded = re.search(r'#if !defined\(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY\)\s*\{UINT64_C\(0x48000000\)', table)
    require(guarded, 'memory.c maps the slot 1 range above the EL0 window for an image with extended device pools')
    for path in images:
        elf = load_elf(path)
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        for start, end in (('__device_pool_start', '__device_pool_end'), ('__uncached_pool_start', '__uncached_pool_end')):
            if start in symbols and symbols[start] != symbols[end]:
                require(symbols[end] <= area[0] or symbols[start] >= area[1],
                        '%s: the %s [%#x, %#x) lies in the slot 1 area' % (path, start, symbols[start], symbols[end]))


def load_elf(path):
    from elftools.elf.elffile import ELFFile
    return ELFFile(open(path, 'rb'))


def check_image(path, record):
    from elftools.elf.elffile import ELFFile
    elf = ELFFile(open(path, 'rb'))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
               if s['st_shndx'] != 'SHN_UNDEF'}
    image, monitor = record['image'], record['monitor']
    require(elf['e_entry'] == image['entry'], '%s: entry differs' % path)
    for segment in elf.iter_segments():
        if segment['p_type'] != 'PT_LOAD' or not segment['p_memsz']:
            continue
        low, high = segment['p_paddr'], segment['p_paddr'] + segment['p_memsz']
        require(image['image_ram'][0] <= low and high <= image['image_ram'][1], '%s: segment [%#x, %#x) outside image RAM' % (path, low, high))
        if segment['p_flags'] & 1:
            require(high <= image['code'][1], '%s: executable segment outside CODE' % path)
        if segment['p_filesz']:
            require(low + segment['p_filesz'] <= monitor['range'][0] or low >= image['session'], '%s: file bytes in the monitor interval' % path)
    require(symbols.get('__a733_smp_result') == monitor['result'][0], '%s: result address differs' % path)
    require(symbols.get('__a733_smp_contract') == monitor['contract'][0], '%s: contract address differs' % path)
    require(symbols['__bss_end'] <= image['bss_limit'], '%s: bss ends above the limit' % path)
    require(monitor['bss_first'] <= symbols['__monitor_bss_start'] and symbols['__tables_end'] <= monitor['range'][1],
            '%s: monitor allocations outside the monitor interval' % path)
    return symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, help='directory for a JSON copy of the record')
    parser.add_argument('--image', type=Path, action='append', default=[], help='a linked image to place against the record')
    args = parser.parse_args()
    record = memory_layout.record()
    for key, value in TODAY.items():
        got = lookup(record, key)
        require(tuple(got) == value if isinstance(value, tuple) else got == value,
                'record %s is %r; the images built today use %r' % (key, got, value))
    require(record is not memory_layout.record(), 'the record is shared between calls')
    check_shape(record)
    check_sources(record)
    check_device_ranges()
    check_consumers(record)
    check_placement(record)
    check_pools(record, args.image)
    for image in args.image:
        check_image(image, record)
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        import json
        (args.output / 'layout.json').write_text(json.dumps(memory_layout.hexed(record), indent=2, sort_keys=True) + '\n')
    print('memory-layout: passed (%d image%s)' % (len(args.image), '' if len(args.image) == 1 else 's'))


if __name__ == '__main__':
    main()
