"""Admission of bounded card writes for the runner; no device access.

Classification is default-deny. An image is
- read-only when it carries sunxi_mmc_read_only_build and passes tools/sd_read_only_scan.py;
- without an SD core when it has no sunxi_mmc_ symbol and no SMHC0 base address in its loadable
  sections (a literal word, or a MOVZ that loads it);
- write-capable in every other case, including an image that renamed its write functions.
Read-only images and images without an SD core pass unchanged. A write-capable image runs only when:
- the submission carries --storage-write;
- the descriptor and the record name the card (card_cid, its CID), a restore point is recorded for that card
  (restore_point below) on the submission's port, and the record's port is the submission's port; the image's enable
  refuses again when the card in the port reports another CID, so a blob made for one card cannot write another;
- an approval record written by the coordinator names the exact encoded bounds (see approval_path);
  workers never write the approvals directory, and a command-line flag is not an approval;
- the hash-bound image descriptor holds `storage_bounds` equal to the record's ranges and backup hash;
- every range lies inside the partition, from the card's own tables in the backup the record names: at or
  after the start of the MBR's FAT32 entry (type 0x0B or 0x0C) and below that start plus BPB_TotSec32 of the
  boot sector at the start; the backup must therefore hold sector 0 and that boot sector;
- the backup is explicit: the descriptor and the record list the backup ranges (sorted, disjoint, each inside the
  partition or exactly sector 0), the backup file holds exactly those sectors in increasing order, and every written
  sector is backed up unless the record's data_ranges exempt it; a data range must lie inside a write range and in the
  data area (after the boot sector's reserved sectors and FATs), so the boot, FSInfo and FAT sectors are never exempt;
- the first input of the run is the version 2 encoding of those bounds (encode_input), checked again after the
  runner has frozen its inputs (verify_delivered).
A boot-area write (descriptor storage_boot_area, admit_boot_area) is the one class outside the partition: one write range,
backed up with sector 0 and sectors 16..511, writing a candidate whose SHA-256 the record names. The record's direction
selects the range: install or restore, the card first stage's extent from its manifest (the first stage, or the extent's
bytes from an earlier backup); mbr, sector 0 alone, on a listed scratch card, with a candidate that keeps the boot code and
moves the start of the single FAT32 partition later while keeping its end; layout, an extent from sector 512 up to the
partition start, on a listed scratch card (docs/designs/autonomous-card-os.md has the card layout).
The record limits what one run writes. The image re-reads the backed-up sectors before its first
write and refuses to write when their SHA-256 differs from the input's hash.

Backup file: records of a u64 little-endian absolute sector followed by its 512 bytes, in increasing sector order;
its SHA-256 is the descriptor's and record's backup_sha256, and the image computes the same hash over the same
sectors, read in that order, before its first write.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct

MAGIC = b'SSBW0003'
SCRATCH_MAGIC = b'SSBW0004'
ROOT = Path(__file__).resolve().parents[1]
APPROVALS = Path(os.environ.get('SUNSTONE_STORAGE_APPROVALS') or Path.home() / '.cache/sunstoneos/coordinator-1031/approvals')
# The restore points: for each card (by CID) the first full capture of it, with the port it was captured on. A write is admitted for a card only when its
# restore point is recorded, and only on the port the capture was made on; the runner's enable refuses when the card in the port has another CID.
RESTORE_POINTS = Path(os.environ.get('SUNSTONE_RESTORE_POINTS') or Path.home() / '.cache/sunstoneos/coordinator-1031/restore-points.json')
# Scratch cards: cards the coordinator lists as holding nothing of value ({cid: {"port", "partition": [first sector, sector count]}}). A write to one is admitted on the card's
# CID and its FAT32 partition alone, by one approval record per card (storage-card-<cid>.json, {"card_cid", "port"}), with no backup, no restore point and no per-image record.
# The image's enable checks the CID and that every write lies inside the partition. A scratch card is restored by formatting its test partition, not by writing sectors back.
SCRATCH_CARDS = Path(os.environ.get('SUNSTONE_SCRATCH_CARDS') or Path.home() / '.cache/sunstoneos/coordinator-1031/scratch-cards.json')
ANY_IMAGE = 'any-image-from-tree'
BASE_SOURCE = ROOT / 'include/a733_mmc_platform.h'   # A733_MMC_BASE(slot): SMHC0 is slot 0


def require(ok, message):
    if not ok:
        raise ValueError(message)


def smhc0_base():
    """The SMHC0 register base, read from the platform header that defines the slot bases."""
    m = re.search(r'#define\s+A733_MMC_BASE\(slot\)\s*\(UINT32_C\((0x[0-9a-fA-F]+)\)', BASE_SOURCE.read_text())
    require(m is not None, 'Cannot derive the SMHC0 base from ' + str(BASE_SOURCE))
    return int(m.group(1), 16)


def _builds_base(word, base):
    """True when the word is a MOVZ (32- or 64-bit, any register) that loads exactly base: its lower
    half is zero and the upper half is the instruction's immediate shifted by 16. A MOVK of that half
    can belong to any constant, so it does not count."""
    require(base & 0xffff == 0, 'The SMHC0 base is expected to have a zero lower half.')
    return any(word & ~0x1f == opcode | 1 << 21 | (base >> 16) << 5 for opcode in (0x52800000, 0xd2800000))


# Symbols that hold the SMHC0 base only as an address-range entry and never drive the controller:
# the bus-master table `masters` and the SMP device-preparation admission function, which lists
# device ranges it admits. A hit inside any other symbol, or outside every symbol, counts.
RANGE_TABLE_SYMBOLS = frozenset({'masters', 'a733_smp_device_preparation_admitted'})


def has_base_literal(elf, base):
    """A loadable segment holds the base as a word or as a MOVZ that loads it, outside the range tables.
    A word in data that no symbol covers does not count: such blobs (model weights) hold any value."""
    table = elf.get_section_by_name('.symtab')
    spans = sorted((s['st_value'], s['st_value'] + s['st_size'], s.name, s['st_info']['type']) for s in table.iter_symbols()
                   if s['st_size'] and s['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT')) if table else []

    def owner(address):
        """The symbol covering the address; a data object over 4 KiB is a blob and counts as none."""
        for lo, hi, n, kind in spans:
            if lo <= address < hi:
                return None if kind == 'STT_OBJECT' and hi - lo > 4096 else n
        return None

    for segment in elf.iter_segments():
        if segment['p_type'] != 'PT_LOAD' or not segment['p_filesz']:
            continue
        data, origin, code = segment.data(), segment['p_vaddr'], bool(segment['p_flags'] & 1)
        for i in range(0, len(data) - 3, 4):
            word = struct.unpack_from('<I', data, i)[0]
            movz = code and _builds_base(word, base)
            if not (word == base or movz):
                continue
            name = owner(origin + i)
            # A MOVZ in code counts unless it sits in a range table. A literal word counts only inside a
            # named, non-table symbol: data that no symbol covers (tensor weights, graph blobs) holds any value.
            if name in RANGE_TABLE_SYMBOLS or (name is None and not movz):
                continue
            return True
    return False


def classify(image):
    """'read-only', 'no-core' or 'write-capable'."""
    from elftools.elf.elffile import ELFFile
    data = Path(image).read_bytes()
    if data[:4] != b'\x7fELF':
        return 'write-capable'                                # a raw binary cannot be inspected
    elf = ELFFile(io.BytesIO(data))
    table = elf.get_section_by_name('.symtab')
    names = {s.name for s in table.iter_symbols() if s['st_shndx'] != 'SHN_UNDEF'} if table else None
    if names is None:
        return 'write-capable'                                # without symbols nothing is proven
    scanner = _scan_module()
    if 'sunxi_mmc_read_only_build' in names and not scanner.scan(data):
        return 'read-only'
    if not any(n.startswith('sunxi_mmc_') for n in names) and not has_base_literal(elf, smhc0_base()):
        return 'no-core'
    return 'write-capable'


def _scan_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location('sd_read_only_scan', ROOT / 'tools/sd_read_only_scan.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def encode_input(ranges, backup_ranges, backup_sha256, card_cid):
    """Version 3: MAGIC, write range count (u32), backup range count (u32), backup SHA-256 (32 bytes), the write
    ranges, then the backup ranges, each (first sector, sector count) as little-endian u64 pairs, then the card's CID
    (16 bytes: the four response words, most significant first, each big-endian, as 32 hexadecimal digits in the report). The hash is over
    the backup records in sector order: u64 sector, then its 512 bytes. The image re-reads exactly the backup
    sectors in that order and refuses to write unless it computes the same hash."""
    digest = bytes.fromhex(backup_sha256)
    require(len(digest) == 32, 'The backup hash must be 64 hexadecimal digits.')
    require(isinstance(card_cid, str) and re.fullmatch(r'[0-9a-f]{32}', card_cid), 'The card identity must be 32 lowercase hexadecimal digits (the CID).')
    return (MAGIC + struct.pack('<II', len(ranges), len(backup_ranges)) + digest +
            b''.join(struct.pack('<QQ', a, n) for a, n in list(ranges) + list(backup_ranges)) + bytes.fromhex(card_cid))


def restore_points(path=None):
    path = Path(path) if path else RESTORE_POINTS
    return json.loads(path.read_text()) if path.is_file() else {}


def register_restore_point(card_cid, port, backup, backup_sha256, path=None, sectors=None):
    """Record the first full capture of a card as its restore point. A card has one: a second call for the same CID is refused,
    whatever it carries, so the restore point is never replaced."""
    require(re.fullmatch(r'[0-9a-f]{32}', card_cid), 'The card identity must be 32 lowercase hexadecimal digits (the CID).')
    require(port in ('3-1', '3-3'), 'Unknown port.')
    raw = Path(backup).read_bytes()
    require(hashlib.sha256(raw).hexdigest() == backup_sha256, 'The restore point file differs from its hash.')
    path = Path(path) if path else RESTORE_POINTS
    points = restore_points(path)
    require(card_cid not in points, 'This card already has a restore point; it is never replaced.')
    points[card_cid] = {'port': port, 'backup': str(Path(backup).resolve()), 'backup_sha256': backup_sha256, 'sectors': len(raw) // 520}
    path.write_text(json.dumps(points, indent=1, sort_keys=True) + '\n')


def restore_point(card_cid, port, path=None):
    """The recorded restore point of the card, which must have been captured on this port and whose file must still match its hash."""
    point = restore_points(path).get(card_cid)
    require(point is not None, f'No restore point is recorded for the card {card_cid}.')
    require(point.get('port') == port, f'The card {card_cid} was captured on port {point.get("port")}, not on port {port}.')
    require(Path(point.get('backup', '')).is_file() and hashlib.sha256(Path(point['backup']).read_bytes()).hexdigest() == point.get('backup_sha256'),
            'The restore point file is missing or differs from its recorded hash.')
    return point


def encode_scratch(partition, card_cid):
    """Version 4 (scratch card): MAGIC, 1, 0, 32 zero bytes, the partition as one (first sector, sector count) range, the card's CID."""
    require(re.fullmatch(r'[0-9a-f]{32}', card_cid), 'The card identity must be 32 lowercase hexadecimal digits (the CID).')
    first, count = partition
    require(type(first) is int and type(count) is int and first > 0 and count > 0, 'The partition must be a positive (first sector, sector count).')
    return SCRATCH_MAGIC + struct.pack('<II', 1, 0) + bytes(32) + struct.pack('<QQ', first, count) + bytes.fromhex(card_cid)


