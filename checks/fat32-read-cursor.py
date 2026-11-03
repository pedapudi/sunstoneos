#!/usr/bin/env python3
"""Check that sequential small reads through the FAT32 service are linear in the file size.

checks/fat32-rw.c `cursor` writes a file, reads it in 1000-byte pieces twice (one fresh cursor per call, which is what `fwr_read` does, and one
cursor kept across the calls), then changes the file under the cursor (append, replace, unlink) and requires the cursor to follow. It prints the
work counters of the service: path resolutions and FAT links followed during a read. For files of 256 KiB and 1 MiB (512-byte
clusters, so 512 and 2048 clusters) this check requires:
  - the data to match the stream at every piece, behind the cursor, past the end, and after every change;
  - with a kept cursor, one resolution and at most one link per cluster boundary crossed (linear: quadrupling the size at most quadruples the work);
  - with a fresh cursor per call the work is quadratic, which the check shows (it grows by about 16) so that the counters are known to measure it.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
BASE, PART_SECTORS, AU = 32768, 73728, 128


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--library', type=Path, default=ROOT / 'drivers/storage/fat32_rw.c', help='service source (mutation checks pass a modified copy)')
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'host_sys.h').write_text('#define SS_SYS(name) host_##name\n')
    binary = out / 'fat32-rw'
    subprocess.run([str(harness.CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *harness.SANITIZE, '-I', str(ROOT / 'include'),
                    '-include', str(out / 'host_sys.h'), str(ROOT / 'checks/fat32-rw.c'), str(a.library),
                    str(ROOT / 'drivers/storage/sunstone_fs.c'), str(ROOT / 'drivers/storage/sunstone_fs_syscalls.c'), '-o', str(binary)], check=True)
    base = out / 'base.img'
    with open(base, 'wb') as f:
        f.truncate(BASE * 512 + PART_SECTORS * 512)
    subprocess.run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', '1', '-n', 'SUNSTONE', '--offset', str(BASE), str(base), str(PART_SECTORS // 2)],
                   check=True, capture_output=True)
    figures = {}
    for size in (256 << 10, 1 << 20):
        img = out / f'run-{size}.img'
        harness.copy_image(base, img)
        r = subprocess.run([str(binary), 'cursor', str(img), str(BASE), str(PART_SECTORS), str(AU), str(size)], capture_output=True, text=True,
                           env=harness.environment())
        assert r.returncode == 0 and 'bad=0' in r.stdout, f'cursor check failed ({size}): {r.stdout}{r.stderr}'
        figures[size] = {k: int(v) for k, v in (kv.split('=') for kv in r.stdout.split()[1:])}
        print(r.stdout.strip())
    small, big = figures[256 << 10], figures[1 << 20]
    for f in (small, big):
        clusters = f['size'] // 512
        assert f['cursor_resolves'] == 1, f
        assert f['cursor_steps'] <= clusters + 2, f                   # each cluster boundary crossed costs one link, once
        assert f['legacy_resolves'] == f['pieces'], f
    assert big['cursor_steps'] <= 4 * small['cursor_steps'] + 4, (small, big)    # linear
    assert big['legacy_steps'] >= 12 * small['legacy_steps'], (small, big)       # quadratic, the counters see it
    assert big['cursor_steps'] * 50 < big['legacy_steps'], big
    print('sequential small reads with a cursor are linear in the file size; the cursor follows appends, replacement and unlink')


if __name__ == '__main__':
    main()
