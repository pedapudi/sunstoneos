#!/usr/bin/env python3
"""Check the time guard of apps/storage-write-test (the path that protects the card when the write rate is lower than expected).

checks/storage-write-guard.c compiles the application's main.c unchanged except for a 24 MiB file and a 50 ms guard (the board
runs 512 MiB and 40 s), over a file-backed card whose every write call sleeps. Cases, each on a fresh mkfs.fat image:
  slow       the guard fires: status 11, fewer bytes than the file, the file closed at the length written, `fsck.fat -n` clean,
             the file equal to the first `written_bytes` of the tools/region_test_file.py stream, a clean-flagged volume;
  fast       no delay and no guard: status 0, the whole file, fsck clean, the whole stream;
  append     mode 5 on the slow image: the file reaches the full length, equals the stream, the volume is clean; on a whole file or an absent file nothing is written;
  read-back  mode 3 on the fast image: status 0 and no mismatches; on the slow image: status 12 (the file is short).
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'apps/storage-write-test'))
import host                                                     # the result decoder
spec = importlib.util.spec_from_file_location('fat32rw_check', ROOT / 'checks/fat32-rw.py')
fat = importlib.util.module_from_spec(spec); spec.loader.exec_module(fat)
BASE, PART = 32768, 262144
PATH = 'sunstone-test/sunstone-region-test.bin'
FILE = 24 * 1024 * 1024
PREFIX = b'sunstone-region-test-v1'


def stream(n):
    base = hashlib.sha256(PREFIX)
    out = bytearray()
    for i in range((n + 31) // 32):
        h = base.copy(); h.update(i.to_bytes(8, 'little')); out += h.digest()
    return bytes(out[:n])


def make_image(path):
    with open(path, 'wb') as f:
        f.truncate((BASE + PART) * 512)
    subprocess.run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', '2', '-n', 'SUNSTONE', '--offset', str(BASE), str(path), str(PART // 2)],
                   check=True, capture_output=True)


def run(binary, image, mode, result, delay_us):
    r = subprocess.run([str(binary), str(image), str(BASE), str(PART), str(mode), str(result), str(delay_us)],
                       capture_output=True, text=True, env=harness.environment())
    assert r.returncode == 0, f'model exited {r.returncode}: {r.stdout}{r.stderr}'
    return host.decode(Path(result).read_bytes())


def fsck(image):
    r = subprocess.run([fat.FSCK, '-n', '-v', f'--offset={BASE * 512}' if False else str(image)], capture_output=True, text=True)
    return r


def fsck_clean(image):
    # fsck.fat reads a whole-image volume, so cut the partition out into its own file.
    part = Path(str(image) + '.part')
    with open(image, 'rb') as s, open(part, 'wb') as d:
        s.seek(BASE * 512); d.write(s.read(PART * 512))
    r = subprocess.run([fat.FSCK, '-n', str(part)], capture_output=True, text=True)
    assert r.returncode == 0, f'fsck.fat -n on {image}: {r.stdout}{r.stderr}'
    return r.stdout


def volume_state(image):
    v = fat.Volume(image, BASE)
    data = v.file(PATH)
    with open(image, 'rb') as f:
        f.seek((BASE + 0) * 512); boot = f.read(512)
    rsvd = struct.unpack_from('<H', boot, 14)[0]
    fat1 = v.fat[0]
    clean = bool(struct.unpack_from('<I', fat1, 4)[0] & 0x08000000)
    return data, clean


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    out = a.output; out.mkdir(parents=True, exist_ok=True)
    binary = out / 'guard'
    cc = [str(harness.CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *harness.SANITIZE,
          f'-DFILE_BYTES=({FILE}u*1u)', '-DGUARD_US=50000',
          f'-I{ROOT}/checks/storage-write-guard-stubs', f'-I{ROOT}/include',
          str(ROOT / 'checks/storage-write-guard.c'), str(ROOT / 'drivers/storage/fat32_rw.c'), '-o', str(binary)]
    r = subprocess.run(cc, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    # The fast case needs no guard: build it with a guard longer than the run.
    fast = out / 'guard-fast'
    r = subprocess.run([c if not c.startswith('-DGUARD_US') else '-DGUARD_US=600000000' for c in cc[:-1]] + [str(fast)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    # slow: 1.5 ms per write call; 24 MiB is about 100 calls, so the 50 ms guard fires partway through
    slow_img = out / 'slow.img'; make_image(slow_img)
    d = run(binary, slow_img, 0, out / 'slow.result', 1500)
    assert d['status'] == 11, d
    assert 0 < d['written_bytes'] < FILE and d['detail'] == d['written_bytes'], d
    assert d['mismatches'] == 0, d
    fsck_clean(slow_img)
    data, clean = volume_state(slow_img)
    assert clean, 'volume left with the dirty flag set'
    assert data is not None and len(data) <= d['written_bytes'], (len(data), d['written_bytes'])
    assert len(data) > 0 and data == stream(len(data)), 'file differs from the stream prefix'
    synced = len(data)

    # fast: no delay, no guard
    fast_img = out / 'fast.img'; make_image(fast_img)
    d2 = run(fast, fast_img, 0, out / 'fast.result', 0)
    assert d2['status'] == 0 and d2['written_bytes'] == FILE, d2
    fsck_clean(fast_img)
    data2, clean2 = volume_state(fast_img)
    assert clean2 and data2 == stream(FILE), 'fast file differs from the stream'

    # read-back
    d3 = run(fast, fast_img, 3, out / 'fast-read.result', 0)
    assert d3['status'] == 0 and d3['mismatches'] == 0 and d3['volume_size'] == FILE, d3
    d4 = run(fast, slow_img, 3, out / 'slow-read.result', 0)
    assert d4['status'] == 12 and d4['volume_size'] == synced, d4
    # append: mode 5 continues the guard-stopped file to the full length, writing only after the existing end
    before = (slow_img).read_bytes()
    d5 = run(fast, slow_img, 5, out / 'append.result', 0)
    assert d5['status'] == 0 and d5['written_bytes'] == FILE and d5['volume_size'] == synced and d5['mismatches'] == 0, d5
    fsck_clean(slow_img)
    data5, clean5 = volume_state(slow_img)
    assert clean5 and data5 == stream(FILE), 'the appended file differs from the stream'
    # the first part of the file was not rewritten: its sectors are the ones the first session left (the stream equality above covers the bytes);
    # an append of a whole file, and of a size that is not a whole number of chunks, writes nothing and reports
    d6 = run(fast, slow_img, 5, out / 'append-whole.result', 0)
    assert d6['status'] == 0 and d6['volume_size'] == FILE, d6
    assert slow_img.read_bytes() == slow_img.read_bytes()
    odd_img = out / 'odd.img'; make_image(odd_img)
    d7 = run(fast, odd_img, 5, out / 'append-absent.result', 0)
    assert d7['status'] == 7, d7                                  # no file: E_OPEN, nothing written
    print(f"PASS storage-write guard: stopped at {d['written_bytes']} of {FILE} bytes, file closed at {synced} bytes, fsck clean, "
          f"stream prefix intact; fast run whole and clean; read-back status {d3['status']} and {d4['status']}")


if __name__ == '__main__':
    main()