def scratch_cards(path=None):
    path = Path(path) if path else SCRATCH_CARDS
    return json.loads(path.read_text()) if path.is_file() else {}


def card_approval_path(card_cid, root=None):
    return (Path(root) if root else APPROVALS) / ('storage-card-' + card_cid + '.json')


def admit_scratch(descriptor, args, approvals=None, scratch=None):
    """Admission of a write to a scratch card: the card is listed, on this port, with this partition; the coordinator has one record for the card; the first input is the
    version 4 blob of the partition and the CID. Returns the blob."""
    s = descriptor.get('storage_scratch')
    require(isinstance(s, dict) and set(s) == {'card_cid', 'partition'}, 'storage_scratch needs card_cid and partition.')
    cid, partition = s['card_cid'], s['partition']
    require(isinstance(partition, list) and len(partition) == 2, 'The partition must be [first sector, sector count].')
    entry = scratch_cards(scratch).get(cid) or {}
    require(entry, f'The card {cid} is not listed as a scratch card.')
    require(entry.get('port') == args.port, f'The scratch card {cid} is on port {entry.get("port")}, not on port {args.port}.')
    require(entry.get('partition') == partition, 'The partition differs from the scratch card\'s listed partition.')
    blob = encode_scratch(tuple(partition), cid)
    require(args.input and Path(args.input[0]).read_bytes() == blob, 'The first input must be the encoding of storage_scratch.')
    record = card_approval_path(cid, approvals)
    require(record.is_file(), f'No coordinator approval record for the scratch card ({record.name}).')
    data = json.loads(record.read_text())
    require(data.get('card_cid') == cid and data.get('port') == args.port, 'The scratch card\'s record is for another card or port.')
    args.storage_admission = hashlib.sha256(blob).hexdigest()
    return blob


