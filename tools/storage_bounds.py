"""Admission of bounded card writes for the runner; no device access.

Classification is default-deny. An image is
- read-only when it carries sunxi_mmc_read_only_build and passes tools/sd_read_only_scan.py;
- without an SD core when it has no sunxi_mmc_ symbol and no SMHC0 base address in its loadable
  sections (a literal word, or a MOVZ that loads it);
- write-capable in every other case, including an image that renamed its write functions.
Read-only images and images without an SD core pass unchanged. A write-capable image runs only when:
- the submission carries --storage-write on port 3-1;
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

MAGIC = b'SSBW0002'
ROOT = Path(__file__).resolve().parents[1]
APPROVALS = Path(os.environ.get('SUNSTONE_STORAGE_APPROVALS') or Path.home() / '.cache/sunstoneos/coordinator-1031/approvals')
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


def encode_input(ranges, backup_ranges, backup_sha256):
    """Version 2: MAGIC, write range count (u32), backup range count (u32), backup SHA-256 (32 bytes), the write
    ranges, then the backup ranges, each (first sector, sector count) as little-endian u64 pairs. The hash is over
    the backup records in sector order: u64 sector, then its 512 bytes. The image re-reads exactly the backup
    sectors in that order and refuses to write unless it computes the same hash."""
    digest = bytes.fromhex(backup_sha256)
    require(len(digest) == 32, 'The backup hash must be 64 hexadecimal digits.')
    return (MAGIC + struct.pack('<II', len(ranges), len(backup_ranges)) + digest +
            b''.join(struct.pack('<QQ', a, n) for a, n in list(ranges) + list(backup_ranges)))


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
    require(args.port == '3-1', 'Storage write runs on port 3-1 only.')
    bounds = descriptor.get('storage_bounds')
    require(isinstance(bounds, dict) and set(bounds) == {'ranges', 'backup_ranges', 'backup_sha256'},
            'Storage write requires descriptor storage_bounds with ranges, backup_ranges and backup_sha256.')
    ranges, backup_ranges = _pairs(bounds['ranges'], 'ranges'), _pairs(bounds['backup_ranges'], 'backup_ranges')
    require(isinstance(bounds['backup_sha256'], str) and re.fullmatch(r'[0-9a-f]{64}', bounds['backup_sha256']), 'Invalid storage_bounds.')
    blob = encode_input(ranges, backup_ranges, bounds['backup_sha256'])
    require(args.input and Path(args.input[0]).read_bytes() == blob, 'The first input must be the encoding of storage_bounds.')
    record_path = approval_path(blob, approvals)
    require(record_path.is_file(), f'No coordinator approval record for these bounds ({record_path.name}).')
    record = json.loads(record_path.read_text())
    require(record.get('ranges') == bounds['ranges'] and record.get('backup_ranges') == bounds['backup_ranges'] and
            record.get('backup_sha256') == bounds['backup_sha256'], 'The approval record differs from storage_bounds.')
    require(record.get('port') == args.port, 'The approval record is for another port.')
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
