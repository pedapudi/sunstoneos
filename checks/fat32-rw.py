#!/usr/bin/env python3
"""Check the FAT32 write service's crash-consistency order, its recovery at each step of that order, the write guard and the write accounting.

The writer (drivers/storage/fat32_rw.c, include/fat32_rw.h) claims that a power cut at any point of a session leaves a volume that
mount-time recovery repairs. It holds because a session changes the card in a fixed order, each step harmless until the next one
makes it reachable:
  1. data clusters are written to clusters that the FAT still shows free (the service reserves them in memory only), so a cut here
     leaves no trace;
  2. at the first sync the dirty flag (FAT entry 1, clean bit) is cleared, in FAT copy 1 and then copy 2, so that a cut from here
     on is seen at the next mount;
  3. the file's FAT chain is written, copy 1 then copy 2; until the directory entry exists the chain is an orphan that recovery
     frees;
  4. the directory entry is written; the file is now present with its synced size;
  5. the FSInfo free count and next-free hint are written (they are hints, recomputed by recovery);
  6. at unmount the dirty flag is set again (clean), copy 2 then copy 1, so mount reads the flag from the copy written last.
The specification the code follows moves step 1 below step 2 (data after the flag); the code writes the data first because the
clusters are unreachable until step 3, which makes the early flag unnecessary for them. This check runs the one session of
create, write across several clusters, sync, close, unmount; asserts that the observed order of the writes is the order above
(a reordering such as the directory entry before the FAT chain fails here); cuts the session at each boundary between the steps
and at the boundary between the two FAT copies of a step (ten cuts); and for each cut runs the service's own recovery in memory
(`serve` mode of checks/fat32-rw.c, one open image), then asserts: the file is absent before step 4 and whole after it; the dirty
flag is clean; the two FAT copies agree; every allocated cluster belongs to the root directory or to the file (no lost cluster);
FSInfo free count equals the free clusters; a second mount writes nothing; and fsck.fat -n reports a clean volume.
Then: the write guard, POSIX-like calls through the newlib system calls, and write-size accounting.
Usage: fat32-rw.py --output DIR
"""
import argparse
import multiprocessing
import os
from pathlib import Path
import random
import resource
import shutil
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
BASE, PART_SECTORS, AU, MAXW = 32768, 73728, 128, 64
SIZE = 3000                      # bytes written by the session: six clusters of 512 bytes
CLEAN_BIT = 0x08000000
NAME = 'SESSION.BIN'
CLANG = harness.CLANG
# The order of a session's writes, as (kind, FAT copy or 0): see the docstring.
EXPECTED = [('DATA', 0), ('FLAG_DIRTY', 1), ('FLAG_DIRTY', 2), ('FAT', 1), ('FAT', 2), ('DIRENT', 0), ('FSINFO', 0),
            ('FLAG_CLEAN', 2), ('FLAG_CLEAN', 1)]


def run(command, **kwargs):
    r = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    if r.returncode:
        raise SystemExit(f'{command}: {r.stdout}{r.stderr}')
    return r.stdout
FSCK = '/usr/sbin/fsck.fat'


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]