# The boot area below the first partition (docs/designs/card-fel-recovery.md#installing-the-first-stage-on-the-port-3-3-card):
# one write range, the first-stage extent from tools/card-fel-image.py's manifest, backed up with sector 0 and sectors 16..511.
BOOT_AREA_BACKUP = [[0, 1], [16, 496]]
BOOT_AREA_LIMIT = 2048
BOOT_AREA_KEYS = {'ranges', 'backup_ranges', 'backup_sha256', 'card_cid', 'candidate_sha256'}
FIRST_STAGE_SCHEMA = 'sunstone-card-fel-image-v1'
WRITE_EXTENT_MODE = 10      # apps/storage-write-test mode 10: the raw-extent write
EXTENT_HEADER = 512         # its mode input: u32 mode, u32 0, u64 first sector, zero to 512 bytes, then the candidate sectors


def extent_header(first):
    return struct.pack('<IIQ', WRITE_EXTENT_MODE, 0, first).ljust(EXTENT_HEADER, b'\0')


def write_extent_input(first, candidate):
    """The second input of an apps/storage-write-test mode 10 run: the header, then the candidate sectors."""
    require(len(candidate) and len(candidate) % 512 == 0, 'The candidate is not a whole number of sectors.')
    return extent_header(first) + bytes(candidate)


