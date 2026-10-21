#!/usr/bin/env python3
"""Exercise tools/boot-slots.py on sparse ordinary card image files.

Covers allocation against a backup, standby-slot planning, header-last
application with interruption, compare-before-write refusal, container
extents, resume instead of re-planning, foreign headers, address limits,
rollback and header admission rules. No block device is opened.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('boot_slots', ROOT / 'tools/boot-slots.py')
slots = importlib.util.module_from_spec(spec)
spec.loader.exec_module(slots)
CARD_SECTORS = 131072
SLOT_A, SLOT_B = '40960:8192', '49152:8192'


def refused(action):
    try:
        action()
    except SystemExit as stop:
        if stop.code:
            return
        raise AssertionError('Command succeeded unexpectedly')
    except ValueError:
        return
    raise AssertionError('Command succeeded unexpectedly')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New ordinary evidence directory')
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    log = (out / 'boot-slots.log').open('w')

    def tool(*argv):
        with contextlib.redirect_stdout(log):
            slots.main([str(v) for v in argv])

    def verify(path):
        fd, count = slots.open_image(path)
        try:
            return slots.public(slots.inspect(fd, count))
        finally:
            os.close(fd)

    def card(name, partitions=()):
        path = out / name
        with path.open('xb') as stream:
            stream.truncate(CARD_SECTORS * 512)
            if partitions:
                mbr = bytearray(512)
                for i, (kind, first, count) in enumerate(partitions):
                    struct.pack_into('<4xB3xII', mbr, 446 + 16 * i, kind, first, count)
                mbr[510:512] = b'\x55\xaa'
                stream.write(mbr)
        return path

    cases = []
    backup = card('backup.img', [(0x83, 65536, 65536)])
    allocation = out / 'allocation.json'
    tool('allocate', '--backup', backup, '--slot-a', SLOT_A, '--slot-b', SLOT_B, '--output', allocation)
    recorded = json.loads(allocation.read_text())
    assert recorded['partition_table'] == 'mbr' and recorded['card_sectors'] == CARD_SECTORS
    assert recorded['backup']['sha256'] == hashlib.sha256(backup.read_bytes()).hexdigest()
    cases.append('allocate-against-mbr-backup')
    for name, a, b, table in (('slot-in-partition', SLOT_A, '65536:8192', [(0x83, 65536, 65536)]),
                              ('slot-in-boot-area', '32768:8192', SLOT_B, []),
                              ('unaligned-slot', '40961:8192', SLOT_B, []),
                              ('overlapping-slots', SLOT_A, '40960:8192', []),
                              ('partition-in-boot-area', SLOT_A, SLOT_B, [(0x0c, 8192, 32768)]),
                              ('gpt', SLOT_A, SLOT_B, [(0xee, 1, CARD_SECTORS - 1)])):
        source = card('refuse-%s.img' % name, table)
        refused(lambda: tool('allocate', '--backup', source, '--slot-a', a, '--slot-b', b,
                             '--output', out / ('refuse-%s.json' % name)))
        assert not (out / ('refuse-%s.json' % name)).exists()
        cases.append('allocate-refuses-' + name)
    foreign = card('foreign-header.img')
    with foreign.open('r+b') as stream:
        stream.seek(slots.HEADER_SECTORS['B'] * 512)
        stream.write(b'user data')
    refused(lambda: tool('allocate', '--backup', foreign, '--slot-a', SLOT_A, '--slot-b', SLOT_B,
                         '--output', out / 'refuse-foreign.json'))
    link = out / 'backup-link.img'
    link.symlink_to(backup.name)
    refused(lambda: tool('allocate', '--backup', link, '--slot-a', SLOT_A, '--slot-b', SLOT_B,
                         '--output', out / 'refuse-link.json'))
    refused(lambda: tool('verify', '--card-image', out))
    cases.append('allocate-refuses-foreign-header-symlink-and-directory')

    payloads = {}
    for name, size in (('image1', 9000), ('asset1', 5000), ('image2', 4100), ('asset2', 700)):
        payloads[name] = bytes((i * 7 + size) & 255 for i in range(size))
        (out / name).write_bytes(payloads[name])

    def plan(source, number, name, *extra):
        tool('plan', '--allocation', allocation, '--card-image', source,
             '--image', '%s@0x41000000' % (out / ('image%d' % number)), '--entry', '0x41000100',
             '--asset', '%s@0x44000000' % (out / ('asset%d' % number)), *extra, '--output', out / name)
        return out / name / 'plan.json'

    # A synthetic container exercises only the manifest extents, not boot0.
    container = bytearray(20 << 20)
    boot0, package = os.urandom(245760), os.urandom(16384 + 1024)
    container[8192:8192 + len(boot0)] = boot0
    container[16793600:16793600 + len(package)] = package
    (out / 'synthetic.microsd-monitor-container.bin').write_bytes(container)
    manifest = {'kind': 'a733-microsd-monitor-container',
                'files': {'microsd-monitor-container.bin': {
                    'bytes': len(container), 'sha256': hashlib.sha256(container).hexdigest()}},
                'card_writes': [
                    {'name': 'boot0', 'offset': 8192, 'bytes': len(boot0),
                     'sha256': hashlib.sha256(boot0).hexdigest()},
                    {'name': 'monitor-package', 'offset': 16793600, 'bytes': len(package),
                     'sha256': hashlib.sha256(package).hexdigest()}]}
    (out / 'synthetic.manifest.json').write_text(json.dumps(manifest))

    working = out / 'working.img'
    shutil.copyfile(backup, working)
    first = plan(working, 1, 'plan-1', '--container', out / 'synthetic.manifest.json')
    plan_1 = json.loads(first.read_text())
    assert plan_1['slot'] == 'A' and plan_1['generation'] == 1
    assert [w['name'] for w in plan_1['writes']] == ['payload', 'header', 'boot0', 'monitor-package']
    assert verify(working)['selected'] is None
    tool('apply', '--plan', first, '--card-image', working)
    state = verify(working)
    assert state['selected'] == 'A' and state['slots']['B']['header'] == 'absent'
    data = working.read_bytes()
    assert data[8192:8192 + len(boot0)] == boot0
    assert data[16793600:16793600 + len(package)] == package
    assert data[:512] == backup.read_bytes()[:512]
    cases.append('install-slot-a-then-monitor-extents-only')

    # An install that stopped after its header must be resumed: planning the
    # same contents again would overwrite the slot that holds the old image.
    partial = out / 'partial.img'
    shutil.copyfile(backup, partial)
    container_plan = plan(partial, 1, 'plan-partial', '--container', out / 'synthetic.manifest.json')
    tool('apply', '--plan', container_plan, '--card-image', partial, '--stop-after', 'header')
    assert verify(partial)['selected'] == 'A'
    assert partial.read_bytes()[8192:8192 + len(boot0)] != boot0
    refused(lambda: plan(partial, 1, 'plan-partial-again', '--container', out / 'synthetic.manifest.json'))
    assert not (out / 'plan-partial-again').exists()
    tool('apply', '--plan', container_plan, '--card-image', partial)
    data = partial.read_bytes()
    assert data[8192:8192 + len(boot0)] == boot0 and data[16793600:16793600 + len(package)] == package
    cases.append('interrupted-install-refuses-replan-and-resumes')

    def forged_header(name, generation, shift):
        path = out / name
        shutil.copyfile(working, path)
        extents = [dict(e, first_sector=e['first_sector'] + shift) for e in plan_1['extents']]
        with path.open('r+b') as stream:
            if shift:
                stream.seek(40960 * 512)
                region = stream.read(8192 * 512)
                stream.seek((40960 + shift) * 512)
                stream.write(region)
            stream.seek(slots.HEADER_SECTORS['A'] * 512)
            stream.write(slots.pack_header('A', generation, plan_1['entry'], extents))
        assert verify(path)['selected'] == 'A'
        return path

    # A valid header whose extents lie in the standby region keeps that region.
    foreign_extent = forged_header('foreign-extent.img', 1, 8192)
    refused(lambda: plan(foreign_extent, 2, 'plan-foreign-extent'))
    assert not (out / 'plan-foreign-extent').exists()
    cases.append('plan-refuses-standby-region-used-by-valid-header')
    # Values that do not fit the 64-bit header fields refuse without a traceback.
    last = forged_header('last-generation.img', slots.MASK64, 0)
    refused(lambda: plan(last, 2, 'plan-last-generation'))
    refused(lambda: tool('plan', '--allocation', allocation, '--card-image', working,
                         '--image', '%s@0x41000000' % (out / 'image2'), '--entry', hex(1 << 64),
                         '--output', out / 'plan-wide-entry'))
    refused(lambda: tool('plan', '--allocation', allocation, '--card-image', working,
                         '--image', '%s@%#x' % (out / 'image2', 1 << 64), '--entry', '0x41000000',
                         '--output', out / 'plan-wide-load'))
    assert not any((out / n).exists() for n in ('plan-last-generation', 'plan-wide-entry', 'plan-wide-load'))
    cases.append('plan-refuses-64-bit-overflow-cleanly')

    second = plan(working, 2, 'plan-2')
    plan_2 = json.loads(second.read_text())
    assert plan_2['slot'] == 'B' and plan_2['generation'] == 2 and plan_2['before']['selected'] == 'A'
    tool('apply', '--plan', second, '--card-image', working, '--stop-after', 'payload')
    state = verify(working)
    assert state['selected'] == 'A' and state['slots']['B']['header'] == 'absent'
    cases.append('interrupted-before-header-keeps-slot-a')
    tool('apply', '--plan', second, '--card-image', working)
    state = verify(working)
    assert state['selected'] == 'B' and state['slots']['A']['header'] == 'valid'
    cases.append('resumed-header-commit-selects-slot-b')

    third = plan(working, 1, 'plan-3')
    assert json.loads(third.read_text())['slot'] == 'A'
    damage = 40960 * 512 + 100  # One byte of the slot A image.

    def flip(path):
        with path.open('r+b') as stream:
            stream.seek(damage)
            value = stream.read(1)[0]
            stream.seek(damage)
            stream.write(bytes([value ^ 0x40]))

    flip(working)
    before = working.read_bytes()
    refused(lambda: tool('apply', '--plan', third, '--card-image', working))
    assert working.read_bytes() == before
    cases.append('changed-image-refused-before-any-write')
    refused(lambda: tool('rollback', '--card-image', working, '--output', out / 'rollback-refused'))
    assert not (out / 'rollback-refused').exists()
    cases.append('rollback-refuses-damaged-previous-slot')

    flip(working)
    slot_b = working.read_bytes()[49152 * 512:57344 * 512]
    rollback = out / 'rollback'
    tool('rollback', '--card-image', working, '--output', rollback)
    assert [w['name'] for w in json.loads((rollback / 'plan.json').read_text())['writes']] == ['invalidate-B']
    tool('apply', '--plan', rollback / 'plan.json', '--card-image', working)
    state = verify(working)
    assert state['selected'] == 'A' and state['slots']['B']['header'] == 'absent'
    assert working.read_bytes()[49152 * 512:57344 * 512] == slot_b
    cases.append('rollback-invalidates-active-header-only')

    changed = out / 'changed-table.img'
    shutil.copyfile(working, changed)
    with changed.open('r+b') as stream:
        stream.seek(446 + 8)
        stream.write(struct.pack('<I', 65537))
    refused(lambda: plan(changed, 2, 'plan-changed-table'))
    refused(lambda: tool('plan', '--allocation', allocation, '--card-image', working,
                         '--image', '%s@0x41000000' % (out / 'image1'), '--entry', '0x41000000',
                         *sum((['--asset', '%s@%#x' % (out / 'asset1', 0x44000000 + i * 0x100000)]
                               for i in range(4)), []), '--output', out / 'plan-too-many'))
    big = out / 'big.bin'
    big.write_bytes(bytes(8192 * 512 + 1))
    refused(lambda: tool('plan', '--allocation', allocation, '--card-image', working,
                         '--image', '%s@0x41000000' % big, '--entry', '0x41000000',
                         '--output', out / 'plan-big'))
    refused(lambda: tool('plan', '--allocation', allocation, '--card-image', working,
                         '--image', '%s@0x41000000' % (out / 'image1'), '--entry', '0x41003000',
                         '--output', out / 'plan-entry'))
    cases.append('plan-refuses-changed-table-extent-count-size-and-entry')

    # Header admission mirrors the loader; each case changes one rule.
    extents = [{'first_sector': 40960, 'bytes': 4096, 'load': 0x41000000, 'kind': 1, 'sha256': '00' * 32}]
    good = slots.pack_header('A', 5, 0x41000000, extents)
    assert slots.check_header(good, 'A', CARD_SECTORS)[0] == 'valid'
    assert slots.check_header(good, 'B', CARD_SECTORS)[0] == 'fields'
    assert slots.check_header(bytes(512), 'A', CARD_SECTORS)[0] == 'absent'

    def resealed(offset, value):
        raw = bytearray(good)
        raw[offset:offset + len(value)] = value
        raw[480:] = hashlib.sha256(raw[:480]).digest()
        return bytes(raw)

    for name, raw in (('version', resealed(8, struct.pack('<I', 2))),
                      ('inverse', resealed(24, struct.pack('<Q', 5))),
                      ('reserved', resealed(400, b'\x01')),
                      ('unused-extent', resealed(128 + 8, b'\x01')),
                      ('asset-kind-first', resealed(64 + 24, struct.pack('<I', 2))),
                      ('boot-area', resealed(64, struct.pack('<Q', 40959))),
                      ('beyond-card', resealed(64, struct.pack('<Q', CARD_SECTORS - 7))),
                      ('below-window', resealed(64 + 16, struct.pack('<Q', 0x40fff000))),
                      ('entry', resealed(40, struct.pack('<Q', 0x41001000)))):
        assert slots.check_header(raw, 'A', CARD_SECTORS)[0] == 'fields', name
    assert slots.check_header(good[:16] + b'\x04' + good[17:], 'A', CARD_SECTORS)[0] == 'digest'
    overlap = extents + [{'first_sector': 40968, 'bytes': 100, 'load': 0x41000800, 'kind': 2, 'sha256': '00' * 32}]
    assert slots.check_header(slots.pack_header('A', 5, 0x41000000, overlap), 'A', CARD_SECTORS)[0] == 'fields'
    cases.append('header-rules-match-loader')
    log.close()
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'cases': cases, 'block_devices_opened': False,
        'scope': 'Host slot tool on sparse ordinary files; loader agreement is checked by standalone-boot-loader.py.'},
        indent=2) + '\n')
    print('PASS boot-slot tool:', len(cases), 'cases')


if __name__ == '__main__':
    main()
