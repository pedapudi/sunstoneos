#!/usr/bin/env python3
"""Check the linked integrated OS image's fixed interrupt table, start list and layout.

The image must hold exactly one definition of each board application hook,
no per-device interrupt hook from the single-application images, a table of
distinct shared peripheral interrupts whose handlers are linked functions,
and a start list whose entries are distinct linked functions. The fixed
recognition inputs and arena must not overlap one another or any allocated
image section, and must lie in the stage-2 loader's load window. Their
extents come from the generated input header and the recognizer header; the
window comes from the loader source. The table, start list and lifetime must
match the build's options: an audio build adds the DMA0 interrupt (INTID 143)
and the audio start entry, a radio build the radio entry and a headset build
the headset entry. A GPU linear build adds the GPU interrupt (INTID 95) and
the gpu-linear entry, and routes INTID 95 to the recognition core that the
image records in os_recognition_core; its arena is the recognizer header's
12 MiB value, followed by the GPU's 4 MiB read-only view. Radio and headset builds also keep the
1 MiB profile and firmware span at 0x43000000 clear of the image and the
recognition regions.
The standard library suffices, so the build can run this check. Its output also serves the supervised preparation (apps/os/prepare.py)
as the image manifest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

HOOKS = ('board_application_task_privileged', 'board_application_pins', 'board_application_start',
         'a733_smp_diagnostic_start', 'a733_smp_contract_valid', 'vApplicationIRQHandler')
# Interrupt hooks that per-device platform patches insert; the table replaces them.
PATCH_HOOKS = ('thermal_application_irq', 'board_host_irq', 'monitor_application_irq',
               'adc_application_irq', 'pwm_application_irq', 'spi_interrupt', 'uart_application_irq',
               'rtc_application_irq')
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


def contract_profile(elf, legacy=False):
    found = [s for s in elf.symbols if s['name'] == 'os_contract_profile' and s['shndx'] != 0]
    if not found and legacy:
        return None
    assert len(found) == 1 and found[0]['bind'] == 1 and found[0]['size'] == 16, 'Missing or malformed linked OS contract profile'
    magic, size = struct.unpack('<2Q', elf.read(found[0]['value'], 16))
    assert magic in (0x415337334f534331, 0x415337334f524331), 'Unknown linked OS lifetime'
    assert size in (808, 904, 992, 1088, 1080, 1176), 'Unknown linked OS session size'
    return {'lifetime': 'resident' if magic == 0x415337334f524331 else 'supervised', 'session_bytes': size}


def inference_configuration(elf, legacy=False):
    """The adapter's actual linked count, placement and task priorities."""
    found = [s for s in elf.symbols if s['name'] == 'mimus_parallel_configuration' and s['shndx'] != 0]
    if not found and legacy:
        # The established adapter exposes WorkersFn as exactly "mov w0, #2;
        # ret". Do not turn a missing record into an eight-worker downgrade.
        pair = [s for s in elf.symbols if s['name'] == '_ZN12_GLOBAL__N_17workersEv' and s['shndx'] != 0]
        assert len(pair) == 1 and pair[0]['size'] == 8 and elf.read(pair[0]['value'], 8) == bytes.fromhex('40008052c0035fd6'), 'Legacy image does not contain the established two-worker adapter'
        return None
    assert len(found) == 1 and found[0]['bind'] == 1 and found[0]['size'] == 28, 'Missing or malformed inference configuration'
    words = struct.unpack('<7I', elf.read(found[0]['value'], 28))
    allowed = ((1, 2, 2, 0x0c, 3, 3, 0), (1, 2, 6, 0xc0, 3, 0, 3),
               (1, 8, 6, 0xff, 3, 2, 3))
    assert words in allowed, 'Unsupported inference count, placement or priority'
    return dict(zip(('version', 'workers', 'owner_core', 'core_mask', 'owner_priority',
                     'a55_priority', 'a76_priority'), words))


def number(text):
    """An unsigned integer literal or a product of them, as the headers write sizes."""
    assert re.fullmatch(r'[0-9a-fx* ]+', text), text
    value = 1
    for factor in text.split('*'):
        value *= int(factor.strip(), 0)
    return value


