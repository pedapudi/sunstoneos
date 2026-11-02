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
- the first input of the run is the encoding of those bounds (encode_input), checked again after the
  runner has frozen its inputs (verify_delivered).
The record limits what one run writes. The image re-reads the backed-up sectors before its first
write and refuses to write when their SHA-256 differs from the input's hash.

Backup file: records of a u64 little-endian absolute sector followed by its 512 bytes; its SHA-256 is
the record's backup_sha256.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct

MAGIC = b'SSBW0001'
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


def encode_input(ranges, backup_sha256):
    """MAGIC, count (u32), reserved (u32), backup SHA-256 (32 bytes), then (first sector, sectors) as u64 pairs."""
    digest = bytes.fromhex(backup_sha256)
    require(len(digest) == 32, 'The backup hash must be 64 hexadecimal digits.')
    return MAGIC + struct.pack('<II', len(ranges), 0) + digest + b''.join(struct.pack('<QQ', a, n) for a, n in ranges)


def approval_path(blob, root=None):
    return (Path(root) if root else APPROVALS) / ('storage-' + hashlib.sha256(blob).hexdigest() + '.json')


def parse_backup(path, expected_sha256):
    raw = Path(path).read_bytes()
    require(hashlib.sha256(raw).hexdigest() == expected_sha256, 'The backup file differs from the approved hash.')
    require(len(raw) % 520 == 0 and raw, 'The backup file is not a list of sector records.')
    sectors = {}
    for o in range(0, len(raw), 520):
        sector = struct.unpack_from('<Q', raw, o)[0]
        require(sector not in sectors, 'The backup lists a sector twice.')
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


def validate(ranges, backup_sha256, start, end):
    require(isinstance(ranges, list) and ranges, 'The approval needs at least one range.')
    previous_end = start
    for item in ranges:
        require(isinstance(item, list) and len(item) == 2 and all(type(v) is int for v in item), 'A range is [first sector, sector count].')
        first, count = item
        require(count > 0 and first >= previous_end, 'Ranges must be sorted, disjoint, nonempty and inside the partition.')
        previous_end = first + count
        require(previous_end <= end, 'A range extends past the end of the partition.')
    require(isinstance(backup_sha256, str) and re.fullmatch(r'[0-9a-f]{64}', backup_sha256), 'The backup hash must be 64 lowercase hexadecimal digits.')
    return [tuple(r) for r in ranges]


def admit(descriptor, args, image_sha256, approvals=None):
    """Raise ValueError unless the write-capable image or write request is fully admitted; returns the input blob."""
    require(args.storage_write, 'Image can write the card (or a write was requested); it needs --storage-write, '
            'a coordinator approval record and descriptor storage_bounds.')
    require(args.port == '3-1', 'Storage write runs on port 3-1 only.')
    bounds = descriptor.get('storage_bounds')
    require(isinstance(bounds, dict) and set(bounds) == {'ranges', 'backup_sha256'}, 'Storage write requires descriptor storage_bounds with ranges and backup_sha256.')
    require(isinstance(bounds['ranges'], list) and all(isinstance(r, list) and len(r) == 2 and all(type(v) is int and v >= 0 for v in r) for r in bounds['ranges']) and
            isinstance(bounds['backup_sha256'], str) and re.fullmatch(r'[0-9a-f]{64}', bounds['backup_sha256']), 'Invalid storage_bounds.')
    blob = encode_input([tuple(r) for r in bounds['ranges']], bounds['backup_sha256'])
    require(args.input and Path(args.input[0]).read_bytes() == blob, 'The first input must be the encoding of storage_bounds.')
    record_path = approval_path(blob, approvals)
    require(record_path.is_file(), f'No coordinator approval record for these bounds ({record_path.name}).')
    record = json.loads(record_path.read_text())
    require(record.get('ranges') == bounds['ranges'] and record.get('backup_sha256') == bounds['backup_sha256'],
            'The approval record differs from storage_bounds.')
    require(record.get('port') == args.port, 'The approval record is for another port.')
    require(record.get('image') in (ANY_IMAGE, image_sha256), 'The approval record does not name this image.')
    start, end = partition_bounds(parse_backup(record.get('backup', ''), bounds['backup_sha256']))
    validate(bounds['ranges'], bounds['backup_sha256'], start, end)
    args.storage_admission = hashlib.sha256(blob).hexdigest()
    return blob


def verify_delivered(args):
    """After the runner froze its inputs: the first input is still the admitted blob."""
    admitted = getattr(args, 'storage_admission', None)
    if admitted is None:
        return
    require(args.input and hashlib.sha256(Path(args.input[0]).read_bytes()).hexdigest() == admitted,
            'The delivered first input differs from the admitted storage bounds.')
