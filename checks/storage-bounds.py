#!/usr/bin/env python3
"""Check the runner's bounded card-write admission (tools/storage_bounds.py, policy() in tools/board_images.py).

Builds linked AArch64 images, classifies them (read-only, no SD core, write-capable), runs the real policy()
on every submission shape against coordinator approval records in a temporary directory, and follows the
first input through the real queue (submit, frozen inventory, claim, post-freeze verification).
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
from types import SimpleNamespace
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import board
import board_images as images
import storage_bounds as bounds


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    count = [0]
    refused = harness.refused

    def no(action, reason):
        count[0] += 1
        return refused(action, reason)

    def link(name, source):
        c, elf = out / f'{name}.c', out / f'{name}.elf'
        c.write_text(source)
        r = subprocess.run(['/usr/bin/clang-21', '--target=aarch64-none-elf', '-ffreestanding', '-nostdlib', '-O1', '-g',
                            '-fuse-ld=lld', f'--ld-path={harness.LLD}', '-Wl,-Ttext=0x41000000', '-Wl,--build-id=none',
                            str(c), '-o', str(elf)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        return elf

    base = bounds.smhc0_base()
    assert base == 0x04020000 and bounds.BASE_SOURCE.name == 'a733_mmc_platform.h'
    # Classification.
    readonly = link('readonly', 'volatile const unsigned sunxi_mmc_read_only_build = 0x52444f4e;\n'
                                'int sunxi_mmc_read(void) { return 1; }\nvoid _start(void) {}\n')
    nocore = link('nocore', 'void _start(void) {}\n')
    table = link('table', 'const unsigned masters[2] = {0x04020000, 1};\nvoid _start(void) { (void)masters; }\n')
    blob = link('blob', 'const unsigned weights[2048] = {0x04020000, 1};\nvoid _start(void) { (void)weights; }\n')
    named = link('named', 'int sunxi_mmc_write(void) { return 1; }\nvoid _start(void) {}\n')
    renamed = link('renamed', 'void quiet(void) { *(volatile unsigned *)0x04020000 = 1; }\nvoid _start(void) { quiet(); }\n')
    literal = link('literal', 'const unsigned window[4] = {0x04020000, 0x04020100};\nvoid _start(void) { (void)window; }\n')
    cls = {name: bounds.classify(path) for name, path in
           [('readonly', readonly), ('nocore', nocore), ('table', table), ('blob', blob), ('named', named),
            ('renamed', renamed), ('literal', literal)]}
    assert cls == {'readonly': 'read-only', 'nocore': 'no-core', 'table': 'no-core', 'blob': 'no-core',
                   'named': 'write-capable', 'renamed': 'write-capable', 'literal': 'write-capable'}, cls
    raw = out / 'raw.bin'
    raw.write_bytes(b'MODEL ONLY')
    assert bounds.classify(raw) == 'write-capable'
    # The read-only image must still pass the repository's own scan.
    assert not bounds._scan_module().scan(readonly.read_bytes()), 'read-only stand-in fails the write-path scan'

    # A card backup: sector records, the boot sector first, in a partition of 1,000,000 sectors.
    boot = bytearray(512)
    struct.pack_into('<I', boot, 32, 1_000_000)
    boot[510:512] = b'\x55\xaa'
    backup = out / 'backup.bin'
    backup.write_bytes(struct.pack('<Q', 32768) + bytes(boot) + struct.pack('<Q', 32769) + bytes(512))
    digest = hashlib.sha256(backup.read_bytes()).hexdigest()
    end = 32768 + 1_000_000
    ranges = [[32768, 1], [62720, 64], [65536, 900000]]          # far beyond the earlier 1 GiB cap
    blob_path = out / 'bounds.bin'
    blob_path.write_bytes(bounds.encode_input([tuple(r) for r in ranges], digest))
    other = out / 'other.bin'
    other.write_bytes(blob_path.read_bytes()[:-1] + b'\x01')
    approvals = out / 'approvals'
    approvals.mkdir()

    def record(rs=ranges, h=digest, bk=backup, port='3-1', image=bounds.ANY_IMAGE, directory=approvals):
        name = bounds.encode_input([tuple(r) for r in rs], h)
        path = bounds.approval_path(name, directory)
        path.write_text(json.dumps({'ranges': rs, 'backup_sha256': h, 'backup': str(bk), 'port': port, 'image': image}))
        return path

    def submit(image, descriptor=None, *, flag=True, port='3-1', inputs=(blob_path,), directory=approvals, approved='coordinator'):
        args = SimpleNamespace(approved_by=approved, storage_write=flag, port=port, image=image, input=list(inputs), host_tree=None)
        with patch.dict(os.environ, {'SUNSTONE_STORAGE_APPROVALS': str(directory)}):
            images.policy({'descriptor': descriptor or {}, 'symbols': {}}, args)
        return args

    good = {'storage_bounds': {'ranges': ranges, 'backup_sha256': digest}}
    # Read-only images and images without an SD core pass with no flags and no records.
    for image in (readonly, nocore, table, blob):
        submit(image, flag=False, approved=None, inputs=())
    # Everything else needs admission: no flag, no record, no descriptor, wrong input, wrong port.
    for image in (named, renamed, literal, raw):
        no(lambda: submit(image, flag=False, approved=None, inputs=()), 'needs --storage-write')
        no(lambda: submit(image, good), 'No coordinator approval record')
        record()
        no(lambda: submit(image, good, port='3-3'), 'port 3-1 only')
        no(lambda: submit(image, {}), 'storage_bounds')
        no(lambda: submit(image, good, inputs=()), 'first input')
        no(lambda: submit(image, good, inputs=(other,)), 'first input')
        no(lambda: submit(image, good, directory=out / 'empty'), 'No coordinator approval record')
        submit(image, good)                      # the approval record admits it
        submit(image, good, approved=None)       # --approved-by plays no part: the record is the approval
        (approvals / ('storage-' + hashlib.sha256(blob_path.read_bytes()).hexdigest() + '.json')).unlink()
    # The record must match the descriptor, the port, the image and the partition.
    sha = images.sha(named)
    path = record(image='deadbeef')
    no(lambda: submit(named, good), 'does not name this image')
    record(image=sha)
    submit(named, good)
    record(port='3-3')
    no(lambda: submit(named, good), 'another port')
    record()
    for name, rs in {'below the partition': [[32767, 1]], 'overlap': [[40000, 100], [40050, 10]], 'unsorted': [[50000, 1], [40000, 1]],
                     'empty range': [[40000, 0]], 'past the partition end': [[end - 1, 2]], 'empty list': []}.items():
        d = {'storage_bounds': {'ranges': rs, 'backup_sha256': digest}}
        blob2 = out / 'b2.bin'
        blob2.write_bytes(bounds.encode_input([tuple(r) for r in rs], digest))
        record(rs=rs)
        no(lambda: submit(named, d, inputs=(blob2,)), '')
    record(rs=[[end - 2, 2]])                    # the last two sectors of the partition are admitted
    submit(named, {'storage_bounds': {'ranges': [[end - 2, 2]], 'backup_sha256': digest}},
           inputs=(_write(out / 'b3.bin', bounds.encode_input([(end - 2, 2)], digest)),))
    # The backup the record names must match its hash and carry the boot sector.
    missing = out / 'no-boot.bin'
    missing.write_bytes(struct.pack('<Q', 32769) + bytes(512))
    mh = hashlib.sha256(missing.read_bytes()).hexdigest()
    dm = {'storage_bounds': {'ranges': ranges, 'backup_sha256': mh}}
    bm = _write(out / 'bm.bin', bounds.encode_input([tuple(r) for r in ranges], mh))
    record(h=mh, bk=missing)
    no(lambda: submit(named, dm, inputs=(bm,)), 'boot sector')
    record(h=mh, bk=backup)
    no(lambda: submit(named, dm, inputs=(bm,)), 'differs from the approved hash')
    for h in ('', 'ab', digest.upper(), digest[:-1]):
        no(lambda: submit(named, {'storage_bounds': {'ranges': ranges, 'backup_sha256': h}}), 'Invalid storage_bounds')
    no(lambda: submit(named, {'storage_bounds': {'ranges': ranges, 'backup_sha256': digest, 'extra': 1}}), 'storage_bounds')
    record()
    submit(named, good)

    # Lifecycle: the real queue freezes the first input; a changed byte or a swapped blob cannot run.
    queue_root = out / 'queue'
    source = out / 'source.elf'
    source.write_bytes(named.read_bytes())
    parser = argparse.ArgumentParser()
    board.run_options(parser, True)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--worker', default='storage-write')
    parser.add_argument('--hypothesis', default='bounds reach the image unchanged')
    argv = [str(source), '--port', '3-1', '--evidence', str(out / 'run'), '--kind', 'probe', '--expect', 'complete',
            '--reference', 'docs/board-runner.md#submit-a-frozen-image-and-hypothesis', '--storage-write',
            '--approved-by', 'coordinator', '--preserve-dram']
    with patch.object(board, 'port_idle', return_value=True):
        queue = board.Queue(queue_root)
        job = queue.submit(parser.parse_args(argv + ['--input', str(blob_path)]))
        entry = Path(job['directory'])
        frozen_input = Path(job['options']['input'][0])
        assert frozen_input.read_bytes() == blob_path.read_bytes(), 'the queue froze other bytes than the admitted blob'
        assert job['frozen'][str(frozen_input.relative_to(entry))] == images.sha(blob_path), 'the frozen inventory omits the input'
        # Policy sees the frozen copy, and the post-freeze check passes on it.
        args = SimpleNamespace(approved_by='coordinator', storage_write=True, port='3-1', image=named, input=[frozen_input], host_tree=None)
        with patch.dict(os.environ, {'SUNSTONE_STORAGE_APPROVALS': str(approvals)}):
            images.policy({'descriptor': good, 'symbols': {}}, args)
        bounds.verify_delivered(args)
        # A swap after admission is caught by verify_delivered ...
        args.input = [other]
        no(lambda: bounds.verify_delivered(args), 'differs from the admitted storage bounds')
        # ... and a swap inside the queue entry stops the claim.
        frozen_input.write_bytes(other.read_bytes())
        assert queue.claim('3-1') is None, 'a queue entry with a swapped input was claimed'
        state = json.loads((queue_root / 'state.json').read_text())
        assert any('Queued input bytes changed' in json.dumps(j) for j in state['jobs']), 'the swap was not recorded as a refusal'
        # A resubmission with other bytes is a new entry whose own blob needs its own record.
        second = queue.submit(parser.parse_args(argv + ['--input', str(other), '--hypothesis', 'a different blob']))
        assert Path(second['directory']) != entry and Path(second['options']['input'][0]).read_bytes() == other.read_bytes()
        args = SimpleNamespace(approved_by='coordinator', storage_write=True, port='3-1', image=named,
                               input=[Path(second['options']['input'][0])], host_tree=None)
        with patch.dict(os.environ, {'SUNSTONE_STORAGE_APPROVALS': str(approvals)}):
            no(lambda: images.policy({'descriptor': good, 'symbols': {}}, args), 'first input')
    print(f'storage-bounds: classification of 7 images; read-only and no-core pass unchanged; {count[0]} refusals; '
          'complete submission admitted; first input frozen byte for byte and unswappable')


def _write(path, data):
    path.write_bytes(data)
    return path


if __name__ == '__main__':
    main()