# The radio or Bluetooth profile and its firmware (apps/board/main.c, apps/bluetooth).
PROFILE_SPAN = (0x43000000, 0x43100000)


# The GPU output region and the unleased 2 MiB above it, read-only at EL0 and
# EL1 (apps/os/dispatch.h).
GPU_VIEW = (0x46c00000, 0x47000000)


def arena_bytes(text, gpu_linear):
    """kMimusArenaBytes: the GPU option's value is the first of an #ifdef pair."""
    values = [number(v) for v in re.findall(r'kMimusArenaBytes = ([^;]*);', text)]
    assert len(values) in (1, 2), values
    if len(values) == 2:
        branch = text[text.index('#ifdef SUNSTONE_A733_OS_GPU_LINEAR'):]
        assert branch.index('kMimusArenaBytes') < branch.index('#else'), 'arena branches out of order'
    return values[0] if gpu_linear or len(values) == 1 else values[1]


def layout(elf, assets, recognizer, loader, profile=False, gpu_linear=False):
    text = assets.read_text()
    addresses = [int(v, 0) for v in re.search(r'mimus_asset_address\[3\]=\{([^}]*)\}', text).group(1).split(',')]
    sizes = [int(v, 0) for v in re.search(r'mimus_asset_bytes\[3\]=\{([^}]*)\}', text).group(1).split(',')]
    text = recognizer.read_text()
    arena = [number(re.search(r'kMimusArenaBase = ([^;]*);', text).group(1)), arena_bytes(text, gpu_linear)]
    assert not gpu_linear or arena == [0x46000000, 12 << 20], 'the GPU option needs the 12 MiB arena at 0x46000000'

    text = loader.read_text()
    window = [int(re.search(r'#define %s UINT64_C\((0x[0-9a-fA-F]+)\)' % name, text).group(1), 0)
              for name in ('LOAD_BASE', 'LOAD_LIMIT')]
    regions = [{'name': name, 'start': a, 'end': a + n}
               for name, a, n in zip(('base-model', 'overlay-model', 'recorded-wav'), addresses, sizes)]
    regions.append({'name': 'recognizer-arena', 'start': arena[0], 'end': arena[0] + arena[1]})
    if profile:
        regions.append({'name': 'module-profile', 'start': PROFILE_SPAN[0], 'end': PROFILE_SPAN[1]})
    if gpu_linear:
        regions.append({'name': 'gpu-read-only-view', 'start': GPU_VIEW[0], 'end': GPU_VIEW[1]})
    sections = [(name, s[3], s[3] + s[5]) for name, s in zip(elf.names, elf.sections)
                if s[2] & 2 and s[5]]  # SHF_ALLOC
    for i, r in enumerate(regions):
        assert window[0] <= r['start'] < r['end'] <= window[1], '%s is outside the load window' % r['name']
        for other in regions[:i]:
            assert r['end'] <= other['start'] or other['end'] <= r['start'], (r['name'], other['name'])
        for name, start, end in sections:
            assert r['end'] <= start or end <= r['start'], '%s overlaps section %s' % (r['name'], name)
    return {'load_window': [hex(w) for w in window],
            'regions': [{'name': r['name'], 'start': hex(r['start']), 'end': hex(r['end'])} for r in regions],
            'image_end': hex(max(end for _, _, end in sections))}