class Volume:
    """Independent read-only FAT32 reader over a card image."""

    def __init__(self, path, base):
        self.f = open(path, 'rb')
        b = self.read(base, 1)
        assert u16(b, 510) == 0xaa55 and u16(b, 11) == 512
        self.spc, rsvd, nfats, self.fsz = b[13], u16(b, 14), b[16], u32(b, 36)
        self.root = u32(b, 44)
        self.fat = [self.read(base + rsvd + i * self.fsz, self.fsz) for i in range(nfats)]
        self.data = base + rsvd + nfats * self.fsz
        self.csize = self.spc * 512

    def read(self, sector, count):
        self.f.seek(sector * 512)
        return self.f.read(count * 512)

    def chain(self, c):
        out = []
        while 2 <= c < 0x0ffffff7 and len(out) < 1 << 21:
            out.append(c)
            c = u32(self.fat[0], 4 * c) & 0x0fffffff
        return out

    def content(self, first, size):
        data = b''.join(self.read(self.data + (c - 2) * self.spc, self.spc) for c in self.chain(first))
        return data[:size]

    def listing(self, first):
        """Yield (name, attr, first_cluster, size) of a directory."""
        raw = self.content(first, 1 << 30) if False else b''.join(
            self.read(self.data + (c - 2) * self.spc, self.spc) for c in self.chain(first))
        lfn = {}
        for o in range(0, len(raw), 32):
            e = raw[o:o + 32]
            if e[0] == 0:
                return
            if e[0] == 0xe5:
                lfn = {}
                continue
            if e[11] & 0x3f == 0x0f:
                seq = e[0] & 0x3f
                chars = e[1:11] + e[14:26] + e[28:32]
                lfn[seq] = chars.decode('utf-16le')
                continue
            if e[11] & 0x08 or e[0] == 0x2e:
                lfn = {}
                continue
            if lfn:
                name = ''.join(lfn[k] for k in sorted(lfn)).split('\0')[0].split('￿')[0]
            else:
                stem, ext = e[0:8].decode('ascii').rstrip(), e[8:11].decode('ascii').rstrip()
                name = stem + ('.' + ext if ext else '')
                if e[12] & 0x08:
                    stem = stem.lower()
                if e[12] & 0x10:
                    ext = ext.lower()
                name = stem + ('.' + ext if ext else '')
            lfn = {}
            yield name, e[11], u16(e, 20) << 16 | u16(e, 26), u32(e, 28)

    def lookup(self, path):
        d = self.root
        parts = path.split('/')
        for i, p in enumerate(parts):
            for name, attr, first, size in self.listing(d):
                if name.lower() == p.lower():
                    if i + 1 == len(parts):
                        return attr, first, size
                    d = first or self.root
                    break
            else:
                return None
        return None

    def file(self, path):
        hit = self.lookup(path)
        if hit is None:
            return None
        attr, first, size = hit
        assert not attr & 0x10, path
        return self.content(first, size)


def load_log(path):
    raw = Path(path).read_bytes()
    rec = 8 + 8 + 512
    out = []
    for o in range(0, len(raw), rec):
        sec = struct.unpack_from('<Q', raw, o)[0]
        call = struct.unpack_from('<Q', raw, o + 8)[0] >> 32
        out.append((sec, call, raw[o + 16:o + rec]))
    return out


