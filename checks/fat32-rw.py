#!/usr/bin/env python3
"""Check the FAT32 write service: power-loss sweep, recovery, write accounting and the write guard.

The host model (checks/fat32-rw.c) runs a scenario against a mkfs.fat volume inside a card image
(the partition starts at sector 32768, as on the board) and logs every sector write. A power cut
after k sector writes leaves the first k logged sectors on the card (the model persists a write call's
sectors in ascending order; a card may persist the sectors of one call in another order, so a random
subset of each multi-sector call is also tested). For every cut the check:
  1. runs the service's mount-time recovery on the cut image,
  2. requires `fsck.fat -n` to report a clean volume (exit status 0),
  3. reads the volume with an independent Python FAT32 reader and requires every file to match the
     constraints the scenario recorded: synced bytes intact, a replaced or renamed file whole old or
     whole new, and
  4. requires a second mount to write nothing.
A sample of cuts also cuts the recovery itself at every sector write and recovers again.
"""
import argparse
import multiprocessing
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
BASE, PART_SECTORS, AU, MAXW = 32768, 73728, 128, 64
CLANG = harness.CLANG


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


def load_marks(path):
    groups = {}
    end = None
    for line in Path(path).read_text().splitlines():
        t = line.split()
        if t[0] == 'END':
            end = int(t[1])
        else:
            _, seq, idx, name, lo, hi, ref, absent = t
            groups.setdefault(name, []).append((int(seq), int(idx), int(lo), int(hi), ref, int(absent)))
    return groups, end


def active(groups, k):
    """Per path, the alternatives of the latest group that started at or before write index k."""
    out = {}
    for name, rows in groups.items():
        live = [r for r in rows if r[1] <= k]
        if live:
            seq = max(r[0] for r in live)
            out[name] = [r for r in live if r[0] == seq]
    return out


def load_log(path):
    raw = Path(path).read_bytes()
    rec = 8 + 8 + 512
    out = []
    for o in range(0, len(raw), rec):
        sec = struct.unpack_from('<Q', raw, o)[0]
        call = struct.unpack_from('<Q', raw, o + 8)[0] >> 32
        out.append((sec, call, raw[o + 16:o + rec]))
    return out


def apply(img, entries):
    with open(img, 'r+b') as f:
        for sec, _, data in entries:
            f.seek(sec * 512)
            f.write(data)


