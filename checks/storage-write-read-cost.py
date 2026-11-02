#!/usr/bin/env python3
"""Count the card reads of the application's mode 2 verify over the real geometry, by kind, per fwr_read call.

The card state is the one the first session left (checks/storage-write-geometry.py: the sectors the read-only probe captured, the file's first 368 MiB
filled from the stream, cluster chain 47 to 11,822). apps/storage-write-test/main.c runs mode 2 against the host card model, whose device read logs
each call (first sector and count). A call is split into pieces of at most 256 sectors on the board, so the count of card commands is the sum of
ceil(count / 256) per call. Reads are classified by the region they fall in: the FAT copy (a FAT sector), the root and directory clusters (directory
sectors), the file's data, and the boot area. The output is the totals and the per-fwr_read-call figures for the 4 MiB steps of the board's verify."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('geometry', ROOT / 'checks/storage-write-geometry.py')
geometry = importlib.util.module_from_spec(spec); spec.loader.exec_module(geometry)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--capture', type=Path, default=Path.home() / '.cache/sunstoneos/build/storage-write-cmd25-1101/after-v13-457.bin')
    a = ap.parse_args()
    out = a.output; out.mkdir(parents=True, exist_ok=True)
    cap = a.capture.read_bytes()
    sectors = {struct.unpack_from('<Q', cap, i)[0]: cap[i + 8:i + 520] for i in range(0, len(cap), 520)}
    boot = sectors[32768]
    rsvd, fsz, spc = struct.unpack_from('<H', boot, 14)[0], struct.unpack_from('<I', boot, 36)[0], boot[13]
    total = struct.unpack_from('<I', boot, 32)[0]
    fat1 = 32768 + rsvd; fat2 = fat1 + fsz; data = fat2 + fsz
    img = out / 'card.img'
    with open(img, 'wb') as f:
        f.truncate((32768 + total) * 512)
        for s, b in sectors.items():
            f.seek(s * 512); f.write(b)
        f.seek((data + (geometry.FIRST - 2) * spc) * 512)
        for off in range(0, geometry.SIZE0, 1 << 20):
            f.write(geometry.stream(off, min(1 << 20, geometry.SIZE0 - off)))
    binary = out / 'readcost'
    r = subprocess.run([str(harness.CLANG), '-std=gnu11', '-O2', '-g', '-w', '-DFILE_BYTES=(512*MIB)', '-DGUARD_US=600000000000ull',
                        f'-I{ROOT}/checks/storage-write-guard-stubs', f'-I{ROOT}/include', str(ROOT / 'checks/storage-write-guard.c'),
                        str(ROOT / 'drivers/storage/fat32_rw.c'), '-o', str(binary)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    log = out / 'read.log'
    if log.exists():
        log.rename(out / 'read.log.old')
    env = {**harness.environment(), 'READ_LOG': str(log)}
    res = out / 'result.bin'
    r = subprocess.run([str(binary), str(img), '32768', str(total), '2', str(res), '0'], capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stdout + r.stderr
    sys.path.insert(0, str(ROOT / 'apps/storage-write-test'))
    import host
    d = host.decode(res.read_bytes())
    assert d['status'] == 0 and d['mismatches'] == 0 and d['volume_size'] == geometry.SIZE0, d
    calls = [tuple(map(int, l.split())) for l in log.read_text().split('\n') if l]
    first_data = next(i for i, (s, n) in enumerate(calls) if s >= 65536)
    def kind(s):
        if fat1 <= s < fat1 + fsz or fat2 <= s < fat2 + fsz: return 'FAT'
        if data <= s < data + 2 * spc: return 'directory'
        if s >= 65536: return 'data'
        return 'boot area'
    def tally(items):
        t = {}
        for s, n in items:
            k = kind(s); c = t.setdefault(k, {'device calls': 0, 'sectors': 0, 'card commands': 0, 'single-sector': 0, 'multi-block': 0})
            c['device calls'] += 1; c['sectors'] += n; c['card commands'] += (n + 255) // 256
            if n == 1: c['single-sector'] += 1
            else: c['multi-block'] += (n + 255) // 256
        return t
    before, during = calls[:first_data], calls[first_data:]
    reads = (geometry.SIZE0 + (4 << 20) - 1) // (4 << 20)
    print(f'mode 2 verify of {geometry.SIZE0} bytes in 4 MiB steps = {reads} fwr_read calls (the board\'s step; the model has a 16 MiB work region)')
    print('before the first data read (mount, size lookup, first directory scans):', tally(before))
    t = tally(during)
    print('from the first data read on:')
    for k, v in t.items():
        print(f'  {k:10s} {v["device calls"]:6d} device calls, {v["sectors"]:8d} sectors, {v["card commands"]:6d} card commands ({v["single-sector"]} single-sector, {v["multi-block"]} multi-block pieces)'
              f'  -> per fwr_read call: {v["card commands"] / reads:.1f} commands, {v["sectors"] / reads:.1f} sectors')
    total_cmds = sum(v['card commands'] for v in t.values())
    print(f'  total {total_cmds} card commands, {total_cmds / reads:.1f} per fwr_read call')


if __name__ == '__main__':
    main()
