#!/usr/bin/env python3
"""Plan and check A/B boot slots on ordinary card image files (docs/boot-slots.md).

No command opens a block device. `apply` writes only an existing regular file,
such as a working copy of a card backup; it never writes a card.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys

SECTOR = 512
HEADER_SECTORS = {'A': 8192, 'B': 16384}
BOOT_AREA_SECTORS = 40960
ALLOCATION_UNIT = 8192  # 4 MiB, a common SD erase/allocation unit.
SLOT_MAGIC = int.from_bytes(b'AS73SLOT', 'big')
VERSION, EXTENTS, IMAGE, ASSET = 1, 4, 1, 2
LOAD_BASE, LOAD_LIMIT = 0x41000000, 0x47000000
HEADER = struct.Struct('<QIIQQIIQ16x')
EXTENT = struct.Struct('<QQQII32s')
ALLOCATION_KIND = 'a733-boot-slot-allocation'
PLAN_KIND = 'a733-boot-slot-plan'
MASK64 = (1 << 64) - 1


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sectors(length):
    return (length + SECTOR - 1) // SECTOR


def open_image(path, writable=False):
    """Open an existing regular file without following a final symlink."""
    fd = os.open(path, (os.O_RDWR if writable else os.O_RDONLY) | os.O_NOFOLLOW)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_size % SECTOR or not info.st_size:
        os.close(fd)
        raise ValueError('Card image must be a nonempty regular file of whole sectors: ' + str(path))
    return fd, info.st_size // SECTOR


def read_at(fd, sector, count):
    data = os.pread(fd, count * SECTOR, sector * SECTOR)
    if len(data) != count * SECTOR:
        raise ValueError('Short card image read')
    return data


def pack_header(slot, generation, entry, extents):
    body = HEADER.pack(SLOT_MAGIC, VERSION, 'AB'.index(slot), generation,
                       ~generation & MASK64, len(extents), 0, entry)
    for e in extents:
        body += EXTENT.pack(e['first_sector'], e['bytes'], e['load'], e['kind'], 0,
                            bytes.fromhex(e['sha256']))
    body += bytes(EXTENT.size * (EXTENTS - len(extents)) + 160)
    return body + hashlib.sha256(body).digest()


def check_header(raw, slot, card_sectors):
    """Mirror boot/a733/stage2/loader.c header_check; return (state, header)."""
    magic, version, index, generation, inverse, count, flags, entry = HEADER.unpack_from(raw)
    if magic != SLOT_MAGIC:
        return 'absent', None
    if hashlib.sha256(raw[:480]).digest() != raw[480:512]:
        return 'digest', None
    if (version != VERSION or index != 'AB'.index(slot) or not generation
            or inverse != ~generation & MASK64 or not 1 <= count <= EXTENTS or flags
            or any(raw[48:64]) or any(raw[320:480])):
        return 'fields', None
    extents = []
    for i in range(EXTENTS):
        first, length, load, kind, reserved, digest = EXTENT.unpack_from(raw, 64 + i * EXTENT.size)
        if i >= count:
            if any(raw[64 + i * EXTENT.size:128 + i * EXTENT.size]):
                return 'fields', None
            continue
        size = sectors(length)
        if (kind != (ASSET if i else IMAGE) or reserved or not length
                or length > LOAD_LIMIT - LOAD_BASE or first < BOOT_AREA_SECTORS
                or first > card_sectors or size > card_sectors - first
                or load < LOAD_BASE or load > LOAD_LIMIT or length > LOAD_LIMIT - load):
            return 'fields', None
        for other in extents:
            if not (load + length <= other['load'] or other['load'] + other['bytes'] <= load) or not (
                    first + size <= other['first_sector']
                    or other['first_sector'] + sectors(other['bytes']) <= first):
                return 'fields', None
        extents.append({'first_sector': first, 'bytes': length, 'load': load,
                        'kind': kind, 'sha256': digest.hex()})
    image = extents[0]
    if entry & 3 or entry < image['load'] or entry - image['load'] >= image['bytes']:
        return 'fields', None
    return 'valid', {'slot': slot, 'generation': generation, 'entry': entry, 'extents': extents}


def inspect(fd, card_sectors):
    """Select a slot exactly as the loader does, using image bytes for card reads."""
    slots = {}
    for slot, sector in HEADER_SECTORS.items():
        state, header = check_header(read_at(fd, sector, 1), slot, card_sectors)
        slots[slot] = {'header': state, 'generation': header['generation'] if header else 0,
                       'load': 'none', 'extent': 0, 'parsed': header}
    valid = [s for s in 'AB' if slots[s]['header'] == 'valid']
    # Newest generation first; equal generations try slot A first.
    order = sorted(valid, key=lambda s: (-slots[s]['generation'], s))
    selected = None
    for slot in order:
        result = slots[slot]
        result['load'] = 'done'
        for index, extent in enumerate(result['parsed']['extents']):
            data = read_at(fd, extent['first_sector'], sectors(extent['bytes']))[:extent['bytes']]
            if sha(data) != extent['sha256']:
                result['load'], result['extent'] = 'digest', index
                break
        if result['load'] == 'done':
            selected = slot
            break
    return {'selected': selected, 'slots': slots}


def public(report):
    return {'selected': report['selected'],
            'slots': {s: {k: v for k, v in r.items() if k != 'parsed'}
                      for s, r in report['slots'].items()}}


def mbr_partitions(sector0):
    """Return primary MBR ranges, or None when the sector has no MBR signature."""
    if sector0[510:512] != b'\x55\xaa':
        return None
    ranges = []
    for i in range(4):
        kind, first, count = struct.unpack_from('<4xB3xII', sector0, 446 + i * 16)
        if kind == 0xee:
            raise ValueError('GPT protective MBR: boot0 at sector 16 conflicts with GPT entries')
        if kind and count:
            ranges.append({'type': kind, 'first': first, 'blocks': count})
    return ranges


def overlaps(a_first, a_blocks, b_first, b_blocks):
    return a_first < b_first + b_blocks and b_first < a_first + a_blocks


def parse_extent(text):
    first, blocks = (int(v, 0) for v in text.split(':'))
    return {'first': first, 'blocks': blocks}


def check_allocation(allocation, card_sectors, sector0):
    if (allocation.get('schema') != 1 or allocation.get('kind') != ALLOCATION_KIND
            or allocation.get('owner') != 'SunstoneOS'
            or allocation.get('headers') != HEADER_SECTORS
            or allocation.get('card_sectors') != card_sectors):
        raise ValueError('Allocation does not describe this card image layout')
    if sha(sector0) != allocation.get('mbr_sha256'):
        raise ValueError('Partition sector changed since allocation')
    slots = allocation.get('slots', {})
    if set(slots) != {'A', 'B'}:
        raise ValueError('Allocation must assign exactly slots A and B')
    for name, s in slots.items():
        if (s['first'] % ALLOCATION_UNIT or s['blocks'] % ALLOCATION_UNIT or not s['blocks']
                or s['first'] < BOOT_AREA_SECTORS or s['first'] + s['blocks'] > card_sectors):
            raise ValueError('Slot %s is not whole allocation units beyond the boot area' % name)
    if overlaps(slots['A']['first'], slots['A']['blocks'], slots['B']['first'], slots['B']['blocks']):
        raise ValueError('Slots overlap')
    owned = list(slots.values())
    record = allocation.get('training_record')
    if record is not None:
        # The DRAM training record has one allocation unit of its own
        # (docs/designs/dram-training-restore.md).
        if (set(record) != {'first', 'blocks'} or record['first'] % ALLOCATION_UNIT
                or record['blocks'] != ALLOCATION_UNIT or record['first'] < BOOT_AREA_SECTORS
                or record['first'] + record['blocks'] > card_sectors):
            raise ValueError('The training record is not one allocation unit beyond the boot area')
        if any(overlaps(record['first'], record['blocks'], s['first'], s['blocks']) for s in slots.values()):
            raise ValueError('The training record overlaps a slot')
        owned.append(record)
    for p in mbr_partitions(sector0) or []:
        if overlaps(p['first'], p['blocks'], 0, BOOT_AREA_SECTORS) or any(
                overlaps(p['first'], p['blocks'], s['first'], s['blocks']) for s in owned):
            raise ValueError('Partition %#x at sector %d overlaps the boot area or a slot'
                             % (p['type'], p['first']))
    return slots


def new_directory(path):
    path = Path(path)
    os.mkdir(path, 0o700)
    return path


def write_new(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def allocate(args):
    fd, card_sectors = open_image(args.backup)
    try:
        sector0 = read_at(fd, 0, 1)
        digest = hashlib.sha256()
        for offset in range(0, card_sectors * SECTOR, 1 << 24):
            digest.update(os.pread(fd, 1 << 24, offset))
        for slot, sector in HEADER_SECTORS.items():
            raw = read_at(fd, sector, 1)
            if any(raw) and HEADER.unpack_from(raw)[0] != SLOT_MAGIC:
                raise ValueError('Header sector %d holds data that is not a slot header' % sector)
    finally:
        os.close(fd)
    allocation = {'schema': 1, 'kind': ALLOCATION_KIND, 'owner': 'SunstoneOS',
                  'card_sectors': card_sectors, 'headers': HEADER_SECTORS,
                  'slots': {'A': parse_extent(args.slot_a), 'B': parse_extent(args.slot_b)},
                  **({'training_record': parse_extent(args.training_record)} if args.training_record else {}),
                  'backup': {'bytes': card_sectors * SECTOR, 'sha256': digest.hexdigest()},
                  'mbr_sha256': sha(sector0),
                  'partition_table': 'mbr' if mbr_partitions(sector0) is not None else 'none'}
    check_allocation(allocation, card_sectors, sector0)
    write_new(args.output, (json.dumps(allocation, indent=2) + '\n').encode())
    return allocation


def address(text):
    value = int(text, 0)
    if not 0 <= value <= MASK64:
        raise ValueError('Address does not fit in 64 bits: ' + text)
    return value


def parse_load(text):
    path, load = text.rsplit('@', 1)
    return Path(path), address(load)


def container_writes(manifest_path):
    """Return the boot0 and monitor-package extents named by a container manifest."""
    manifest_path = Path(manifest_path)
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('kind') != 'a733-microsd-monitor-container' or not manifest_path.name.endswith('.manifest.json'):
        raise ValueError('Expected a prepare-microsd-boot.py manifest')
    prefix = manifest_path.name[:-len('manifest.json')]
    image = (manifest_path.parent / (prefix + 'microsd-monitor-container.bin')).read_bytes()
    identity = manifest['files']['microsd-monitor-container.bin']
    if len(image) != identity['bytes'] or sha(image) != identity['sha256']:
        raise ValueError('Container identity mismatch')
    writes = []
    for item in manifest['card_writes']:
        data = image[item['offset']:item['offset'] + item['bytes']]
        if item['offset'] % SECTOR or len(data) != item['bytes'] or sha(data) != item['sha256']:
            raise ValueError('Container extent mismatch: ' + item['name'])
        writes.append((item['name'], item['offset'] // SECTOR,
                       data + bytes(sectors(len(data)) * SECTOR - len(data))))
    return writes


def plan(args):
    allocation_raw = Path(args.allocation).read_bytes()
    allocation = json.loads(allocation_raw)
    fd, card_sectors = open_image(args.card_image)
    try:
        sector0 = read_at(fd, 0, 1)
        slots = check_allocation(allocation, card_sectors, sector0)
        current = inspect(fd, card_sectors)
        generation = max([r['generation'] for r in current['slots'].values()
                          if r['header'] == 'valid'] + [0]) + 1
        if generation > MASK64:
            raise ValueError('A valid header holds the last generation number')
        slot = 'B' if current['selected'] == 'A' else 'A'
        region = slots[slot]
        # A valid header from another tool may place its extents anywhere
        # beyond the boot area; writing such a region would destroy that slot.
        for name, result in current['slots'].items():
            if name == slot:
                continue
            for extent in (result['parsed'] or {'extents': []})['extents']:
                if overlaps(extent['first_sector'], sectors(extent['bytes']),
                            region['first'], region['blocks']):
                    raise ValueError('Valid slot %s header has an extent in the slot %s region'
                                     % (name, slot))
        loads = [(parse_load(args.image), IMAGE)] + [(parse_load(a), ASSET) for a in args.asset]
        if len(loads) > EXTENTS:
            raise ValueError('A slot holds one image and at most three assets')
        payload, extents, sector = bytearray(), [], region['first']
        for (path, load), kind in loads:
            data = Path(path).read_bytes()
            sector = region['first'] + (len(payload) // SECTOR + 7) // 8 * 8
            payload += bytes((sector - region['first']) * SECTOR - len(payload))
            payload += data + bytes(sectors(len(data)) * SECTOR - len(data))
            extents.append({'name': Path(path).name, 'first_sector': sector, 'bytes': len(data),
                            'load': load, 'kind': kind, 'sha256': sha(data)})
        if len(payload) > region['blocks'] * SECTOR:
            raise ValueError('Payload exceeds slot %s' % slot)
        header = pack_header(slot, generation, args.entry, extents)
        if check_header(header, slot, card_sectors)[0] != 'valid':
            raise ValueError('Slot contents violate the loader admission rules')
        # After an interrupted apply that wrote its header, the new slot is
        # selected and a fresh plan would overwrite the previous image.
        active = current['slots'][current['selected']]['parsed'] if current['selected'] else None
        same = lambda e: (e['bytes'], e['load'], e['kind'], e['sha256'])
        if active and active['entry'] == args.entry and \
                [same(e) for e in active['extents']] == [same(e) for e in extents]:
            raise ValueError('Selected slot %s already holds these contents; if an earlier plan '
                             'stopped, resume it with apply' % current['selected'])
        # The payload precedes its header; a new monitor follows a valid slot.
        writes = [('payload', region['first'], bytes(payload)),
                  ('header', HEADER_SECTORS[slot], header)]
        if args.container:
            writes += container_writes(args.container)
        records = []
        for name, first, data in writes:
            previous = read_at(fd, first, len(data) // SECTOR)
            records.append({'name': name, 'file': name + '.bin', 'first_sector': first,
                            'sectors': len(data) // SECTOR, 'sha256': sha(data),
                            'previous_sha256': sha(previous), 'unchanged': previous == data})
    finally:
        os.close(fd)
    for i, a in enumerate(records):
        for b in records[i + 1:]:
            if overlaps(a['first_sector'], a['sectors'], b['first_sector'], b['sectors']):
                raise ValueError('Planned writes overlap')
    output = new_directory(args.output)
    for (name, _, data), record in zip(writes, records):
        write_new(output / record['file'], data)
    result = {'schema': 1, 'kind': PLAN_KIND, 'card_sectors': card_sectors,
              'allocation_sha256': sha(allocation_raw), 'slot': slot, 'generation': generation,
              'entry': args.entry, 'extents': extents, 'before': public(current), 'writes': records}
    write_new(output / 'plan.json', (json.dumps(result, indent=2) + '\n').encode())
    return result


def apply(args):
    plan_path = Path(args.plan)
    plan_data = json.loads(plan_path.read_text())
    if plan_data.get('kind') != PLAN_KIND:
        raise ValueError('Not a boot-slot plan')
    payloads = []
    for record in plan_data['writes']:
        data = (plan_path.parent / record['file']).read_bytes()
        if sha(data) != record['sha256'] or len(data) != record['sectors'] * SECTOR:
            raise ValueError('Plan file identity mismatch: ' + record['file'])
        payloads.append(data)
    fd, card_sectors = open_image(args.card_image, writable=True)
    try:
        if card_sectors != plan_data['card_sectors']:
            raise ValueError('Card image size differs from the plan')
        # Compare every extent before the first write; a changed image stops here.
        for record, data in zip(plan_data['writes'], payloads):
            current = read_at(fd, record['first_sector'], record['sectors'])
            if current != data and sha(current) != record['previous_sha256']:
                raise ValueError('Card image changed since planning: ' + record['name'])
        written = []
        for record, data in zip(plan_data['writes'], payloads):
            if read_at(fd, record['first_sector'], record['sectors']) == data:
                continue
            if os.pwrite(fd, data, record['first_sector'] * SECTOR) != len(data):
                raise ValueError('Short card image write: ' + record['name'])
            os.fsync(fd)
            written.append(record['name'])
            if record['name'] == args.stop_after:
                break
    finally:
        os.close(fd)
    return written


def rollback(args):
    fd, card_sectors = open_image(args.card_image)
    try:
        current = inspect(fd, card_sectors)
        active = current['selected']
        if not active:
            raise ValueError('No slot is selected; restore the card backup instead')
        other = 'B' if active == 'A' else 'A'
        result = current['slots'][other]
        if result['header'] != 'valid':
            raise ValueError('Slot %s is not valid; restore the card backup instead' % other)
        for extent in result['parsed']['extents']:
            data = read_at(fd, extent['first_sector'], sectors(extent['bytes']))[:extent['bytes']]
            if sha(data) != extent['sha256']:
                raise ValueError('Slot %s payload differs from its header; restore the backup' % other)
        previous = read_at(fd, HEADER_SECTORS[active], 1)
    finally:
        os.close(fd)
    output = new_directory(args.output)
    name = 'invalidate-' + active
    write_new(output / (name + '.bin'), bytes(SECTOR))
    result = {'schema': 1, 'kind': PLAN_KIND, 'card_sectors': card_sectors,
              'slot': other, 'generation': result['generation'], 'before': public(current),
              'writes': [{'name': name, 'file': name + '.bin', 'first_sector': HEADER_SECTORS[active],
                          'sectors': 1, 'sha256': sha(bytes(SECTOR)),
                          'previous_sha256': sha(previous), 'unchanged': False}]}
    write_new(output / 'plan.json', (json.dumps(result, indent=2) + '\n').encode())
    return result


def verify(args):
    fd, card_sectors = open_image(args.card_image)
    try:
        return public(inspect(fd, card_sectors))
    finally:
        os.close(fd)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('allocate', help='Record slot ownership checked against a full card backup')
    p.add_argument('--backup', required=True)
    p.add_argument('--slot-a', required=True, metavar='FIRST:BLOCKS')
    p.add_argument('--slot-b', required=True, metavar='FIRST:BLOCKS')
    p.add_argument('--training-record', metavar='FIRST:BLOCKS',
                   help='one allocation unit for the DRAM training record')
    p.add_argument('--output', required=True)
    p = sub.add_parser('plan', help='Prepare the standby slot without writing the image')
    p.add_argument('--allocation', required=True)
    p.add_argument('--card-image', required=True)
    p.add_argument('--image', required=True, metavar='PATH@LOAD')
    p.add_argument('--entry', required=True, type=address)
    p.add_argument('--asset', action='append', default=[], metavar='PATH@LOAD')
    p.add_argument('--container', help='prepare-microsd-boot.py manifest to install after the slot')
    p.add_argument('--output', required=True, help='New plan directory')
    p = sub.add_parser('apply', help='Apply a plan to an ordinary card image file')
    p.add_argument('--plan', required=True)
    p.add_argument('--card-image', required=True)
    p.add_argument('--stop-after', help='Stop after this write, modeling an interrupted update')
    p = sub.add_parser('verify', help='Report slot states and the slot the loader selects')
    p.add_argument('--card-image', required=True)
    p = sub.add_parser('rollback', help='Plan invalidation of the active slot header')
    p.add_argument('--card-image', required=True)
    p.add_argument('--output', required=True, help='New plan directory')
    args = parser.parse_args(argv)
    try:
        result = {'allocate': allocate, 'plan': plan, 'apply': apply,
                  'verify': verify, 'rollback': rollback}[args.command](args)
    except (ValueError, OSError, KeyError, json.JSONDecodeError, struct.error) as error:
        parser.exit(1, 'boot-slots: %s\n' % error)
    print(json.dumps(result, indent=2))
    if args.command == 'verify' and not result['selected']:
        sys.exit(1)


if __name__ == '__main__':
    main()