def fsck_clean(img, workdir):
    part = workdir / 'part.img'
    subprocess.run(['dd', f'if={img}', f'of={part}', 'bs=512', f'skip={BASE}', f'count={PART_SECTORS}', 'status=none'], check=True)
    r = subprocess.run([FSCK, '-n', str(part)], capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr


def verify(img, groups, k, refs, workdir, where):
    ok, text = fsck_clean(img, workdir)
    assert ok, f'{where}: fsck.fat -n failed:\n{text}'
    vol = Volume(img, BASE)
    state = {}
    for name, alts in active(groups, k).items():
        data = vol.file(name)
        state[name] = data
        good = False
        for _, _, lo, hi, ref, absent in alts:
            if data is None:
                good |= bool(absent)
            elif lo <= len(data) <= hi and data == refs[ref][:len(data)]:
                good = True
        assert good, f'{where}: {name} has {None if data is None else len(data)} bytes, not allowed by {alts}'
    old, tmp = state.get('old.bin'), state.get('tmp1.bin')
    if old is not None and old == refs['tmp1']:
        assert tmp is None, f'{where}: both names of a renamed file survive'
    return state


def recover(binary, img, cut_log=None, cut=None):
    cmd = [str(binary), 'recover', str(img), str(BASE), str(PART_SECTORS), str(AU)]
    if cut_log:
        cmd += [str(cut_log)] + ([str(cut)] if cut is not None else [])
    r = subprocess.run(cmd, capture_output=True, text=True, env=harness.environment())
    assert r.returncode == 0, f'recover failed: {r.stdout}{r.stderr}'
    return r.stdout


def check_hint(pre, post, out):
    """After a recovery that rebuilt the free count, the FSInfo next-free hint is the old hint while that cluster is
    free, else the first cluster after the highest used one, never the start of the FAT because that is where a
    full scan finds the lowest hole."""
    if 'recoveries=1' not in out:
        return
    old = struct.unpack_from('<I', Volume(pre, BASE).read(BASE + u16(Volume(pre, BASE).read(BASE, 1), 48), 1), 492)[0]
    v = Volume(post, BASE)
    info = v.read(BASE + u16(v.read(BASE, 1), 48), 1)
    free, hint = struct.unpack_from('<II', info, 488)
    entries = (PART_SECTORS - (v.data - BASE)) // v.spc + 2
    fat = [u32(v.fat[0], 4 * c) & 0x0fffffff for c in range(entries)]
    used = [c for c in range(2, entries) if fat[c]]
    expect = old if 2 <= old < entries and fat[old] == 0 else (max(used) + 1 if used and max(used) + 1 < entries else None)
    assert expect is None or hint == expect, f'recovery set the hint to {hint}, expected {expect} (old {old})'
    assert hint >= 2 and (hint >= entries or fat[hint] == 0), f'the hint {hint} is not a free cluster'


def worker(args):
    binary, base_img, log, marks, work, lo, hi, double_every, seed = args
    work = Path(work) / f'w{lo}'
    work.mkdir(parents=True, exist_ok=True)
    groups, end = load_marks(marks)
    refs = {p.name: p.read_bytes() for p in (Path(work).parent.parent / 'ref').iterdir()}
    entries = load_log(log)
    prefix = work / 'prefix.img'
    harness.copy_image(base_img, prefix)
    apply(prefix, entries[:lo])
    rng = random.Random(seed + lo)
    done = recovered_clusters = doubles = subsets = 0
    for k in range(lo, hi + 1):
        if k > lo:
            apply(prefix, entries[k - 1:k])
        cut = work / 'cut.img'
        harness.copy_image(prefix, cut)
        pre = work / 'pre.img'
        harness.copy_image(cut, pre)
        check_hint(pre, cut, recover(binary, cut))
        verify(cut, groups, k, refs, work, f'cut after {k} sector writes')
        again = recover(binary, cut)
        assert 'sector_writes=0 ' in again, f'second mount wrote: {again}'
        done += 1
        # A call that persisted a random subset of its sectors.
        if 0 < k < len(entries) and entries[k][1] == entries[k - 1][1]:
            call = entries[k][1]
            members = [e for e in entries if e[1] == call]
            first = next(i for i, e in enumerate(entries) if e[1] == call)
            for _ in range(2):
                keep = [e for e in members if rng.random() < 0.5]
                harness.copy_image(base_img, cut)
                apply(cut, entries[:first])
                apply(cut, keep)
                recover(binary, cut)
                verify(cut, groups, first, refs, work, f'random subset of call {call} after {first} sector writes')
                subsets += 1
        if double_every and k % double_every == 0:
            relog = work / 'recovery.log'
            harness.copy_image(pre, cut)
            out = recover(binary, cut, relog)
            rentries = load_log(relog)
            for j in range(len(rentries) + 1):
                harness.copy_image(pre, cut)
                apply(cut, rentries[:j])
                recover(binary, cut)
                verify(cut, groups, k, refs, work, f'recovery cut after {j} of {len(rentries)} sector writes (initial cut {k})')
                doubles += 1
    return done, subsets, doubles


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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--library', type=Path, default=ROOT / 'drivers/storage/fat32_rw.c', help='service source (mutation checks pass a modified copy)')
    ap.add_argument('--double-every', type=int, default=40)
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binary = out / 'fat32-rw'
    (out / 'host_sys.h').write_text('#define SS_SYS(name) host_##name\n')
    run([str(CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *harness.SANITIZE,
         '-I', str(ROOT / 'include'), '-include', str(out / 'host_sys.h'), str(ROOT / 'checks/fat32-rw.c'), str(a.library),
         str(ROOT / 'drivers/storage/sunstone_fs.c'), str(ROOT / 'drivers/storage/sunstone_fs_syscalls.c'),
         '-o', str(binary)])
    (out / 'ref').mkdir()
    (out / 'work').mkdir()
    base = out / 'base.img'
    with open(base, 'wb') as f:
        f.truncate(BASE * 512 + PART_SECTORS * 512)
    subprocess.run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', '1', '-n', 'SUNSTONE', '--offset', str(BASE), str(base),
                    str(PART_SECTORS // 2)], check=True, capture_output=True)
    img = out / 'run.img'
    harness.copy_image(base, img)
    log, marks = out / 'write.log', out / 'marks.txt'
    others = static_checks(binary, base, out)
    snap = out / 'setup.img'
    r = subprocess.run([str(binary), 'scenario', str(img), str(BASE), str(PART_SECTORS), str(log), str(marks),
                        str(out / 'ref'), str(AU), str(MAXW), str(snap)], capture_output=True, text=True,
                       env=harness.environment())
    assert r.returncode == 0, r.stdout + r.stderr
    print(r.stdout.strip())
    # The finished run is clean without recovery: a second mount writes nothing.
    ok, text = fsck_clean(img, out)
    assert ok, text
    prior = recover(binary, img)
    assert 'sector_writes=0 ' in prior, prior
    groups, end = load_marks(marks)
    total = end
    step = (total + a.jobs) // a.jobs
    jobs = [(binary, snap, log, marks, out / 'work', i, min(i + step - 1, total), a.double_every, 1)
            for i in range(0, total + 1, step)]
    with multiprocessing.Pool(len(jobs)) as pool:
        results = pool.map(worker, jobs)
    cuts = sum(r[0] for r in results)
    print(f'power-loss sweep: {cuts} cuts, {sum(r[1] for r in results)} random subsets, '
          f'{sum(r[2] for r in results)} recovery cuts, all fsck.fat -n clean with synced bytes intact')


if __name__ == '__main__':
    main()
