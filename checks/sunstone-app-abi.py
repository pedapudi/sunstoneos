#!/usr/bin/env python3
"""Check the application ABI without an external application checkout.

Compile hello-compute and a synthetic manifest of twelve regions, more than the
fixed table of manifest version 1 held, with every
device bit and access bit, for the host and AArch64. Compare each
section with the generator's packing, run hello-compute at input boundaries,
and check malformed-manifest refusals. Repeat --manifest to include external
application manifests in the same compiler-layout checks.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{output}', '--cc', '/usr/bin/clang', '--clang', '/usr/bin/clang']]
p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--build', type=Path, required=True, help='empty or absent scratch directory')
p.add_argument('--manifest', type=Path, action='append', default=[], help='additional application manifest (repeatable)')
p.add_argument('--cc', default='cc')
p.add_argument('--clang', default='clang')
p.add_argument('--objcopy', default=shutil.which('llvm-objcopy') or '/usr/lib/llvm-21/bin/llvm-objcopy')
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
assert not any(a.build.iterdir()), '%s is not empty' % a.build

spec = importlib.util.spec_from_file_location('sunstone_manifest', ROOT / 'tools/sunstone-manifest.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
TOOL = [sys.executable, '-I', str(ROOT / 'tools/sunstone-manifest.py')]
HELLO = ROOT / 'apps/hello-compute/manifest.json'
# This fixture belongs to the ABI check and contains no application inputs.
RICH = {
    'name': 'abi-fixture',
    'devices': {'required': ['usb-storage'],
                'optional': ['usb-keyboard', 'audio-i2s', 'audio-headset', 'gpu']},
    'cpu': {'cluster': 'a76', 'workers': 7, 'priority': 7,
            'stack_bytes': 65536, 'heap_bytes': 1048576},
    'regions': [
        {'name': 'pinned_content', 'kind': 'input', 'access': ['read', 'gpu-read'],
         'bytes': 17, 'sha256': hashlib.sha256(b'ABI fixture input').hexdigest()},
        {'name': 'fixed_input', 'kind': 'input', 'access': ['read'], 'address': '0x44800000',
         'bytes': 8192, 'sha256': hashlib.sha256(bytes(8192)).hexdigest()},
        {'name': 'scratch', 'kind': 'work', 'access': ['read', 'write'], 'bytes': 4096},
        {'name': 'result', 'kind': 'output', 'access': ['read', 'write', 'gpu-read', 'gpu-write'], 'bytes': 4096},
        {'name': 'gpu_output', 'kind': 'work', 'access': ['read', 'gpu-write'],
         'address': '0x46c00000', 'bytes': 4096},
        {'name': 'gpu_input', 'kind': 'work', 'access': ['read', 'gpu-read'], 'bytes': 4096},
        {'name': 'variable_input', 'kind': 'input', 'access': ['read'], 'bytes': 31},
        {'name': 'fixed_scratch', 'kind': 'work', 'access': ['read', 'write'],
         'address': '0x46e00000', 'bytes': 8192},
    ] + [
        # More regions than the version 1 table held (eight), to show that the table follows the declaration.
        {'name': 'extra_%d' % i, 'kind': 'work', 'access': ['read', 'write'], 'bytes': 4096} for i in range(4)
    ],
    'large_regions': [
        {'name': 'weights', 'bytes': 1 << 30, 'source_bytes': 536870912, 'access': ['read'],
         'file': '/sunstone-test/sunstone-region-test.bin',
         'sha256': 'a0ab21dca50740b257c495faa888058cb757ec5165d27afb0c13cc5bef48eabd'},
        {'name': 'cache', 'bytes': 4 << 20, 'source_bytes': 4 << 20, 'access': ['read', 'write'],
         'first_sector': 1 << 33, 'sha256': hashlib.sha256(b'extent').hexdigest()},
    ],
    'audio': {'rate_hz': 32000, 'channels': 2, 'bits': 32, 'min_frames': 32, 'max_frames': 1024},
    'deadlines': {'run_us': 1234567, 'return_reserve_us': 7654321, 'progress_window_us': 2345678},
    'output_limit': 64512,
    'host_ops': {'first': 0x11, 'count': 3},
}
RICH_PATH = a.build / 'generic-manifest.json'
RICH_PATH.write_text(json.dumps(RICH, indent=2) + '\n')


def run(*argv, **kw):
    return subprocess.run([str(x) for x in argv], check=True, capture_output=True, text=True, **kw)


# 1. Emit each application as its own image.
generated = {}
applications = [('hello_compute', HELLO), ('abi_fixture', RICH_PATH)]
for path in a.manifest:
    name = json.loads(path.read_text())['name'].replace('-', '_')
    assert name not in {n for n, _ in applications}, 'duplicate application name: ' + name
    applications.append((name, path))
for name, path in applications:
    out = a.build / ('generated-' + name)
    run(*TOOL, 'emit', '--manifest', path, '--output', out)
    generated[name] = out
hello = json.loads((generated['hello_compute'] / 'manifest.json').read_text())[0]
rich = json.loads((generated['abi_fixture'] / 'manifest.json').read_text())[0]
assert (rich['devices_required'], rich['devices_optional'], rich['cluster'], rich['workers'], rich['priority']) == (1, 30, 1, 7, 7)
assert len(rich['regions']) == 12 and rich['regions'][3]['access'] == 15
large = rich['large_regions']
assert [r['index'] for r in large] == [0, 1] and large[0]['flags'] == 0 and large[1]['flags'] == 3
assert large[0]['path'] == '/sunstone-test/sunstone-region-test.bin' and large[1]['first_sector'] == 1 << 33
assert (generated['hello_compute'] / 'large.bin').read_bytes() == b''
assert '#define ABI_FIXTURE_LARGE_WEIGHTS 0u' in (generated['abi_fixture'] / 'abi_fixture-app.h').read_text()
assert json.loads(run(*TOOL, 'decode-large', generated['abi_fixture'] / 'large.bin').stdout) == large
assert rich['regions'][0]['address'] == 0x44000000 and rich['regions'][2]['address'] == 0x44200000
assert rich['regions'][6]['sha256'] == '0' * 64
assert rich['audio'] == RICH['audio']
assert (rich['run_deadline_us'], rich['return_reserve_us'], rich['progress_window_us']) == (1234567, 7654321, 2345678)
assert (rich['host_op_first'], rich['host_op_count'], rich['output_limit']) == (0x11, 3, 64512)
assert hello['regions'][0]['address'] == 0x44000000, hex(hello['regions'][0]['address'])

# 2. Compile for both targets and compare the section bytes. The generated
# header must carry the prototypes: -Wmissing-prototypes refuses
# hello_compute_main without one.
warn = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpadded', '-Wmissing-prototypes']
for target, compiler, flags in [('host', a.cc, []),
                                ('aarch64', a.clang, ['--target=aarch64-none-elf', '-ffreestanding', '-mgeneral-regs-only'])]:
    out = a.build / target
    out.mkdir()
    # The session header is for the OS, the loader and host tools; its
    # layout assertions must hold on both targets too.
    run(compiler, *warn, *flags, '-I', ROOT / 'include', '-fsyntax-only', '-x', 'c',
        '-include', 'sunstone_session.h', '/dev/null')
    for name, gen in generated.items():
        include = ['-I', ROOT / 'include', '-I', gen]
        packed = (gen / 'manifest.bin').read_bytes()
        assert len(packed) == tool.MANIFEST_HEAD_BYTES + 56 * len(json.loads((gen / 'manifest.json').read_text())[0]['regions'])
        obj = out / (name + '-manifest.o')
        run(compiler, *warn, *flags, *include, '-O2', '-c', gen / (name + '-manifest.c'), '-o', obj)
        section = out / (name + '-manifest.bin')
        run(a.objcopy, '-O', 'binary', '--only-section=sunstone_manifest', obj, section)
        assert section.read_bytes() == packed, '%s %s compiler layout differs from the tool' % (target, name)
        packed_large = (gen / 'large.bin').read_bytes()
        section_large = out / (name + '-large.bin')
        run(a.objcopy, '-O', 'binary', '--only-section=sunstone_large_regions', obj, section_large)
        assert section_large.read_bytes() == packed_large, '%s %s large section differs from the tool' % (target, name)
        decoded = json.loads(run(*TOOL, 'decode', gen / 'manifest.bin').stdout)
        described = json.loads((gen / 'manifest.json').read_text())
        for entry in described:
            entry.pop('large_regions')      # the manifest section does not carry them; decode-large does
        assert decoded == described, name + ' decoder differs'
    include = ['-I', ROOT / 'include', '-I', generated['hello_compute']]
    main = out / 'main.o'
    run(compiler, *warn, *flags, *include, '-O2', '-c', ROOT / 'apps/hello-compute/main.c', '-o', main)
    if target == 'host':
        model = out / 'model'
        run(a.cc, *warn, *include, '-O2', ROOT / 'checks/sunstone-app-abi.c', main, out / 'hello_compute-manifest.o',
            '-o', model)
        linked = out / 'linked.bin'
        run(a.objcopy, '-O', 'binary', '--only-section=sunstone_manifest', model, linked)
        assert linked.read_bytes() == (generated['hello_compute'] / 'manifest.bin').read_bytes(), 'linked section differs'

# 3. Run hello-compute on inputs at block and region boundaries.
def stream(n):
    out, counter = bytearray(), 0
    while len(out) < n:
        out += hashlib.sha256(counter.to_bytes(8, 'little')).digest()
        counter += 1
    return bytes(out[:n])


limit = hello['regions'][0]['bytes']
cases = [b'', b'Wikipedia', stream(5552), stream(5553), stream(limit)]
for i, data in enumerate(cases):
    source, result = a.build / ('input-%d.bin' % i), a.build / ('result-%d.bin' % i)
    source.write_bytes(data)
    done = run(model, source, result)
    record = result.read_bytes()
    magic, version, size, epoch, generation, status, payload = struct.unpack_from('<7Q', record)
    assert (magic, version, size, status, payload) == (
        0x31544c5553455241, 1, 56 + hello['output_limit'], 1, 16) and len(record) == size
    adler, reserved, length = struct.unpack_from('<IIQ', record, 56)
    assert (adler, reserved, length) == (zlib.adler32(data), 0, len(data)), (i, hex(adler))
    reports = int(re.fullmatch(r'progress (\d+)\n', done.stderr).group(1))
    assert reports == -(-len(data) // (5552 * 12)), (i, reports)
assert zlib.adler32(b'Wikipedia') == 0x11e60398
source = a.build / 'input-oversize.bin'
source.write_bytes(stream(limit + 1))
assert subprocess.run([str(model), str(source), str(a.build / 'result-oversize.bin')]).returncode == 5

# 5. Refusals.
def refused(change, message):
    value = json.loads(HELLO.read_text())
    change(value)
    path = a.build / 'refused.json'
    path.write_text(json.dumps(value))
    r = subprocess.run(TOOL + ['emit', '--manifest', str(path), '--output', str(a.build / 'refused')],
                       capture_output=True, text=True)
    assert r.returncode and message in r.stderr, (message, r.stderr)


def large_refused(change, message):
    value = json.loads(RICH_PATH.read_text())
    change(value['large_regions'])
    path = a.build / 'large-refused.json'
    path.write_text(json.dumps(value))
    r = subprocess.run(TOOL + ['emit', '--manifest', str(path), '--output', str(a.build / 'large-refused')],
                       capture_output=True, text=True)
    assert r.returncode and message in r.stderr, (message, r.stderr)


large_refused(lambda v: v[0].update(bytes=(1 << 30) + 4096), 'multiple of 2 MiB')
large_refused(lambda v: v[0].update(source_bytes=(1 << 30) + 1), 'outside 1..')
large_refused(lambda v: v[0].update(sha256='A' * 64), 'sha256 is 64 lowercase hex digits')
large_refused(lambda v: v[0].update(file='relative/path.bin'), 'absolute path')
large_refused(lambda v: v[0].update(file='/' + 'a' * 255), 'at most 255 characters')
large_refused(lambda v: v[0].update(file='/bad name.bin'), 'absolute path')
large_refused(lambda v: v[0].update(first_sector=5), 'not both')
large_refused(lambda v: v[0].pop('file'), 'not both')
large_refused(lambda v: v[1].update(first_sector=0), 'first_sector')
large_refused(lambda v: v[1].update(access=['write']), 'access is [read]')
large_refused(lambda v: v[1].update(name='weights'), 'distinct name')
large_refused(lambda v: v.extend([dict(v[0], name='c%d' % i, bytes=4 << 30, source_bytes=1 << 20) for i in range(3)]), 'together exceed the 11 GiB')
large_refused(lambda v: v[0].update(unknown=1), 'unknown keys')
large_refused(lambda v: v[0].update(bytes=12 << 30), 'outside')

capture = {'rate_hz': 16000, 'channels': 1, 'bits': 16, 'min_frames': 16000, 'max_frames': 80000}
refused(lambda v: v['regions'][0].update(access=['read', 'write']), 'an input is read-only')
refused(lambda v: v['regions'].append({'name': 'scratch', 'kind': 'work', 'access': ['read', 'write'],
                                       'bytes': 4096, 'address': '0x44000000'}) or
        v['regions'][0].update(address='0x44000000'), 'overlaps region')
refused(lambda v: v['regions'][0].update(address='0x43000000'), 'outside the application window')
refused(lambda v: v.update(host_ops={'first': 4, 'count': 2}), 'overlap the OS reports 0x05..0x08')
refused(lambda v: v.update(host_ops={'first': 0x7f, 'count': 2}), 'reach the OS range')
refused(lambda v: v.update(devices={'required': ['audio-i2s'], 'optional': ['audio-headset']}, audio=capture),
        'a required capture device excludes the other')
refused(lambda v: v.update(devices={'required': ['audio-i2s']}), 'a capture device needs an audio format')
refused(lambda v: v.update(devices={'required': ['audio-i2s']}, audio=dict(capture, min_frames=90000)),
        'min_frames exceeds max_frames')
refused(lambda v: v.update(devices={'required': ['lidar']}), "unknown device 'lidar'")
# The radio station is a device (SS_DEVICE_RADIO, bit 8).
radio = json.loads(HELLO.read_text())
radio['devices'] = {'required': ['radio'], 'optional': []}
(a.build / 'radio.json').write_text(json.dumps(radio))
run(*TOOL, 'emit', '--manifest', a.build / 'radio.json', '--output', a.build / 'radio')
assert json.loads((a.build / 'radio' / 'manifest.json').read_text())[0]['devices_required'] == 1 << 8
refused(lambda v: v['cpu'].update(privilege='el1'), 'unknown cpu keys')
refused(lambda v: v.update(output_limit=tool.OUTPUT_LIMIT + 1), 'output_limit')
assert tool.OUTPUT_LIMIT + tool.RESULT_HEADER_BYTES == 0xf00000
# A supervised session ends within 60 s; a resident image has no such bound.
refused(lambda v: v['deadlines'].update(progress_window_us=tool.SUPERVISED_SESSION_US + 1), 'progress_window_us')
_long = json.loads(HELLO.read_text())
_long['deadlines']['progress_window_us'] = 3600 * 10**6
(a.build / 'resident-window.json').write_text(json.dumps(_long))
assert tool.load(a.build / 'resident-window.json', resident=True)['progress_window_us'] == 3600 * 10**6
try:
    tool.load(a.build / 'resident-window.json')
    raise AssertionError('a supervised window above 60 s was accepted')
except SystemExit:
    pass
refused(lambda v: v['regions'].append({'name': 'scratch', 'kind': 'work', 'access': ['read', 'write'], 'bytes': 100}),
        'capacity is a multiple of 4 KiB')
# GPU leases cover whole 2 MiB CPU blocks, including bytes beyond an input's
# exact content length. Pinned neighbours cannot occupy that rounded tail.
for access,kind,size in ((['read','gpu-read'],'input',17),
                         (['read','gpu-write'],'work',4096),
                         (['read','write','gpu-read'],'work',4096)):
    def gpu_tail(v,access=access,kind=kind,size=size):
        v['regions'][0].update(address='0x44000000',access=access,kind=kind,bytes=size)
        v['regions'].append({'name':'neighbour','kind':'work','access':['read','write'],
                             'bytes':4096,'address':'0x44001000'})
    refused(gpu_tail,'overlaps region')
refused(lambda v:v['regions'][0].update(address='0x44001000',access=['read','gpu-read']),
        'a pinned GPU address is 2 MiB aligned')
refused(lambda v:v['regions'][0].update(address='0x46e00000',bytes=0x200001,access=['read','gpu-read']),
        'outside the application window')


# Automatic GPU placement skips a CPU region inside its candidate's rounded
# tail; the exact boundary remains available to another pinned region.
placement_paths=[]
for name,gpu_address,neighbour_address,expected in (
        ('automatic-gpu',None,0x44001000,0x44200000),
        ('rounded-boundary',0x44000000,0x44200000,0x44000000)):
    value=json.loads(HELLO.read_text())
    value['regions'][0].update(bytes=17,access=['read','gpu-read'])
    if gpu_address is not None:value['regions'][0]['address']=gpu_address
    value['regions'].append({'name':'neighbour','kind':'work','access':['read','write'],
                             'bytes':4096,'address':neighbour_address})
    source=a.build/(name+'.json');source.write_text(json.dumps(value))
    manifest=tool.assign(tool.load(source))
    assert manifest['regions'][0]['address']==expected
    assert tool.span(manifest['regions'][0])==(expected,expected+tool.BLOCK)
    placement_paths.append(name)
result = {'passed': True, 'manifest_head_bytes': tool.MANIFEST_HEAD_BYTES,
          'hello_sha256': hashlib.sha256((generated['hello_compute'] / 'manifest.bin').read_bytes()).hexdigest(),
          'generic_sha256': hashlib.sha256((generated['abi_fixture'] / 'manifest.bin').read_bytes()).hexdigest()}
result['gpu_rounded_span_cases']={'refusals':6,'accepted':placement_paths}
result['external_sha256'] = {name: hashlib.sha256((generated[name] / 'manifest.bin').read_bytes()).hexdigest()
                             for name, _ in applications[2:]}
print(json.dumps(result))
