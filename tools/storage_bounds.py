"""Admission of bounded card writes for the runner; no device access.

An image that defines the card write path (sunxi_mmc_write*, sunxi_mmc_erase*, fwr_*, the storage
update service) runs only when all of these hold:
- the submission carries --storage-write and --approved-by coordinator, on port 3-1;
- the hash-bound image descriptor holds `storage_bounds`: sorted, disjoint absolute sector ranges
  inside the FAT32 partition and the SHA-256 of the metadata backup;
- the first input of the run is the encoding of those bounds (encode_input), so the image sees the
  same ranges and hash that the runner admitted. The image reads the backed-up sectors again before
  its first write and refuses to write when their SHA-256 differs from the input's hash.
An image that defines none of those symbols (the read-only build) passes unchanged.
"""
import hashlib
import re
import struct

MAGIC = b'SSBW0001'
PARTITION_FLOOR = 32768            # first sector of the card's FAT32 partition
MAX_SECTORS = (1 << 21) + 4096     # 1 GiB of data plus metadata
MAX_RANGES = 64
WRITE_PATTERNS = (r'^sunxi_mmc_write', r'^sunxi_mmc_erase', r'^fwr_', r'^a733_storage_update_', r'^storage_update_')
_WRITE = re.compile('|'.join(WRITE_PATTERNS))


def require(ok, message):
    if not ok:
        raise ValueError(message)


def write_symbols(names):
    """The defined symbols that name the card write path."""
    return sorted(n for n in names if _WRITE.search(n))


def encode_input(ranges, backup_sha256):
    """MAGIC, count (u32), reserved (u32), backup SHA-256 (32 bytes), then (first sector, sectors) as u64 pairs."""
    digest = bytes.fromhex(backup_sha256)
    require(len(digest) == 32, 'The backup hash must be 64 hexadecimal digits.')
    return MAGIC + struct.pack('<II', len(ranges), 0) + digest + b''.join(struct.pack('<QQ', a, n) for a, n in ranges)


def validate(bounds):
    require(isinstance(bounds, dict) and set(bounds) == {'ranges', 'backup_sha256'}, 'storage_bounds needs exactly ranges and backup_sha256.')
    ranges = bounds['ranges']
    require(isinstance(ranges, list) and 1 <= len(ranges) <= MAX_RANGES, 'storage_bounds needs between 1 and 64 ranges.')
    previous_end, total = PARTITION_FLOOR, 0
    for item in ranges:
        require(isinstance(item, list) and len(item) == 2 and all(type(v) is int for v in item), 'A range is [first sector, sector count].')
        first, count = item
        require(count > 0 and first >= previous_end, 'Ranges must be sorted, disjoint, nonempty and at or after the partition start.')
        previous_end = first + count
        total += count
    require(total <= MAX_SECTORS, 'storage_bounds allows too many sectors.')
    require(isinstance(bounds['backup_sha256'], str) and re.fullmatch(r'[0-9a-f]{64}', bounds['backup_sha256']),
            'The backup hash must be 64 lowercase hexadecimal digits.')
    return [tuple(r) for r in ranges], bounds['backup_sha256']


def admit(descriptor, args, writers):
    """Raise ValueError unless a write-capable image or a write request is fully admitted."""
    requested = bool(descriptor.get('storage_write') or args.storage_write)
    require(requested, 'Image defines the card write path (' + ', '.join(writers[:3]) +
            '); it needs --storage-write, --approved-by coordinator and descriptor storage_bounds.')
    require(args.approved_by == 'coordinator', 'Storage write requires --approved-by coordinator.')
    require(args.storage_write, 'Storage write requires the --storage-write flag.')
    require(args.port == '3-1', 'Storage write runs on port 3-1 only.')
    require('storage_bounds' in descriptor, 'Storage write requires descriptor storage_bounds.')
    ranges, digest = validate(descriptor['storage_bounds'])
    require(args.input and args.input[0].read_bytes() == encode_input(ranges, digest),
            'The first input must be the encoding of storage_bounds.')
