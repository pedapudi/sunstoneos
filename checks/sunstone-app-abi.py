#!/usr/bin/env python3
"""Check the prototype application interface (docs/designs/app-abi.md) on the host.

1. The manifest tool emits hello-compute and the draft mimus manifest as one
   image, assigning hello-compute's input above mimus's pinned regions.
2. The interface headers and both generated manifests compile for the host
   and for aarch64-none-elf; the section bytes from both compilers equal the
   bytes the tool packs, and the tool's decoder reproduces the manifests.
3. hello-compute runs against the host model of the OS calls and returns the
   Adler-32 of each test input in the OS result record.
4. The draft mimus manifest pins today's addresses, sizes and digests.
5. The tool refuses manifests that break the documented rules.
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
MIMUS = ROOT / 'apps/mimus-board/manifest.json'


def run(*argv, **kw):
    return subprocess.run([str(x) for x in argv], check=True, capture_output=True, text=True, **kw)


# 1. Emit both applications as one image.
gen = a.build / 'generated'
run(*TOOL, 'emit', '--manifest', MIMUS, '--manifest', HELLO, '--output', gen)
manifests = json.loads((gen / 'manifests.json').read_text())
assert [m['name'] for m in manifests] == ['mimus', 'hello-compute']
hello = manifests[1]
# mimus pins [0x44000000, 0x4501d000) and [0x46000000, 0x47000000); the first
# free 2 MiB boundary above 0x45000000's input is 0x45200000.
assert hello['regions'][0]['address'] == 0x45200000, hex(hello['regions'][0]['address'])
packed = (gen / 'manifests.bin').read_bytes()
assert len(packed) == 2 * tool.MANIFEST_BYTES

# 2. Compile for both targets and compare the section bytes.
include = ['-I', ROOT / 'include', '-I', gen]
warn = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpadded']
sections = {}
for target, compiler, flags in [('host', a.cc, []),
                                ('aarch64', a.clang, ['--target=aarch64-none-elf', '-ffreestanding', '-mgeneral-regs-only'])]:
    out = a.build / target
    out.mkdir()
    objects = []
    for source in [gen / 'mimus-manifest.c', gen / 'hello_compute-manifest.c', ROOT / 'apps/hello-compute/main.c']:
        obj = out / (source.stem + '.o')
        run(compiler, *warn, *flags, *include, '-O2', '-c', source, '-o', obj)
        objects.append(obj)
    # The session header is for the OS, the loader and host tools; its
    # layout assertions must hold on both targets too.
    run(compiler, *warn, *flags, *include, '-fsyntax-only', '-x', 'c', '-include', 'sunstone_session.h', '/dev/null')
    raw = b''
    for obj in objects[:2]:
        section = out / (obj.stem + '.bin')
        run(a.objcopy, '-O', 'binary', '--only-section=sunstone_manifest', obj, section)
        raw += section.read_bytes()
    sections[target] = raw
    if target == 'host':
        model = out / 'model'
        run(a.cc, *warn, *include, '-O2', ROOT / 'checks/sunstone-app-abi.c', *objects[1:], '-o', model)
        # The linked host executable keeps exactly hello-compute's manifest.
        linked = out / 'linked.bin'
        run(a.objcopy, '-O', 'binary', '--only-section=sunstone_manifest', model, linked)
        assert linked.read_bytes() == packed[tool.MANIFEST_BYTES:], 'linked section differs'
for target, raw in sections.items():
    assert raw == packed, '%s compiler layout differs from the tool' % target
decoded = json.loads(run(*TOOL, 'decode', gen / 'manifests.bin').stdout)
assert decoded == manifests, 'decoder does not reproduce the manifests'

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
    magic, version, size, epoch, generation, application, status, payload = struct.unpack_from('<5QIIQ', record)
    assert (magic, version, size, application, status, payload) == (
        0x31544c5553455241, 1, 56 + hello['output_limit'], 0, 1, 16) and len(record) == size
    adler, reserved, length = struct.unpack_from('<IIQ', record, 56)
    assert (adler, reserved, length) == (zlib.adler32(data), 0, len(data)), (i, hex(adler))
    reports = int(re.fullmatch(r'progress (\d+)\n', done.stderr).group(1))
    assert reports == -(-len(data) // (5552 * 12)), (i, reports)
assert zlib.adler32(b'Wikipedia') == 0x11e60398
source = a.build / 'input-oversize.bin'
source.write_bytes(stream(limit + 1))
assert subprocess.run([str(model), str(source), str(a.build / 'result-oversize.bin')]).returncode == 5

# 4. The mimus draft pins the values that the current build fixes.
mimus = manifests[0]
models = json.loads((ROOT / 'apps/bluetooth/recognition-models.json').read_text())['models']
wav = json.loads((ROOT / 'apps/mimus-board/inputs.json').read_text())['recorded_wav']
configure = (ROOT / 'apps/mimus-board/configure.py').read_text()
addresses = [int(x, 16) for x in re.search(r'mimus_asset_address\[3\]=\{([^}]*)\}', configure).group(1).split(',')]
for region, expected, address in zip(mimus['regions'][:3], models + [wav], addresses):
    assert (region['address'], region['bytes'], region['sha256']) == (address, expected['bytes'], expected['sha256'])
dispatch = (ROOT / 'apps/os/dispatch.h').read_text()
def constant(name):
    return int(re.search(r'#define %s UINT64_C\((0x[0-9a-fA-F]+)\)' % name, dispatch).group(1), 16)
assert (mimus['regions'][3]['address'], mimus['regions'][3]['bytes']) == (constant('OS_DISPATCH_ARENA'), constant('OS_DISPATCH_CODE_BYTES'))
assert (mimus['regions'][4]['address'], mimus['regions'][4]['bytes']) == (constant('OS_DISPATCH_READ_ONLY'), constant('OS_DISPATCH_READ_ONLY_BYTES'))
os_h = (ROOT / 'apps/os/os.h').read_text()
assert mimus['audio']['max_frames'] == int(re.search(r'#define OS_AUDIO_MAX_SAMPLES (\d+)u', os_h).group(1))
services = (ROOT / 'apps/os/services.c').read_text()
assert mimus['progress_window_us'] == int(re.search(r'\{"mimus",(\d+)\}', services).group(1))
cpu_return = (ROOT / 'apps/os/cpu-return.h').read_text()
assert mimus['return_reserve_us'] == int(re.search(r'OS_CPU_RETURN_RESERVE_US UINT64_C\((\d+)\)', cpu_return).group(1))

# 5. Refusals.
def refused(change, message):
    value = json.loads(HELLO.read_text())
    change(value)
    path = a.build / 'refused.json'
    path.write_text(json.dumps(value))
    r = subprocess.run(TOOL + ['emit', '--manifest', str(MIMUS), '--manifest', str(path), '--output', str(a.build / 'refused')],
                       capture_output=True, text=True)
    assert r.returncode and message in r.stderr, (message, r.stderr)


refused(lambda v: v['regions'][0].update(access=['read', 'write']), 'an input is read-only')
refused(lambda v: v['regions'][0].update(address='0x46000000'), 'overlaps mimus region arena')
refused(lambda v: v['regions'][0].update(address='0x43000000'), 'outside the application window')
refused(lambda v: v.update(host_ops={'first': 4, 'count': 2}), 'host operations overlap mimus')
refused(lambda v: v.update(host_ops={'first': 0x7f, 'count': 2}), 'reach the OS range')
refused(lambda v: v.update(devices={'required': ['audio-i2s'], 'optional': ['audio-headset']},
                           audio={'rate_hz': 16000, 'channels': 1, 'bits': 16, 'max_frames': 16000}),
        'a required capture device excludes the other')
refused(lambda v: v.update(devices={'required': ['audio-i2s']}), 'a capture device needs an audio format')
refused(lambda v: v.update(name='mimus'), 'application mimus listed twice')
refused(lambda v: v['regions'].append({'name': 'scratch', 'kind': 'work', 'access': ['read', 'write'], 'bytes': 100}),
        'capacity is a multiple of 4 KiB')
print(json.dumps({'passed': True, 'manifest_bytes': tool.MANIFEST_BYTES,
                  'section_sha256': hashlib.sha256(packed).hexdigest(),
                  'hello_input_address': hex(hello['regions'][0]['address'])}))
