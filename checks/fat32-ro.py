#!/usr/bin/env python3
"""Check the read-only FAT32 reader against images from mkfs.fat and an independent Python writer.

Builds FAT32 volumes with 512-byte and 4 KiB clusters, one inside a card image at sector 32768, writes
files (long names, a fragmented pair, a subdirectory, an empty file, a multi-cluster root directory),
and runs the reader through a strict file-backed device model (checks/fat32-ro.c). Every malformed
variant must be refused with its own status.
"""
import argparse
import atexit
import shutil
import hashlib
import mmap
from pathlib import Path
import random
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
SECTOR = 512
SFN_CHARS = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_')
u16 = lambda b, o: struct.unpack_from('<H', b, o)[0]
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]


class Fat32:
    """FAT32 writer over a formatted volume that starts at sector base of the image."""

    def __init__(self, img, base):
        self.img, self.base = img, base
        b = img[base * SECTOR:(base + 1) * SECTOR]
        assert u16(b, 11) == SECTOR and u16(b, 510) == 0xaa55
        self.spc, rsvd, self.nfats = b[13], u16(b, 14), b[16]
        self.fatsz, total = u32(b, 36), u32(b, 32)
        self.fat_start, self.root = base + rsvd, u32(b, 44)
        self.data = base + rsvd + self.nfats * self.fatsz
        self.count = (total - rsvd - self.nfats * self.fatsz) // self.spc
        self.csize, self.next = self.spc * SECTOR, 3
        self.dirs = {}

    def get(self, c): return u32(self.img, self.fat_start * SECTOR + 4 * c) & 0x0fffffff

    def set(self, c, v):
        for f in range(self.nfats):
            struct.pack_into('<I', self.img, (self.fat_start + f * self.fatsz) * SECTOR + 4 * c, v)

    def alloc(self, n):
        out = []
        while len(out) < n:
            assert self.next < self.count + 2
            if self.get(self.next) == 0:
                out.append(self.next)
            self.next += 1
        return out

    def chain(self, clusters):
        for c, n in zip(clusters, clusters[1:] + [None]):
            self.set(c, n if n is not None else 0x0fffffff)

    def sector(self, c): return self.data + (c - 2) * self.spc

    def write(self, clusters, data):
        for i, c in enumerate(clusters):
            part = data[i * self.csize:(i + 1) * self.csize]
            self.img[self.sector(c) * SECTOR:self.sector(c) * SECTOR + len(part)] = part

    def extend_root(self, extra):
        c = self.root
        while self.get(c) < 0x0ffffff8:
            c = self.get(c)
        new = self.alloc(extra)
        self.chain([c] + new)
        self.write(new, bytes(self.csize * extra))

    def sfn(self, name, used):
        stem, _, ext = name.rpartition('.') if '.' in name else (name, '', '')

        def plain(part, limit):
            return 0 < len(part) <= limit and set(part.upper()) <= SFN_CHARS and part in (part.lower(), part.upper())
        if plain(stem, 8) and (not ext or plain(ext, 3)):
            case = (0x08 if stem != stem.upper() else 0) | (0x10 if ext != ext.upper() else 0)
            return (stem.upper().ljust(8) + ext.upper().ljust(3)).encode(), case, False
        body = ''.join(ch for ch in stem.upper() if ch in SFN_CHARS)[:6]
        tail = ''.join(ch for ch in ext.upper() if ch in SFN_CHARS)[:3]
        for i in range(1, 100):
            short = ((body + '~' + str(i)).ljust(8) + tail.ljust(3)).encode()
            if short not in used:
                return short, 0, True
        raise AssertionError(name)

    @staticmethod
    def raw(short, attr, case, first, size):
        e = bytearray(32)
        e[0:11] = short
        e[11], e[12] = attr, case
        struct.pack_into('<HHHHHHHI', e, 14, 0x6000, 0x5a95, 0x5a95, first >> 16, 0x6000, 0x5a95, first & 0xffff, size)
        return bytes(e)

    def entries(self, name, attr, first, size, used):
        short, case, long = self.sfn(name, used)
        used.add(short)
        sfn = self.raw(short, attr, case, first, size)
        out = []
        if long:
            s = 0
            for ch in short:
                s = (((s & 1) << 7) + (s >> 1) + ch) & 0xff
            raw = name.encode('utf-16-le')
            units = [u16(raw, i) for i in range(0, len(raw), 2)]
            parts = [units[i:i + 13] for i in range(0, len(units), 13)]
            if len(parts[-1]) < 13:
                parts[-1] = parts[-1] + [0] + [0xffff] * (12 - len(parts[-1]))
            for n in range(len(parts), 0, -1):
                e = bytearray(32)
                e[0] = n | (0x40 if n == len(parts) else 0)
                e[11], e[13] = 0x0f, s
                for i, off in enumerate([1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]):
                    struct.pack_into('<H', e, off, parts[n - 1][i])
                out.append(bytes(e))
        return out + [sfn]

    def root_slots(self):
        slots, c = [], self.root
        while True:
            slots += [self.sector(c) * SECTOR + i * 32 for i in range(self.csize // 32)]
            n = self.get(c)
            if n >= 0x0ffffff8:
                return slots
            c = n

    def place(self, slots, entries):
        free = [o for o in slots if self.img[o] == 0]
        assert len(free) >= len(entries)
        # Entries of one name must be adjacent.
        start = next(i for i in range(len(free)) if all(free[i + k] == free[i] + 32 * k for k in range(len(entries))
                                                          if i + k < len(free)) and i + len(entries) <= len(free))
        for o, e in zip(free[start:], entries):
            self.img[o:o + 32] = e

    def populate(self, tree, layout, slots=None, parent=0):
        slots = slots or self.root_slots()
        used, info = set(), {}
        for name, value in tree.items():
            if isinstance(value, dict):
                c = self.alloc(1)
                self.chain(c)
                o = self.sector(c[0]) * SECTOR
                self.img[o:o + self.csize] = bytes(self.csize)
                inner = [self.raw(b'.          ', 0x10, 0, c[0], 0), self.raw(b'..         ', 0x10, 0, parent, 0)]
                self.img[o:o + 64] = b''.join(inner)
                sub = [o + 64 + 32 * i for i in range(self.csize // 32 - 2)]
                sub_info = self.populate(value, layout, sub, c[0])
                info[name] = {'dir': c[0], 'children': sub_info}
                self.place(slots, self.entries(name, 0x10, c[0], 0, used))
            else:
                clusters = layout.get(name) or self.alloc((len(value) + self.csize - 1) // self.csize)
                if clusters:
                    self.chain(clusters)
                    self.write(clusters, value)
                info[name] = {'clusters': clusters, 'size': len(value)}
                self.place(slots, self.entries(name, 0x20, clusters[0] if clusters else 0, len(value), used))
        return info


def run(command, **kwargs):
    r = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    if r.returncode:
        raise SystemExit(f'{command}: {r.stdout}{r.stderr}')
    return r.stdout


def build(out, name, spc, size_mib, base, bigdir=False):
    """A card image with one FAT32 partition at sector base; returns (path, fs, files, info)."""
    rng = random.Random(0x5eed + spc)
    files = {
        'sunstone-region-test.bin': rng.randbytes(3 * 1024 * 1024 + 123),
        'SHORT.TXT': b'short name upper\n',
        'lower.txt': b'short name lower\n',
        'Mixed Case Name.dat': rng.randbytes(70000),
        'café.txt': b'non-ascii\n',
        'empty.bin': b'',
        'fragmented.bin': rng.randbytes(400 * 1024),
        'other fragmented.bin': rng.randbytes(300 * 1024),
        'models': {'deep': {'Weights File.bin': rng.randbytes(150000)}},
    }
    total = size_mib * 2048
    path = out / f'{name}.img'
    run(['truncate', '-s', str(total * SECTOR), path])         # a sparse file: only the written sectors take memory
    run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', str(spc), '-n', 'SUNSTONE', '-i', '5a733002', '-h', str(base),
         '--offset', str(base), path, str((total - base) // 2)])
    img = mapped(path)
    fs = Fat32(img, base)
    if bigdir:
        fs.extend_root(3)
    # Interleave the two fragmented files in short runs.
    need = {k: -(-len(files[k]) // fs.csize) for k in ('fragmented.bin', 'other fragmented.bin')}
    layout, runs = {k: [] for k in need}, {'fragmented.bin': 5, 'other fragmented.bin': 2}
    while any(len(layout[k]) < need[k] for k in need):
        for k in need:
            take = min(runs[k], need[k] - len(layout[k]))
            layout[k] += fs.alloc(take) if take else []
    info = fs.populate(files, layout)
    if bigdir:
        for i in range(30):                    # push the root directory into its second cluster
            fs.place(fs.root_slots(), fs.entries(f'filler{i:02d}.dat', 0x20, 0, 0, set()))
    img.flush()
    return path, fs, files, info


def mapped(path):
    """The image as a writable memory map: a view that slices and packs like a bytearray, with no copy of the whole image."""
    with open(path, 'r+b') as stream:
        return mmap.mmap(stream.fileno(), 0)


def mutate(base_path, out, name, change):
    """A sparse copy of the image with one change applied (the copy holds only the sectors that hold data)."""
    path = out / f'{name}.img'
    run(['cp', '--reflink=auto', '--sparse=auto', base_path, path])       # holes stay holes, found without reading them
    img = mapped(path)
    change(img)
    img.close()
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    record_dir = args.output                              # the result record; the case images (hundreds of megabytes) live in RAM
    record_dir.mkdir(parents=True, exist_ok=True)
    out = harness.ram_directory('fat32-ro-')
    atexit.register(shutil.rmtree, out, True)
    binary = out / 'fat32-ro'
    run(['clang-21', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', f'-I{ROOT}/include', ROOT / 'checks/fat32-ro.c', ROOT / 'drivers/storage/fat32_ro.c',
         ROOT / 'drivers/storage/storage_digest.c', '-o', binary])
    env = {**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0'}

    def tool(*a):
        r = subprocess.run([str(binary), *map(str, a)], capture_output=True, text=True, env=env)
        assert r.returncode == 0, (a, r.stdout, r.stderr)
        return r.stdout.strip()

    results = []

    def ok(name):
        results.append(name)
        print('PASS', name, flush=True)

    for label, spc, size, base, bigdir in (('spc1', 1, 68, 0, True), ('spc8', 8, 320, 32768, False)):
        image, fs, files, info = build(out, label, spc, size, base, bigdir)
        # Volume fields and free space against an independent scan of the FAT.
        facts = dict(zip(*[iter(tool('info', image, base).split())] * 2))
        free = sum(1 for c in range(2, fs.count + 2) if fs.get(c) == 0)
        assert facts['status'] == 'ok' and int(facts['counted_free']) == free and int(facts['clusters']) == fs.count, facts
        assert int(facts['spc']) == spc and int(facts['data_start']) == fs.data
        with open(image, 'rb') as stream:
            stream.seek(base * SECTOR)
            raw = stream.read(4 * SECTOR)
        hint = u32(raw, SECTOR + 488)
        assert int(facts['hint_free']) == hint and int(facts['hint_next']) == u32(raw, SECTOR + 492) or hint == 0xffffffff, facts
        ok(f'{label}-volume-and-free-space')

        # Root listing: names, sizes, long-name verification.
        listing = [line.split('|') for line in tool('list', image, base).splitlines()]
        assert listing[-1] == ['end ok'], listing[-1]
        names = {row[0]: row for row in listing[:-1]}
        expect = {k: (len(v) if isinstance(v, bytes) else 0) for k, v in files.items()}
        expect['caf?.txt'] = expect.pop('café.txt')
        for k, size_ in expect.items():
            assert int(names[k][1]) == size_, (k, names[k])
        assert names['SHORT.TXT'][4] == '0' and names['lower.txt'][4] == '0'   # short names only
        assert names['sunstone-region-test.bin'][4] == '1'                      # verified long name
        if bigdir:
            assert len(names) == len(expect) + 30
        ok(f'{label}-root-listing')

        # Lookups (case-insensitive, subdirectory path) and the chain of every file.
        for path, data in (('SUNSTONE-REGION-TEST.BIN', files['sunstone-region-test.bin']),
                           ('models/deep/weights file.bin', files['models']['deep']['Weights File.bin']),
                           ('short.txt', files['SHORT.TXT']), ('LOWER.TXT', files['lower.txt'])):
            o = tool('open', image, base, path)
            assert o.startswith('open ok') and f'size {len(data)}' in o, (path, o)
        assert tool('open', image, base, 'empty.bin').startswith('open ok size 0')
        assert tool('open', image, base, 'missing.bin') == 'lookup not-found'
        assert tool('open', image, base, 'Short.TXT/x') == 'lookup not-found'
        ok(f'{label}-lookup')

        # Extents against the writer's cluster lists, for several request sizes, and the fragmented pair.
        def sectors_of(clusters, size_bytes):
            total = -(-size_bytes // SECTOR)
            seq = [fs.sector(c) + i for c in clusters for i in range(spc)]
            return seq[:total]
        for path, key, size_ in (('fragmented.bin', 'fragmented.bin', len(files['fragmented.bin'])),
                                 ('other fragmented.bin', 'other fragmented.bin', len(files['other fragmented.bin'])),
                                 ('sunstone-region-test.bin', 'sunstone-region-test.bin', len(files['sunstone-region-test.bin']))):
            want = sectors_of(info[key]['clusters'], size_)
            for maxs in (1, 3, 7, 256):
                lines = tool('extents', image, base, path, maxs).splitlines()
                got, total = [], 0
                for line in lines[:-1]:
                    s, n, b = map(int, line.split())
                    got += range(s, s + n)
                    total += b
                assert lines[-1] == f'extents {len(lines) - 1} bytes {size_} status ok' and total == size_ and got == want, (path, maxs)
            if key != 'sunstone-region-test.bin':
                assert len(tool('extents', image, base, path, 256).splitlines()) > 2   # fragmented
        ok(f'{label}-extents-and-fragmentation')

        # Streaming through two buffers: bytes, digest, and the pipeline.
        for path, data in (('fragmented.bin', files['fragmented.bin']), ('sunstone-region-test.bin', files['sunstone-region-test.bin']),
                           ('empty.bin', b''), ('models/deep/Weights File.bin', files['models']['deep']['Weights File.bin'])):
            dest = out / f'{label}.stream'
            o = tool('stream', image, base, path, dest).split()
            fields = dict(zip(o[0::2], o[1::2]))
            assert fields['stream'] == 'ok' and dest.read_bytes() == data and fields['sha256'] == hashlib.sha256(data).hexdigest(), (path, o)
            calls, over, req = int(fields['calls']), int(fields['overlapped']), int(fields['requests'])
            assert calls == req and (calls == 0 or over == calls - 1), (path, fields)   # every consume but the last overlaps a request
        ok(f'{label}-pipelined-stream')

        # Malformed images.
        first = lambda key: info[key]['clusters']
        entry_offset = {}

        def find_entry(img, short):
            for off in fs.root_slots():
                if bytes(img[off:off + 11]) == short:
                    return off
            raise AssertionError(short)
        fl = first('fragmented.bin')

        def case(name, change, expect_open, path='fragmented.bin'):
            p = mutate(image, out, f'{label}-{name}', change)
            r = tool('open', p, base, path)
            assert expect_open in r, (name, r)
            r2 = tool('stream', p, base, path, out / 'x.bin') if 'open ok' not in r and path == 'fragmented.bin' and 'lookup' not in r else None
            if r2:
                assert r2.startswith('open ') or r2.startswith('stream') is False or 'stream ok' not in r2, (name, r2)
            ok(f'{label}-refuses-{name}')

        case('truncated-chain', lambda im: fs_set(im, fs, fl[2], 0x0fffffff), 'truncated')
        case('chain-loop', lambda im: fs_set(im, fs, fl[-1], fl[0]), 'chain-long')
        case('chain-continues-past-end', lambda im: fs_set(im, fs, fl[-1], fl[-2]), 'chain-long')
        case('free-cluster-in-chain', lambda im: fs_set(im, fs, fl[3], 0), 'bad-chain')
        case('bad-cluster-mark', lambda im: fs_set(im, fs, fl[3], 0x0ffffff7), 'bad-chain')
        case('reserved-cluster-value', lambda im: fs_set(im, fs, fl[3], 1), 'bad-chain')
        case('out-of-range-cluster', lambda im: fs_set(im, fs, fl[3], fs.count + 2), 'bad-chain')
        case('cross-link-into-root-directory', lambda im: fs_set(im, fs, fl[3], fs.root), 'cross-linked')
        case('size-smaller-than-chain', lambda im: patch_size(im, find_entry(im, b'FRAGME~1BIN'), 400 * 1024 - 3 * spc * SECTOR - 1), 'chain-long')
        case('size-larger-than-chain', lambda im: patch_size(im, find_entry(im, b'FRAGME~1BIN'), 400 * 1024 + 2 * spc * SECTOR), 'truncated')
        case('empty-file-with-cluster', lambda im: patch_first(im, find_entry(im, b'EMPTY   BIN'), 5), 'chain-long', 'empty.bin')
        case('nonempty-file-without-cluster', lambda im: patch_first(im, find_entry(im, b'FRAGME~1BIN'), 0), 'truncated')
        case('first-cluster-out-of-range', lambda im: patch_first(im, find_entry(im, b'FRAGME~1BIN'), fs.count + 5), 'bad-dir')
        case('directory-with-size', lambda im: patch_size(im, find_entry(im, b'MODELS     '), 7), 'bad-dir', 'models/deep/Weights File.bin')
        case('wrong-long-name-checksum', lambda im: fix_checksum(im, fs, b'SUNSTO~1BIN'), 'lookup not-found', 'sunstone-region-test.bin')

        def loop_root(im):
            c = fs.root
            while fs.get(c) < 0x0ffffff8:
                c = fs.get(c)
            fs_set(im, fs, c, fs.root)
            for off in range(fs.sector(fs.root) * SECTOR, (fs.sector(fs.root) + spc) * SECTOR, 32):
                if im[off] == 0:
                    im[off:off + 32] = Fat32.raw(b'JUNK    DAT', 0x20, 0, 0, 0)
        if spc == 1:
            # A looping root directory with every slot used never reaches an end marker.
            def fill_root(im):
                for off in fs.root_slots():
                    if im[off] == 0:
                        im[off:off + 32] = Fat32.raw(b'JUNK    DAT', 0x20, 0, 0, 0)
                loop_root(im)
            case('looping-directory', fill_root, 'lookup bad-dir', 'nothing.bin')
    # Boot sector variants on a small volume.
    image, fs, files, info = build(out, 'boot', 1, 68, 0, True)
    def boot(name, off, size, value, expect):
        p = mutate(image, out, f'boot-{name}', lambda im: im.__setitem__(slice(off, off + size), value.to_bytes(size, 'little')))
        assert tool('mount', p, 0) == expect, (name, tool('mount', p, 0))
        ok(f'boot-{name}-{expect}')
    assert tool('mount', image, 0) == 'ok'
    boot('signature', 510, 2, 0, 'bad-boot')
    boot('sector-size-1024', 11, 2, 1024, 'unsupported')
    boot('sector-size-300', 11, 2, 300, 'bad-boot')
    boot('cluster-size-3', 13, 1, 3, 'bad-boot')
    boot('cluster-size-zero', 13, 1, 0, 'bad-boot')
    boot('no-fats', 16, 1, 0, 'bad-boot')
    boot('three-fats', 16, 1, 3, 'bad-boot')
    boot('root-entries', 17, 2, 512, 'unsupported')
    boot('small-total', 19, 2, 1000, 'unsupported')
    boot('fat16-size', 22, 2, 9, 'unsupported')
    boot('version', 42, 2, 1, 'bad-boot')
    boot('fat-size-zero', 36, 4, 0, 'bad-boot')
    boot('fat-too-small', 36, 4, 10, 'bad-boot')
    boot('total-beyond-partition', 32, 4, 68 * 2048 + 1, 'bad-boot')
    boot('root-cluster-zero', 44, 4, 0, 'bad-boot')
    boot('root-cluster-beyond', 44, 4, 0x0fffffff, 'bad-boot')
    boot('media-byte', 21, 1, 0xf0, 'bad-boot')
    boot('reserved-one', 14, 2, 1, 'bad-boot')
    # A FAT16 volume is refused as not FAT32.
    part = out / 'fat16.img'
    run(['truncate', '-s', str(16 * 1024 * 1024), part])
    run(['/usr/sbin/mkfs.fat', '-F', '16', part])
    assert tool('mount', part, 0) == 'unsupported'
    ok('fat16-unsupported')
    # A corrupt FSInfo only drops the hints.
    p = mutate(image, out, 'fsinfo-bad', lambda im: im.__setitem__(slice(SECTOR, SECTOR + 4), b'\0\0\0\0'))
    facts = dict(zip(*[iter(tool('info', p, 0).split())] * 2))
    assert facts['status'] == 'ok' and facts['hint_free'] == '4294967295'
    ok('fsinfo-corrupt-drops-hints')
    harness.save(record_dir / 'result.json', {'physical_access': False, 'cases': results})
    print(len(results), 'FAT32 reader checks passed')


def fs_set(im, fs, cluster, value):
    for f in range(fs.nfats):
        struct.pack_into('<I', im, (fs.fat_start + f * fs.fatsz) * SECTOR + 4 * cluster, value)


def patch_size(im, off, size): struct.pack_into('<I', im, off + 28, size)


def patch_first(im, off, cluster):
    struct.pack_into('<H', im, off + 20, cluster >> 16)
    struct.pack_into('<H', im, off + 26, cluster & 0xffff)


def fix_checksum(im, fs, short):
    for off in fs.root_slots():
        if bytes(im[off:off + 11]) == short:
            im[off - 32 + 13] ^= 0xff           # the checksum byte of the long entry before it
            return
    raise AssertionError(short)


if __name__ == '__main__':
    main()
