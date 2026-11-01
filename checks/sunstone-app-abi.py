#!/usr/bin/env python3
"""Check the prototype application interface (docs/designs/app-abi.md) on the host.

1. The manifest tool emits hello-compute and the draft mimus manifest, each
   as the one application of its image.
2. The interface headers and both generated manifests compile for the host
   and for aarch64-none-elf; the section bytes from both compilers equal the
   bytes the tool packs, and the tool's decoder reproduces the manifests.
3. hello-compute runs against the host model of the OS calls and returns the
   Adler-32 of each test input in the OS result record.
4. The draft mimus manifest pins today's addresses, sizes and digests.
5. The tool refuses eleven manifests that break the documented rules.
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


# 1. Emit each application as its own image.
generated = {}
for name, path in [('hello_compute', HELLO), ('mimus', MIMUS)]:
    out = a.build / ('generated-' + name)
    run(*TOOL, 'emit', '--manifest', path, '--output', out)
    generated[name] = out
hello = json.loads((generated['hello_compute'] / 'manifest.json').read_text())[0]
mimus_manifest = json.loads((generated['mimus'] / 'manifest.json').read_text())
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

# 4. The mimus draft pins the values that the current build fixes.
mimus = mimus_manifest[0]
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
capture_h = (ROOT / 'apps/mimus-board/capture.h').read_text()
assert mimus['audio']['max_frames'] == int(re.search(r'#define MIMUS_CAPTURE_MAX_SAMPLES (\d+)u', capture_h).group(1))
assert mimus['audio']['min_frames'] == int(re.search(r'#define MIMUS_CAPTURE_MIN_SAMPLES (\d+)u', capture_h).group(1))
assert mimus['host_op_first'] == 0 and mimus['host_op_count'] == 5
assert 'progress_window_us' in (ROOT / 'tools/sunstone-manifest.py').read_text()
cpu_return = (ROOT / 'apps/os/cpu-return.h').read_text()
assert mimus['return_reserve_us'] == int(re.search(r'OS_CPU_RETURN_RESERVE_US UINT64_C\((\d+)\)', cpu_return).group(1))

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
print(json.dumps({'passed': True, 'manifest_bytes': tool.MANIFEST_BYTES,
                  'hello_sha256': hashlib.sha256((generated['hello_compute'] / 'manifest.bin').read_bytes()).hexdigest(),
                  'mimus_sha256': hashlib.sha256((generated['mimus'] / 'manifest.bin').read_bytes()).hexdigest()}))