def mbr_partition_start(backup_sectors):
    """First sector of the MBR's single FAT32 partition (type 0x0B or 0x0C), from sector 0 alone."""
    mbr = backup_sectors.get(0)
    require(mbr is not None and mbr[510:512] == b'\x55\xaa', 'The backup lacks a valid sector 0 (MBR).')
    fat32 = [mbr[446 + 16 * i:462 + 16 * i] for i in range(4) if mbr[446 + 16 * i + 4] in (0x0b, 0x0c)]
    require(len(fat32) == 1, 'The MBR must hold exactly one FAT32 partition entry.')
    start = struct.unpack_from('<I', fat32[0], 8)[0]
    require(start > 0, 'The FAT32 partition starts at sector 0.')
    return start


def partition_extent(mbr):
    """(first sector, end sector) of the MBR's single FAT32 entry."""
    start = mbr_partition_start({0: mbr})
    entry = next(mbr[446 + 16 * i:462 + 16 * i] for i in range(4) if mbr[446 + 16 * i + 4] in (0x0b, 0x0c))
    return start, start + struct.unpack_from('<I', entry, 12)[0]


def mbr_move(current, candidate):
    """(new start, new end) of a candidate MBR that differs from the current one only in its FAT32 entry's start and size:
    the boot code, the disk signature, the other entries, the entry's status and type and the boot signature are kept."""
    require(len(candidate) == 512 and candidate[510:512] == b'\x55\xaa', 'The MBR candidate is not one sector with the boot signature.')
    entries = [i for i in range(4) if current[446 + 16 * i + 4] in (0x0b, 0x0c)]
    require(len(entries) == 1, 'The current MBR must hold exactly one FAT32 partition entry.')
    at = 446 + 16 * entries[0]
    require(candidate[:at + 8] == current[:at + 8] and candidate[at + 16:] == current[at + 16:],
            'The MBR candidate changes more than the FAT32 entry\'s start and size.')
    return partition_extent(candidate)


