#!/usr/bin/env python3
"""Freeze a manifest-bound typed session and application inputs without device access.

This produces offline inputs. The caller must separately establish board,
service and recovery authority before using a transport launcher.

build_launcher and request_bytes produce the SRAM launcher, launch request and
contract that start an OS image over FEL. Every application's launch uses them.
"""
import argparse
import functools
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('os_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
require = host.require
SCHEMA = 'sunstone-session-inputs-v1'
# The OS launch contract and request (apps/os/os.h) and the session page.
CONTRACT_MAGIC, REQUEST_MAGIC, SESSION_ADDRESS = 0x415337334f534331, 0x415337334f534c31, 0x41f00000


@functools.cache
def board():
    """The supervised board preparation, loaded only by the launch functions."""
    return host.module('os_board_preparation', ROOT / 'apps/board/prepare.py')


def request_bytes(address, identity, raw):
    """The OS launch request and contract for the session bytes raw."""
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    request, contract = board().request_bytes(address, identity, epoch, generation, lease, raw)
    request = struct.pack('<Q', REQUEST_MAGIC) + request[8:]
    contract = bytearray(contract)
    struct.pack_into('<Q', contract, 184, len(raw))
    return request, bytes(contract)


def build_launcher(out, session_bytes, clang, lld, watchdog_mode=None):
    """Build the board's SRAM launcher for the OS request, contract and session size.

    With watchdog_mode the launcher admits exactly that host-armed WDT0 state,
    system reset and reset output instead of a disabled watchdog; its refusal
    path then parks, and the watchdog returns the board to FEL."""
    out = Path(out)
    text = (ROOT / 'platform/a733-smp-supervised/launch.c').read_text()
    (out / 'base-supervised-launch.c').write_text(text)
    changes = {
        '0x4153373353554c33': '%#x' % REQUEST_MAGIC, 'REQUEST[1]==3': 'REQUEST[1]==2',
        'CONTRACT->version==3': 'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':
            'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==%du' % session_bytes,
        '20*A733_SMP_COUNTER_HZ': 'A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ'}
    if watchdog_mode is not None:
        changes['!(rd(0x02050014)&1) && !(rd(0x02050010)&0x100) &&'] = (
            '(rd(0x02050014)&0xffff)==%#x && (rd(0x02050010)&0xffff)==1 && (rd(0x02050018)&0xffff)==2 &&'
            % watchdog_mode)
    text = board().adapt_launcher(text, changes)
    (out / 'board-launch-base.c').write_text(text)
    for name, path in [('launch.c', ROOT / 'apps/board/launch.c'), ('launch.S', ROOT / 'platform/a733-smp-observe/launch.S'),
                       ('launch.ld', ROOT / 'platform/a733-smp-observe/launch.ld'),
                       ('boot.h', ROOT / 'platform/a733-smp-cached/boot.h'),
                       ('a733_board_application.h', ROOT / 'include/a733_board_application.h')]:
        shutil.copy2(path, out / name)
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall',
             '-Wextra', '-Werror', '-DSUNSTONE_BOARD_SUPERVISED=1', f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC:#x}ULL',
             '-I' + str(out)]
    commands = [[clang, *flags, '-c', out / name, '-o', out / (name + '.o')] for name in ('launch.c', 'launch.S')]
    commands += [[lld, '-T', out / 'launch.ld', out / 'launch.c.o', out / 'launch.S.o', '-o', out / 'launch.elf'],
                 ['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', out / 'launch.elf', out / 'launch.bin']]
    for command in commands:
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        with (out / 'launcher-build.log').open('a') as log:
            log.write(result.stdout + result.stderr)
        require(result.returncode == 0, 'OS launcher build failed; inspect the retained log.')
    (out / 'launcher-commands.json').write_text(json.dumps([[str(x) for x in c] for c in commands], indent=2) + '\n')
    require(0 < (out / 'launch.bin').stat().st_size <= 0xc00, 'OS launcher exceeds reserved SRAM.')


def prepare(image, prefix, services, inputs, output, *, devices, flags=0):
    image = Path(image).read_bytes()
    info = host.image_manifest(image)
    prefix = bytes(prefix)
    values = [bytes(data) for data in inputs]
    facts, uploads = host.input_facts(info['manifest'], values)
    session = host.session_bytes(prefix, services, info['manifest'], info['sha256'], devices, flags, values)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    files = {'image.elf': image, 'session.bin': session}
    for item, data in zip(uploads, values):
        item['name'] = 'input-%d.bin' % item.pop('index')
        files[item['name']] = data
    for name, data in files.items():
        (output / name).write_bytes(data)
    record = {'schema': SCHEMA, 'physical_access': False, 'admission_required': True,
              'manifest_sha256': info['sha256'], 'devices': devices, 'flags': flags,
              'epoch': struct.unpack_from('<Q', prefix, 24)[0],
              'generation': struct.unpack_from('<Q', prefix, 32)[0], 'uploads': uploads,
              'files': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
    (output / 'prepared.json').write_text(json.dumps(record, indent=2) + '\n')
    return validate_bundle(output)


def validate_bundle(output):
    output = Path(output)
    record = json.loads((output / 'prepared.json').read_text())
    require(record.get('schema') == SCHEMA and record.get('physical_access') is False and
            record.get('admission_required') is True, 'Unsupported session input bundle.')
    require({'image.elf', 'session.bin'} <= set(record['files']), 'Missing session input files.')
    files = {}
    for name, digest in record['files'].items():
        require(Path(name).name == name, 'Invalid bundle filename.')
        data = (output / name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == digest, 'Session input changed.')
        files[name] = data
    info = host.image_manifest(files['image.elf'])
    require(info['sha256'] == record['manifest_sha256'], 'Application manifest changed.')
    decoded = host.session_decode(files['session.bin'], info['manifest'], info['sha256'])
    require((decoded['devices'], decoded['flags']) == (record['devices'], record['flags']), 'Device grant changed.')
    expected = []
    for index, region in enumerate(host.regions(info['manifest'], (1,))):
        name = 'input-%d.bin' % index
        require(name in files, 'Missing frozen input.')
        expected.append({'name': name, 'address': region['address'], 'bytes': len(files[name]),
                         'sha256': hashlib.sha256(files[name]).hexdigest()})
    require(record['uploads'] == expected and set(files) == {'image.elf', 'session.bin'} | {x['name'] for x in expected},
            'Upload plan differs from manifest.')
    facts, _ = host.input_facts(info['manifest'], [files[x['name']] for x in expected])
    require(decoded['extents'].get(host.INPUTS, b'') == facts, 'Session inputs differ from frozen uploads.')
    require(struct.unpack_from('<2Q', files['session.bin'], 24) == (record['epoch'], record['generation']),
            'Session identity changed.')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--input', type=Path, action='append', default=[])
    parser.add_argument('--service', action='append', default=[], metavar='TYPE=FILE')
    parser.add_argument('--devices', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--bind-sid', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    services = {}
    for item in args.service:
        kind, path = item.split('=', 1)
        kind = int(kind, 0)
        require(kind not in services, 'Duplicate service argument.')
        services[kind] = Path(path).read_bytes()
    print(json.dumps(prepare(args.image, args.prefix.read_bytes(), services,
          [p.read_bytes() for p in args.input], args.output, devices=args.devices,
          flags=host.BIND_SID if args.bind_sid else 0), indent=2))


if __name__ == '__main__':
    main()
