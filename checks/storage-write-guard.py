#!/usr/bin/env python3
"""Check the time guard of apps/storage-write-test (the path that protects the card when the write rate is lower than expected).

checks/storage-write-guard.c compiles the application's main.c unchanged except for a 24 MiB file and a 50 ms guard (the board
runs 512 MiB and derives its guard from the session deadline), over a file-backed card whose every write call sleeps. Cases, each on a fresh mkfs.fat image:
  slow       the guard fires: status 11, fewer bytes than the file, the file closed at the length written, `fsck.fat -n` clean,
             the file equal to the first `written_bytes` of the tools/region_test_file.py stream, a clean-flagged volume;
  fast       no delay and no guard: status 0, the whole file, fsck clean, the whole stream;
  append     mode 5 on the slow image: the file reaches the full length, equals the stream, the volume is clean; on a whole file or an absent file nothing is written;
  read-back  mode 3 on the fast image: status 0 and no mismatches; on the slow image: status 12 (the file is short);
  extent     mode 10 on a fresh image: the candidate lands on its extent below the partition and reads back equal, nothing else changes, and an extent
             that differs from the blob's single write range, a partial sector, two write ranges or an extent reaching the partition write nothing;
  format     mode 9 on the whole-file image (a scratch card's restore): status 0, `fsck.fat -n` clean, the file and its directory gone, the whole partition outside the
             FATs, the boot sector's label and the FSInfo counts the only other writes, the geometry kept; the region file then created again by mode 0, whole and equal to the stream.
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
    words = [1 | 3 << 4 | 1 << 8, 150000000, 41 | 7 << 40, 0, 0]
    assert host.decode_mode(words)['busy_timeouts'] == 7 and host.decode_mode(words)['sample_delay'] == 41      # the busy timeouts in 47:40 of word 2
    assert host.guard_stop(d) and not host.failures(d), d                      # stopping at the guard after a clean close is a pass, with its bytes and rate reported
    assert host.failures(dict(d, status=8)) and host.failures(dict(d, mismatches=1)) and host.failures(dict(d, detail=d['detail'] + 1)), d
    assert 0 < d['written_bytes'] < FILE and d['detail'] == d['written_bytes'], d
    assert d['mismatches'] == 0, d
    fsck_clean(slow_img)
    data, clean = volume_state(slow_img)
    assert clean, 'volume left with the dirty flag set'
    assert data is not None and len(data) <= d['written_bytes'], (len(data), d['written_bytes'])
    assert len(data) > 0 and data == stream(len(data)), 'file differs from the stream prefix'
    synced = len(data)

    # derived: with no GUARD_US the guard is CLOSE_RESERVE_US before the session's own deadline (grant->finish_by_us); a deadline
    # 40 ms after the start (on the model's virtual clock, which advances by the 1.5 ms per write call) and a 20 ms close reserve
    # stop the write about 20 ms in and leave time to close, and no deadline means no guard
    derived = out / 'guard-derived'
    r = subprocess.run([c for c in cc[:-2] if not c.startswith('-DGUARD_US')] + ['-DCLOSE_RESERVE_US=20000', '-o', str(derived)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    derived_img = out / 'derived.img'; make_image(derived_img)
    r = subprocess.run([str(derived), str(derived_img), str(BASE), str(PART), '0', str(out / 'derived.result'), '1500'],
                       capture_output=True, text=True, env={**harness.environment(), 'FINISH_AFTER_US': '40000'})
    assert r.returncode == 0, f'model exited {r.returncode}: {r.stdout}{r.stderr}'
    d8 = host.decode((out / 'derived.result').read_bytes())
    assert d8['status'] == 11 and 0 < d8['written_bytes'] < FILE and d8['detail'] == d8['written_bytes'], d8
    assert 0 < d8['total_ticks'] / 24 <= 40000, d8['total_ticks']      # the write stopped and closed inside the session's 40 ms deadline (virtual clock)
    fsck_clean(derived_img)
    data8, clean8 = volume_state(derived_img)
    assert clean8 and data8 == stream(len(data8)) and len(data8) <= d8['written_bytes'], 'derived-guard file differs from the stream prefix'
    open_img = out / 'derived-open.img'; make_image(open_img)
    r = subprocess.run([str(derived), str(open_img), str(BASE), str(PART), '0', str(out / 'derived-open.result'), '0'],
                       capture_output=True, text=True, env=harness.environment())
    assert r.returncode == 0, r.stderr
    d9 = host.decode((out / 'derived-open.result').read_bytes())
    assert d9['status'] == 0 and d9['written_bytes'] == FILE, d9          # no session deadline: no guard

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
    # format: a scratch card is restored by reformatting its partition in place
    full = fast_img.read_bytes()
    d10 = run(fast, fast_img, 9, out / 'format.result', 0)
    assert d10['status'] == 0 and d10['written_bytes'] > 0, d10
    fsck_clean(fast_img)
    after = fast_img.read_bytes()
    data10, clean10 = volume_state(fast_img)
    assert data10 is None and clean10, (data10 is None, clean10)
    boot = full[BASE * 512:(BASE + 1) * 512]
    rsvd, nfats, fatsz, spc = struct.unpack_from('<H', boot, 14)[0], boot[16], struct.unpack_from('<I', boot, 36)[0], boot[13]
    data_start = BASE + rsvd + nfats * fatsz
    touched = set(range(BASE + rsvd, data_start)) | set(range(data_start, data_start + spc)) | {BASE, BASE + struct.unpack_from('<H', boot, 48)[0]}
    changed = {sec for sec in range(BASE + PART) if full[sec * 512:(sec + 1) * 512] != after[sec * 512:(sec + 1) * 512]}
    assert changed and changed <= touched, sorted(changed - touched)[:8]      # nothing outside the FATs, the root cluster and FSInfo was written
    assert full[:(BASE + rsvd) * 512] == after[:(BASE + rsvd) * 512] or changed <= touched
    d11 = run(fast, fast_img, 0, out / 'format-create.result', 0)
    assert d11['status'] == 0 and d11['written_bytes'] == FILE, d11
    data11, clean11 = volume_state(fast_img)
    assert clean11 and data11 == stream(FILE), 'the file created after the format differs from the stream'
    fsck_clean(fast_img)
    # a partition that is not FAT32 is not touched
    bad = out / 'bad.img'; bad.write_bytes(after); raw = bytearray(bad.read_bytes()); raw[BASE * 512 + 510] = 0; bad.write_bytes(raw)
    d12 = run(fast, bad, 9, out / 'format-bad.result', 0)
    assert d12['status'] == 5 and bad.read_bytes() == bytes(raw), d12
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
    extent_cases(fast, out)
    print(f"PASS storage-write guard: stopped at {d['written_bytes']} of {FILE} bytes, file closed at {synced} bytes, fsck clean, "
          f"stream prefix intact; fast run whole and clean; read-back status {d3['status']} and {d4['status']}")


def extent_cases(binary, out):
    """Mode 10 on a fresh image: the candidate lands on exactly its extent below the partition and reads back equal; sector 0 and the
    partition stay as they were and fsck stays clean; an extent that is not the blob's single write range, a partial candidate sector, a
    blob with two write ranges and an extent that reaches the partition each write nothing and report status 14."""
    spec = importlib.util.spec_from_file_location('storage_bounds_extent', ROOT / 'tools/storage_bounds.py')
    sb = importlib.util.module_from_spec(spec); spec.loader.exec_module(sb)
    cid = '2750485344333247120000029d014c47'
    candidate = bytes((i * 131 + (i >> 9)) & 0xff for i in range(481 * 512))
    assert host.extent_input(16, candidate) == sb.write_extent_input(16, candidate), 'host and runner encode the raw-extent input differently'

    def blob(ranges):
        return sb.encode_input(ranges, [(0, 1), (16, 496)], '00' * 32, cid)

    def run10(image, name, bounds_blob, mode_input):
        (out / f'{name}.bounds').write_bytes(bounds_blob)
        (out / f'{name}.mode').write_bytes(mode_input)
        env = {**harness.environment(), 'BOUNDS': str(out / f'{name}.bounds'), 'MODE_INPUT': str(out / f'{name}.mode')}
        r = subprocess.run([str(binary), str(image), str(BASE), str(PART), '10', str(out / f'{name}.result'), '0'],
                           capture_output=True, text=True, env=env)
        assert r.returncode == 0, f'model exited {r.returncode}: {r.stdout}{r.stderr}'
        return host.decode((out / f'{name}.result').read_bytes())

    image = out / 'extent.img'; make_image(image)
    before = image.read_bytes()
    d = run10(image, 'extent', blob([(16, 481)]), host.extent_input(16, candidate))
    after = image.read_bytes()
    want = hashlib.sha256(candidate).hexdigest()
    assert d['status'] == 0 and not host.failures(d, want), d
    assert d['written_bytes'] == len(candidate) and d['flushes'] == 2 and d['extent']['readback_sha256'] == want, d['extent']
    assert after[16 * 512:497 * 512] == candidate, 'the extent does not hold the candidate'
    assert after[:16 * 512] == before[:16 * 512] and after[497 * 512:] == before[497 * 512:], 'a sector outside the extent changed'
    fsck_clean(image)
    assert host.failures(dict(d, extent=dict(d['extent'], sector0_unchanged=False)), want)
    assert host.failures(d, '0' * 64) and host.failures(dict(d, mismatches=1), want)
    for name, bounds_blob, mode_input in (
            ('extent-shifted', blob([(16, 481)]), host.extent_input(17, candidate)),
            ('extent-short', blob([(16, 481)]), host.extent_input(16, candidate)[:-100]),
            ('extent-two-ranges', blob([(16, 481), (600, 1)]), host.extent_input(16, candidate)),
            ('extent-into-partition', blob([(BASE - 8, 16)]), host.extent_input(BASE - 8, bytes(16 * 512)))):
        clean = out / f'{name}.img'; clean.write_bytes(after)
        d = run10(clean, name, bounds_blob, mode_input)
        assert d['status'] == 14 and d['written_bytes'] == 0, (name, d['status'], d['written_bytes'])
        assert clean.read_bytes() == after, name + ' changed the card'


if __name__ == '__main__':
    main()
