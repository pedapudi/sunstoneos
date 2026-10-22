#!/usr/bin/env python3
"""Read-only FatFs port: pinned sources, FAT16/FAT32/exFAT card images and AArch64 compile; no hardware.

mkfs.fat and mkfs.exfat format each volume. This script adds files with its own
FAT and exFAT writers, which share no code with FatFs, then runs the host model
in checks/fatfs-port.c over a RAM device and over the polled SD core.
"""
import argparse
import hashlib
import json
import random
import shutil
import struct
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--fatfs', type=Path, default=Path.home() / 'git/esp-idf/components/fatfs/src',
               help='Directory holding the pinned FatFs files listed in lib/fatfs/source.json')
p.add_argument('--evidence', type=Path, required=True, help='Fresh external directory')
p.add_argument('--cc', default='/usr/bin/clang-21')
p.add_argument('--llvm', type=Path, default=Path('/usr/lib/llvm-21/bin'))
p.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
p.add_argument('--c-headers', type=Path, default=Path.home() /
               '.cache/sunstoneos/host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=False)
root = Path(__file__).resolve().parents[1]
records = []
inputs = {}
SECTOR = 512
BOOT_AREA = 40960  # First sector after the boot area in docs/boot-slots.md.


def run(command, check=True):
    result = subprocess.run([str(c) for c in command], cwd=root, capture_output=True, text=True)
    records.append(dict(command=[str(c) for c in command], returncode=result.returncode,
                        stdout=result.stdout, stderr=result.stderr))
    (a.evidence / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
    if check:
        assert result.returncode == 0, records[-1]
    return result


# Pinned FatFs sources are copied into the evidence before use.
manifest = json.loads((root / 'lib/fatfs/source.json').read_text())
fatfs = a.evidence / 'fatfs'
fatfs.mkdir()
for name, digest in manifest['files'].items():
    data = (a.fatfs / name).read_bytes()
    assert hashlib.sha256(data).hexdigest() == digest, name
    (fatfs / name).write_bytes(data)
    inputs[str(a.fatfs / name)] = digest
license_text = (fatfs / 'ff.c').read_text().split('*/')[0]
assert 'FatFs - Generic FAT Filesystem Module  R0.16' in license_text
assert 'Redistributions of source code must retain the above copyright notice' in license_text


SFN_CHARS = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_')


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]


class Fat:
    """FAT16/FAT32 writer over a formatted volume inside a card image."""

    def __init__(self, img, base):
        self.img, self.base = img, base
        b = img[base * SECTOR:(base + 1) * SECTOR]
        assert u16(b, 11) == SECTOR and u16(b, 510) == 0xaa55
        self.spc, rsvd, self.nfats, root_entries = b[13], u16(b, 14), b[16], u16(b, 17)
        fatsz = u16(b, 22) or u32(b, 36)
        total = u16(b, 19) or u32(b, 32)
        self.fat_start, self.fatsz = base + rsvd, fatsz
        self.root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR
        self.root_start = base + rsvd + self.nfats * fatsz
        self.data = self.root_start + self.root_sectors
        self.count = (total - (self.data - base)) // self.spc
        assert self.count >= 4085
        self.bits = 16 if self.count < 65525 else 32
        self.root = u32(b, 44) if self.bits == 32 else 0
        self.csize = self.spc * SECTOR
        self.next = 2

    def get(self, c):
        o = self.fat_start * SECTOR + c * self.bits // 8
        return (u16(self.img, o) if self.bits == 16 else u32(self.img, o) & 0x0fffffff)

    def set(self, c, v):
        for f in range(self.nfats):
            o = (self.fat_start + f * self.fatsz) * SECTOR + c * self.bits // 8
            if self.bits == 16:
                struct.pack_into('<H', self.img, o, v)
            else:
                struct.pack_into('<I', self.img, o, (u32(self.img, o) & 0xf0000000) | v)

    def eoc(self): return 0xffff if self.bits == 16 else 0x0fffffff

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
            self.set(c, n if n is not None else self.eoc())

    def sector(self, c): return self.data + (c - 2) * self.spc

    def write(self, clusters, data):
        for i, c in enumerate(clusters):
            part = data[i * self.csize:(i + 1) * self.csize]
            o = self.sector(c) * SECTOR
            self.img[o:o + len(part)] = part

    def sfn(self, name, used):
        """Short name, its case flags and whether long-name entries are needed."""
        stem, _, ext = name.rpartition('.') if '.' in name else (name, '', '')

        def plain(part, limit):
            return (0 < len(part) <= limit and set(part.upper()) <= SFN_CHARS and
                    part in (part.lower(), part.upper()))
        if plain(stem, 8) and (not ext or plain(ext, 3)):
            case = (0x08 if stem != stem.upper() else 0) | (0x10 if ext != ext.upper() else 0)
            return (stem.upper().ljust(8) + ext.upper().ljust(3)).encode(), case, False
        body = ''.join(ch for ch in stem.upper() if ch in SFN_CHARS)[:6]
        tail = ''.join(ch for ch in ext.upper() if ch in SFN_CHARS)[:3]
        for i in range(1, 10):
            short = ((body + '~' + str(i)).ljust(8) + tail.ljust(3)).encode()
            if short not in used:
                return short, 0, True
        raise AssertionError(name)

    @staticmethod
    def raw(short, attr, case, first, size):
        e = bytearray(32)
        e[0:11] = short
        e[11], e[12] = attr, case
        struct.pack_into('<HHHHHHHI', e, 14, 0x6000, 0x5a95, 0x5a95, first >> 16, 0x6000, 0x5a95,
                         first & 0xffff, size)
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
            units = list(name.encode('utf-16-le'))
            units = [units[i] | units[i + 1] << 8 for i in range(0, len(units), 2)]
            parts = [units[i:i + 13] for i in range(0, len(units), 13)]
            if len(parts[-1]) < 13:
                parts[-1] = parts[-1] + [0] + [0xffff] * (12 - len(parts[-1]))
            for n in range(len(parts), 0, -1):
                e = bytearray(32)
                e[0] = n | (0x40 if n == len(parts) else 0)
                e[11], e[13] = 0x0f, s
                u = parts[n - 1]
                for i, off in enumerate([1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]):
                    struct.pack_into('<H', e, off, u[i])
                out.append(bytes(e))
        return out + [sfn]

    def root_slots(self):
        if self.bits == 16:
            return [(self.root_start * SECTOR + i * 32) for i in range(self.root_sectors * SECTOR // 32)]
        slots, c = [], self.root
        while True:
            slots += [self.sector(c) * SECTOR + i * 32 for i in range(self.csize // 32)]
            n = self.get(c)
            if n >= 0x0ffffff8:
                return slots
            c = n

    def place(self, slots, entries):
        free = [o for o in slots if self.img[o] == 0]
        assert len(free) >= len(entries) and free[:len(entries)] == list(range(free[0], free[0] + 32 * len(entries), 32))
        for o, e in zip(free, entries):
            self.img[o:o + 32] = e

    def populate(self, tree, layout):
        """tree maps names to bytes or subtrees; layout fixes some cluster lists."""
        self.place(self.root_slots(), self.directory(tree, '', set(), layout, 0))

    def directory(self, tree, prefix, used, layout, parent):
        out = []
        for name, value in tree.items():
            path = prefix + name
            if isinstance(value, dict):
                c = self.alloc(1)
                self.chain(c)
                o = self.sector(c[0]) * SECTOR
                self.img[o:o + self.csize] = bytes(self.csize)
                inner = [self.raw(b'.          ', 0x10, 0, c[0], 0), self.raw(b'..         ', 0x10, 0, parent, 0)]
                inner += self.directory(value, path + '/', set(), layout, c[0])
                assert len(inner) * 32 <= self.csize
                self.img[o:o + 32 * len(inner)] = b''.join(inner)
                out += self.entries(name, 0x10, c[0], 0, used)
            else:
                clusters = layout.get(path) or self.alloc((len(value) + self.csize - 1) // self.csize)
                if clusters:
                    self.chain(clusters)
                    self.write(clusters, value)
                out += self.entries(name, 0x20, clusters[0] if clusters else 0, len(value), used)
        return out


class ExFat:
    """exFAT writer over a volume formatted by mkfs.exfat."""

    def __init__(self, img, base):
        self.img, self.base = img, base
        b = img[base * SECTOR:(base + 1) * SECTOR]
        assert b[3:11] == b'EXFAT   ' and b[108] == 9
        self.fat_start, self.heap, self.count, self.root = base + u32(b, 80), base + u32(b, 88), u32(b, 92), u32(b, 96)
        self.csize = SECTOR << b[109]
        self.next = 2
        for o in self.slots(self.root, None):
            if img[o] == 0x81:
                self.bitmap = self.sector(u32(img, o + 20)) * SECTOR
                assert u32(img, o + 24) * 8 >= self.count
        assert self.bitmap

    def sector(self, c): return self.heap + (c - 2) * (self.csize // SECTOR)
    def used(self, c): return self.img[self.bitmap + (c - 2) // 8] >> ((c - 2) % 8) & 1

    def mark(self, c):
        self.img[self.bitmap + (c - 2) // 8] |= 1 << ((c - 2) % 8)

    def fat(self, c, v): struct.pack_into('<I', self.img, self.fat_start * SECTOR + 4 * c, v)
    def fat_get(self, c): return u32(self.img, self.fat_start * SECTOR + 4 * c)

    def slots(self, first, clusters):
        out = []
        for c in (clusters or self.walk(first)):
            out += [self.sector(c) * SECTOR + i * 32 for i in range(self.csize // 32)]
        return out

    def walk(self, c):
        while True:
            yield c
            c = self.fat_get(c)
            if c >= 0xfffffff8:
                return

    def alloc(self, n):
        out = []
        while len(out) < n:
            assert self.next < self.count + 2
            if not self.used(self.next):
                out.append(self.next)
            self.next += 1
        return out

    def store(self, clusters, data, contiguous):
        for c in clusters:
            self.mark(c)
        if not contiguous:
            for c, n in zip(clusters, clusters[1:] + [0xffffffff]):
                self.fat(c, n)
        for i, c in enumerate(clusters):
            part = data[i * self.csize:(i + 1) * self.csize]
            o = self.sector(c) * SECTOR
            self.img[o:o + len(part)] = part

    @staticmethod
    def entry_set(name, attr, first, length, contiguous):
        units = [ord(ch) for ch in name]
        h = 0
        for u in [ord(ch) for ch in name.upper()]:
            for byte in (u & 0xff, u >> 8):
                h = ((0x8000 if h & 1 else 0) + (h >> 1) + byte) & 0xffff
        names = [units[i:i + 15] for i in range(0, len(units), 15)]
        primary = bytearray(32)
        primary[0], primary[1] = 0x85, 1 + len(names)
        struct.pack_into('<HHIII', primary, 4, attr, 0, 0x5a956000, 0x5a956000, 0x5a956000)
        stream = bytearray(32)
        stream[0], stream[1], stream[3] = 0xc0, 1 | (2 if contiguous else 0), len(units)
        struct.pack_into('<HHQIIQ', stream, 4, h, 0, length, 0, first, length)
        out = [primary, stream]
        for part in names:
            e = bytearray(32)
            e[0] = 0xc1
            for i, u in enumerate(part):
                struct.pack_into('<H', e, 2 + 2 * i, u)
            out.append(e)
        s = 0
        for i, byte in enumerate(b''.join(out)):
            if i not in (2, 3):
                s = ((0x8000 if s & 1 else 0) + (s >> 1) + byte) & 0xffff
        struct.pack_into('<H', out[0], 2, s)
        return [bytes(e) for e in out]

    def place(self, slots, entries):
        free = [o for o in slots if self.img[o] == 0]
        assert len(free) >= len(entries)
        for o, e in zip(free, entries):
            self.img[o:o + 32] = e

    def populate(self, tree, layout, fragmented):
        self.place(self.slots(self.root, None), self.directory(tree, '', layout, fragmented))

    def directory(self, tree, prefix, layout, fragmented):
        out = []
        for name, value in tree.items():
            path = prefix + name
            if isinstance(value, dict):
                c = self.alloc(1)
                o = self.sector(c[0]) * SECTOR
                self.img[o:o + self.csize] = bytes(self.csize)
                self.mark(c[0])
                inner = self.directory(value, path + '/', layout, fragmented)
                self.place(self.slots(c[0], c), inner)
                out += self.entry_set(name, 0x10, c[0], self.csize, True)
            else:
                clusters = layout.get(path) or self.alloc((len(value) + self.csize - 1) // self.csize)
                contiguous = path not in fragmented
                assert not (contiguous and clusters) or clusters == list(range(clusters[0], clusters[0] + len(clusters)))
                self.store(clusters, value, contiguous)
                out += self.entry_set(name, 0x20, clusters[0] if clusters else 0, len(value), contiguous)
        return out


rng = random.Random(0x5a733)
files = {
    'config.txt': b'model=models/base.bin\noverlay=models/mimus overlay model.bin\nrate=16000\n',
    'empty.txt': b'',
    'models': {
        'base.bin': rng.randbytes(300 * 1024 + 123),
        'mimus overlay model.bin': rng.randbytes(70 * 1024 + 5),
        'Contiguous Model.bin': rng.randbytes(96 * 1024),
    },
    'tail.bin': rng.randbytes(12 * 1024 + 7),
}
paths = ['config.txt', 'empty.txt', 'models/base.bin', 'models/mimus overlay model.bin',
         'models/Contiguous Model.bin', 'tail.bin']


def content(path):
    v = files
    for part in path.split('/'):
        v = v[part]
    return v


def interleave(fs, a_path, b_path, run_a, run_b):
    """Allocate two files in alternating runs so that both are fragmented."""
    need = {a_path: -(-len(content(a_path)) // fs.csize), b_path: -(-len(content(b_path)) // fs.csize)}
    out = {a_path: [], b_path: []}
    while any(len(out[k]) < need[k] for k in out):
        for k, n in ((a_path, run_a), (b_path, run_b)):
            take = min(n, need[k] - len(out[k]))
            out[k] += fs.alloc(take) if take else []
    return out


def tail_layout(fs):
    """Place tail.bin in the last clusters of the volume."""
    n = -(-len(content('tail.bin')) // fs.csize)
    top = fs.count + 2
    clusters = list(range(top - n, top))
    assert not any(fs.get(c) if isinstance(fs, Fat) else fs.used(c) for c in clusters)
    return clusters


def card(name, kind, partitioned=True, size_mib=68):
    total = size_mib * 2048
    img = bytearray(total * SECTOR)
    base = BOOT_AREA if partitioned else 0
    part = a.evidence / 'images' / (name + '.part')
    part.parent.mkdir(exist_ok=True)
    run(['truncate', '-s', str((total - base) * SECTOR), part])
    if kind == 'exfat':
        run(['/usr/sbin/mkfs.exfat', '-q', '-L', 'SUNSTONE', '-c', '4K', part])
    else:
        # FAT32 uses one-sector clusters; FAT16 uses four sectors per cluster.
        run(['/usr/sbin/mkfs.fat', '-F', kind[3:], '-s', '1' if kind == 'fat32' else '4', '-n', 'SUNSTONE',
             '-h', str(base), '-i', '5a733001', part])
    img[base * SECTOR:] = part.read_bytes()
    part.unlink()
    fs = Fat(img, base) if kind != 'exfat' else ExFat(img, base)
    layout = interleave(fs, 'models/base.bin', 'models/mimus overlay model.bin', 5, 2)
    layout['tail.bin'] = tail_layout(fs)
    if kind == 'exfat':
        fs.populate(files, layout, {'models/base.bin', 'models/mimus overlay model.bin'})
    else:
        fs.populate(files, layout)
    if partitioned:
        # Unrelated boot-area contents: boot0 and both slot headers.
        img[16 * SECTOR:496 * SECTOR] = rng.randbytes(480 * SECTOR)
        img[8192 * SECTOR:8193 * SECTOR] = rng.randbytes(SECTOR)
        img[16384 * SECTOR:16385 * SECTOR] = rng.randbytes(SECTOR)
    path = a.evidence / 'images' / (name + '.img')
    path.write_bytes(img)
    if partitioned:
        ptype = '7' if kind == 'exfat' else 'c'
        r = subprocess.run(['/usr/sbin/sfdisk', '--no-reread', '--no-tell-kernel', '-q', str(path)],
                           input=f'label: dos\nstart={base}, size={total - base}, type={ptype}\n',
                           capture_output=True, text=True)
        records.append(dict(command=['sfdisk', str(path)], returncode=r.returncode, stdout=r.stdout, stderr=r.stderr))
        assert r.returncode == 0, r.stderr
        written = path.read_bytes()
        assert written[SECTOR:] == img[SECTOR:]  # The partition table changes sector 0 only.
        img[:] = written
    # End of the last sector holding tail.bin data; nothing after it is read.
    tail_end = fs.sector(layout['tail.bin'][-1]) + ((len(content('tail.bin')) - 1) % fs.csize) // SECTOR + 1
    return path, img, fs, layout, tail_end


expected_dir = a.evidence / 'expected'
expected_dir.mkdir()
pairs = []
for i, path in enumerate(paths):
    out = expected_dir / f'{i}.bin'
    out.write_bytes(content(path))
    pairs += [path, out]

images = {}
for name, kind, partitioned, size in [('fat32', 'fat32', True, 68), ('exfat', 'exfat', True, 68),
                                      ('fat16', 'fat16', False, 16)]:
    images[name] = card(name, kind, partitioned, size)
fat32_path, fat32_img, fat32, fat32_layout, fat32_tail = images['fat32']
exfat_path, exfat_img, exfat, exfat_layout, exfat_tail = images['exfat']
# The writers produced fragmented files; the model must follow both chains.
for fs, layout in ((fat32, fat32_layout), (exfat, exfat_layout)):
    c = layout['models/base.bin']
    assert any(y != x + 1 for x, y in zip(c, c[1:]))


def variant(name, source, change):
    img = bytearray(source)
    change(img)
    path = a.evidence / 'images' / (name + '.img')
    path.write_bytes(img)
    return path


def bad_fat_chain(img):
    c = fat32_layout['models/base.bin'][2]
    for f in range(fat32.nfats):
        struct.pack_into('<I', img, (fat32.fat_start + f * fat32.fatsz) * SECTOR + 4 * c, fat32.count + 10)


def bad_exfat_chain(img):
    c = exfat_layout['models/base.bin'][2]
    struct.pack_into('<I', img, exfat.fat_start * SECTOR + 4 * c, 1)


def bad_exfat_sum(img):
    # Change the stored name of the base model; its entry-set checksum no longer matches.
    o = img.find('base.bin'.encode('utf-16-le'), exfat.heap * SECTOR)
    assert o > 0
    img[o] = ord('B')


def no_filesystem(img):
    img[BOOT_AREA * SECTOR:(BOOT_AREA + 1) * SECTOR] = bytes(SECTOR)


def gpt(img):
    # Protective MBR only: FatFs here is built without 64-bit LBA and GPT support.
    img[446:510] = bytes(64)
    img[446 + 4] = 0xee
    struct.pack_into('<II', img, 446 + 8, 1, len(img) // SECTOR - 1)
    img[SECTOR:SECTOR + 8] = b'EFI PART'


# Host model with sanitizers. FatFs compiles as third-party code without -Werror.
host = a.evidence / 'fatfs-model'
inc = ['-Iinclude', '-Ilib/fatfs', f'-I{fatfs}']
ffconf = ['-include', 'lib/fatfs/ffconf.h']  # As lib/fatfs/fatfs.cmake compiles FatFs.
san = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
objects = []
for src, strict in [('drivers/storage/fatfs_port.c', True), ('drivers/storage/sunxi_mmc.c', True),
                    ('checks/fatfs-port.c', True), (fatfs / 'ff.c', False), (fatfs / 'ffunicode.c', False)]:
    obj = a.evidence / (Path(src).stem + '.host.o')
    run([a.cc, '-std=gnu11', '-Wall', '-Wextra'] + (['-Werror'] if strict else ffconf) + inc + san +
        ['-c', src, '-o', obj])
    objects.append(obj)
run([a.cc] + san + objects + ['-o', host])

results = []


def case(*args):
    r = run([host] + list(args))
    line = r.stdout.strip()
    print(line)
    results.append(dict(args=[str(x) for x in args], output=line))
    return line


FR_DISK_ERR, FR_INT_ERR, FR_NO_FILESYSTEM = 1, 2, 13
max_blocks = []
for name in ('fat32', 'exfat', 'fat16'):
    path = images[name][0]
    line = case('ram-nominal', path, *pairs)
    max_blocks.append(int(line.split('max-blocks=')[1]))
assert max(max_blocks) == 8 and min(max_blocks) >= 1
base_pair = ['models/base.bin', expected_dir / f'{paths.index("models/base.bin")}.bin']
for name in ('fat32', 'exfat'):
    case('ram-refusals', images[name][0], *base_pair)
    case('ram-retained', images[name][0], *base_pair)
    case('mmc-nominal', images[name][0], *pairs)
    case('ram-short', images[name][0], 'tail.bin', str(images[name][4] - 1))
for kind in ('crc', 'removal', 'deadline'):
    case('mmc-fault', fat32_path, 'models/base.bin', kind)
case('mmc-fault', exfat_path, 'models/base.bin', 'crc')
case('ram-expect', variant('fat32-bad-chain', fat32_img, bad_fat_chain), 'models/base.bin', str(FR_INT_ERR))
case('ram-expect', variant('exfat-bad-chain', exfat_img, bad_exfat_chain), 'models/base.bin', str(FR_INT_ERR))
case('ram-expect', variant('exfat-bad-checksum', exfat_img, bad_exfat_sum), 'models/base.bin', str(FR_INT_ERR))
case('ram-expect', variant('no-filesystem', fat32_img, no_filesystem), 'config.txt', str(FR_NO_FILESYSTEM))
case('ram-expect', variant('gpt', fat32_img, gpt), 'config.txt', str(FR_NO_FILESYSTEM))

# Native AArch64 objects with the image compile options, as one relocatable.
native = ['--target=aarch64-none-elf', '-mcpu=cortex-a55', '-march=armv8-a', '-mstrict-align', '-ffreestanding',
          '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
          '-ffunction-sections', '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-fstack-usage',
          '-isystem', str(a.c_headers)]
native_objects = []
for src, strict in [('drivers/storage/fatfs_port.c', True), (fatfs / 'ff.c', False), (fatfs / 'ffunicode.c', False)]:
    obj = a.evidence / (Path(src).stem + '.o')
    run([a.cc, '-std=gnu11'] + native + (['-Werror'] if strict else ffconf) + inc + ['-c', src, '-o', obj])
    native_objects.append(obj)
combined = a.evidence / 'fatfs-port-aarch64.o'
run([a.lld, '-r'] + native_objects + ['-o', combined])
size = run([a.llvm / 'llvm-size', combined]).stdout
print(size, end='')
undefined = {line.split()[-1] for line in run([a.llvm / 'llvm-nm', '--undefined-only', combined]).stdout.splitlines()}
allowed = {'memcpy', 'memset', 'memcmp', 'strchr', 'sunxi_mmc_read', 'sunxi_mmc_poll', 'sunxi_mmc_take'}
assert undefined <= allowed, undefined - allowed
defined = {line.split()[-1] for line in run([a.llvm / 'llvm-nm', '--defined-only', combined]).stdout.splitlines()}
for writer in ('f_write', 'f_sync', 'f_unlink', 'f_mkdir', 'f_rename', 'f_truncate', 'f_mkfs', 'f_expand',
               'fatfs_port_disk_write', 'fatfs_port_disk_ioctl'):
    assert writer not in defined, writer
for reader in ('f_mount', 'f_open', 'f_read', 'f_close', 'f_lseek', 'f_stat', 'f_opendir', 'f_readdir',
               'fatfs_port_bind', 'fatfs_port_mount', 'fatfs_port_load', 'fatfs_port_mmc_device'):
    assert reader in defined, reader
stack = {}
for su in a.evidence.glob('*.su'):
    if su.name.endswith('.host.su'):
        continue
    for line in su.read_text().splitlines():
        where, n, kind = line.rsplit('\t', 2)
        stack[where.split(':')[-1]] = (int(n), kind)
deepest = sorted(stack.items(), key=lambda kv: -kv[1][0])[:8]
print('largest native frames:', ', '.join(f'{k}={v[0]}' for k, v in deepest))

for relative in ['include/fatfs_port.h', 'include/sunxi_mmc.h', 'drivers/storage/fatfs_port.c',
                 'drivers/storage/sunxi_mmc.c', 'lib/fatfs/ffconf.h', 'lib/fatfs/source.json', 'lib/fatfs/fatfs.cmake',
                 'checks/fatfs-port.c', 'checks/fatfs-port.py']:
    inputs[relative] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
(a.evidence / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
(a.evidence / 'results.json').write_text(json.dumps(dict(cases=results, max_blocks=max_blocks,
    native_undefined=sorted(undefined), largest_frames=deepest), indent=2) + '\n')
shutil.rmtree(a.evidence / 'images')  # Reproducible from this script; keeps evidence small.
print('PASS: %d host cases and native AArch64 compile; evidence retained at %s' % (len(results), a.evidence))
