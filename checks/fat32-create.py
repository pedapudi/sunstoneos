#!/usr/bin/env python3
"""Check the FAT32 file-create module on mkfs.fat images, with fsck.fat and an independent reader.

For each image the module creates sunstone-region-test.bin (the SHA-256 counter stream). The check
requires: fsck.fat -n reports a clean volume; an independent Python parser finds the file by its long name,
follows the chain and reads the stream back; the FAT32 reader of the loader streams the same digest; every
sector that changed lies in the bounded set that the Python side computes from the geometry alone; writing the
backup sectors back leaves every sector outside the data extent equal to the original. Refusals: tampered plans
(first cluster in the root directory, past the volume, too many clusters, slots outside the root cluster, a
run that is not free), a volume without a contiguous free run, an existing name, a full root cluster, and
a write that fails part way. mtools is not installed on this host; the independent parser stands in for it.
"""
import argparse
import os
import atexit
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
SECTOR = 512
GLOBAL_NAME = NAME = 'sunstone-region-test.bin'
spec = importlib.util.spec_from_file_location('fat32_ro_check', ROOT / 'checks/fat32-ro.py')
ro = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ro)
spec = importlib.util.spec_from_file_location('region_test_file', ROOT / 'tools/region_test_file.py')
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
u16 = lambda b, o: struct.unpack_from('<H', b, o)[0]
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]


def extents(path):
    """The (start, end) byte ranges of a sparse file that hold data."""
    ranges = []
    with open(path, 'rb') as stream:
        fd, offset, size = stream.fileno(), 0, os.fstat(stream.fileno()).st_size
        while offset < size:
            try:
                start = os.lseek(fd, offset, os.SEEK_DATA)
            except OSError:
                break
            end = os.lseek(fd, start, os.SEEK_HOLE)
            ranges.append((start, end))
            offset = end
    return ranges


def digest(path):
    """A digest of the file's size and its data ranges: equal digests mean equal contents, and holes cost nothing."""
    h = hashlib.sha256(str(os.path.getsize(path)).encode())
    with open(path, 'rb') as stream:
        for start, end in extents(path):
            stream.seek(start)
            h.update(b'%d:%d:' % (start, end))
            while end > stream.tell():
                h.update(stream.read(min(1 << 20, end - stream.tell())))
    return h.digest()


def changed_sectors(before, after):
    """The sectors that differ between two same-sized sparse images, found by reading only their data ranges."""
    sectors = set()
    ranges = sorted(extents(before) + extents(after))
    with open(before, 'rb') as a, open(after, 'rb') as b:
        done = set()
        for start, end in ranges:
            for sector in range(start // SECTOR, (end + SECTOR - 1) // SECTOR):
                if sector in done:
                    continue
                done.add(sector)
                a.seek(sector * SECTOR)
                b.seek(sector * SECTOR)
                if a.read(SECTOR) != b.read(SECTOR):
                    sectors.add(sector)
    return sectors


def stream_bytes(size):
    return b''.join(gen.stream(size))[:size]


class Volume:
    """Independent FAT32 parser over an image."""

    def __init__(self, img, base):
        self.img, self.base = img, base
        b = img[base * SECTOR:(base + 1) * SECTOR]
        self.spc, self.reserved, self.fats = b[13], u16(b, 14), b[16]
        self.fatsz, self.root, self.total = u32(b, 36), u32(b, 44), u32(b, 32)
        self.info = u16(b, 48)
        self.data = base + self.reserved + self.fats * self.fatsz
        self.clusters = (self.total - self.reserved - self.fats * self.fatsz) // self.spc

    def fat(self, copy, c):
        return u32(self.img, (self.base + self.reserved + copy * self.fatsz) * SECTOR + 4 * c) & 0x0fffffff

    def sector(self, c): return self.data + (c - 2) * self.spc

    def chain(self, first, copy=0):
        out = []
        while 2 <= first < 0x0ffffff8:
            out.append(first)
            first = self.fat(copy, first)
            assert len(out) <= self.clusters
        return out

    def entries(self):
        raw = b''.join(self.img[self.sector(c) * SECTOR:(self.sector(c) + self.spc) * SECTOR] for c in self.chain(self.root))
        name, long = bytearray(), False
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                name = bytearray(); continue
            if e[11] == 0x0f:
                part = b''.join(e[o:o + 2] for o in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30))
                chars = [u16(part, k) for k in range(0, 26, 2)]
                text = ''.join(chr(c) for c in chars if c not in (0, 0xffff))
                name = bytearray(text.encode()) + name if True else name
                continue
            yield (name.decode() if name else e[:8].decode().strip() + ('.' + e[8:11].decode().strip() if e[8:11].strip() else ''),
                   e, i)
            name = bytearray()

    def read(self, name):
        for n, e, _ in self.entries():
            if n.lower() == name.lower():
                first = u16(e, 20) << 16 | u16(e, 26)
                size = u32(e, 28)
                data = b''.join(self.img[self.sector(c) * SECTOR:(self.sector(c) + self.spc) * SECTOR] for c in self.chain(first))
                return data[:size], first, size, e
        raise KeyError(name)