def extent_sha256(backup_sectors, first, count):
    """SHA-256 of the extent's sector bytes, concatenated in order, from a parsed backup."""
    require(all(s in backup_sectors for s in range(first, first + count)), 'The source backup does not hold the extent.')
    return hashlib.sha256(b''.join(backup_sectors[s] for s in range(first, first + count))).hexdigest()


def first_stage_extent(path, expected_sha256):
    """(first sector, sector count, first-stage SHA-256) from a first-stage manifest whose file hash is the approved one."""
    raw = Path(path).read_bytes()
    require(hashlib.sha256(raw).hexdigest() == expected_sha256, 'The first-stage manifest differs from the approved hash.')
    manifest = json.loads(raw)
    stage = manifest.get('first_stage') if isinstance(manifest, dict) else None
    require(manifest.get('schema') == FIRST_STAGE_SCHEMA and isinstance(stage, dict) and
            all(type(stage.get(k)) is int for k in ('first_sector', 'sectors', 'bytes')) and
            stage['bytes'] == 512 * stage['sectors'] and re.fullmatch(r'[0-9a-f]{64}', str(stage.get('sha256'))),
            'The first-stage manifest is not a card first-stage manifest.')
    return stage['first_sector'], stage['sectors'], stage['sha256']


def admit_boot_area(descriptor, args, image_sha256, approvals=None):
    """Admission of a raw write below the first partition, backed up with sector 0 and sectors 16..511, writing a
    candidate whose hash the coordinator's record names. The record's direction selects the extent: the first-stage
    extent for install (the first stage) or restore (the extent's bytes from an earlier backup); sector 0 for mbr; an
    extent from sector 512 below the partition for layout, the last two on a listed scratch card only. Inputs: the version 3 blob, then the mode input of
    apps/storage-write-test mode 10: its 512-byte header for the extent's first sector followed by the candidate
    sectors (write_extent_input). Returns the blob."""
    b = descriptor.get('storage_boot_area')
    require(isinstance(b, dict) and set(b) == BOOT_AREA_KEYS,
            'storage_boot_area needs ranges, backup_ranges, backup_sha256, card_cid and candidate_sha256.')
    ranges, backup_ranges = _pairs(b['ranges'], 'ranges'), _pairs(b['backup_ranges'], 'backup_ranges')
    require(len(ranges) == 1, 'A boot-area write has exactly one write range.')
    require(b['backup_ranges'] == BOOT_AREA_BACKUP, 'A boot-area write backs up exactly sector 0 and sectors 16..511.')
    for key in ('backup_sha256', 'candidate_sha256'):
        require(isinstance(b[key], str) and re.fullmatch(r'[0-9a-f]{64}', b[key]), 'Invalid storage_boot_area.')
    first, count = ranges[0]
    require(count > 0, 'The boot-area extent is empty.')
    require((first, count) == (0, 1) or 16 <= first and first + count <= 512 or first >= 512,
            'The boot-area extent lies outside the backed-up sectors 16..511 and is neither sector 0 alone nor from sector 512.')
    blob = encode_input(ranges, backup_ranges, b['backup_sha256'], b['card_cid'])
    require(args.input and Path(args.input[0]).read_bytes() == blob, 'The first input must be the encoding of storage_boot_area.')
    require(len(args.input) == 2, 'A boot-area write takes the bounds blob and the raw-extent mode input.')
    mode = Path(args.input[1]).read_bytes()
    require(mode[:EXTENT_HEADER] == extent_header(first), 'The second input must be the raw-extent mode for the extent\'s first sector.')
    candidate = mode[EXTENT_HEADER:]
    require(len(candidate) == 512 * count, 'The candidate is not the extent\'s length.')
    require(hashlib.sha256(candidate).hexdigest() == b['candidate_sha256'], 'The candidate differs from storage_boot_area.')
    record_path = approval_path(blob, approvals)
    require(record_path.is_file(), f'No coordinator approval record for these bounds ({record_path.name}).')
    record = json.loads(record_path.read_text())
    require(all(record.get(k) == b[k] for k in ('ranges', 'backup_ranges', 'backup_sha256', 'candidate_sha256')),
            'The approval record differs from storage_boot_area.')
    require(record.get('port') == args.port, 'The boot-area approval record is for another port.')
    require(record.get('card_cid') == b['card_cid'], 'The boot-area approval record is for another card.')
    restore_point(b['card_cid'], args.port)
    require(record.get('image') in (ANY_IMAGE, image_sha256), 'The approval record does not name this image.')
    sectors = parse_backup(record.get('backup', ''), b['backup_sha256'])
    listed = {x for f, n in backup_ranges for x in range(f, f + n)}
    require(listed == set(sectors), 'The backup file does not hold exactly the listed backup sectors.')
    start = mbr_partition_start(sectors)
    direction = record.get('direction')
    if direction in ('mbr', 'layout'):
        scratch_entry = scratch_cards().get(b['card_cid']) or {}
        require(scratch_entry.get('port') == args.port, 'An mbr or layout write needs the card listed as a scratch card on this port.')
    if direction == 'mbr':
        require((first, count) == (0, 1), 'An mbr write covers sector 0 alone.')
        new_start, new_end = mbr_move(sectors[0], candidate)
        old_start, old_end = partition_extent(sectors[0])
        require(old_start < new_start and new_end == old_end, 'The new MBR must move the partition start later and keep its end.')
        require(new_start >= 512, 'The new partition starts inside the backed-up sectors.')
    elif direction == 'layout':
        require(first >= 512 and first + count <= start, 'A layout extent lies from sector 512 up to the FAT32 partition start.')
    else:
        require(16 <= first and first + count <= 512, 'The boot-area extent lies outside the backed-up sectors 16..511.')
        require(first + count <= BOOT_AREA_LIMIT, 'The boot-area extent reaches sector 2048.')
        require(first + count <= start, 'The boot-area extent overlaps the FAT32 partition.')
    if direction in ('mbr', 'layout'):
        args.storage_admission = hashlib.sha256(blob).hexdigest()
        args.storage_candidate = b['candidate_sha256']
        return blob
    manifest_first, manifest_count, stage_sha256 = first_stage_extent(record.get('first_stage_manifest', ''),
                                                                     record.get('first_stage_manifest_sha256', ''))
    require((first, count) == (manifest_first, manifest_count), 'The write range differs from the first-stage extent.')
    if direction == 'install':
        require(b['candidate_sha256'] == stage_sha256, 'The install candidate is not the manifest\'s first stage.')
    else:
        require(direction == 'restore', 'The approval record names no direction (install or restore).')
        original = parse_backup(record.get('original_backup', ''), record.get('original_backup_sha256', ''))
        require(extent_sha256(original, first, count) == b['candidate_sha256'],
                'The restore candidate is not the extent\'s bytes in the original backup.')
    args.storage_admission = hashlib.sha256(blob).hexdigest()
    args.storage_candidate = b['candidate_sha256']
    return blob


