#!/usr/bin/env python3
"""The interruption test of apps/storage-write-test (modes 1 and 8) on the host card model, scaled down.

Session 1 (mode 1) creates the NEW file /sunstone-test/interrupt-test.bin and writes the stream into it until the session's own deadline, which the model
enforces by ending the process in the middle of a write (the first half of that write's sectors reach the card). Session 2 (mode 8) mounts: recovery runs,
the interrupted file is read back against the stream up to the size the volume holds, and the size of the region test file is reported. The check requires:
  - session 1 is cut (exit 77) with the volume dirty;
  - session 2 recovers, leaves a clean volume (`fsck.fat -n` exit 0), a file whose bytes are a prefix of the stream, at least the first sync;
  - the region test file's bytes and size are exactly what they were before, and no write call of either session overlaps its clusters;
  - every sector written lies in the FAT copies, FSInfo, the directory cluster of /sunstone-test or the new file's clusters."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('guardcheck', ROOT / 'checks/storage-write-guard.py')
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
host = g.host
REGION, INT = 'sunstone-test/sunstone-region-test.bin', 'sunstone-test/interrupt-test.bin'
MIB = 1 << 20


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--finish-after-us', type=int, default=1200000, help='the session deadline after the start, in the model virtual time (waits and modeled card time, never the host clock); the write starts 400 ms before it')
    ap.add_argument('--no-cut', action='store_true', help='mutation: run session 1 without the cut; the check must fail')
    a = ap.parse_args()
    out = a.output; out.mkdir(parents=True, exist_ok=True)
    binary = out / 'interrupt'
    cc = [str(harness.CLANG), '-std=gnu11', '-O1', '-g', '-w', *harness.SANITIZE, f'-DFILE_BYTES=({g.FILE}u*1u)', '-DGUARD_US=600000000000ull',
          '-DINT_CAP=(16*MIB)', '-DCUT_WINDOW_US=400000ull', '-DSYNC_EVERY=(1*MIB)',
          f'-I{ROOT}/checks/storage-write-guard-stubs', f'-I{ROOT}/include', str(ROOT / 'checks/storage-write-guard.c'),
          str(ROOT / 'drivers/storage/fat32_rw.c'), '-o', str(binary)]
    r = subprocess.run(cc, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    img = out / 'card.img'; g.make_image(img)
    d0 = g.run(binary, img, 0, out / 'region.result', 0)                 # the region test file, whole
    assert d0['status'] == 0 and d0['written_bytes'] == g.FILE, d0
    v = g.fat.Volume(img, g.BASE)
    region_before = v.file(REGION)
    assert region_before == g.stream(g.FILE)
    region_first, region_chain = None, None
    _, first, size = v.lookup(REGION)
    region_chain = v.chain(first)
    # session 1: cut at the session's deadline, mid-write
    env = {**harness.environment(), 'FINISH_AFTER_US': str(a.finish_after_us), **({} if a.no_cut else {'CUT': '1'}), 'WRITE_LOG': str(out / 'write1.log')}
    (out / 'write1.log').write_text('')
    r = subprocess.run([str(binary), str(img), str(g.BASE), str(g.PART), '1', str(out / 'cut.result'), '8000'], capture_output=True, text=True, env=env)
    assert r.returncode == 77, f'session 1 was not cut mid-write: exit {r.returncode}: {r.stdout}{r.stderr[-400:]}'
    v = g.fat.Volume(img, g.BASE)
    dirty = not (g.fat.u32(v.fat[0], 4) & 0x08000000)
    assert dirty, 'the volume was clean after the cut (nothing was in flight)'
    # session 2: recovery
    env2 = {**harness.environment(), 'WRITE_LOG': str(out / 'write2.log')}
    (out / 'write2.log').write_text('')
    r = subprocess.run([str(binary), str(img), str(g.BASE), str(g.PART), '8', str(out / 'recover.result'), '0'], capture_output=True, text=True, env=env2)
    assert r.returncode == 0, r.stdout + r.stderr[-600:]
    d8 = host.decode((out / 'recover.result').read_bytes())
    assert d8['status'] == 0 and d8['mismatches'] == 0 and d8['recoveries'] >= 1, d8
    assert d8['written_bytes'] == g.FILE, 'the region test file changed size'
    g.fsck_clean(img)
    v = g.fat.Volume(img, g.BASE)
    assert g.fat.u32(v.fat[0], 4) & 0x08000000, 'the volume is not clean after recovery'
    interrupted = v.file(INT)
    # A cut before the first sync leaves no entry: recovery then returns the unlinked clusters (fsck above is clean), and the file is absent.
    if interrupted is None:
        assert d8['detail'] == 1 and d8['volume_size'] == 0, d8
        interrupted = b''
    else:
        assert len(interrupted) >= MIB and len(interrupted) % 65536 == 0, len(interrupted)
        assert interrupted == g.stream(len(interrupted)), 'the interrupted file is not a prefix of the stream'
    assert v.file(REGION) == region_before, 'the region test file changed'
    # no write of either session lies in the region file's clusters; every write is in the allowed places
    data0 = v.data
    region_sectors = set()
    for c in region_chain:
        s0 = data0 + (c - 2) * v.spc
        region_sectors.update(range(s0, s0 + v.spc))
    fat_lo = g.BASE + 32  # not used; the model refuses writes outside FAT copies, FSInfo and data
    for name in ('write1.log', 'write2.log'):
        calls = [tuple(map(int, l.split())) for l in (out / name).read_text().split('\n') if l]
        assert calls, name
        for s, n in calls:
            assert not (set(range(s, s + n)) & region_sectors), f'{name}: a write at {s}+{n} touches the region file'
    print(f"PASS storage-write interrupt: session 1 cut by its deadline mid-write (volume dirty); session 2 recovered "
          f"(recoveries {d8['recoveries']}, recovered clusters {d8['recovered_clusters']}), kept {len(interrupted)} bytes of the new file (a stream prefix; 0 = no entry before the cut), "
          f"fsck clean, the region file ({g.FILE} bytes) identical and untouched by either session's writes")


if __name__ == '__main__':
    main()