def gpu_configuration(elf, required=False):
    """Actual bounded caller, task placement and immutable GPU inputs in this ELF."""
    def one(name):
        found = [s for s in elf.symbols if s['name'] == name and s['shndx'] not in (0, 0xfff1)]
        assert len(found) == 1, 'Missing or ambiguous GPU symbol: ' + name
        return found[0]

    present = [s for s in elf.symbols if s['name'] == 'os_gpu_configuration' and s['shndx'] != 0]
    if not present:
        linked = any(s['name'] in ('a733_gpu_mimus_finish', 'os_gpu_linear_start', 'gpu_firmware') and s['shndx'] != 0
                     for s in elf.symbols)
        assert not required and not linked, 'GPU image lacks the bounded caller configuration'
        return None
    config = one('os_gpu_configuration')
    assert config['size'] == 32, 'GPU configuration size differs'
    values = struct.unpack('<8I', elf.read(config['value'], 32))
    keys = ('version', 'reply_us', 'lifecycle_us', 'wait_ticks', 'worker_priority', 'core', 'core_hz', 'supply_uv')
    result = dict(zip(keys, values))
    assert values[:5] == (1, 2000000, 1000000, 10, 4) and values[5] in (2, 6), 'GPU caller configuration differs'
    assert values[6:] == (1008000000, 960000), 'GPU operating point differs'
    inference = inference_configuration(elf)
    assert inference['workers'] == 2 and inference['owner_core'] == result['core'], 'GPU inference placement differs'
    assert elf.read(one('os_recognition_core')['value'], 1)[0] == result['core'], 'GPU interrupt owner differs'
    routes = one('a733_irq_routes')
    assert routes['size'] % ROUTE_BYTES == 0, 'GPU interrupt table geometry differs'
    irq = [row for row in struct.iter_unpack('<HBB4xQ', elf.read(routes['value'], routes['size'])) if row[0] == 95]
    assert irq == [(95, result['core'], 0xd8, one('a733_gpu_mimus_irq')['value'])], 'GPU interrupt route differs'
    start = one('os_applications')
    assert start['size'] % APPLICATION_BYTES == 0, 'GPU start table geometry differs'
    entries = [hooks for name, *hooks in struct.iter_unpack('<4Q', elf.read(start['value'], start['size']))
               if elf.cstring(name) == 'gpu-linear']
    assert entries == [[one(n)['value'] for n in ('os_gpu_linear_task_privileged', 'os_gpu_linear_pins', 'os_gpu_linear_start')]], 'GPU start entry differs'
    assert any('SetExactSumHook' in s['name'] and s['shndx'] != 0 and s['type'] == 2 for s in elf.symbols), 'GPU engine hook is absent'
    start, end = one('gpu_firmware')['value'], one('gpu_firmware_end')['value']
    assert 0 < end - start <= 2 << 20, 'GPU firmware extent differs'
    result['firmware_sha256'] = hashlib.sha256(elf.read(start, end - start)).hexdigest()
    assert result['firmware_sha256'] == '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2', 'GPU firmware differs'
    tables = {}
    for name in ('os_placement_table', 'os_placement_tensors', 'os_placement_gpu_ordinals'):
        symbol = one(name)
        assert 0 < symbol['size'] <= 640 * 16, 'GPU placement extent differs'
        tables[name] = hashlib.sha256(elf.read(symbol['value'], symbol['size'])).hexdigest()
    result['placement_sha256'] = tables
    return result


