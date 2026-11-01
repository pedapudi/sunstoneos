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
WINDOW = (0x44000000, 0x47000000)
PAGE = 0x1000
BLOCK = 0x200000
# OS host operations: the fault, NPU, thermal and performance reports keep
# 0x05..0x08, and new OS operations start at 0x80.
OS_REPORTS = (0x05, 0x09)
OS_HOST_FIRST = 0x80
OUTPUT_LIMIT = 65536


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
    known = {'name', 'devices', 'cpu', 'regions', 'audio', 'deadlines', 'output_limit', 'host_ops'}
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
         'heap_bytes': integer(cpu.get('heap_bytes', 0), name + ' heap_bytes', 0, 64 << 20)}
    require(m['stack_bytes'] % 16 == 0 and m['heap_bytes'] % 16 == 0, name + ': stack and heap are 16-byte multiples')
    d = raw['deadlines']
    m['run_deadline_us'] = integer(d.get('run_us', 0), name + ' run_us')
    m['return_reserve_us'] = integer(d['return_reserve_us'], name + ' return_reserve_us')
    m['progress_window_us'] = integer(d['progress_window_us'], name + ' progress_window_us', 1, 60000000)
    m['output_limit'] = integer(raw.get('output_limit', 0), name + ' output_limit', 0, OUTPUT_LIMIT)
    ops = raw.get('host_ops', {'first': 0, 'count': 0})
    first = m['host_op_first'] = integer(ops['first'], name + ' host_ops.first', 0, OS_HOST_FIRST - 1)
    count = m['host_op_count'] = integer(ops['count'], name + ' host_ops.count', 0, OS_HOST_FIRST)
    require(first + count <= OS_HOST_FIRST, name + ': host operations reach the OS range 0x80..0xff')
    require(not count or first + count <= OS_REPORTS[0] or OS_REPORTS[1] <= first,
            name + ': host operations overlap the OS reports 0x05..0x08')
    require(count or not first, name + ': an empty host range starts at 0')
    audio = raw.get('audio')
    if (required | optional) & CAPTURE:
        require(audio is not None, name + ': a capture device needs an audio format')
        m['audio'] = {'rate_hz': integer(audio['rate_hz'], name + ' rate_hz', 8000, 192000),
                      'channels': integer(audio['channels'], name + ' channels', 1, 2),
                      'bits': integer(audio['bits'], name + ' bits', 16, 32),
                      'min_frames': integer(audio['min_frames'], name + ' min_frames', 1, 1 << 24),
                      'max_frames': integer(audio['max_frames'], name + ' max_frames', 1, 1 << 24)}
        require(m['audio']['bits'] in (16, 24, 32), name + ': bits is 16, 24 or 32')
        require(m['audio']['min_frames'] <= m['audio']['max_frames'], name + ': min_frames exceeds max_frames')
    else:
        require(audio is None, name + ': an audio format needs a capture device')
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
        size = integer(r['bytes'], where + ' bytes', 1, WINDOW[1] - WINDOW[0])
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
    return m


def span(r):
    # GPU leases and CPU output permissions reserve complete 2 MiB blocks.
    alignment = BLOCK if r['access'] & (ACCESS['gpu-read'] | ACCESS['gpu-write']) else PAGE
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
        require(WINDOW[0] <= low < high <= WINDOW[1], '%s: [%#x, %#x) outside the application window' % (where, low, high))
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
    return {k: v for k, v in m.items() if k != 'region_names'}


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
             '__attribute__((aligned(4))) uint8_t ss_capture_buffer[%d];' % (64 + max(8192, -(-(m['audio']['max_frames']*m['audio']['channels']*(m['audio']['bits']//8))//4096)*4096) if (m['devices_required']|m['devices_optional'])&4 else 64),
             'const size_t ss_capture_capacity = sizeof(ss_capture_buffer)-SS_CAPTURE_HEADROOM;',
             '__attribute__((used,retain)) const struct os_application os_applications[] = {',
             '    {"thermal",os_thermal_task_privileged,os_thermal_pins,os_thermal_start},',
             '#ifdef SUNSTONE_A733_OS_NPU',
             '    {"npu",os_npu_task_privileged,os_npu_pins,os_npu_start},', '#endif',
             '#ifdef SUNSTONE_A733_OS_AUDIO',
             '    {"audio",os_audio_task_privileged,os_audio_pins,os_capture_start},', '#endif',
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
             'const struct a733_irq_route a733_irq_routes[] = {', '    {71,0,0xd8,os_thermal_irq},',
             '#ifdef SUNSTONE_BOARD_USB2_IRQ', '    {187,0,0xb0,board_host_irq},', '#endif',
             '#ifdef SUNSTONE_A733_OS_NPU', '    {97,OS_NPU_CORE,0xd8,os_npu_irq},', '#endif',
             '#ifdef SUNSTONE_A733_OS_AUDIO', '    {143,0,0xd8,os_audio_irq},', '#endif',
             '#ifdef SUNSTONE_A733_OS_GPU', ('    {95,A733_SMP_APPLICATION_CORE,0xd8,os_gpu_queue_irq},' if m.get('cluster', 0) else '    {95,2,0xd8,os_gpu_queue_irq},'), '#endif', '};',
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
    a = p.parse_args()
    if a.command == 'decode':
        json.dump(unpack(a.section.read_bytes()), sys.stdout, indent=2)
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
    (a.output / 'manifest.json').write_text(json.dumps([public(m)], indent=2) + '\n')


if __name__ == '__main__':
    main()
