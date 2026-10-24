"""Validate a pre-provider checkpoint only after a new ROM instance is observed."""
import struct

PACKET_MAGIC = 0x3150434555504741
RECEIPT_MAGIC = 0x475055454e543031
ENTRY_MEMORY_READY = 1

def require(condition, message):
    if not condition:
        raise ValueError(message)

def validate_reports(monitor, platform, receipt, packet, prepared):
    require((len(monitor), len(platform), len(receipt), len(packet)) == (128, 16384, 104, 320),
            'Incomplete entry checkpoint reports.')
    m = struct.unpack('<16Q', monitor); h = struct.unpack_from('<16Q', platform)
    r = struct.unpack('<13Q', receipt)
    magic, version, size, epoch, generation, first, second, count, lines = struct.unpack_from('<QIIQQ4I', packet)
    words = struct.unpack_from('<64I', packet, 48); start, end = struct.unpack_from('<2Q', packet, 304)
    g = prepared['geometry']; clean_lines = (g['clean_end'] - g['clean_start']) // 64
    require(prepared['execution_profile'] == 'gpu-entry-memory-checkpoint-v1', 'Wrong entry checkpoint profile.')
    require(m[:3] == (0x4153373347535232, prepared['epoch'], prepared['generation']) and
            m[3:6] == (5, 0, ENTRY_MEMORY_READY) and m[7] == 1, 'No matched pre-provider checkpoint.')
    require((magic, version, size, epoch, generation, first, second, count) ==
            (PACKET_MAGIC, 1, 320, prepared['epoch'], prepared['generation'], 0, 0, 0) and not any(words),
            'Wrong checkpoint identity or nonzero GPU job result.')
    require(r[:6] == (RECEIPT_MAGIC, prepared['epoch'], prepared['generation'], prepared['lease'], ENTRY_MEMORY_READY, 1)
            and r[6:11] == (g['clean_start'], g['clean_end'], 0, 0, clean_lines) and lines == clean_lines,
            'Checkpoint publication ownership differs.')
    require(m[6] <= start <= end == m[8] < r[11] <= m[6] + 60 * 24000000, 'Checkpoint deadline differs.')
    require(h[:3] == (0x534b4552, 5, 0) and h[3] == r[12] and h[3] <= 16127 and h[4:8] == (0, 0, 0, 0)
            and m[6] <= h[8] <= h[9] == m[8], 'Checkpoint platform report differs.')
    require(platform[256+h[3]] == 0, 'Unterminated checkpoint log.')
    text = platform[256:256+h[3]].decode('ascii')
    marker = 'GPU_CHECKPOINT=ENTRY_MEMORY_READY; providers never constructed\n'
    require(text.count(marker) == 1 and text.endswith(marker), 'Missing checkpoint terminal marker.')
    return dict(normal_return=True, checkpoint='ENTRY_MEMORY_READY', gpu_jobs=0, gpu_functional_success=False,
                clean_lines=lines, cleanup_start=start, cleanup_end=end, log=text)