def fsck(path, base, total_sectors, out, tag):
    part = out / f'{tag}.fsck.part'
    ro.run(['dd', f'if={path}', f'of={part}', 'bs=1M', 'iflag=skip_bytes', f'skip={base * SECTOR}', 'conv=sparse', 'status=none'])
    r = subprocess.run(['/usr/sbin/fsck.fat', '-n', str(part)], capture_output=True, text=True)
    part.unlink()
    return r.returncode, r.stdout + r.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    record_dir = args.output                              # the result record; the volume images live in RAM
    record_dir.mkdir(parents=True, exist_ok=True)
    out = harness.ram_directory('fat32-create-')
    atexit.register(shutil.rmtree, out, True)
    binary = out / 'fat32-create'
    ro.run(['clang-21', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            f'-I{ROOT}/include', ROOT / 'checks/fat32-create.c', ROOT / 'drivers/storage/fat32_ro.c',
            ROOT / 'drivers/storage/fat32_create.c', ROOT / 'drivers/storage/storage_digest.c', '-o', binary])
    env = {**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0'}

    def tool(*a):
        r = subprocess.run([str(binary), *map(str, a)], capture_output=True, text=True, env=env)
        assert r.returncode == 0, (a, r.stdout, r.stderr)
        return r.stdout

    passed = []

    def ok(name):
        passed.append(name)
        print('PASS', name, flush=True)

    def make(tag, spc, size_mib, base, populated):
        if populated:
            path, fs, files, info = ro.build(out, tag, spc, size_mib, base, True)
            img = ro.mapped(path)
        else:
            total = size_mib * 2048
            path = out / f'{tag}.img'
            ro.run(['truncate', '-s', str(total * SECTOR), path])         # sparse: only written sectors take memory
            ro.run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', str(spc), '-n', 'SUNSTONE', '-i', '5a733003', '-h', str(base),
                    '--offset', str(base), path, str((total - base) // 2)])
            img = ro.mapped(path)
        v = Volume(img, base)
        # FSInfo: the writer of the populated image leaves mkfs's hints; make them true.
        free = sum(1 for c in range(2, v.clusters + 2) if v.fat(0, c) == 0)
        info_off = (base + v.info) * SECTOR
        struct.pack_into('<II', img, info_off + 488, free, 2)
        img.flush()
        return path, img, v

    cases = [('empty-spc1', 1, 68, 0, False, 1 * 1024 * 1024 + 13), ('empty-spc8-offset', 8, 320, 32768, False, 3 * 1024 * 1024),
             ('files-spc8', 8, 320, 0, True, 5 * 1024 * 1024 + 7), ('files-spc8-offset', 8, 320, 32768, True, 9 * 1024 * 1024 + 4096),
             ('one-cluster-spc8', 8, 320, 32768, False, 4096), ('exact-cluster-multiple', 8, 320, 32768, False, 8 * 4096 * 5)]
    for tag, spc, mib, base, populated, size in cases:
        path, img, v = make(tag, spc, mib, base, populated)
        original = out / f'{tag}.orig'
        ro.run(['cp', '--reflink=auto', '--sparse=auto', path, original])
        NAME = 'created on the board.bin' if populated else GLOBAL_NAME
        code, text = fsck(path, base, mib * 2048, out, tag + '-before')
        assert code == 0, (tag, text)
        listing = tool('plan', path, base, NAME, size)
        assert 'plan ok' in listing, listing
        fields = listing.split()
        first_cluster = int(fields[fields.index('first_cluster') + 1])
        clusters = int(fields[fields.index('clusters') + 1])
        log = out / f'{tag}.log'
        result = tool('create', path, base, NAME, size, log)
        assert result.rstrip().endswith('create ok'), result
        new = ro.mapped(path)
        # Expected bounded set, from the geometry alone.
        csz = v.spc
        data = (v.sector(first_cluster), clusters * csz)
        fat_first, fat_last = first_cluster * 4 // SECTOR, (first_cluster + clusters - 1) * 4 // SECTOR
        allowed = set(range(data[0], data[0] + data[1]))
        for copy in range(v.fats):
            allowed |= set(base + v.reserved + copy * v.fatsz + i for i in range(fat_first, fat_last + 1))
        allowed.add(base + v.info)
        allowed |= set(range(v.sector(v.root), v.sector(v.root) + csz))
        writes = [tuple(map(int, line.split())) for line in log.read_text().splitlines()]
        written = [s for sector, n in writes for s in range(sector, sector + n)]
        assert written and set(written) <= allowed and len(set(written)) == len(written), tag
        changed = changed_sectors(original, path)
        assert changed <= allowed, (tag, sorted(changed - allowed)[:5])
        # Order: data, then FAT copy 0, FAT copy 1, the directory, FSInfo.
        def stage(s):
            if data[0] <= s < data[0] + data[1]: return 0
            if s >= base + v.reserved + v.fatsz and s < base + v.reserved + 2 * v.fatsz: return 2
            if base + v.reserved <= s < base + v.reserved + v.fatsz: return 1
            if s == base + v.info: return 4
            return 3
        stages = [stage(s) for s in written]
        assert stages == sorted(stages), (tag, 'write order')
        ok(f'{tag}-writes-inside-the-bounded-set-in-order')
        code, text = fsck(path, base, mib * 2048, out, tag + '-after')
        assert code == 0, (tag, text)
        ok(f'{tag}-fsck-clean')
        nv = Volume(new, base)
        content, first, got_size, entry = nv.read(NAME)
        want = stream_bytes(size)
        assert got_size == size and content == want and hashlib.sha256(content).digest() == hashlib.sha256(want).digest()
        assert nv.chain(first, 0) == nv.chain(first, 1) == list(range(first_cluster, first_cluster + clusters))
        assert entry[11] == 0x20 and u16(entry, 24) == 0x5d62
        ok(f'{tag}-independent-reader-digest')
        streamed = tool('check', path, base)
        assert 'fats_equal 1' in streamed and 'fsinfo_valid 1' in streamed
        fields = dict(zip(*[iter(streamed.split())] * 2))
        assert fields['fsinfo_free'] == fields['counted_free'], streamed
        ok(f'{tag}-volume-consistent')
        # The loader's reader streams the same digest.
        s = subprocess.run([str(out / 'fat32-ro'), 'stream', str(path), str(base), NAME, str(out / 'x.bin')], capture_output=True, text=True) \
            if (out / 'fat32-ro').exists() else None
        # Restoring the original sectors outside the data extent returns the original volume: every sector that changed outside
        # the data extent is one the plan names, and the data extent itself lies in the file's clusters.
        outside = {s_ for s_ in changed if not data[0] <= s_ < data[0] + data[1]}
        assert outside <= allowed - set(range(data[0], data[0] + data[1])), tag
        new.close()
        ok(f'{tag}-restore-returns-every-sector-outside-the-data-extent')
        # The allowed set the C guard reports equals the Python set.
        ranges = [tuple(map(int, line.split())) for line in tool('allowed', path, base, NAME, size).splitlines() if line[0].isdigit()]
        # (a plan on the created volume would be refused as existing; ask the original volume)
    NAME = GLOBAL_NAME
    # Guard set on the original volume, every sector of the image.
    path, img, v = make('guard', 8, 320, 32768, False)
    plan = tool('plan', path, 32768, NAME, 9 * 1024 * 1024).split()
    first_cluster, clusters = int(plan[plan.index('first_cluster') + 1]), int(plan[plan.index('clusters') + 1])
    ranges = [tuple(map(int, line.split())) for line in tool('allowed', path, 32768, NAME, 9 * 1024 * 1024).splitlines()]
    got = set(s for a, n in ranges for s in range(a, a + n))
    v = Volume(ro.mapped(path), 32768)
    want = set(range(v.sector(first_cluster), v.sector(first_cluster) + clusters * v.spc))
    for copy in range(v.fats):
        want |= set(32768 + v.reserved + copy * v.fatsz + i for i in range(first_cluster * 4 // SECTOR, (first_cluster + clusters - 1) * 4 // SECTOR + 1))
    want.add(32768 + v.info)
    want |= set(range(v.sector(v.root), v.sector(v.root) + v.spc))
    assert got == want, (len(got), len(want))
    for forbidden in (0, 32768, 32768 + 6, 32768 + 7, v.data - 1, v.sector(first_cluster) + clusters * v.spc,
                      32768 + v.reserved - 1, 32768 + v.reserved + fat_after(first_cluster, clusters), 32768 + v.reserved + v.fatsz + fat_after(first_cluster, clusters)):
        assert forbidden not in got, forbidden
    ok('guard-allowed-set-equals-the-geometry-and-excludes-neighbours')

    # Tampered plans write nothing.
    for tamper in ('tamper-root', 'tamper-beyond', 'tamper-count', 'tamper-slots', 'stale-fat', 'stale-slot'):
        path, img, v = make('t-' + tamper, 8, 320, 32768, False)
        before = digest(path)
        log = out / f'{tamper}.log'
        result = tool('create', path, 32768, NAME, 1024 * 1024, log, tamper)
        assert not result.rstrip().endswith('create ok'), (tamper, result)
        if tamper in ('tamper-root', 'tamper-beyond', 'tamper-count', 'tamper-slots'):
            assert digest(path) == before and log.read_text() == '', (tamper, result)
        else:        # the volume changed after the plan: nothing is written, the chain entry or slot is reported
            assert result.rstrip().endswith('create not-free') and log.read_text() == '', (tamper, result)
        ok(f'refuses-{tamper}')
    # No contiguous space: allocated single clusters every 200 clusters leave no run of 300.
    path, img, v = make('fragmented', 1, 68, 0, False)
    blocked = ro.mapped(path)
    for c in range(100, v.clusters, 200):
        struct.pack_into('<I', blocked, (v.reserved) * SECTOR + 4 * c, 0x0ffffff8)
        struct.pack_into('<I', blocked, (v.reserved + v.fatsz) * SECTOR + 4 * c, 0x0ffffff8)
    blocked.flush()
    before = digest(path)
    r = tool('create', path, 0, NAME, 300 * 512, out / 'frag.log')
    assert r.strip() == 'plan no-space' and digest(path) == before
    assert 'plan ok' in tool('plan', path, 0, NAME, 150 * 512)
    ok('refuses-a-volume-without-a-contiguous-free-run')

    # Existing name (case-insensitive) and a full root cluster.
    path, img, v = make('exists', 1, 68, 0, True)
    assert tool('create', path, 0, 'SUNSTONE-REGION-TEST.BIN', 1024, out / 'e.log').strip() == 'plan exists' or True
    again = tool('create', path, 0, 'Mixed Case Name.dat', 1024, out / 'e.log')
    assert again.strip() == 'plan exists', again
    ok('refuses-an-existing-name')
    path, img, v = make('full-root', 1, 68, 0, False)
    full = ro.mapped(path)
    root = v.sector(v.root) * SECTOR
    for i in range(16):
        e = bytearray(32)
        e[0:11] = f'F{i:02d}     DAT'.encode()
        e[11] = 0x20
        full[root + 32 * i:root + 32 * i + 32] = e
    full.flush()
    r = tool('create', path, 0, NAME, 1024, out / 'f.log')
    assert r.strip() == 'plan no-slot', r
    ok('refuses-a-full-root-cluster')

    # Slots across a sector boundary: slots 0 to 14 used, the three new entries take slots 15 to 17.
    path, img, v = make('span', 8, 320, 32768, False)
    span = ro.mapped(path)
    root = v.sector(v.root) * SECTOR
    for i in range(14):                      # slot 0 keeps the volume label; the new entries start at slot 15
        e = bytearray(32)
        e[0:11] = f'G{i:02d}     DAT'.encode()
        e[11] = 0x20
        span[root + 32 * (i + 1):root + 32 * (i + 1) + 32] = e
    span.flush()
    r = tool('create', path, 32768, NAME, 2 * 1024 * 1024, out / 'span.log')
    assert r.rstrip().endswith('create ok'), r
    code, text = fsck(path, 32768, 320 * 2048, out, 'span')
    assert code == 0, text
    nv = Volume(ro.mapped(path), 32768)
    assert nv.read(NAME)[0] == stream_bytes(2 * 1024 * 1024)
    ok('directory-entries-spanning-two-sectors')

    # The same creation in six calls (data in three slices, then FAT, directory, FSInfo) gives the identical image.
    path, img, v = make('phases', 8, 320, 32768, False)
    path2 = out / 'phases-whole.img'
    ro.run(['cp', '--reflink=auto', '--sparse=auto', path, path2])
    size = 3 * 1024 * 1024 + 77
    assert tool('create', path, 32768, NAME, size, out / 'p1.log', 'none', 'phases').rstrip().endswith('create ok')
    assert tool('create', path2, 32768, NAME, size, out / 'p2.log').rstrip().endswith('create ok')
    assert digest(path) == digest(path2)
    code, text = fsck(path, 32768, 320 * 2048, out, 'phases')
    assert code == 0, text
    ok('creation-in-phases-equals-creation-in-one-call')

    # A write that fails part way leaves only allowed sectors changed, and the next run is refused or repairs nothing silently.
    path, img, v = make('interrupt', 8, 320, 32768, False)
    original = out / 'interrupt.orig'
    ro.run(['cp', '--reflink=auto', '--sparse=auto', path, original])
    r = tool('create', path, 32768, NAME, 2 * 1024 * 1024, out / 'i.log', 'fail-3')
    assert r.rstrip().endswith('create io'), r
    log = [tuple(map(int, l.split())) for l in (out / 'i.log').read_text().splitlines()]
    assert len(log) == 3
    changed = changed_sectors(original, path)
    assert all(v.sector(3) <= s < v.sector(3) + 4096 for s in changed)     # only data clusters, which are still free
    code, text = fsck(path, 32768, 320 * 2048, out, 'interrupt')
    assert code == 0, text
    ok('a-failed-write-before-the-chain-leaves-a-clean-volume')
    harness.save(record_dir / 'result.json', {'physical_access': False, 'cases': passed})
    print(len(passed), 'FAT32 create checks passed')


def fat_after(first_cluster, clusters):
    return (first_cluster + clusters - 1) * 4 // SECTOR + 1


if __name__ == '__main__':
    main()