def approval_path(blob, root=None):
    return (Path(root) if root else APPROVALS) / ('storage-' + hashlib.sha256(blob).hexdigest() + '.json')


def parse_backup(path, expected_sha256):
    """The backup file's sectors, which must be listed in increasing sector order; returns {sector: 512 bytes}."""
    raw = Path(path).read_bytes()
    require(hashlib.sha256(raw).hexdigest() == expected_sha256, 'The backup file differs from the approved hash.')
    require(len(raw) % 520 == 0 and raw, 'The backup file is not a list of sector records.')
    sectors, previous = {}, -1
    for o in range(0, len(raw), 520):
        sector = struct.unpack_from('<Q', raw, o)[0]
        require(sector > previous, 'The backup must list its sectors in increasing order, once each.')
        previous = sector
        sectors[sector] = raw[o + 8:o + 520]
    return sectors


def partition_bounds(backup_sectors):
    """(first sector, end sector) of the FAT32 partition: the start from the MBR's single FAT32 entry
    (type 0x0B or 0x0C) in sector 0, the size from BPB_TotSec32 of the boot sector at that start."""
    mbr = backup_sectors.get(0)
    require(mbr is not None and mbr[510:512] == b'\x55\xaa', 'The backup lacks a valid sector 0 (MBR).')
    entries = [mbr[446 + 16 * i:462 + 16 * i] for i in range(4)]
    fat32 = [e for e in entries if e[4] in (0x0b, 0x0c)]
    require(len(fat32) == 1, 'The MBR must hold exactly one FAT32 partition entry.')
    start = struct.unpack_from('<I', fat32[0], 8)[0]
    require(start > 0, 'The FAT32 partition starts at sector 0.')
    boot = backup_sectors.get(start)
    require(boot is not None and boot[510:512] == b'\x55\xaa', 'The backup lacks the partition boot sector.')
    total = struct.unpack_from('<I', boot, 32)[0]
    require(total > 0, 'The boot sector reports no sectors.')
    return start, start + total