def fsck_clean(img, workdir):
    part = workdir / 'part.img'
    # 1 MiB blocks: the same bytes as bs=512 with a seventy-thousandth of the system calls; holes stay holes.
    subprocess.run(['dd', f'if={img}', f'of={part}', 'bs=1M', 'iflag=skip_bytes,count_bytes', f'skip={BASE * 512}',
                    f'count={PART_SECTORS * 512}', 'conv=sparse', 'status=none'], check=True)
    r = subprocess.run([FSCK, '-n', str(part)], capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr


class Server:
    """The model's mount recovery on one open image (checks/fat32-rw.c, `serve`): recover, then undo to restore the sectors the
    recovery changed, so no case copies the image."""

    def __init__(self, binary, img):
        self.process = subprocess.Popen([str(binary), 'serve', str(img), str(BASE), str(PART_SECTORS), str(AU)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, env=harness.environment())

    def ask(self, line):
        self.process.stdin.write(line + '\n')
        self.process.stdin.flush()
        reply = self.process.stdout.readline()
        assert reply and 'failed' not in reply and 'unknown' not in reply, f'recover failed: {reply!r}'
        return reply

    def recover(self, log=None):
        return self.ask('recover' + (f' {log}' if log else ''))

    def undo(self):
        self.ask('undo')

    def close(self):
        self.process.stdin.close()
        self.process.wait()



def phases(entries, base_img):
    """The session's writes as consecutive (kind, FAT copy) phases, with the number of writes that end each phase."""
    boot = Path(base_img).read_bytes()[BASE * 512:BASE * 512 + 512]
    reserved, fats, fsz, info, spc, root = u16(boot, 14), boot[16], u32(boot, 36), u16(boot, 48), boot[13], u32(boot, 44)
    fat1, data = BASE + reserved, BASE + reserved + fats * fsz
    root_sectors = range(data + (root - 2) * spc, data + (root - 1) * spc)
    with open(base_img, 'rb') as stream:
        def sector(n):
            stream.seek(n * 512)
            return stream.read(512)
        current, kinds = {}, []
        for sec, _, data_bytes in entries:
            before = current.get(sec) or sector(sec)
            current[sec] = data_bytes
            if sec == BASE + info:
                kind = ('FSINFO', 0)
            elif fat1 <= sec < data:
                copy = 1 + (sec - fat1) // fsz
                changed = [i for i in range(128) if before[4 * i:4 * i + 4] != data_bytes[4 * i:4 * i + 4]]
                if changed == [1] and (sec - fat1) % fsz == 0:
                    kind = ('FLAG_CLEAN' if u32(data_bytes, 4) & CLEAN_BIT else 'FLAG_DIRTY', copy)
                else:
                    kind = ('FAT', copy)
            elif sec in root_sectors:
                kind = ('DIRENT', 0)
            else:
                kind = ('DATA', 0)
            kinds.append(kind)
    result = []
    for number, kind in enumerate(kinds, 1):
        if result and result[-1][0] == kind:
            result[-1][1] = number
        else:
            result.append([kind, number])
    return [(kind, end) for kind, end in result]


def lost_clusters_and_copies(img):
    """(allocated clusters outside the root directory and the file's chain, whether the FAT copies agree, clean flag, free count)."""
    vol = Volume(img, BASE)
    entries = (PART_SECTORS - (vol.data - BASE)) // vol.spc + 2
    allocated = {c for c in range(2, entries) if u32(vol.fat[0], 4 * c) & 0x0fffffff}
    hit = vol.lookup(NAME)
    owned = set(vol.chain(vol.root)) | (set(vol.chain(hit[1])) if hit else set())
    info = vol.read(BASE + u16(vol.read(BASE, 1), 48), 1)
    free = sum(1 for c in range(2, entries) if not u32(vol.fat[0], 4 * c) & 0x0fffffff)
    return allocated - owned, vol.fat[0] == vol.fat[1], bool(u32(vol.fat[0], 4) & CLEAN_BIT and u32(vol.fat[1], 4) & CLEAN_BIT), \
        u32(info, 488), free


def cuts(binary, base, session_log, work):
    entries = load_log(session_log)
    seen = phases(entries, base)
    print('observed order of the session\'s %d sector writes: %s' % (len(entries), ' '.join('%s%s' % (k, c or '') for (k, c), _ in seen)))
    assert [kind for kind, _ in seen] == EXPECTED, f'the session writes in this order: {seen}\nthe documented order is: {EXPECTED}'
    dirent_done = next(end for (kind, _), end in seen if kind == 'DIRENT')
    img = work / 'cut.img'
    harness.copy_image(base, img)
    server = Server(binary, img)
    boundaries = [0] + [end for _, end in seen]
    fd = os.open(img, os.O_RDWR)
    results, applied = [], 0
    try:
        for cut in boundaries:
            for sec, _, data in entries[applied:cut]:
                os.pwrite(fd, data, sec * 512)
            applied = cut
            where = f'cut after {cut} of {len(entries)} sector writes'
            out = server.recover()
            check = work / 'check.img'
            harness.copy_image(img, check)                    # one image per cut: ten in all
            lost, same, clean, hint_free, free = lost_clusters_and_copies(img)
            assert same, f'{where}: the FAT copies differ after recovery'
            assert clean, f'{where}: the dirty flag is still set after recovery'
            assert not lost, f'{where}: lost clusters {sorted(lost)[:8]}'
            assert hint_free in (free, 0xffffffff), f'{where}: FSInfo says {hint_free} free clusters, the FAT has {free}'
            vol = Volume(img, BASE)
            hit = vol.lookup(NAME)
            if cut < dirent_done:
                assert hit is None, f'{where}: the file exists before its directory entry was written'
            else:
                assert hit is not None and vol.file(NAME) == bytes(stream_bytes(SIZE)), f'{where}: the synced file is not whole'
            ok, text = fsck_clean(img, work)
            assert ok, f'{where}: fsck.fat -n failed:\n{text}'
            again = server.recover()
            assert 'sector_writes=0 ' in again, f'{where}: a second mount wrote: {again}'
            server.undo()
            results.append((cut, out.strip()))
            print(f'PASS {where}: {out.strip()}')
    finally:
        server.close()
        os.close(fd)
    return len(results)


def stream_bytes(n, seed=7):
    """The bytes the session writes (stream_fill of checks/fat32-rw.c)."""
    out = bytearray()
    for i in range(n):
        x = ((i >> 3) + seed * 0x9e3779b97f4a7c15) & 0xffffffffffffffff
        x ^= x >> 30
        x = (x * 0xbf58476d1ce4e5b9) & 0xffffffffffffffff
        x ^= x >> 27
        x = (x * 0x94d049bb133111eb) & 0xffffffffffffffff
        x ^= x >> 31
        out.append((x >> (8 * (i & 7))) & 0xff)
    return out


def static_checks(binary, base, out):
    """Write guard, POSIX-like calls through the newlib system calls, and write-size accounting."""
    results = {}
    for name in ('guard', 'posix', 'accounting'):
        img = out / f'{name}.img'
        harness.copy_image(base, img)
        r = subprocess.run([str(binary), name, str(img), str(BASE), str(PART_SECTORS), str(AU)], capture_output=True,
                           text=True, env=harness.environment())
        assert r.returncode == 0, f'{name}: {r.stdout}{r.stderr}'
        if name != 'guard':
            ok, text = fsck_clean(img, out)
            assert ok, f'{name}: fsck.fat -n failed:\n{text}'
        results[name] = r.stdout
        print(r.stdout.strip())
    fields = dict(kv.split('=') for kv in results['accounting'].split('\n')[0].split() if '=' in kv)
    full_units = int(fields['bytes']) // (AU * 512)
    assert int(fields['data_writes_before_close']) <= full_units + 1, fields
    assert fields['unaligned_ends'] == '0' and fields['writes_without_yield'] == '0', fields
    assert int(fields['total_writes']) <= full_units + 12, fields
    return results


def recover(binary, img):
    r = subprocess.run([str(binary), 'recover', str(img), str(BASE), str(PART_SECTORS), str(AU)], capture_output=True, text=True,
                       env=harness.environment())
    assert r.returncode == 0, f'recover failed: {r.stdout}{r.stderr}'
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--library', type=Path, default=ROOT / 'drivers/storage/fat32_rw.c', help='service source (mutation checks pass a modified copy)')
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    work = harness.ram_directory('fat32-rw-')      # the images live in RAM and are removed on exit, error and SIGTERM
    binary = work / 'fat32-rw'
    (work / 'host_sys.h').write_text('#define SS_SYS(name) host_##name\n')
    run([str(CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *harness.SANITIZE,
         '-I', str(ROOT / 'include'), '-include', str(work / 'host_sys.h'), str(ROOT / 'checks/fat32-rw.c'), str(a.library),
         str(ROOT / 'drivers/storage/sunstone_fs.c'), str(ROOT / 'drivers/storage/sunstone_fs_syscalls.c'),
         '-o', str(binary)])
    base = work / 'base.img'
    with open(base, 'wb') as f:
        f.truncate(BASE * 512 + PART_SECTORS * 512)
    subprocess.run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', '1', '-n', 'SUNSTONE', '--offset', str(BASE), str(base),
                    str(PART_SECTORS // 2)], check=True, capture_output=True)
    static_checks(binary, base, work)
    img, log = work / 'session.img', work / 'session.log'
    harness.copy_image(base, img)
    print(run([binary, 'session', img, BASE, PART_SECTORS, log, AU, SIZE], env=harness.environment()).strip())
    count = cuts(binary, base, log, work)
    harness.save(out / 'result.json', {'physical_access': False, 'cuts': count, 'order': [list(k) for k in EXPECTED]})
    print(f'PASS FAT32 write service: the session writes in the documented order and recovery is correct at all {count} cuts')


if __name__ == '__main__':
    main()
