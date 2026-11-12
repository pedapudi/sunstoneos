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
  fresh      (fresh_format_cases) mode 9 with a new volume range inside a 1.1 GiB image's partition: a target outside the partition, or under the FAT32 cluster minimum,
             writes nothing; otherwise `fsck.fat -n` is clean at the volume's offset, the BPB equals `mkfs.fat -F 32 -s 32 -R 32 -a -h FIRST`, only the boot, FSInfo, backup
             boot, backup FSInfo, both FATs and the root cluster are written (the boot sector last) and nothing outside the volume changes; mode 0 then creates the file once
             sector 0 points at the volume. Mode 10 at sector 0 writes an MBR that moves the partition to the volume and mounts it there; an MBR pointing at an unformatted area
             fails at the mount (status 5), an MBR with no FAT32 entry at status 4, a sector 0 write the card drops at status 13, a write elsewhere that changes sector 0 at status 15;
  slot       (boot_slot_case) mode 10 with one boot slot of 40,960 sectors writes and reads back equal, and the manifest's mode region is the header plus those sectors;
  format     mode 9 on the whole-file image (a scratch card's restore): status 0, `fsck.fat -n` clean, the file and its directory gone, the whole partition outside the
             FATs, the boot sector's label and the FSInfo counts the only other writes, the geometry kept; the region file then created again by mode 0, whole and equal to the stream.
"""
import argparse
import hashlib
import importlib.util
import json
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


def make_image(path, base=BASE, part=PART):
    with open(path, 'wb') as f:
        f.truncate((base + part) * 512)
    subprocess.run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', '2', '-n', 'SUNSTONE', '--offset', str(base), str(path), str(part // 2)],
                   check=True, capture_output=True)


def run(binary, image, mode, result, delay_us):
    r = subprocess.run([str(binary), str(image), str(BASE), str(PART), str(mode), str(result), str(delay_us)],
                       capture_output=True, text=True, env=harness.environment())
    assert r.returncode == 0, f'model exited {r.returncode}: {r.stdout}{r.stderr}'
    return host.decode(Path(result).read_bytes())


def fsck(image):
    r = subprocess.run([fat.FSCK, '-n', '-v', f'--offset={BASE * 512}' if False else str(image)], capture_output=True, text=True)
    return r


def fsck_clean(image, base=BASE, sectors=PART):
    # fsck.fat reads a whole-image volume, so cut the partition out into its own file.
    part = Path(str(image) + '.part')
    with open(image, 'rb') as s, open(part, 'wb') as d:
        s.seek(base * 512)
        for _ in range(0, sectors, 16384):
            d.write(s.read(min(16384, sectors - _) * 512))
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
    fresh_format_cases(fast, out)
    boot_slot_case(fast, out)
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
    assert host.failures(dict(d, extent=dict(d['extent'], sector0_expected=False)), want)
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


FIRST, COUNT = 139264, 2_200_000          # the new volume, 32 sectors per cluster: 68,7xx clusters, just above the FAT32 minimum
END = FIRST + COUNT
OLD_BASE, OLD_PART = 2048, END - 2048     # the card's present partition, which ends where the new volume ends
SPEC = importlib.util.spec_from_file_location('storage_bounds_modes', ROOT / 'tools/storage_bounds.py')
SB = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(SB)
CID = '2750485344333247120000029d014c47'


def mbr(first, count, signature=True):
    sector = bytearray(512)
    sector[446 + 4] = 0x0c
    struct.pack_into('<II', sector, 446 + 8, first, count)
    if signature:
        sector[510:512] = b'\x55\xaa'
    return bytes(sector)


def run_model(binary, image, base, sectors, mode, name, out, mode_input=None, bounds=None, env=None, delay=0):
    """One run of the model; returns (decoded result, write calls as (sector, count), read calls). Every run gets its own logs."""
    logs = {k: out / f'{name}.{k}.log' for k in ('write', 'read')}
    for log in logs.values():
        log.unlink(missing_ok=True)
    extra = {'WRITE_LOG': str(logs['write']), 'READ_LOG': str(logs['read']), **(env or {})}
    if mode_input is not None:
        (out / f'{name}.mode').write_bytes(mode_input); extra['MODE_INPUT'] = str(out / f'{name}.mode')
    if bounds is not None:
        (out / f'{name}.bounds').write_bytes(bounds); extra['BOUNDS'] = str(out / f'{name}.bounds')
    r = subprocess.run([str(binary), str(image), str(base), str(sectors), str(mode), str(out / f'{name}.result'), str(delay)],
                       capture_output=True, text=True, env={**harness.environment(), **extra})
    assert r.returncode == 0, f'{name}: model exited {r.returncode}: {r.stdout}{r.stderr}'
    calls = lambda log: [tuple(int(x) for x in line.split()) for line in log.read_text().splitlines()] if log.exists() else []
    return host.decode((out / f'{name}.result').read_bytes()), calls(logs['write']), calls(logs['read'])


def sectors_of(calls):
    return {s + i for s, n in calls for i in range(n)}


def differing_sectors(a, b):
    """The sectors at which two equal-length card images differ."""
    changed = set()
    step = 1 << 14
    with open(a, 'rb') as fa, open(b, 'rb') as fb:
        sector = 0
        while True:
            x, y = fa.read(step * 512), fb.read(step * 512)
            if not x:
                return changed
            if x != y:
                changed |= {sector + i for i in range(len(x) // 512) if x[i * 512:(i + 1) * 512] != y[i * 512:(i + 1) * 512]}
            sector += step


def copy_image(src, dst):
    subprocess.run(['cp', '--sparse=always', str(src), str(dst)], check=True)


def fresh_format_cases(binary, out):
    """Mode 9 with a new volume range, and the mode 10 MBR write that makes it the partition."""
    work = out / 'fresh'; work.mkdir(exist_ok=True)
    pristine, image = work / 'pristine.img', work / 'fresh.img'
    make_image(pristine, OLD_BASE, OLD_PART)
    copy_image(pristine, image)

    # a target that is not wholly inside the MBR partition writes nothing and reports status 14
    for name, first, count in (('below', OLD_BASE - 1, COUNT), ('past-end', FIRST, COUNT + 1), ('no-count', FIRST, 0), ('huge', FIRST, 1 << 40), ('wrapped', (1 << 64) - 8, 16)):
        d, writes, _ = run_model(binary, image, OLD_BASE, OLD_PART, 9, 'refuse-' + name, work, host.format_input(first, count))
        assert d['status'] == 14 and d['detail'] == 1 and d['written_bytes'] == 0 and not writes, (name, d['status'], d['detail'], writes[:3])
    d, writes, _ = run_model(binary, image, OLD_BASE, OLD_PART, 9, 'refuse-small', work, host.format_input(FIRST, 1 << 20))      # 32,768 clusters: not FAT32
    assert d['status'] == 14 and d['detail'] == 2 and not writes, d
    assert not differing_sectors(pristine, image), 'a refused format changed the card'

    # the new volume: fsck clean at its offset, only the allowed sectors written, nothing outside it changed, the boot sector written last
    d, writes, _ = run_model(binary, image, OLD_BASE, OLD_PART, 9, 'format', work, host.format_input(FIRST, COUNT))
    assert d['status'] == 0 and d['written_bytes'] > 0 and not host.failures(d), d
    fatsz = -(-(COUNT - 32) // 4097)
    allowed = {FIRST, FIRST + 1, FIRST + 6, FIRST + 7} | set(range(FIRST + 32, FIRST + 32 + 2 * fatsz + 32))
    written = sectors_of(writes)
    assert written <= allowed and {FIRST, FIRST + 1, FIRST + 6, FIRST + 7} <= written, sorted(written - allowed)[:8]
    assert writes[-1] == (FIRST, 1) and all(w[0] != FIRST for w in writes[:-1]), 'the boot sector is not the last write'
    assert (FIRST + 6, 1) in writes and (FIRST + 7, 1) in writes and (FIRST + 1, 1) in writes
    changed = differing_sectors(pristine, image)
    assert changed and changed <= written, sorted(changed - written)[:8]
    assert not any(s < FIRST or s >= END for s in changed), 'a sector outside the new volume changed'
    fsck_clean(image, FIRST, COUNT)
    raw = image.read_bytes()
    sector = lambda n: raw[(FIRST + n) * 512:(FIRST + n + 1) * 512]
    assert sector(6) == sector(0) and sector(7) == sector(1) and sector(0)[510:512] == b'\x55\xaa'
    assert struct.unpack_from('<I', sector(0), 28)[0] == FIRST and struct.unpack_from('<I', sector(0), 32)[0] == COUNT
    # the boot sector's geometry is mkfs.fat -F 32 -s 32 -R 32 -h FIRST (BPB, extended fields and file system type; the jump, OEM name and boot code differ)
    reference = work / 'reference.img'
    with open(reference, 'wb') as f:
        f.truncate(COUNT * 512)
    subprocess.run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', '32', '-R', '32', '-a', '-h', str(FIRST), '-i', '57534f53', str(reference), str(COUNT // 2)],
                   check=True, capture_output=True)
    with open(reference, 'rb') as f:
        ref = f.read(512)
    assert ref[11:90] == sector(0)[11:90], (ref[11:90].hex(), sector(0)[11:90].hex())

    # mode 0 creates the region test file once sector 0 points at the new volume (the MBR write is simulated by patching sector 0)
    with open(image, 'r+b') as f:
        f.write(mbr(FIRST, COUNT))
    d, _, _ = run_model(binary, image, FIRST, COUNT, 0, 'create', work)
    assert d['status'] == 0 and d['written_bytes'] == FILE, d
    data = fat.Volume(image, FIRST).file(PATH)
    assert data == stream(FILE), 'the file on the new volume differs from the stream'
    fsck_clean(image, FIRST, COUNT)

    # mode 10 writes sector 0: an MBR that moves the partition to the new volume, then mounts it (the reads show the mount at FIRST and not at the old start)
    formatted = work / 'formatted.img'
    copy_image(pristine, formatted)
    d, _, _ = run_model(binary, formatted, OLD_BASE, OLD_PART, 9, 'format-again', work, host.format_input(FIRST, COUNT))
    assert d['status'] == 0, d
    candidate = mbr(FIRST, COUNT)
    bounds = SB.encode_input([(0, 1)], [(0, 1), (16, 496)], '00' * 32, CID)
    d, writes, reads = run_model(binary, formatted, OLD_BASE, OLD_PART, 10, 'mbr', work, host.extent_input(0, candidate), bounds)
    assert d['status'] == 0 and not host.failures(d, hashlib.sha256(candidate).hexdigest()) and d['extent']['sector0_expected'], d
    assert writes == [(0, 1)] and formatted.read_bytes()[:512] == candidate, 'the MBR write wrote more than sector 0, or not the candidate'
    assert (FIRST, 1) in reads and all(r[0] != OLD_BASE for r in reads), 'the mount did not use the new partition entry'
    # an MBR candidate that points at an unformatted area fails at the mount with status 5 (the detail is the mount's error), after the MBR is written
    stale = work / 'stale.img'; copy_image(pristine, stale)
    nowhere = mbr(1_500_000, 600_000)
    d, writes, _ = run_model(binary, stale, OLD_BASE, OLD_PART, 10, 'mbr-unformatted', work, host.extent_input(0, nowhere), bounds)
    assert d['status'] == 5 and d['detail'] != 0 and d['mismatches'] == 0 and host.failures(d), d
    assert writes == [(0, 1)] and stale.read_bytes()[:512] == nowhere
    # an MBR without a FAT32 entry fails with status 4
    d, _, _ = run_model(binary, stale, OLD_BASE, OLD_PART, 10, 'mbr-none', work, host.extent_input(0, bytes(510) + b'\x55\xaa'), bounds)
    assert d['status'] == 4 and host.failures(d), d
    # an extent at sector 0 whose read-back differs (the card keeps the old sector) fails on the mismatch and on sector 0
    kept = work / 'kept.img'; copy_image(pristine, kept)
    d, writes, _ = run_model(binary, kept, OLD_BASE, OLD_PART, 10, 'mbr-dropped', work, host.extent_input(0, candidate), bounds, env={'DROP_SECTOR0': '1'})
    assert d['status'] == 13 and d['mismatches'] > 0 and not d['extent']['sector0_expected'] and host.failures(d), d
    # an extent elsewhere must leave sector 0 as it was: a write that also changes sector 0 is status 15
    other = bytes(range(256)) * 2
    d, _, _ = run_model(binary, kept, OLD_BASE, OLD_PART, 10, 'sector0-clobbered', work, host.extent_input(16, other),
                        SB.encode_input([(16, 1)], [(0, 1), (16, 496)], '00' * 32, CID), env={'CLOBBER_SECTOR0': '1'})
    assert d['status'] == 15 and not d['extent']['sector0_expected'] and host.failures(d), d


def boot_slot_case(binary, out):
    """Mode 10 with one boot slot: 40,960 sectors (20 MiB) below the partition write and read back equal; the mode region of the manifest holds them."""
    work = out / 'slot'; work.mkdir(exist_ok=True)
    base, part = 65536, 262144
    image = work / 'slot.img'; make_image(image, base, part)
    candidate = bytes((i * 7 + (i >> 9) * 3) & 0xff for i in range(40960 * 512))
    first = 16
    mode_input = host.extent_input(first, candidate)
    manifest = json.loads((ROOT / 'apps/storage-write-test/manifest.json').read_text())
    region = next(r for r in manifest['regions'] if r['name'] == 'mode')
    assert region['bytes'] == len(mode_input) == 512 + 40960 * 512, (region['bytes'], len(mode_input))
    try:
        host.extent_input(first, candidate + bytes(512))
        raise AssertionError('a candidate longer than the mode region was accepted')
    except ValueError:
        pass
    before = image.read_bytes()
    d, writes, _ = run_model(binary, image, base, part, 10, 'slot', work, mode_input, SB.encode_input([(first, 40960)], [(0, 1), (16, 496)], '00' * 32, CID))
    want = hashlib.sha256(candidate).hexdigest()
    assert d['status'] == 0 and not host.failures(d, want) and d['written_bytes'] == len(candidate) and d['extent']['readback_sha256'] == want, d
    after = image.read_bytes()
    assert after[first * 512:(first + 40960) * 512] == candidate and after[:first * 512] == before[:first * 512] and after[(first + 40960) * 512:] == before[(first + 40960) * 512:]
    assert sectors_of(writes) == set(range(first, first + 40960))
    fsck_clean(image, base, part)


if __name__ == '__main__':
    main()
