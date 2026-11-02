#!/usr/bin/env python3
"""Check the runner's bounded card-write admission (tools/storage_bounds.py, policy() in tools/board_images.py).

Compiles a read-only image stand-in and a write-capable one, then runs the real policy() on each
submission shape. Only the complete, approved, port 3-1 submission with matching input is admitted.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    images = load('board_images_check', ROOT / 'tools/board_images.py')
    bounds = load('storage_bounds_check', ROOT / 'tools/storage_bounds.py')

    def build(name, source):
        c = a.output / f'{name}.c'
        c.write_text(source)
        o = a.output / f'{name}.o'
        r = subprocess.run(['/usr/bin/clang-21', '--target=aarch64-none-elf', '-c', '-O1', str(c), '-o', str(o)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        return o

    readonly = build('readonly', 'volatile int sunxi_mmc_read_only_build = 0x52444f4e;\nint sunxi_mmc_read(void) { return 1; }\n')
    writer = build('writer', 'int sunxi_mmc_write_multi(void) { return 1; }\n')
    fwr = build('fwr', 'int fwr_sync(void) { return 1; }\n')
    ranges = [[32768, 1], [62720, 64], [65536, 1048576]]
    digest = hashlib.sha256(b'backup').hexdigest()
    blob = a.output / 'bounds.bin'
    blob.write_bytes(bounds.encode_input([tuple(r) for r in ranges], digest))
    other = a.output / 'other.bin'
    other.write_bytes(blob.read_bytes()[:-1] + b'\x01')

    def submit(image, descriptor=None, *, approved='coordinator', flag=True, port='3-1', inputs=(blob,)):
        args = SimpleNamespace(approved_by=approved, storage_write=flag, port=port, image=image, input=list(inputs), host_tree=None)
        info = {'descriptor': descriptor or {}, 'symbols': images.symbols(image)}
        images.policy(info, args)

    count = [0]
    refused = harness.refused

    def counted(action, reason):
        count[0] += 1
        return refused(action, reason)
    harness.refused = counted
    good = {'storage_bounds': {'ranges': ranges, 'backup_sha256': digest}}
    # A read-only build passes with no flags, as before.
    submit(readonly, flag=False, approved=None, inputs=())
    harness.refused(lambda: submit(readonly, good, approved=None, flag=True, inputs=()), 'Storage write requires --approved-by coordinator')
    # A write-capable image without admission is refused, whatever else it carries.
    for image in (writer, fwr):
        harness.refused(lambda: submit(image, flag=False, approved=None, inputs=()), 'defines the card write path')
        harness.refused(lambda: submit(image, good, flag=False, inputs=()), 'defines the card write path')
        harness.refused(lambda: submit(image, good, approved=None), 'Storage write requires --approved-by coordinator')
        harness.refused(lambda: submit(image, good, port='3-3'), 'port 3-1 only')
        harness.refused(lambda: submit(image, {}), 'storage_bounds')
        harness.refused(lambda: submit(image, good, inputs=()), 'first input')
        harness.refused(lambda: submit(image, good, inputs=(other,)), 'first input')
    bad = {
        'below the partition': [[32767, 1]],
        'overlap': [[40000, 100], [40050, 10]],
        'unsorted': [[50000, 1], [40000, 1]],
        'empty range': [[40000, 0]],
        'too many sectors': [[40000, 1 << 22]],
        'empty list': [],
    }
    for name, rs in bad.items():
        harness.refused(lambda: submit(writer, {'storage_bounds': {'ranges': rs, 'backup_sha256': digest}}), '')
    for h in ('', 'ab', digest.upper(), digest[:-1]):
        harness.refused(lambda: submit(writer, {'storage_bounds': {'ranges': ranges, 'backup_sha256': h}}), 'backup hash')
    harness.refused(lambda: submit(writer, {'storage_bounds': {'ranges': ranges, 'backup_sha256': digest, 'extra': 1}}), 'exactly ranges')
    # The complete submission is admitted.
    submit(writer, good)
    submit(fwr, good)
    print(f'storage-bounds: read-only image unchanged; {count[0]} refusals; complete submission admitted')


if __name__ == '__main__':
    main()
