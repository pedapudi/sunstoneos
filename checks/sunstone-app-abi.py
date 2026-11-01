#!/usr/bin/env python3
"""Check the application ABI without an external application checkout.

Compile hello-compute and a synthetic manifest covering all region slots,
device bits and access bits for the host and AArch64. Compare each 552-byte
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
assert len(rich['regions']) == 8 and rich['regions'][3]['access'] == 15
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
        assert len(packed) == tool.MANIFEST_BYTES
        obj = out / (name + '-manifest.o')
        run(compiler, *warn, *flags, *include, '-O2', '-c', gen / (name + '-manifest.c'), '-o', obj)
        section = out / (name + '-manifest.bin')
        run(a.objcopy, '-O', 'binary', '--only-section=sunstone_manifest', obj, section)
        assert section.read_bytes() == packed, '%s %s compiler layout differs from the tool' % (target, name)
        decoded = json.loads(run(*TOOL, 'decode', gen / 'manifest.bin').stdout)
        assert decoded == json.loads((gen / 'manifest.json').read_text()), name + ' decoder differs'
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
refused(lambda v: v.update(devices={'required': ['radio']}), "unknown device 'radio'")
refused(lambda v: v['cpu'].update(privilege='el1'), 'unknown cpu keys')
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

def automatic_gpu_tail(v):
    v['regions'][0].update(bytes=tool.WINDOW[1]-tool.WINDOW[0]-tool.PAGE,access=['read','gpu-read'])
    v['regions'].append({'name':'neighbour','kind':'work','access':['read','write'],
                         'bytes':tool.PAGE,'address':tool.WINDOW[1]-tool.PAGE})
refused(automatic_gpu_tail,'outside the application window')

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
result = {'passed': True, 'manifest_bytes': tool.MANIFEST_BYTES,
          'hello_sha256': hashlib.sha256((generated['hello_compute'] / 'manifest.bin').read_bytes()).hexdigest(),
          'generic_sha256': hashlib.sha256((generated['abi_fixture'] / 'manifest.bin').read_bytes()).hexdigest()}
result['gpu_rounded_span_cases']={'refusals':6,'accepted':placement_paths}
result['external_sha256'] = {name: hashlib.sha256((generated[name] / 'manifest.bin').read_bytes()).hexdigest()
                             for name, _ in applications[2:]}
print(json.dumps(result))
