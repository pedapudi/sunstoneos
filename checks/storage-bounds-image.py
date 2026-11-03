#!/usr/bin/env python3
"""The image's bounds object (drivers/storage/a733_storage_bounds.c) accepts exactly what tools/storage_bounds.py encodes."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import storage_bounds as bounds


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binary = out / 'bounds-image'
    r = subprocess.run([str(harness.CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *harness.SANITIZE,
                        '-I', str(ROOT / 'include'), str(ROOT / 'checks/storage-bounds-image.c'),
                        str(ROOT / 'drivers/storage/a733_storage_bounds.c'), str(ROOT / 'drivers/storage/storage_digest.c'),
                        '-o', str(binary)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    # A card image holding the backup sectors; the backup file's records are those sectors in order.
    card = out / 'card.img'
    # 100 backup ranges (the MBR, boot and FSInfo, the root cluster, then 97 single FAT sectors) and 400 write ranges:
    # far more than a fixed array would hold, since the image walks the delivered input in place.
    backup_ranges = sorted([(0, 1), (32768, 2), (62656, 64)] + [(40000 + 3 * i, 1) for i in range(97)])
    sectors = [s for first, count in backup_ranges for s in range(first, first + count)]
    content = {s: bytes((s * 7 + i) & 0xff for i in range(512)) for s in sectors}
    with open(card, 'wb') as f:
        f.truncate(70000 * 512)
        for s, data in content.items():
            f.seek(s * 512)
            f.write(data)
    backup = b''.join(struct.pack('<Q', s) + content[s] for s in sectors)
    digest = hashlib.sha256(backup).hexdigest()
    ranges = [(32768, 2), (62656, 64), (62720, 64), (65536, 4000)] + [(40000 + 3 * i, 1) for i in range(97)] + \
        [(100000 + 10 * i, 5) for i in range(300)]
    ranges.sort()
    blob = out / 'blob.bin'
    blob.write_bytes(bounds.encode_input(ranges, backup_ranges, digest, '0123456789abcdef0123456789abcdef'))
    r = subprocess.run([str(binary), str(blob), str(card)], capture_output=True, text=True, env=harness.environment())
    assert r.returncode == 0, r.stdout + r.stderr
    print(r.stdout.strip())


if __name__ == '__main__':
    main()
