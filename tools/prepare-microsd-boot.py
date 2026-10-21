#!/usr/bin/env python3
"""Prepare an unqualified microSD monitor container using the pinned host packer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile

REVISION = 'a95129bf30ea11886cc35f08ebb3e69695a26656'
BASE_URL = 'https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/' + REVISION
BOOT0_URL = BASE_URL + '/external/packages/pack-uboot/sun60iw2/bin/boot0_sdcard_orangepizero3w.fex'
PACKER_URL = BASE_URL + '/external/packages/pack-uboot/tools/dragonsecboot'
BOOT0_HASH = '829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3'
PACKER_HASH = 'e5f9076504a0ccde29c5978bcfc341c1c5ac02d947c47b849a5e8d26433545f6'
BOOT0_BYTES, PACKER_BYTES = 0x3c000, 9105656
IMAGE_BYTES, BOOT0_OFFSET, PACKAGE_OFFSET = 20 << 20, 8192, 16793600
LOAD_ADDRESS, LOAD_LIMIT, HEADER_BYTES = 0x47000000, 0x47400000, 0x108
PACKAGE_CAPACITY = IMAGE_BYTES - PACKAGE_OFFSET
INPUT_KIND = 'a733-microsd-monitor-container-input'
MONITOR_FORMAT = 'sunstone-a733-flat-monitor-v1'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def directory(path):
    """Open every directory component without following symbolic links."""
    path = Path(os.path.abspath(path))
    if path == Path('/dev') or Path('/dev') in path.parents:
        raise ValueError('Device paths are forbidden')
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                            dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


def read_regular(path, maximum):
    """Snapshot bounded ordinary input bytes; do not follow any symlinks."""
    path = Path(os.path.abspath(path))
    parent = directory(path.parent)
    try:
        fd = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                     dir_fd=parent)
    finally:
        os.close(parent)
    with os.fdopen(fd, 'rb') as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
            raise ValueError('Input must be a nonempty bounded regular file: ' + str(path))
        data = source.read(maximum + 1)
        after = os.fstat(source.fileno())
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
        if len(data) != before.st_size or identity(before) != identity(after):
            raise ValueError('Input changed while copied: ' + str(path))
        return data


def exact_keys(value, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ValueError('Unexpected input contract fields')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate input contract field: ' + key)
        result[key] = value
    return result


def load_monitor(contract_path):
    contract_path = Path(os.path.abspath(contract_path))
    raw = read_regular(contract_path, 16384)
    contract = json.loads(raw, object_pairs_hook=unique_object)
    exact_keys(contract, ('schema', 'kind', 'monitor'))
    if type(contract['schema']) is not int or contract['schema'] != 1 or contract['kind'] != INPUT_KIND:
        raise ValueError('Unsupported monitor input contract')
    monitor = contract['monitor']
    exact_keys(monitor, ('format', 'path', 'bytes', 'sha256'))
    if (monitor['format'] != MONITOR_FORMAT or not isinstance(monitor['path'], str)
            or not monitor['path'] or type(monitor['bytes']) is not int
            or not HEADER_BYTES + 4 <= monitor['bytes'] <= LOAD_LIMIT - LOAD_ADDRESS
            or not isinstance(monitor['sha256'], str) or len(monitor['sha256']) != 64):
        raise ValueError('Invalid typed monitor extent or identity')
    path = contract_path.parent / monitor['path']
    data = read_regular(path, LOAD_LIMIT - LOAD_ADDRESS)
    if len(data) != monitor['bytes'] or digest(data) != monitor['sha256']:
        raise ValueError('Monitor whole-file identity mismatch')
    entry = validate_monitor(data)
    return data, {'contract_sha256': digest(raw), 'format': MONITOR_FORMAT,
                  'bytes': len(data), 'sha256': digest(data), 'load_address': LOAD_ADDRESS,
                  'reserved_end': LOAD_LIMIT, 'branch_target_offset': entry}


def validate_monitor(data):
    if not HEADER_BYTES + 4 <= len(data) <= LOAD_LIMIT - LOAD_ADDRESS:
        raise ValueError('Monitor exceeds admitted load interval or lacks entry instruction')
    if data[:4] == b'\x7fELF' or data[4:12] != b'monitor\0':
        raise ValueError('Expected flat monitor header, not an ELF or stock role substitution')
    branch, = struct.unpack_from('<I', data)
    immediate = branch & 0x03ffffff
    offset = (immediate - (1 << 26) if immediate & (1 << 25) else immediate) * 4
    if branch & 0xfc000000 != 0x14000000 or not HEADER_BYTES <= offset <= len(data) - 4:
        raise ValueError('First instruction must branch over mutation header into this payload')
    if struct.unpack_from('<I', data, 0x2c)[0] != LOAD_ADDRESS:
        raise ValueError('Monitor loadword differs from reserved address')
    if align(1024 + align(len(data), 1024), 16384) > PACKAGE_CAPACITY:
        raise ValueError('Monitor package exceeds published container capacity')
    return offset


def validate_package(data, monitor):
    expected = align(1024 + align(len(monitor), 1024), 16384)
    if len(data) != expected or len(data) > PACKAGE_CAPACITY:
        raise ValueError('Unexpected package extent')
    header = struct.unpack_from('<16s12I', data)
    # Only checksum differs from this complete canonical single-item header.
    canonical = (b'sunxi-package\0\0\0', 0x89119800, header[2], 0, 0, 1,
                 len(data), 0, 0, 0, 0, 0, 0x3b45494d)
    if header != canonical:
        raise ValueError('Unexpected package header, item count or reserved fields')
    item = struct.unpack_from('<64s76I', data, 64)
    canonical_item = (b'monitor' + bytes(57), 1024, len(monitor), 0, 3, 0, 0,
                      *([0] * 69), 0x3b454949)
    if item != canonical_item:
        raise ValueError('Unexpected package item identity, geometry or reserved fields')
    total = sum(word[0] for word in struct.iter_unpack('<I', data))
    if (total - header[2] + 0x5f0a6c39) & 0xffffffff != header[2]:
        raise ValueError('Package additive checksum mismatch')
    if (data[1024:1024 + len(monitor)] != monitor or any(data[432:1024])
            or any(data[1024 + len(monitor):])):
        raise ValueError('Package payload or zero padding mismatch')


def pinned(path, size, expected):
    data = read_regular(path, size)
    if len(data) != size or digest(data) != expected:
        raise ValueError('Pinned dependency identity mismatch: ' + str(path))
    return data


def publish(output, files):
    """Create only exclusive regular files under an already-open parent."""
    output = Path(os.path.abspath(output))
    if output.name in ('', '.', '..'):
        raise ValueError('Output must name a new filename prefix')
    parent = directory(output.parent)
    try:
        names = [output.name] + [output.name + '.' + name for name in files]
        if any('/' in name or name in ('.', '..') for name in files):
            raise ValueError('Output suffix must be a simple filename')
        # Preflight avoids routine partial output; O_EXCL remains authoritative
        # against a concurrent entry created after this observation.
        for name in names:
            try:
                os.stat(name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                continue
            raise FileExistsError('Output entry already exists: ' + name)
        for name, data in files.items():
            target = os.open(output.name + '.' + name,
                             os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                             0o600, dir_fd=parent)
            with os.fdopen(target, 'wb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
        os.fsync(parent)
    finally:
        os.close(parent)


def prepare(args):
    monitor, identity = load_monitor(args.input)
    boot0 = pinned(args.boot0, BOOT0_BYTES, BOOT0_HASH)
    packer = pinned(args.packer, PACKER_BYTES, PACKER_HASH)
    # Execute a verified private copy, never a path that can change after hashing.
    with tempfile.TemporaryDirectory(prefix='sunstone-monitor-package-') as temporary:
        workspace = Path(temporary)
        tool = workspace / 'dragonsecboot'
        tool.write_bytes(packer)
        tool.chmod(0o700)
        (workspace / 'monitor.fex').write_bytes(monitor)
        config = b'[package]\r\nitem=monitor, monitor.fex\r\n'
        (workspace / 'boot_package.cfg').write_bytes(config)
        log_path = workspace / 'packer.log'
        with log_path.open('xb') as log:
            subprocess.run([str(tool), '-pack', 'boot_package.cfg'], cwd=workspace,
                           env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'},
                           stdout=log, stderr=subprocess.STDOUT, timeout=30, check=True)
        package = read_regular(workspace / 'boot_package.fex', PACKAGE_CAPACITY)
        validate_package(package, monitor)
        log = read_regular(log_path, 65536)
    if BOOT0_OFFSET + len(boot0) > PACKAGE_OFFSET or PACKAGE_OFFSET + len(package) > IMAGE_BYTES:
        raise ValueError('Container extent overlap or overflow')
    image = bytearray(IMAGE_BYTES)
    image[BOOT0_OFFSET:BOOT0_OFFSET + len(boot0)] = boot0
    image[PACKAGE_OFFSET:PACKAGE_OFFSET + len(package)] = package
    files = {'monitor.bin': monitor, 'boot_package.cfg': config,
             'boot_package.fex': package, 'microsd-monitor-container.bin': image,
             'packer.log': log}
    manifest = {'schema': 1, 'kind': 'a733-microsd-monitor-container',
                'purpose': 'Unqualified ordinary-file host packaging artifact',
                'qualification': {'cpu_entry': False, 'application_functionality': False,
                                  'physical_boot': False, 'storage_security': False},
                'monitor': identity,
                'layout': {'bytes': IMAGE_BYTES, 'fill': 0, 'boot0_offset': BOOT0_OFFSET,
                           'package_offset': PACKAGE_OFFSET, 'backup_emitted': False},
                # Only these extents carry container data. Its zero gaps are not
                # card contents; a card install must not write them over slot headers.
                'card_writes': [
                    {'name': 'boot0', 'offset': BOOT0_OFFSET, 'bytes': len(boot0),
                     'sha256': digest(boot0)},
                    {'name': 'monitor-package', 'offset': PACKAGE_OFFSET,
                     'bytes': len(package), 'sha256': digest(package)}],
                'dependencies': {'boot0': {'url': BOOT0_URL, 'sha256': BOOT0_HASH},
                                 'packer': {'url': PACKER_URL, 'sha256': PACKER_HASH}},
                'provenance_limit': 'Exact packer/source equivalence and individual tool license '
                                    'grant unresolved; published tool source has All Rights Reserved notices.',
                'command': ['dragonsecboot', '-pack', 'boot_package.cfg'],
                'files': {name: {'bytes': len(data), 'sha256': digest(data)}
                          for name, data in files.items()}}
    files['manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    publish(args.output, files)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='Typed JSON monitor contract')
    parser.add_argument('--boot0', type=Path, required=True, help='Exact pinned unmodified board boot0')
    parser.add_argument('--packer', type=Path, required=True, help='Exact pinned dragonsecboot executable')
    parser.add_argument('--output', type=Path, required=True, help='New filename prefix under an existing ordinary parent')
    args = parser.parse_args()
    try:
        prepare(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')
    print(args.output)


if __name__ == '__main__':
    main()
