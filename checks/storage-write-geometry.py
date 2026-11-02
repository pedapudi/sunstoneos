#!/usr/bin/env python3
"""Run the append (mode 5) of apps/storage-write-test over the real card's geometry and check the writes against the approved plan.

The card state is the one the first session left (sectors captured by the read-only backup probe after the v13 run: MBR, boot area, FSInfo, FAT copies
through index 159, the root and directory clusters), placed at their sector numbers in a sparse 62.5 GB image, with the file's first 368 MiB
(11776 clusters from cluster 47, sector 65536) filled from the stream. The application's own main.c runs at 512 MiB with no guard against the
host card model, which logs every write call. The check requires: the file whole, equal to the stream, its chain exact; the FAT copies equal;
every write call inside the plan's write ranges; and it prints the highest FAT sector written in each copy, the FSInfo next-free hint and free count.

Usage: storage-write-geometry.py --output DIR [--capture FILE] [--plan plan.json]"""
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
FIRST, SIZE0, FULL = 47, 385875968, 512 * 1024 * 1024


def stream(offset, n):
    base = hashlib.sha256(PREFIX)
    out = bytearray()
    for i in range(offset // 32, (offset + n + 31) // 32):
        h = base.copy(); h.update(i.to_bytes(8, 'little')); out += h.digest()
    return bytes(out[:n])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--capture', type=Path, default=Path.home() / '.cache/sunstoneos/build/storage-write-cmd25-1101/after-v13-457.bin')
    ap.add_argument('--plan', type=Path, default=SD / 'plan.json')
    a = ap.parse_args()
    out = a.output; out.mkdir(parents=True, exist_ok=True)
    plan = json.loads(a.plan.read_text())
    cap = a.capture.read_bytes()
    sectors = {struct.unpack_from('<Q', cap, i)[0]: cap[i + 8:i + 520] for i in range(0, len(cap), 520)}
    boot = sectors[32768]
    rsvd, nf, fsz, spc = struct.unpack_from('<H', boot, 14)[0], boot[16], struct.unpack_from('<I', boot, 36)[0], boot[13]
    total = struct.unpack_from('<I', boot, 32)[0]
    fat1 = 32768 + rsvd; fat2 = fat1 + fsz; data = fat2 + fsz
    img = out / 'card.img'
    with open(img, 'wb') as f:
        f.truncate((32768 + total) * 512)
        for s, b in sectors.items():
            f.seek(s * 512); f.write(b)
        # the first session's data: clusters 47.. from the stream
        f.seek((data + (FIRST - 2) * spc) * 512)
        for off in range(0, SIZE0, 1 << 20):
            f.write(stream(off, min(1 << 20, SIZE0 - off)))
    # the application at the real size with no guard, over the host card
    binary = out / 'geometry'
    cc = [str(harness.CLANG), '-std=gnu11', '-O2', '-g', '-w', '-DFILE_BYTES=(512*MIB)', '-DGUARD_US=600000000000ull',
          f'-I{ROOT}/checks/storage-write-guard-stubs', f'-I{ROOT}/include', str(ROOT / 'checks/storage-write-guard.c'),
          str(ROOT / 'drivers/storage/fat32_rw.c'), '-o', str(binary)]
    r = subprocess.run(cc, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    log = out / 'write.log'
    if log.exists():
        log.rename(out / 'write.log.old')
    res = out / 'result.bin'
    env = {**harness.environment(), 'WRITE_LOG': str(log)}
    r = subprocess.run([str(binary), str(img), '32768', str(total), '5', str(res), '0'], capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stdout + r.stderr
    sys.path.insert(0, str(ROOT / 'apps/storage-write-test'))
    import host
    d = host.decode(res.read_bytes())
    assert d['status'] == 0 and d['written_bytes'] == FULL and d['volume_size'] == SIZE0 and d['mismatches'] == 0, d
    # every write call inside the plan's write ranges
    ranges = [(f, f + c) for f, c in plan['write_ranges']]
    calls = [tuple(map(int, l.split())) for l in log.read_text().split('\n') if l]
    outside = [(s, n) for s, n in calls if not any(lo <= s and s + n <= hi for lo, hi in ranges)]
    assert calls and not outside, f'writes outside the plan: {outside[:5]}'
    hi1 = max((s + n - 1 - fat1 for s, n in calls if fat1 <= s < fat1 + fsz), default=None)
    hi2 = max((s + n - 1 - fat2 for s, n in calls if fat2 <= s < fat2 + fsz), default=None)
    assert hi1 is not None and hi2 is not None
    with open(img, 'rb') as f:
        def sec(n):
            f.seek(n * 512); return f.read(512)
        info = sec(32769)
        free, nxt = struct.unpack_from('<II', info, 488)
        # the chain from the directory entry
        root_dir = b''.join(sec(data + 1 * spc + i) for i in range(spc))
        entry = next(root_dir[o:o + 32] for o in range(0, len(root_dir), 32) if root_dir[o:o + 11] == b'SUNSTO~1BIN')
        first = struct.unpack_from('<H', entry, 20)[0] << 16 | struct.unpack_from('<H', entry, 26)[0]
        size = struct.unpack_from('<I', entry, 28)[0]
        fat = b''.join(sec(fat1 + i) for i in range(hi1 + 1))
        c, chain = first, []
        while 2 <= c < 0x0ffffff8:
            chain.append(c); c = struct.unpack_from('<I', fat, 4 * c)[0] & 0x0fffffff
        copies_equal = all(sec(fat1 + i) == sec(fat2 + i) for i in range(hi1 + 1))
        assert size == FULL and first == FIRST and len(chain) == FULL // (spc * 512) and chain == list(range(FIRST, FIRST + len(chain))), (size, first, len(chain))
        assert c >= 0x0ffffff8 and copies_equal
        last = chain[-1]
        # the data of the whole file
        f.seek((data + (FIRST - 2) * spc) * 512)
        for off in range(0, FULL, 1 << 20):
            assert f.read(1 << 20) == stream(off, 1 << 20), f'file differs at {off}'
        clean = bool(struct.unpack_from('<I', sec(fat1), 4)[0] & 0x08000000)
    assert clean
    data_lo = min(s for s, n in calls if s >= data), max(s + n for s, n in calls)
    print(f'PASS storage-write geometry: appended {FULL - SIZE0} bytes in {len(calls)} write calls, all inside the plan; file whole ({len(chain)} clusters, {FIRST}..{last}), stream equal, FAT copies equal, volume clean')
    print(f'  highest FAT sector written: copy 1 index {hi1}, copy 2 index {hi2} (approved indexes 0 to 128); last chained cluster {last} is in FAT index {last * 4 // 512}')
    print(f'  FSInfo after: free clusters {free} (before 1895659), next-free hint {nxt} (the cluster after the chain end is {last + 1}); data written sectors {data_lo[0]} to {data_lo[1] - 1} (approved data range 65536 to 1114111)')
    assert hi1 <= 128 and hi2 <= 128 and nxt <= last + 1, (hi1, hi2, nxt, last)


if __name__ == '__main__':
    main()
