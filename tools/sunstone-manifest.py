#!/usr/bin/env python3
"""Generate and decode SunstoneOS application manifests (docs/designs/app-abi.md).

`emit` validates an image's application manifest.json, assigns the addresses
of unpinned regions, and writes a C source that places the binary manifest
in the section "sunstone_manifest", a header with the region indices and the
prototypes of the application's functions, and the packed bytes that the C
compiler must reproduce. `decode` reads the bytes of that section, as host
tools do from a linked image, and prints the manifests as JSON.

An image holds one application. Status: prototype. The OS build does not
call this tool yet.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import memory_layout

MAGIC = 0x5453464e4d505041
VERSION = 1
MANIFEST_BYTES = 552
REGIONS = 8
HEAD = struct.Struct('<QII16sIIBBBBIQQQQIBBBB')
AUDIO = struct.Struct('<IHHII')
REGION = struct.Struct('<QQII32s')
assert HEAD.size == 88 and AUDIO.size == 16 and REGION.size == 56
assert HEAD.size + AUDIO.size + REGIONS * REGION.size == MANIFEST_BYTES

DEVICES = {'usb-storage': 1 << 0, 'usb-keyboard': 1 << 1, 'audio-i2s': 1 << 2,
           'audio-headset': 1 << 3, 'gpu': 1 << 4,
           'usb-mouse': 1 << 5, 'usb-serial': 1 << 6, 'usb-camera': 1 << 7}
CAPTURE = DEVICES['audio-i2s'] | DEVICES['audio-headset']
CLUSTER = {'a55': 0, 'a76': 1}
KIND = {'input': 1, 'work': 2, 'output': 3}
ACCESS = {'read': 1, 'write': 2, 'gpu-read': 4, 'gpu-write': 8}
# Application regions lie in the EL0 read-write blocks above the OS-owned
# radio and Bluetooth profile reservation and below the loader limit.
WINDOW = tuple(memory_layout.record()['window'])
SLOT1 = tuple(memory_layout.record()['slot1_area'])
AREAS = (WINDOW, SLOT1)
PAGE = 0x1000
BLOCK = 0x200000
# OS host operations: the fault, NPU, thermal and performance reports keep
# 0x05..0x08, and new OS operations start at 0x80.
OS_REPORTS = (0x05, 0x09)
OS_HOST_FIRST = 0x80
OUTPUT_LIMIT = 65536


def failure_record_bytes():
    """The size of struct ss_failure, from the static assertion beside it in include/sunstone_session.h."""
    text = (Path(__file__).resolve().parents[1] / 'include/sunstone_session.h').read_text()
    found = re.search(r'sizeof\(struct ss_failure\)\s*==\s*(\d+)', text)
    if found is None:
        raise SystemExit('include/sunstone_session.h has no size assertion for struct ss_failure')
    return int(found.group(1))


FAILURE_RECORD_BYTES = failure_record_bytes()


def fail(message):
    raise SystemExit('manifest: ' + message)


def require(ok, message):
    if not ok:
        fail(message)


def bits(names, where):
    value = 0
    for name in names:
        require(name in DEVICES, '%s: unknown device %r' % (where, name))
        value |= DEVICES[name]
    return value


def integer(value, where, low=0, high=(1 << 64) - 1):
    if isinstance(value, str):
        require(re.fullmatch(r'0x[0-9a-fA-F]+|[0-9]+', value) is not None, where + ': not an integer')
        value = int(value, 0)
    require(type(value) is int and low <= value <= high, '%s: %r outside %d..%d' % (where, value, low, high))
    return value


def load(path):
    """manifest.json -> normalized dict with integer fields."""
    raw = json.loads(Path(path).read_text())
    known = {'name', 'devices', 'cpu', 'regions', 'large_regions', 'audio', 'deadlines', 'output_limit', 'host_ops', 'npu'}
    require(set(raw) <= known, '%s: unknown keys %s' % (path, sorted(set(raw) - known)))
    name = raw['name']
    require(isinstance(name, str) and re.fullmatch(r'[a-z][a-z0-9-]{0,14}', name) is not None,
            '%s: name must be 1-15 characters [a-z0-9-] starting with a letter' % path)
    devices = raw.get('devices', {})
    required = bits(devices.get('required', []), name)
    optional = bits(devices.get('optional', []), name)
    require(not required & optional, name + ': a device is both required and optional')
    # A session grants at most one capture device; listing both is allowed
    # only when both are optional.
    wanted, listed = required & CAPTURE, (required | optional) & CAPTURE
    require(not wanted or (listed == wanted and bin(wanted).count('1') == 1),
            name + ': a required capture device excludes the other')
    cpu = raw['cpu']
    require(set(cpu) <= {'cluster', 'workers', 'priority', 'stack_bytes', 'heap_bytes'},
            name + ': unknown cpu keys %s' % sorted(set(cpu) - {'cluster', 'workers', 'priority', 'stack_bytes', 'heap_bytes'}))
    require(cpu.get('cluster', 'a55') in CLUSTER, name + ': cluster is a55 or a76')
    m = {'name': name, 'devices_required': required, 'devices_optional': optional,
         'cluster': CLUSTER[cpu.get('cluster', 'a55')],
         'workers': integer(cpu.get('workers', 0), name + ' workers', 0, 7),
         'priority': integer(cpu['priority'], name + ' priority', 1, 30),
         'stack_bytes': integer(cpu['stack_bytes'], name + ' stack_bytes', 1024, 1 << 20),
         'heap_bytes': integer(cpu.get('heap_bytes', 0), name + ' heap_bytes', 0, LARGE_GIB)}
    require(m['stack_bytes'] % 16 == 0 and m['heap_bytes'] % 16 == 0, name + ': stack and heap are 16-byte multiples')
    d = raw['deadlines']
    m['run_deadline_us'] = integer(d.get('run_us', 0), name + ' run_us')
    m['return_reserve_us'] = integer(d['return_reserve_us'], name + ' return_reserve_us')
    m['progress_window_us'] = integer(d['progress_window_us'], name + ' progress_window_us', 1, 60000000)
    m['output_limit'] = integer(raw.get('output_limit', 0), name + ' output_limit', 0, OUTPUT_LIMIT)
    require(m['output_limit'] >= FAILURE_RECORD_BYTES,
            '%s: output_limit %d is smaller than the %d-byte failure record (struct ss_failure in '
            'include/sunstone_session.h), which the failure return publishes in the output' % (name, m['output_limit'], FAILURE_RECORD_BYTES))
    ops = raw.get('host_ops', {'first': 0, 'count': 0})
    first = m['host_op_first'] = integer(ops['first'], name + ' host_ops.first', 0, OS_HOST_FIRST - 1)
    count = m['host_op_count'] = integer(ops['count'], name + ' host_ops.count', 0, OS_HOST_FIRST)
    require(first + count <= OS_HOST_FIRST, name + ': host operations reach the OS range 0x80..0xff')
    require(not count or first + count <= OS_REPORTS[0] or OS_REPORTS[1] <= first,
            name + ': host operations overlap the OS reports 0x05..0x08')
    require(count or not first, name + ': an empty host range starts at 0')
    # What the application declares of the NPU (docs/designs/npu-resources.md). It is not part of the packed binary:
    # the session record carries it (npu_word, apps/os/host.py), and the operating system admits it against capacity.
    if 'npu' in raw:
        n = raw['npu']
        require(set(n) <= {'slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'}, name + ': unknown npu keys')
        require({'slots', 'in_flight', 'graphs'} <= set(n), name + ': npu declares slots, in_flight and graphs')

        def count(value, label, low):
            return integer(len(value) if isinstance(value, list) else value, name + ' npu ' + label, low, (1 << 32) - 1)
        m['npu'] = {'slots': count(n['slots'], 'slots', 1), 'in_flight': count(n['in_flight'], 'in_flight', 1),
                    'graphs': count(n['graphs'], 'graphs', 1), 'links': count(n.get('links', 0), 'links', 0),
                    'graph_bytes': count(n.get('graph_bytes', 0), 'graph_bytes', 0),
                    'tensor_bytes': count(n.get('tensor_bytes', 0), 'tensor_bytes', 0)}
    audio = raw.get('audio')
    if audio is not None or (required | optional) & CAPTURE:
        require(audio is not None, name + ': a capture device needs an audio format')
        m['audio'] = {'rate_hz': integer(audio['rate_hz'], name + ' rate_hz', 8000, 192000),
                      'channels': integer(audio['channels'], name + ' channels', 1, 2),
                      'bits': integer(audio['bits'], name + ' bits', 16, 32),
                      'min_frames': integer(audio['min_frames'], name + ' min_frames', 1, 1 << 24),
                      'max_frames': integer(audio['max_frames'], name + ' max_frames', 1, 1 << 24)}
        require(m['audio']['bits'] in (16, 24, 32), name + ': bits is 16, 24 or 32')
        require(m['audio']['min_frames'] <= m['audio']['max_frames'], name + ': min_frames exceeds max_frames')
    else:
        m['audio'] = {'rate_hz': 0, 'channels': 0, 'bits': 0, 'min_frames': 0, 'max_frames': 0}
    regions, names = [], []
    require(len(raw.get('regions', [])) <= REGIONS, name + ': at most %d regions' % REGIONS)
    for i, r in enumerate(raw.get('regions', [])):
        where = '%s region %d' % (name, i)
        require(re.fullmatch(r'[a-z][a-z0-9_]{0,23}', r.get('name', '')) is not None and r['name'] not in names,
                where + ': distinct name [a-z0-9_] required')
        names.append(r['name'])
        require(r.get('kind') in KIND, where + ': kind is input, work or output')
        kind = KIND[r['kind']]
        access = 0
        for a in r.get('access', []):
            require(a in ACCESS, where + ': unknown access %r' % a)
            access |= ACCESS[a]
        size = integer(r['bytes'], where + ' bytes', 1, SLOT1[1] - SLOT1[0])
        digest = r.get('sha256')
        if digest is not None:
            require(kind == KIND['input'] and re.fullmatch(r'[0-9a-f]{64}', digest) is not None,
                    where + ': only an input pins a 64-digit lowercase SHA-256')
        if kind == KIND['input']:
            require(access & ACCESS['read'] and not access & (ACCESS['write'] | ACCESS['gpu-write']),
                    where + ': an input is read-only')
        else:
            require(size % PAGE == 0, where + ': capacity is a multiple of 4 KiB')
            require(access & ACCESS['read'], where + ': the CPU reads work and output regions')
        if kind == KIND['output']:
            require(access & ACCESS['write'], where + ': the CPU writes an output region')
        address = r.get('address')
        if address is not None:
            address = integer(address, where + ' address')
            require(address % PAGE == 0, where + ': a pinned address is 4 KiB aligned')
            if access & (ACCESS['gpu-read'] | ACCESS['gpu-write']):
                require(address % BLOCK == 0, where + ': a pinned GPU address is 2 MiB aligned')
        regions.append({'address': address, 'bytes': size, 'kind': kind, 'access': access,
                        'sha256': digest or '0' * 64})
    m['regions'] = regions
    m['region_names'] = names
    m['large_regions'], m['large_names'] = load_large(name, raw.get('large_regions', []))
    add_heap_region(name, m)
    return m


LARGE = struct.Struct('<IIQQQ32s48s')
assert LARGE.size == 112
LARGE_GIB = 11 << 30      # the upper memory; the only bound on the number and total size of large regions
LARGE_PATH = re.compile(r'/[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*')


def load_large(name, items):
    """The large_regions of manifest.json: regions of upper memory loaded from the card (sunstone_large.h)."""
    require(isinstance(items, list) and len(items) <= LARGE_GIB // BLOCK, name + ': more large regions than 2 MiB blocks in upper memory')
    records, names = [], []
    for i, r in enumerate(items):
        where = '%s large region %d' % (name, i)
        require(set(r) <= {'name', 'bytes', 'source_bytes', 'access', 'file', 'first_sector', 'sha256'},
                where + ': unknown keys %s' % sorted(set(r) - {'name', 'bytes', 'source_bytes', 'access', 'file', 'first_sector', 'sha256'}))
        require(re.fullmatch(r'[a-z][a-z0-9_]{0,23}', r.get('name', '')) is not None and r['name'] not in names,
                where + ': distinct name [a-z0-9_] required')
        names.append(r['name'])
        size = integer(r['bytes'], where + ' bytes', BLOCK, LARGE_GIB)
        require(size % BLOCK == 0, where + ': capacity is a multiple of 2 MiB')
        source = integer(r['source_bytes'], where + ' source_bytes', 1, size)
        access = r.get('access', ['read'])
        require(access in (['read'], ['read', 'write']), where + ': access is [read] or [read, write]')
        digest = r.get('sha256', '')
        require(re.fullmatch(r'[0-9a-f]{64}', digest) is not None and digest != '0' * 64,
                where + ': a 64-digit lowercase SHA-256 other than zero is required')
        file, sector = r.get('file'), r.get('first_sector')
        require((file is None) != (sector is None), where + ': name a file or a first_sector, not both')
        flags = 1 if 'write' in access else 0
        if file is not None:
            require(isinstance(file, str) and len(file) <= 47 and LARGE_PATH.fullmatch(file) is not None,
                    where + ': file is an absolute path of at most 47 characters [A-Za-z0-9._/-]')
            sector = 0
        else:
            flags |= 2
            sector = integer(sector, where + ' first_sector', 1, (1 << 40))
            file = ''
        records.append({'index': i, 'flags': flags, 'bytes': size, 'source_bytes': source, 'first_sector': sector,
                        'sha256': digest, 'path': file})
    require(sum(r['bytes'] for r in records) <= LARGE_GIB, name + ': large regions together exceed the 11 GiB of upper memory')
    return records, names


LARGE_HEAP = 4          # SS_LARGE_HEAP in include/sunstone_large.h


def add_heap_region(name, m):
    """A heap above the linked limit is a region of the ledger: the last large record, read-write, with no source.

    The image links only the OS's share of the heap then (cmake/sunstone-image.cmake), so the region holds the
    manifest's heap_bytes on top of that share, rounded up to 2 MiB."""
    layout = memory_layout.record()
    if m['heap_bytes'] <= layout['heap_link_max']:
        return
    size = -(-(m['heap_bytes'] + layout['heap_base']) // BLOCK) * BLOCK
    m['large_regions'].append({'index': len(m['large_regions']), 'flags': LARGE_HEAP | 1, 'bytes': size, 'source_bytes': 0,
                               'first_sector': 0, 'sha256': '0' * 64, 'path': ''})
    require(sum(r['bytes'] for r in m['large_regions']) <= LARGE_GIB,
            name + ': large regions and heap together exceed the 11 GiB of upper memory')


def pack_large(m):
    return b''.join(LARGE.pack(r['index'], r['flags'], r['bytes'], r['source_bytes'], r['first_sector'],
                               bytes.fromhex(r['sha256']), r['path'].encode()) for r in m['large_regions'])


def unpack_large(raw):
    require(len(raw) % LARGE.size == 0, 'large section size %d is not a record multiple' % len(raw))
    out = []
    for base in range(0, len(raw), LARGE.size):
        index, flags, size, source, sector, digest, path = LARGE.unpack_from(raw, base)
        heap = flags == LARGE_HEAP | 1
        require(index == base // LARGE.size and not flags & ~7 and size and size % BLOCK == 0 and
                (heap and not source and not sector and not any(digest) or 0 < source <= size and any(digest) and not flags & LARGE_HEAP),
                'large record at %d is malformed' % base)
        text = path.rstrip(b'\0')
        require(b'\0' not in text and len(path) - len(text) >= 1, 'large record at %d: path has no terminator' % base)
        out.append({'index': index, 'flags': flags, 'bytes': size, 'source_bytes': source, 'first_sector': sector,
                    'sha256': digest.hex(), 'path': text.decode()})
    return out


def span(r):
    # GPU leases and CPU output permissions reserve complete 2 MiB blocks.
    alignment = BLOCK if r['access'] & (ACCESS['gpu-read'] | ACCESS['gpu-write']) or r['address'] >= SLOT1[0] else PAGE
    return r['address'], r['address'] + -(-r['bytes'] // alignment) * alignment


def assign(m):
    """Place unpinned regions at the lowest free 2 MiB boundary in the window.

    A second application per image would pass every manifest of the image
    through this placement, so that regions of different applications are
    checked against each other."""
    taken = []

    def place(i, r):
        where = '%s region %s' % (m['name'], m['region_names'][i])
        low, high = span(r)
        require(any(a <= low and high <= b for a, b in AREAS),
                '%s: [%#x, %#x) outside the application window and the slot 1 area' % (where, low, high))
        require(low < SLOT1[0] or low % BLOCK == 0, '%s: a region in the slot 1 area starts on a 2 MiB boundary' % where)
        for other, (a, b) in taken:
            require(high <= a or b <= low, '%s overlaps region %s' % (where, other))
        taken.append((m['region_names'][i], (low, high)))

    for i, r in enumerate(m['regions']):
        if r['address'] is not None:
            place(i, r)
    for i, r in enumerate(m['regions']):
        if r['address'] is None:
            r['address'] = WINDOW[0]
            while True:
                low, high = span(r)
                if high > WINDOW[1] and low < SLOT1[0]:          # the window is full: continue in the slot 1 area
                    r['address'] = SLOT1[0]
                    continue
                clash = [b for _, (a, b) in taken if not (high <= a or b <= low)]
                if not clash:
                    break
                r['address'] = -(-max(clash) // BLOCK) * BLOCK
            place(i, r)
    return m


def pack(m):
    out = HEAD.pack(MAGIC, VERSION, MANIFEST_BYTES, m['name'].encode(), m['devices_required'],
                    m['devices_optional'], m['cluster'], m['workers'], m['priority'], 0,
                    m['stack_bytes'], m['heap_bytes'], m['run_deadline_us'], m['return_reserve_us'],
                    m['progress_window_us'], m['output_limit'], m['host_op_first'],
                    m['host_op_count'], len(m['regions']), 0)
    a = m['audio']
    out += AUDIO.pack(a['rate_hz'], a['channels'], a['bits'], a['min_frames'], a['max_frames'])
    for r in m['regions']:
        out += REGION.pack(r['address'], r['bytes'], r['kind'], r['access'], bytes.fromhex(r['sha256']))
    out += bytes(REGION.size * (REGIONS - len(m['regions'])))
    assert len(out) == MANIFEST_BYTES
    return out


def unpack(raw):
    """Section bytes -> list of manifests; refuses anything pack() cannot produce."""
    require(len(raw) % MANIFEST_BYTES == 0 and raw, 'section size %d is not a manifest multiple' % len(raw))
    result = []
    for base in range(0, len(raw), MANIFEST_BYTES):
        b = raw[base:base + MANIFEST_BYTES]
        (magic, version, size, name, required, optional, cluster, workers, priority, reserved0, stack, heap,
         run, reserve, window, output, first, count, regions, reserved1) = HEAD.unpack_from(b)
        require((magic, version, size) == (MAGIC, VERSION, MANIFEST_BYTES) and not reserved0 and not reserved1 and
                regions <= REGIONS, 'manifest at %d: header differs' % base)
        rate, channels, bits_, low, high = AUDIO.unpack_from(b, HEAD.size)
        m = {'name': name.rstrip(b'\0').decode(), 'devices_required': required, 'devices_optional': optional,
             'cluster': cluster, 'workers': workers, 'priority': priority,
             'stack_bytes': stack, 'heap_bytes': heap, 'run_deadline_us': run, 'return_reserve_us': reserve,
             'progress_window_us': window, 'output_limit': output,
             'host_op_first': first, 'host_op_count': count,
             'audio': {'rate_hz': rate, 'channels': channels, 'bits': bits_, 'min_frames': low, 'max_frames': high},
             'regions': []}
        for i in range(REGIONS):
            address, length, kind, access, digest = REGION.unpack_from(b, HEAD.size + AUDIO.size + i * REGION.size)
            if i < regions:
                m['regions'].append({'address': address, 'bytes': length, 'kind': kind, 'access': access,
                                     'sha256': digest.hex()})
            else:
                require(not (address or length or kind or access or any(digest)), 'manifest at %d: unused region set' % base)
        result.append(m)
    return result


def public(m):
    return {k: v for k, v in m.items() if k not in ('region_names', 'large_names')}


def cname(name):
    return name.replace('-', '_')


def c_source(m):
    c = cname(m['name'])
    a = m['audio']
    lines = ['/* Application manifest: %s. */' % m['name'],
             '#include "%s-app.h"' % c,
             '__attribute__((used, retain, section("sunstone_manifest"), aligned(8)))',
             'const struct ss_manifest %s_manifest = {' % c,
             '    .magic = SS_MANIFEST_MAGIC, .version = SS_MANIFEST_VERSION, .bytes = sizeof(struct ss_manifest),',
             '    .name = "%s",' % m['name'],
             '    .devices_required = %#xu, .devices_optional = %#xu,' % (m['devices_required'], m['devices_optional']),
             '    .cluster = %d, .workers = %d, .priority = %d, .stack_bytes = %du,' % (
                 m['cluster'], m['workers'], m['priority'], m['stack_bytes']),
             '    .heap_bytes = UINT64_C(%d), .run_deadline_us = UINT64_C(%d),' % (m['heap_bytes'], m['run_deadline_us']),
             '    .return_reserve_us = UINT64_C(%d), .progress_window_us = UINT64_C(%d),' % (
                 m['return_reserve_us'], m['progress_window_us']),
             '    .output_limit = %du,' % m['output_limit'],
             '    .host_op_first = %d, .host_op_count = %d, .region_count = %d,' % (
                 m['host_op_first'], m['host_op_count'], len(m['regions'])),
             '    .audio = {%du, %du, %du, %du, %du},' % (
                 a['rate_hz'], a['channels'], a['bits'], a['min_frames'], a['max_frames']),
             '    .regions = {']
    for r in m['regions']:
        digest = ','.join('0x' + r['sha256'][i:i + 2] for i in range(0, 64, 2))
        lines.append('        {UINT64_C(%#x), UINT64_C(%#x), %d, %#xu, {%s}},' % (
            r['address'], r['bytes'], r['kind'], r['access'], digest))
    lines += ['    },', '};', '']
    if m['large_regions']:
        lines += ['__attribute__((used, retain, section("sunstone_large_regions"), aligned(8)))',
                  'const struct ss_large_region %s_large_regions[] = {' % c]
        for r in m['large_regions']:
            digest = ','.join('0x' + r['sha256'][i:i + 2] for i in range(0, 64, 2))
            lines.append('    {%du, %#xu, UINT64_C(%#x), UINT64_C(%#x), UINT64_C(%d), {%s}, "%s"},' % (
                r['index'], r['flags'], r['bytes'], r['source_bytes'], r['first_sector'], digest, r['path']))
        lines += ['};', '']
    return '\n'.join(lines)


def c_header(m):
    c = cname(m['name'])
    guard = c.upper() + '_APP_H'
    lines = ['/* The %s application\'s functions' % m['name'],
             ' * and its indices into ss_grant()->regions. */',
             '#ifndef ' + guard, '#define ' + guard, '#include "sunstone_app.h"',
             '#ifdef __cplusplus', 'extern "C" {', '#endif',
             'extern const struct ss_manifest %s_manifest;' % c,
             '/* Optional: once, before the scheduler starts. */',
             'void %s_init(void);' % c,
             '/* The main task, at EL0, after input authentication. */',
             'void %s_main(const struct ss_grant *grant);' % c]
    for i, n in enumerate(m['region_names']):
        lines.append('#define %s_REGION_%s %du' % (c.upper(), n.upper(), i))
    for i, n in enumerate(m['large_names']):
        lines.append('#define %s_LARGE_%s %du' % (c.upper(), n.upper(), i))
    lines.append('#define %s_LARGE_COUNT %du' % (c.upper(), len(m['large_names'])))
    lines += ['#ifdef __cplusplus', '}', '#endif', '#endif', '']
    return '\n'.join(lines)


def c_tables(m):
    noapp=m is None
    if noapp:
        m={'name':'os-core','stack_bytes':16,'output_limit':0,'progress_window_us':1,
           'devices_required':0,'devices_optional':0,'audio':{'max_frames':0,'channels':0,'bits':0}}
    c = cname(m['name'])
    lines = ['/* Fixed image composition from the application manifest. */',
             '#include "os.h"', '#include "application.h"', '#include "irq_table.h"',
             '#include "%s-app.h"' % c,
             'extern void %s_init(void) __attribute__((weak));' % c,
             'static StackType_t app_stack[%d / sizeof(StackType_t)];' % m['stack_bytes'],
             'static StaticTask_t app_task;',
             'const struct ss_application ss_application = {&%s_manifest,%s_init,%s_main,app_stack,&app_task};' % (c,c,c),
             '__attribute__((used,retain,aligned(8))) uint8_t ss_result_record[%d];' % (56+m['output_limit']),
             'const size_t ss_result_capacity = %du;' % m['output_limit'],
             '#include "app_large.h"',
             '/* Storage for the large regions this manifest declares: the count is the manifest\'s, not a constant. */',
             'const unsigned os_large_capacity = %du;' % len(m.get('large_regions', [])),
             'struct large_slot os_large_slot_storage[%d];' % max(1, len(m.get('large_regions', []))),
             '/* The ledger also holds the manifest regions that lie in the slot 1 area. */',
             'const unsigned os_ledger_capacity = %du;' % (len(m.get('large_regions', [])) + len(m.get('regions', [])) + 2),
             'struct app_region_entry os_large_entry_storage[%d];' % max(1, len(m.get('large_regions', [])) + len(m.get('regions', [])) + 2),
             '/* A task waits on one region at a time: one entry per task of the application. */',
             'const unsigned os_large_waiter_capacity = %du;' % (m.get('workers', 0) + 1),
             'TaskHandle_t os_large_waiter_task[%d];' % (m.get('workers', 0) + 1),
             'unsigned os_large_waiter_region[%d];' % (m.get('workers', 0) + 1),
             '/* The OS waits of the application\'s tasks that the OS may wake (apps/os/application.c). */',
             'const unsigned os_wake_capacity = %du;' % (m.get('workers', 0) + 1),
             'TaskHandle_t os_wake_task[%d];' % (m.get('workers', 0) + 1),
             'unsigned os_wake_gave[%d];' % (m.get('workers', 0) + 1),
             'int os_large_task_privileged(void (*)(void *),void *);',
             'int os_large_pins(struct a733_pin_handoff *);',
             'int os_large_start(void);',
             '__attribute__((aligned(4))) uint8_t ss_capture_buffer[%d];' % (64 + max(8192, -(-(m['audio']['max_frames']*m['audio']['channels']*(m['audio']['bits']//8))//4096)*4096) if m['audio']['max_frames'] else 64),
             'const size_t ss_capture_capacity = sizeof(ss_capture_buffer)-SS_CAPTURE_HEADROOM;',
             '__attribute__((used,retain)) const struct os_application os_applications[] = {',
             '    {"thermal",os_thermal_task_privileged,os_thermal_pins,os_thermal_start},',
             '    {"large",os_large_task_privileged,os_large_pins,os_large_start},',
             '#ifdef SUNSTONE_A733_OS_NPU',
             '    {"npu",os_npu_task_privileged,os_npu_pins,os_npu_start},', '#endif',
             '#ifdef SUNSTONE_A733_OS_AUDIO',
             '    {"audio",os_audio_task_privileged,os_audio_pins,os_capture_start},', '#endif',
             '#ifdef SUNSTONE_A733_OS_DMA',
             '    {"dma",os_dma_task_privileged,os_dma_pins,os_dma_start},', '#endif',
             '#ifdef SUNSTONE_A733_OS_RADIO',
             '    {"radio",os_radio_task_privileged,os_radio_pins,os_radio_start},', '#endif',
             '#ifdef SUNSTONE_A733_OS_HEADSET',
             '    {"headset",os_headset_task_privileged,os_headset_pins,os_headset_start},', '#endif',
             '#ifdef SUNSTONE_A733_OS_GPU',
             '    {"gpu-queue",os_gpu_queue_task_privileged,os_gpu_queue_pins,os_gpu_queue_start_entry},', '#endif',
             '    {"%s",os_application_task_privileged,os_application_pins,os_application_start},' % m['name'],
             '};', 'const unsigned os_application_count=sizeof(os_applications)/sizeof(os_applications[0]);',
             '__attribute__((used,retain)) const struct os_service os_services[OS_SERVICES] = {',
             '    {"board-pmic",1000000},{"board-usb0",1000000},{"board-typec",1000000},',
             '    {"board-power",1000000},{"board-usb2",1000000},{"%s",%du},{"thermal",1000000},' % (m['name'],m['progress_window_us']),
             '#ifdef SUNSTONE_A733_OS_NPU', '    {"npu",5000000},', '#endif',
             '#ifdef SUNSTONE_A733_OS_AUDIO', '    {"audio",1000000},', '#endif',
             '#ifdef SUNSTONE_A733_OS_RADIO', '    {"radio",1000000},{"network",1000000},', '#endif',
             '#ifdef SUNSTONE_A733_OS_HEADSET', '    {"bluetooth",1000000},{"headset",1000000},', '#endif', '};',
             '#ifdef SUNSTONE_BOARD_USB2_IRQ', 'extern void board_host_irq(void);', '#endif',
             '#ifdef SUNSTONE_A733_HW_FACTS', 'extern void os_hw_facts_irq(void);', '#endif',
             '#ifdef SUNSTONE_A733_OS_SD_WRITE', 'extern void os_sd_irq(void);', '#endif',
             'const struct a733_irq_route a733_irq_routes[] = {', '    {71,0,0xd8,os_thermal_irq},',
             '#ifdef SUNSTONE_BOARD_USB2_IRQ', '    {187,0,0xd8,board_host_irq},', '#endif',
             '#ifdef SUNSTONE_A733_OS_NPU', '    {97,OS_NPU_CORE,0xd8,os_npu_irq},', '#endif',
             '#ifdef SUNSTONE_A733_OS_SD_WRITE', '    {193,0,0xd8,os_sd_irq},', '#endif',
             '#ifdef SUNSTONE_A733_OS_AUDIO', '    {143,0,0xd8,os_audio_irq},', '#endif',
             '#ifdef SUNSTONE_A733_OS_DMA', '    {143,0,0xd8,os_dma_irq},', '#endif',
             '#ifdef SUNSTONE_A733_OS_GPU', ('    {95,A733_SMP_APPLICATION_CORE,0xd8,os_gpu_queue_irq},' if m.get('cluster', 0) else '    {95,2,0xd8,os_gpu_queue_irq},'), '#endif',
             '#ifdef SUNSTONE_A733_HW_FACTS', '    {97,0,0xd8,os_hw_facts_irq},{143,0,0xd8,os_hw_facts_irq},{95,0,0xd8,os_hw_facts_irq},', '#endif', '};',
             'const unsigned a733_irq_route_count=sizeof(a733_irq_routes)/sizeof(a733_irq_routes[0]);',
             '#ifdef SUNSTONE_A733_OS_GPU', '#include "gpu-runtime.h"',
             '__attribute__((used,retain)) const uint32_t os_gpu_configuration[8]={',
             '    1,OS_GPU_REPLY_US,OS_GPU_OPEN_US,OS_GPU_WAIT_TICKS,OS_GPU_PRIORITY,',
             ('    A733_SMP_APPLICATION_CORE,' if m.get('cluster', 0) else '    2,'),
             '#if defined(SUNSTONE_A733_OS_PERF) && OS_PERF_BOOT_MODE == 2',
             '    A733_GPU_LOW_HZ,A733_GPU_LOW_UV};', '#else',
             '    A733_GPU_CORE_HZ,A733_GPU_SUPPLY_UV};', '#endif', '#endif', '']
    # Placement is an OS setting; the GPU route uses the same admitted pair.
    if noapp:
        lines=[line for line in lines if not any(key in line for key in ('os_core-app.h','os_core_init','app_stack','app_task','{\"os-core\",os_application'))]
        lines.insert(4,'const struct ss_application ss_application={0};')
    lines.insert(4, '#include "a733_smp_placement.h"')
    return '\n'.join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    e = sub.add_parser('emit')
    e.add_argument('--manifest', type=Path)
    e.add_argument('--no-application', action='store_true')
    e.add_argument('--output', type=Path, required=True)
    d = sub.add_parser('decode')
    d.add_argument('section', type=Path)
    dl = sub.add_parser('decode-large')
    dl.add_argument('section', type=Path)
    a = p.parse_args()
    if a.command == 'decode':
        json.dump(unpack(a.section.read_bytes()), sys.stdout, indent=2)
        print()
        return
    if a.command == 'decode-large':
        json.dump(unpack_large(a.section.read_bytes()), sys.stdout, indent=2)
        print()
        return
    require(bool(a.manifest) != a.no_application, 'select --manifest or --no-application')
    if a.no_application:
        a.output.mkdir(parents=True,exist_ok=True)
        (a.output/'sunstone-tables.c').write_text(c_tables(None))
        (a.output/'manifest.bin').write_bytes(b'')
        (a.output/'manifest.json').write_text('[]\n')
        return
    m = assign(load(a.manifest))
    a.output.mkdir(parents=True, exist_ok=True)
    c = cname(m['name'])
    (a.output / (c + '-manifest.c')).write_text(c_source(m))
    (a.output / (c + '-app.h')).write_text(c_header(m))
    (a.output / 'sunstone-tables.c').write_text(c_tables(m))
    (a.output / 'manifest.bin').write_bytes(pack(m))
    (a.output / 'large.bin').write_bytes(pack_large(m))
    (a.output / 'manifest.json').write_text(json.dumps([public(m)], indent=2) + '\n')


if __name__ == '__main__':
    main()