def _pairs(items, what):
    require(isinstance(items, list) and all(isinstance(r, list) and len(r) == 2 and all(type(v) is int and v >= 0 for v in r) for r in items),
            f'{what} must be a list of [first sector, sector count].')
    return [tuple(r) for r in items]


def _sorted_disjoint(pairs, low, what):
    previous_end = low
    for first, count in pairs:
        require(count > 0 and first >= previous_end, f'{what} must be sorted, disjoint, nonempty and inside the partition.')
        previous_end = first + count
    return previous_end


def _covered(pairs, sector):
    return any(first <= sector < first + count for first, count in pairs)


def data_start(boot, start):
    """First sector of the data area from the boot sector: reserved sectors plus the FATs after the partition start."""
    reserved, fats = struct.unpack_from('<H', boot, 14)[0], boot[16]
    fat_size = struct.unpack_from('<I', boot, 36)[0]
    require(reserved > 0 and 1 <= fats <= 2 and fat_size > 0, 'The boot sector is not a FAT32 boot sector.')
    return start + reserved + fats * fat_size


def validate(ranges, backup_ranges, data_ranges, backup_sha256, sectors, start, end):
    """Check the write ranges, the backup ranges and the data exemption against the backup's own tables."""
    require(ranges, 'The approval needs at least one write range.')
    require(backup_ranges, 'The approval needs at least one backup range.')
    require(_sorted_disjoint(ranges, start, 'Write ranges') <= end, 'A range extends past the end of the partition.')
    # A backup range lies in the partition, or is exactly sector 0.
    require(all(c > 0 for _, c in backup_ranges), 'A backup range is empty.')
    for first, count in backup_ranges:
        require((first, count) == (0, 1) or (start <= first and first + count <= end), 'A backup range lies outside the partition.')
    require(all(backup_ranges[i][0] + backup_ranges[i][1] <= backup_ranges[i + 1][0] for i in range(len(backup_ranges) - 1)),
            'Backup ranges must be sorted and disjoint.')
    # The backup file holds exactly the listed sectors.
    listed = {x for first, count in backup_ranges for x in range(first, first + count)}
    require(listed == set(sectors), 'The backup file does not hold exactly the listed backup sectors.')
    require(isinstance(backup_sha256, str) and re.fullmatch(r'[0-9a-f]{64}', backup_sha256), 'The backup hash must be 64 lowercase hexadecimal digits.')
    # Data exemption: inside the data area, inside the write ranges; everything else that is written is backed up.
    boot = sectors[start]
    first_data = data_start(boot, start)
    require(_sorted_disjoint(data_ranges, start, 'Data ranges') <= end if data_ranges else True, 'Data ranges are outside the partition.')
    for first, count in data_ranges:
        require(first >= first_data, 'A data range lies in the boot, FSInfo or FAT area, which are metadata.')
        require(any(w0 <= first and first + count <= w0 + wn for w0, wn in ranges), 'A data range is not inside a write range.')
    for first, count in ranges:
        for sector in range(first, first + count):
            if not _covered(data_ranges, sector):
                require(_covered(backup_ranges, sector), f'Metadata sector {sector} is written but not backed up.')
    return ranges


