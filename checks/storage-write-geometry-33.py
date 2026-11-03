#!/usr/bin/env python3
"""Run the creation of the region file (mode 0 of apps/storage-write-test) over the geometry of 3-3's card and check the writes against the plan.

The card state is the restore point of 3-3 (the 589 sectors captured by the read-only backup probe: MBR, boot area and FSInfo, the first 258
sectors of each FAT, the root and the next cluster), placed at their sector numbers in a sparse image of the partition. The application's own main.c
runs at 512 MiB with no guard against the host card model, which logs every write call. The check requires: the file whole, equal to the stream, its chain
exact and starting at the AU-aligned cluster the plan assumes; the FAT copies equal; every write call inside the plan's write ranges and every
metadata sector written backed up; the volume clean.

Usage: storage-write-geometry-33.py --output DIR [--backup FILE] [--plan plan-3-3.json]"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
SD = Path.home() / '.cache/sunstoneos/sd-backup'
PREFIX = b'sunstone-region-test-v1'
FULL = 512 * 1024 * 1024
PLAN = {'write_ranges': [[2049, 1], [2080, 258], [16992, 258], [31904, 64], [32768, 1048576]],
        'backup_ranges': [[0, 1], [2048, 8], [2080, 258], [16992, 258], [31904, 64]], 'data_ranges': [[32768, 1048576]]}


def stream(offset, n):
    base = hashlib.sha256(PREFIX)
    out = bytearray()
    for i in range(offset // 32, (offset + n + 31) // 32):
        h = base.copy(); h.update(i.to_bytes(8, 'little')); out += h.digest()
    return bytes(out[:n])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--backup', type=Path, default=SD / 'backup-3-3-restore-point.bin')
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    raw = a.backup.read_bytes()
    sectors = {struct.unpack_from('<Q', raw, i)[0]: raw[i + 8:i + 520] for i in range(0, len(raw), 520)}
    # the backup holds exactly the planned backup ranges
    assert set(sectors) == {s for f, n in PLAN['backup_ranges'] for s in range(f, f + n)}
    mbr, boot = sectors[0], sectors[2048]
    start = struct.unpack_from('<I', mbr, 446 + 8)[0]
    total = struct.unpack_from('<I', boot, 32)[0]
    rsvd, nf, fsz, spc = struct.unpack_from('<H', boot, 14)[0], boot[16], struct.unpack_from('<I', boot, 36)[0], boot[13]
    assert (start, total, rsvd, nf, fsz, spc) == (2048, 61065216, 32, 2, 14912, 32)
    fat1, fat2 = start + rsvd, start + rsvd + fsz
    data = fat2 + fsz
    assert (fat1, fat2, data) == (2080, 16992, 31904)
    img = out / 'card.img'
    with open(img, 'wb') as f:
        f.truncate((start + total) * 512)
        for s, b in sectors.items():
            f.seek(s * 512); f.write(b)
    binary = out / 'geometry'
    cc = [str(harness.CLANG), '-std=gnu11', '-O2', '-g', '-w', '-DFILE_BYTES=(512*MIB)', '-DGUARD_US=600000000000ull',
          f'-I{ROOT}/checks/storage-write-guard-stubs', f'-I{ROOT}/include', str(ROOT / 'checks/storage-write-guard.c'),
          str(ROOT / 'drivers/storage/fat32_rw.c'), '-o', str(binary)]
    r = subprocess.run(cc, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    log = out / 'write.log'
    res = out / 'result.bin'
    env = {**harness.environment(), 'WRITE_LOG': str(log), 'AU_SECTORS': '8192'}      # 3-3's allocation unit (SD Status, 4 MiB)
    r = subprocess.run([str(binary), str(img), str(start), str(total), '0', str(res), '0'], capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stdout + r.stderr
    sys.path.insert(0, str(ROOT / 'apps/storage-write-test'))
    import host
    d = host.decode(res.read_bytes())
    assert d['status'] == 0 and d['written_bytes'] == FULL and d['mismatches'] == 0, d
    ranges = [(f, f + c) for f, c in PLAN['write_ranges']]
    calls = [tuple(map(int, l.split())) for l in log.read_text().split('\n') if l]
    outside = [(s, n) for s, n in calls if not any(lo <= s and s + n <= hi for lo, hi in ranges)]
    assert calls and not outside, f'writes outside the plan: {outside[:5]}'
    # every metadata sector written is in the backup (the data range is exempt)
    data_lo, data_hi = PLAN['data_ranges'][0][0], PLAN['data_ranges'][0][0] + PLAN['data_ranges'][0][1]
    backed = {s for f, n in PLAN['backup_ranges'] for s in range(f, f + n)}
    meta = {s for first, n in calls for s in range(first, first + n) if not data_lo <= s < data_hi}
    assert meta <= backed, sorted(meta - backed)[:5]
    hi1 = max((s + n - 1 - fat1 for s, n in calls if fat1 <= s < fat1 + fsz), default=None)
    hi2 = max((s + n - 1 - fat2 for s, n in calls if fat2 <= s < fat2 + fsz), default=None)
    assert hi1 is not None and hi2 is not None
    with open(img, 'rb') as f:
        def sec(n):
            f.seek(n * 512); return f.read(512)
        info = sec(start + 1)
        free, nxt = struct.unpack_from('<II', info, 488)
        # the directory chain: the root cluster holds the test directory, whose cluster holds the file
        def entries(cluster_sector):
            blob = b''.join(sec(cluster_sector + i) for i in range(spc))
            return [blob[o:o + 32] for o in range(0, len(blob), 32)]
        root = entries(data)
        folder = next(e for e in root if e[:1] not in (b'\x00', b'\xe5') and e[11] == 0x10 and e[:11] != b'.          ')
        dir_cluster = struct.unpack_from('<H', folder, 20)[0] << 16 | struct.unpack_from('<H', folder, 26)[0]
        files = [e for e in entries(data + (dir_cluster - 2) * spc) if e[:1] not in (b'\x00', b'\xe5') and e[11] in (0x20, 0x00) and e[11] != 0x0f]
        entry = next(e for e in files if struct.unpack_from('<I', e, 28)[0] == FULL)
        first = struct.unpack_from('<H', entry, 20)[0] << 16 | struct.unpack_from('<H', entry, 26)[0]
        fat = b''.join(sec(fat1 + i) for i in range(hi1 + 1))
        c, chain = first, []
        while 2 <= c < 0x0ffffff8:
            chain.append(c); c = struct.unpack_from('<I', fat, 4 * c)[0] & 0x0fffffff
        copies_equal = all(sec(fat1 + i) == sec(fat2 + i) for i in range(hi1 + 1))
        first_sector = data + (first - 2) * spc
        assert len(chain) == FULL // (spc * 512) and chain == list(range(first, first + len(chain))), (len(chain), first)
        assert first_sector == 32768 and first_sector % 8192 == 0, first_sector                  # the AU-aligned start the plan assumes (cluster 29)
        assert c >= 0x0ffffff8 and copies_equal
        f.seek(first_sector * 512)
        for off in range(0, FULL, 1 << 20):
            assert f.read(1 << 20) == stream(off, 1 << 20), f'file differs at {off}'
        clean = bool(struct.unpack_from('<I', sec(fat1), 4)[0] & 0x08000000)
    assert clean
    print(f'PASS storage-write geometry on 3-3: {len(calls)} write calls, all inside the plan, every metadata sector backed up; file whole ({len(chain)} clusters, {first}..{chain[-1]}, '
          f'sector {first_sector}), stream equal, FAT copies equal, volume clean')
    print(f'  highest FAT sector written: copy 1 index {hi1}, copy 2 index {hi2} (planned indexes 0 to 257); last chained cluster {chain[-1]} is in FAT index {chain[-1] * 4 // 512}; '
          f'FSInfo after: free {free}, next-free hint {nxt}')


if __name__ == '__main__':
    main()
