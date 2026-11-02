#!/usr/bin/env python3
"""Check the repository TOC1 packer against the format spec and the pinned vendor packer."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos'))
PACKER_SHA256 = 'e5f9076504a0ccde29c5978bcfc341c1c5ac02d947c47b849a5e8d26433545f6'


def load(path):
    spec = importlib.util.spec_from_file_location('toc_package', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def up(value, step):
    return (value + step - 1) // step * step


def check_spec(package, items):
    """Verify the format from the published layout, without the packer's own decoder."""
    count = len(items)
    first = up(64 + 368 * count, 1024)
    total = first
    for _, payload in items:
        total += len(payload)
        total = up(total, 1024)
    total = up(total, 16384)
    assert len(package) == total, 'extent'
    name, magic, checksum, serial, status, nr, valid, major, minor, r0, r1, r2, end = \
        struct.unpack_from('<16s12I', package, 0)
    assert name == b'sunxi-package' + bytes(3) and magic == 0x89119800 and end == 0x3b45494d
    assert (serial, status, major, minor, r0, r1, r2) == (0,) * 7
    assert nr == count and valid == total
    words = struct.unpack('<%dI' % (total // 4), package)
    assert (sum(words) - checksum + 0x5f0a6c39) & 0xffffffff == checksum, 'checksum'
    offset = first
    for index, (label, payload) in enumerate(items):
        base = 64 + 368 * index
        raw, start, length, encrypt, kind, run_addr, position = struct.unpack_from('<64s6I', package, base)
        assert raw == label.encode().ljust(64, b'\0')
        assert (start, length, encrypt, kind, run_addr, position) == (offset, len(payload), 0, 3, 0, 0)
        assert not any(package[base + 88:base + 364]) and \
            struct.unpack_from('<I', package, base + 364)[0] == 0x3b454949
        assert package[start:start + length] == payload
        offset = up(start + length, 1024)
        assert not any(package[start + length:offset])
    assert not any(package[64 + 368 * count:first]) and not any(package[offset:])


def vendor_pack(vendor, config, files, directory):
    for name, data in files.items():
        (directory / name).write_bytes(data)
    (directory / 'boot_package.cfg').write_bytes(config)
    with (directory / 'vendor.log').open('wb') as log:
        subprocess.run([str(vendor), '-pack', 'boot_package.cfg'], cwd=directory, stdout=log,
                       stderr=subprocess.STDOUT, timeout=60, check=True,
                       env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
    package = (directory / 'boot_package.fex').read_bytes()
    (directory / 'boot_package.fex').unlink()
    return package, (directory / 'vendor.log').read_bytes().decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory')
    parser.add_argument('--packer', type=Path, default=CACHE / 'sources/board-boot/dragonsecboot',
                        help='Pinned vendor executable; the comparison is skipped when it is absent')
    parser.add_argument('--module', type=Path, default=ROOT / 'tools/toc_package.py',
                        help='Packer module under test')
    args = parser.parse_args()
    args.output.mkdir(mode=0o700, exist_ok=False)
    toc = load(args.module)
    vendor = None
    if args.packer.is_file():
        if hashlib.sha256(args.packer.read_bytes()).hexdigest() != PACKER_SHA256:
            raise SystemExit('Vendor executable is not the pinned file: ' + str(args.packer))
        vendor = args.packer
    rng = random.Random(733)
    blob = lambda size: bytes(rng.getrandbits(8) for _ in range(size))
    cases = []
    # (case name, config text, {file: bytes}); the config lists items in order.
    monitor_sizes = [1, 4, 268, 1023, 1024, 1025, 15359, 15360, 15361, 16384, 17407, 31743, 31744, 31745, 49152]
    work = []
    for size in monitor_sizes:
        work.append(('monitor-%d' % size, b'[package]\r\nitem=monitor, monitor.fex\r\n',
                     {'monitor.fex': blob(size)}))
    work.append(('monitor-empty-file', b'[package]\r\nitem=monitor, monitor.fex\r\n', {'monitor.fex': b''}))
    normal = {'u-boot.fex': blob(5000), 'monitor.fex': blob(300), 'scp.fex': blob(1024), 'optee.fex': blob(16384)}
    work.append(('normal-three-items', b'[package]\r\n;item=Item_TOC_name, Item_filename,\r\n'
                 b'item=u-boot,                 u-boot.fex\r\nitem=monitor,                monitor.fex\r\n'
                 b'item=scp,                   scp.fex\r\n;item=optee,                optee.fex\r\n', normal))
    work.append(('normal-four-items-optee', b'[package]\nitem=u-boot,u-boot.fex\nitem=monitor,monitor.fex\n'
                 b'item=scp,scp.fex\nitem=optee,optee.fex\n', normal))
    work.append(('comments-tabs-sections', b'[ other ]\nitem=zz, zz\n[package ]\n# note\n\titem=\tscp ,\tscp.fex ; tail\n'
                 b'item=u-boot, u-boot.fex # tail\n[after]\nitem=yy,yy\n', normal))
    many = {'f%d.fex' % i: blob(rng.randrange(0, 3000)) for i in range(16)}
    work.append(('sixteen-items', b'[package]\r\n' + b''.join(
        b'item=i%d, f%d.fex\r\n' % (i, i) for i in range(16)), many))
    work.append(('zero-length-middle', b'[package]\r\nitem=a, f0.fex\r\nitem=b, empty.fex\r\nitem=c, f1.fex\r\n',
                 {'f0.fex': many['f0.fex'], 'empty.fex': b'', 'f1.fex': many['f1.fex']}))
    work.append(('long-name', b'[package]\r\nitem=' + b'n' * 63 + b', f0.fex\r\n', {'f0.fex': many['f0.fex']}))
    work.append(('table-spills-kib', b'[package]\r\n' + b''.join(
        b'item=i%d, f.fex\r\n' % i for i in range(3)), {'f.fex': blob(2048)}))
    for name, config, files in work:
        directory = args.output / name
        directory.mkdir()
        for filename, data in files.items():
            (directory / filename).write_bytes(data)
        (directory / 'boot_package.cfg').write_bytes(config)
        package, log = toc.pack_config(directory / 'boot_package.cfg', directory)
        entries = toc.parse_config(config)
        items = [(label, files[path]) for label, path in entries]
        check_spec(package, items)
        assert toc.unpack(package) == items
        assert toc.pack(items) == package
        if vendor:
            vendor_dir = args.output / (name + '.vendor')
            vendor_dir.mkdir()
            expected, vendor_log = vendor_pack(vendor, config, files, vendor_dir)
            assert package == expected, 'packer output differs from the vendor executable: ' + name
            assert log == vendor_log, 'packer log differs from the vendor executable: ' + name
        cases.append(name)
    # Inputs the vendor tool mishandles or the format cannot hold are refused.
    def rejected(action):
        try:
            action()
        except (ValueError, OSError):
            return
        raise AssertionError('Malformed input admitted')
    rejected(lambda: toc.pack([]))
    rejected(lambda: toc.pack([('x', b'')] * 17))
    rejected(lambda: toc.pack([('n' * 64, b'')]))
    rejected(lambda: toc.pack([('', b'')]))
    rejected(lambda: toc.pack([('big', bytes(10 << 20))]))
    rejected(lambda: toc.parse_config(b'item=a, b\n'))
    rejected(lambda: toc.parse_config(b'[package]\nitem=a\n'))
    rejected(lambda: toc.parse_config(b'[package]\n' + b'item=a, b\n' * 17))
    cases.append('refusals')
    # Corrupt each format field of a valid package; the decoder must refuse every one.
    package = toc.pack([('monitor', blob(300))])
    for label, offset in [('magic', 16), ('checksum', 20), ('count', 32), ('valid-length', 36), ('end', 60),
                          ('item-name', 64 + 63), ('item-offset', 128), ('item-length', 134), ('item-type', 140),
                          ('item-reserved', 200), ('item-end', 64 + 364), ('payload-padding', 1024 + 300),
                          ('tail', len(package) - 1)]:
        data = bytearray(package)
        data[offset] ^= 1
        if label in ('item-name', 'item-offset', 'item-length', 'item-type', 'item-reserved', 'item-end',
                     'payload-padding', 'tail'):
            struct.pack_into('<I', data, 20, 0x5f0a6c39)
            struct.pack_into('<I', data, 20, sum(w[0] for w in struct.iter_unpack('<I', data)) & 0xffffffff)
        rejected(lambda: toc.unpack(bytes(data)))
        cases.append('corrupt-' + label)
    (args.output / 'result.json').write_text(json.dumps({
        'passed': True, 'vendor_comparison': bool(vendor), 'cases': cases,
        'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest()}, indent=2) + '\n')
    print('PASS toc package:', len(cases), 'cases;',
          'byte-identical to the vendor executable' if vendor else 'vendor comparison skipped (executable absent)')


if __name__ == '__main__':
    main()