def admit(descriptor, args, image_sha256, approvals=None):
    """Raise ValueError unless the write-capable image or write request is fully admitted; returns the input blob."""
    require(args.storage_write, 'Image can write the card (or a write was requested); it needs --storage-write, '
            'a coordinator approval record and descriptor storage_bounds.')
    require(args.port in ('3-1', '3-3'), 'Storage write runs on a known port only.')
    classes = [k for k in ('storage_bounds', 'storage_scratch', 'storage_boot_area') if k in descriptor]
    require(len(classes) <= 1, 'A descriptor names one of storage_bounds, storage_scratch and storage_boot_area.')
    if 'storage_scratch' in descriptor:
        return admit_scratch(descriptor, args, approvals)
    if 'storage_boot_area' in descriptor:
        return admit_boot_area(descriptor, args, image_sha256, approvals)
    bounds = descriptor.get('storage_bounds')
    require(isinstance(bounds, dict) and set(bounds) == {'ranges', 'backup_ranges', 'backup_sha256', 'card_cid'},
            'Storage write requires descriptor storage_bounds with ranges, backup_ranges, backup_sha256 and card_cid.')
    ranges, backup_ranges = _pairs(bounds['ranges'], 'ranges'), _pairs(bounds['backup_ranges'], 'backup_ranges')
    require(isinstance(bounds['backup_sha256'], str) and re.fullmatch(r'[0-9a-f]{64}', bounds['backup_sha256']), 'Invalid storage_bounds.')
    blob = encode_input(ranges, backup_ranges, bounds['backup_sha256'], bounds['card_cid'])
    require(args.input and Path(args.input[0]).read_bytes() == blob, 'The first input must be the encoding of storage_bounds.')
    record_path = approval_path(blob, approvals)
    require(record_path.is_file(), f'No coordinator approval record for these bounds ({record_path.name}).')
    record = json.loads(record_path.read_text())
    require(record.get('ranges') == bounds['ranges'] and record.get('backup_ranges') == bounds['backup_ranges'] and
            record.get('backup_sha256') == bounds['backup_sha256'], 'The approval record differs from storage_bounds.')
    require(record.get('port') == args.port, 'The approval record is for another port.')
    require(record.get('card_cid') == bounds['card_cid'], 'The approval record is for another card.')
    restore_point(bounds['card_cid'], args.port)
    require(record.get('image') in (ANY_IMAGE, image_sha256), 'The approval record does not name this image.')
    data_ranges = _pairs(record.get('data_ranges', []), 'data_ranges')
    sectors = parse_backup(record.get('backup', ''), bounds['backup_sha256'])
    start, end = partition_bounds(sectors)
    validate(ranges, backup_ranges, data_ranges, bounds['backup_sha256'], sectors, start, end)
    args.storage_admission = hashlib.sha256(blob).hexdigest()
    return blob


def verify_delivered(args):
    """After the runner froze its inputs: the first input is still the admitted blob."""
    admitted = getattr(args, 'storage_admission', None)
    if admitted is None:
        return
    require(args.input and hashlib.sha256(Path(args.input[0]).read_bytes()).hexdigest() == admitted,
            'The delivered first input differs from the admitted storage bounds.')
    candidate = getattr(args, 'storage_candidate', None)
    if candidate is not None:
        require(len(args.input) == 2 and
                hashlib.sha256(Path(args.input[1]).read_bytes()[EXTENT_HEADER:]).hexdigest() == candidate,
                'The delivered candidate differs from the admitted boot-area candidate.')