def check(path, assets, recognizer, loader, audio=False, radio=False, headset=False, gpu_linear=False,
          cpu_clock=False, npu=False, perf=False):
    elf = Elf(path.read_bytes())
    defined = [s for s in elf.symbols if s['name'] and s['shndx'] not in (0, 0xfff1)]
    functions = {}
    for s in defined:
        if s['type'] == 2:  # STT_FUNC
            functions.setdefault(s['value'], set()).add(s['name'])

    def one(name):
        found = [s for s in defined if s['name'] == name and s['bind'] == 1]
        assert len(found) == 1, '%s must have one global definition, found %d' % (name, len(found))
        return found[0]

    for name in HOOKS:
        assert one(name)['type'] == 2, name
    present = sorted({s['name'] for s in defined} & set(PATCH_HOOKS))
    assert not present, 'per-device interrupt hooks linked: %s' % present

    table, count = one('a733_irq_routes'), one('a733_irq_route_count')
    routes = struct.unpack_from('<I', elf.read(count['value'], 4))[0]
    assert 0 < routes <= 64 and table['size'] == routes * ROUTE_BYTES, (routes, table['size'])
    raw = elf.read(table['value'], table['size'])
    interrupts = []
    for i in range(routes):
        intid, core, priority, handler = struct.unpack_from('<HBB4xQ', raw, i * ROUTE_BYTES)
        assert 32 <= intid < 1020 and core < 8 and priority >= 0xb0, (intid, core, priority)
        assert handler in functions, 'interrupt %d handler %#x is not a linked function' % (intid, handler)
        interrupts.append({'intid': intid, 'core': core, 'priority': priority,
                           'handler': sorted(functions[handler])})
    ids = [r['intid'] for r in interrupts]
    assert len(ids) == len(set(ids)), 'duplicate interrupt IDs %s' % ids

    start = one('os_applications')
    assert start['size'] and start['size'] % APPLICATION_BYTES == 0, start['size']
    raw = elf.read(start['value'], start['size'])
    applications, entries = [], []
    for i in range(start['size'] // APPLICATION_BYTES):
        name, *hooks = struct.unpack_from('<4Q', raw, i * APPLICATION_BYTES)
        for hook in hooks:
            assert hook in functions, 'start entry %d hook %#x is not a linked function' % (i, hook)
        entries += hooks
        applications.append({'name': elf.cstring(name), 'task_privileged': sorted(functions[hooks[0]]),
                             'pins': sorted(functions[hooks[1]]), 'start': sorted(functions[hooks[2]])})
    names = [a['name'] for a in applications]
    assert len(names) == len(set(names)), 'duplicate start entries %s' % names
    assert len(entries) == len(set(entries)), 'a start function is listed twice'
    hooks = {one(n)['value'] for n in HOOKS[:3]}
    assert not hooks & set(entries), 'a start entry is a board hook itself'
    expected = (['mimus', 'thermal'] + (['npu'] if npu else []) + (['audio'] if audio else []) + (['radio'] if radio else []) +
                (['headset'] if headset else []) + (['gpu-linear'] if gpu_linear else []))
    assert names == expected, 'start entries %s differ from %s' % (names, expected)
    assert ids == [71] + ([97] if npu else []) + ([143] if audio else []) + ([95] if gpu_linear else []), \
        'interrupt IDs %s differ from the build' % ids
    if gpu_linear:
        # Any other core takes the status-4 stop on a GPU completion.
        recognition = one('os_recognition_core')
        assert recognition['size'] == 1, recognition['size']
        core = elf.read(recognition['value'], 1)[0]
        assert core in (2, 6), 'recognition core %d is not the first of a core pair' % core
        assert interrupts[ids.index(95)]['core'] == core, \
            'INTID 95 is routed to core %d, the recognizer runs on core %d' % (interrupts[ids.index(95)]['core'], core)
    # The CPU clock build's owner step is linked exactly when the build says so,
    # so preparation can tell the session size from this record.
    owner = [s for s in defined if s['name'] == 'os_cpu_clock_step']
    assert bool(owner) == cpu_clock, 'os_cpu_clock_step linked: %s, CPU clock build: %s' % (bool(owner), cpu_clock)
    names = {s['name'] for s in defined}
    thermal = [name in names for name in ('os_thermal_record_begin', 'os_thermal_record_end', 'os_thermal_report_copy')]
    assert all(thermal) or not any(thermal), 'Incomplete thermal report implementation'
    assert not any(thermal) or cpu_clock, 'Thermal report outside CPU-clock profile'
    npu_profile = None
    if npu:
        assert not any((audio, radio, headset, gpu_linear, cpu_clock and not perf and contract_profile(elf)['lifetime'] != 'resident')), 'Unsupported combined NPU profile'
        symbols = {s['name']: s for s in elf.symbols if s['shndx'] != 0}
        start, end = (symbols[n]['value'] for n in ('__os_npu_pool_start', '__os_npu_pool_end'))
        assert (start, end) == (0x47400000, 0x47c00000), 'NPU reservation differs'
        for name, section in zip(elf.names, elf.sections):
            if section[2] & 2 and section[5]:
                assert section[3] + section[5] <= start or section[3] >= end, ('NPU overlap', name)
        assert interrupts[ids.index(97)]['core'] == 1, 'NPU interrupt core differs'
        catalogue = one('os_npu_catalogue_digest')
        selected = one('os_npu_client_model')
        npu_profile = {'range': [start, end], 'catalogue': elf.read(catalogue['value'], 32).hex(),
                       'client_model': struct.unpack('<I', elf.read(selected['value'], 4))[0]}
    # The performance-mode service and its boot mode (apps/os/perf.c), which
    # preparation and the host command read from this record.
    boot = [s for s in defined if s['name'] == 'os_perf_boot_mode']
    assert bool(boot) == perf and (not perf or cpu_clock), 'os_perf_boot_mode linked: %s, performance-mode build: %s' % (
        bool(boot), perf)
    perf_operation = None
    perf_boot = None
    if perf:
        value = struct.unpack('<I', elf.read(one('os_perf_boot_mode')['value'], 4))[0]
        assert value in (1, 2), 'performance boot mode %d' % value
        perf_boot = 'high' if value == 1 else 'low'
        perf_operation = struct.unpack('<I', elf.read(one('os_perf_usb_operation')['value'], 4))[0]
        assert perf_operation == 8, 'Performance opcode collides with thermal diagnostic'
    gpu_profile = gpu_configuration(elf, required=gpu_linear)
    assert bool(gpu_profile) == gpu_linear, 'GPU configuration differs from build selection'
    return {'inference': inference_configuration(elf), 'gpu_profile': gpu_profile, 'thermal_report': all(thermal), 'cpu_critical_trip': 'a733_thermal_policy_cpu_limit' in names,
            'npu_profile': npu_profile, 'perf_boot_mode': perf_boot, 'perf_operation': perf_operation, 'interrupts': interrupts, 'applications': applications,
            'layout': layout(elf, assets, recognizer, loader, radio or headset, gpu_linear)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--build-id', type=Path)
    parser.add_argument('--assets', type=Path, required=True, help='generated mimus input header (assets.h)')
    parser.add_argument('--recognizer', type=Path, required=True, help='apps/mimus/recorded.h')
    parser.add_argument('--loader', type=Path, required=True, help='boot/a733/stage2/loader.c')
    parser.add_argument('--lifetime', choices=('supervised', 'resident'), required=True)
    parser.add_argument('--audio', choices=('0', '1'), default='0')
    parser.add_argument('--radio', choices=('0', '1'), default='0')
    parser.add_argument('--headset', choices=('0', '1'), default='0')
    parser.add_argument('--gpu-linear', choices=('0', '1'), default='0')
    parser.add_argument('--npu', choices=('0', '1'), default='0')
    parser.add_argument('--cpu-clock', choices=('0', '1'), default='0')
    parser.add_argument('--perf', choices=('0', '1'), default='0')
    parser.add_argument('--workers', type=int, choices=(2, 8), default=2)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    audio, radio, headset = args.audio == '1', args.radio == '1', args.headset == '1'
    gpu_linear, cpu_clock = args.gpu_linear == '1', args.cpu_clock == '1'
    try:
        result = check(args.image, args.assets, args.recognizer, args.loader, audio, radio, headset, gpu_linear,
                       cpu_clock, args.npu == '1', args.perf == '1')
        profile = contract_profile(Elf(args.image.read_bytes()), legacy=True)
        if profile is not None:
            assert profile['lifetime'] == args.lifetime, 'Linked lifetime differs from build selection'
            assert profile['session_bytes'] == 808 + (184 if audio else 272 if args.npu == '1' else 0) + (96 if cpu_clock else 0), 'Linked session size differs from build selection'
            result['contract_profile'] = profile
        assert result['inference']['workers'] == args.workers, 'Linked worker count differs from build selection'
        if args.workers == 8:
            assert not any((audio, radio, headset, gpu_linear)), 'Unsupported eight-worker OS profile'
        if gpu_linear and args.lifetime == 'supervised':
            assert not any((audio, radio, headset, args.npu == '1', args.perf == '1')), 'Unsupported supervised GPU profile'
    except AssertionError as error:
        print('FAIL integrated OS link check:', error, file=sys.stderr)
        return 1
    result.update({'passed': True, 'target': 'sunstone-a733-os', 'lifetime': args.lifetime, 'audio': audio,
                   'radio': radio, 'headset': headset, 'gpu_linear': gpu_linear, 'cpu_clock': cpu_clock, 'npu': args.npu == '1',
                   'perf_modes': args.perf == '1',
                   'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest()})
    if result['perf_boot_mode'] is None:
        del result['perf_boot_mode']
    if args.build_id:
        result['build_id'] = args.build_id.read_text().strip()
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS integrated OS link check:', len(result['interrupts']), 'interrupts,',
          len(result['applications']), 'start entries,', len(result['layout']['regions']),
          'fixed input regions')
    return 0


if __name__ == '__main__':
    sys.exit(main())
