#!/usr/bin/env python3
"""Exercise the real ordinary-file preparer once with a nonbootable monitor."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('microsd_preparer', ROOT / 'tools/prepare-microsd-boot.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def synthetic(length=0x10c):
    data = bytearray(length)
    struct.pack_into('<I', data, 0, 0x14000042)  # B +0x108, header shape only.
    data[4:12] = b'monitor\0'
    struct.pack_into('<I', data, 0x2c, 0x47000000)
    struct.pack_into('<I', data, 0x108, 0xd503201f)
    return data


def rejected(action):
    try:
        action()
    except (ValueError, OSError):
        return
    raise AssertionError('Malformed input or preservation condition admitted')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boot0', type=Path, required=True)
    parser.add_argument('--packer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New ordinary evidence directory')
    args = parser.parse_args()
    args.output.mkdir(mode=0o700, exist_ok=False)
    fixture = args.output / 'synthetic-monitor.bin'
    original = bytes(synthetic())
    fixture.write_bytes(original)
    contract = {'schema': 1, 'kind': host.INPUT_KIND, 'monitor': {
        'format': host.MONITOR_FORMAT, 'path': fixture.name,
        'bytes': len(original), 'sha256': hashlib.sha256(original).hexdigest()}}
    source = args.output / 'input.json'
    source.write_text(json.dumps(contract, indent=2) + '\n')
    output = args.output / 'prepared'
    prepared = lambda name: output.with_name(output.name + '.' + name)
    command = [sys.executable, str(ROOT / 'tools/prepare-microsd-boot.py'),
               '--input', str(source), '--boot0', str(args.boot0),
               '--packer', str(args.packer), '--output', str(output)]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    (args.output / 'wrapper.stdout').write_text(result.stdout)
    (args.output / 'wrapper.stderr').write_text(result.stderr)
    package = prepared('boot_package.fex').read_bytes()
    image = prepared('microsd-monitor-container.bin').read_bytes()
    boot0 = host.pinned(args.boot0, host.BOOT0_BYTES, host.BOOT0_HASH)
    assert len(image) == host.IMAGE_BYTES
    assert image[host.BOOT0_OFFSET:host.BOOT0_OFFSET + len(boot0)] == boot0
    assert image[host.PACKAGE_OFFSET:host.PACKAGE_OFFSET + len(package)] == package
    assert not any(image[:host.BOOT0_OFFSET] + image[host.BOOT0_OFFSET + len(boot0):host.PACKAGE_OFFSET]
                   + image[host.PACKAGE_OFFSET + len(package):])
    assert fixture.read_bytes() == original
    manifest = json.loads(prepared('manifest.json').read_text())
    assert not any(manifest['qualification'].values())
    assert manifest['card_writes'] == [
        {'name': 'boot0', 'offset': host.BOOT0_OFFSET, 'bytes': len(boot0), 'sha256': host.digest(boot0)},
        {'name': 'monitor-package', 'offset': host.PACKAGE_OFFSET, 'bytes': len(package),
         'sha256': host.digest(package)}]
    for name, identity in manifest['files'].items():
        data = prepared(name).read_bytes()
        assert len(data) == identity['bytes'] and host.digest(data) == identity['sha256']
    cases = ['actual-wrapper-one-monitor-geometry-checksum-whole-image-hash']
    # No further packer executions: malformed headers and output ownership are local checks.
    for name, offset, value in [('ELF', 0, 0x464c457f), ('wrong-load', 0x2c, 0x47000004),
                                ('header-entry', 0, 0x14000001), ('outside-entry', 0, 0x14000100),
                                ('backward-entry', 0, 0x17ffffff), ('BL-entry', 0, 0x94000042)]:
        data = synthetic()
        struct.pack_into('<I', data, offset, value)
        rejected(lambda: host.validate_monitor(data))
        cases.append(name)
    largest = host.PACKAGE_CAPACITY - 1024
    host.validate_monitor(synthetic(largest))
    rejected(lambda: host.validate_monitor(synthetic(largest + 4)))
    rejected(lambda: host.validate_monitor(synthetic((4 << 20) + 4)))
    cases.append('exact-package-and-load-capacity-edges')
    # Corruptions cover distinct ownership, checksum, payload and extent constraints.
    for name, offset in [('extra-item', 32), ('wrong-item-offset', 128),
                         ('wrong-item-name', 64), ('checksum', 20), ('payload', 1024),
                         ('padding', len(package) - 1)]:
        data = bytearray(package)
        data[offset] ^= 1
        if name in ('payload', 'padding'):
            struct.pack_into('<I', data, 20, 0x5f0a6c39)
            checksum = sum(word[0] for word in struct.iter_unpack('<I', data)) & 0xffffffff
            struct.pack_into('<I', data, 20, checksum)
        rejected(lambda: host.validate_package(data, original))
        cases.append(name)
    rejected(lambda: host.validate_package(package[:-4], original))
    cases.append('truncated-package')
    bad = dict(contract['monitor']);bad['sha256'] = '0' * 64
    (args.output / 'bad-input.json').write_text(json.dumps({**contract, 'monitor': bad}))
    rejected(lambda: host.load_monitor(args.output / 'bad-input.json'))
    cases.append('whole-input-hash-mismatch')
    link = args.output / 'input-link'
    link.symlink_to(fixture.name)
    rejected(lambda: host.read_regular(link, 4096))
    linked_parent = args.output / 'linked-parent'
    linked_parent.symlink_to('.', target_is_directory=True)
    rejected(lambda: host.publish(linked_parent / 'new', {'keep': b'bad'}))
    rejected(lambda: host.publish(output, {'monitor.bin': b'bad'}))
    protected = args.output / 'protected'
    protected.write_bytes(b'preserve')
    rejected(lambda: host.publish(protected, {'keep': b'bad'}))
    existing_directory = args.output / 'existing-directory'
    existing_directory.mkdir()
    rejected(lambda: host.publish(existing_directory, {'keep': b'bad'}))
    assert not (existing_directory / 'keep').exists()
    output_link = args.output / 'output-link'
    output_link.symlink_to(prepared('monitor.bin').name)
    rejected(lambda: host.publish(output_link, {'keep': b'bad'}))
    assert protected.read_bytes() == b'preserve'
    assert prepared('monitor.bin').read_bytes() == original
    assert not (args.output / 'new.keep').exists()
    race = args.output / 'raced'
    real_open = host.os.open
    def racing_open(path, flags, *positional, **keywords):
        if path == 'raced.keep':
            race.with_name('raced.keep').write_bytes(b'concurrent-owner')
        return real_open(path, flags, *positional, **keywords)
    host.os.open = racing_open
    try:
        rejected(lambda: host.publish(race, {'keep': b'bad'}))
    finally:
        host.os.open = real_open
    assert race.with_name('raced.keep').read_bytes() == b'concurrent-owner'
    cases.append('symlink-existing-file-directory-and-concurrent-file-preservation')
    (args.output / 'result.json').write_text(json.dumps({
        'passed': True, 'scope': 'Host container generation/validation and ordinary-file preservation only',
        'synthetic_nonbootable': True, 'packer_invocations': 1,
        'cpu_execution': False, 'physical_access': False, 'command': command,
        'image_sha256': host.digest(image), 'package_sha256': host.digest(package),
        'input_sha256': host.digest(original), 'cases': cases}, indent=2) + '\n')
    print('PASS ordinary-file monitor packaging:', len(cases), 'focused cases')


if __name__ == '__main__':
    main()
