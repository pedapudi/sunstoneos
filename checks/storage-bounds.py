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

    # A card backup: sector records in increasing order: the MBR, the boot sector and the first FSInfo sector. This
    # card's partition starts at 32,768 and holds 1,000,000 sectors; 64 reserved sectors and two FATs of 14,912 sectors
    # put its data area at 62,656 (the check expects exactly those values, derived from the tables).
    def card_backup(path, start=32768, total=1_000_000, ptype=0x0c, extra_entry=None, mbr_ok=True):
        mbr = bytearray(512)
        mbr[446 + 4] = ptype
        struct.pack_into('<II', mbr, 446 + 8, start, total)
        if extra_entry:
            mbr[462 + 4] = extra_entry
            struct.pack_into('<II', mbr, 462 + 8, start + total, 100)
        mbr[510:512] = b'\x55\xaa'
        boot = bytearray(512)
        struct.pack_into('<H', boot, 14, 64)
        boot[16] = 2
        struct.pack_into('<II', boot, 32, total, 14912)
        boot[510:512] = b'\x55\xaa'
        root = start + 64 + 2 * 14912                                       # the root cluster: 64 sectors at the data start
        records = (struct.pack('<Q', 0) + bytes(mbr) if mbr_ok else b'') + struct.pack('<Q', start) + bytes(boot) + \
            struct.pack('<Q', start + 1) + bytes(512) + b''.join(struct.pack('<Q', root + i) + bytes(512) for i in range(64))
        path.write_bytes(records)
        return path
    backup = card_backup(out / 'backup.bin')
    digest = hashlib.sha256(backup.read_bytes()).hexdigest()
    assert bounds.partition_bounds(bounds.parse_backup(backup, digest)) == (32768, 32768 + 1_000_000)
    end = 32768 + 1_000_000
    ranges = [[32768, 2], [62656, 64], [62720, 64], [65536, 900000]]          # boot+FSInfo, root cluster, new directory, data
    backup_ranges = [[0, 1], [32768, 2], [62656, 64]]
    data_ranges = [[62720, 64], [65536, 900000]]                              # far beyond the earlier 1 GiB cap
    approvals = out / 'approvals'
    approvals.mkdir()

    cid = '0123456789abcdef0123456789abcdef'
    # The restore point of this card (its first full capture), on port 3-1.
    registry = out / 'restore-points.json'
    bounds.register_restore_point(cid, '3-1', backup, digest, registry)

    def blob_of(rs, bk, h, card=None):
        return bounds.encode_input([tuple(r) for r in rs], [tuple(r) for r in bk], h, card or cid)

    def record(rs=ranges, bk=backup_ranges, h=digest, backup_file=backup, data=data_ranges, port='3-1',
               image=bounds.ANY_IMAGE, directory=approvals):
        path = bounds.approval_path(blob_of(rs, bk, h), directory)
        path.write_text(json.dumps({'ranges': rs, 'backup_ranges': bk, 'data_ranges': data, 'backup_sha256': h,
                                    'backup': str(backup_file), 'port': port, 'image': image, 'card_cid': cid}))
        return path

    counter = [0]

    def case(rs=ranges, bk=backup_ranges, h=digest, backup_file=backup, data=data_ranges, **kw):
        """Write the record and the input blob for these bounds; return (descriptor, input path)."""
        record(rs, bk, h, backup_file, data, **kw)
        counter[0] += 1
        blob_file = _write(out / f'case-{counter[0]}.bin', blob_of(rs, bk, h))
        return {'storage_bounds': {'ranges': rs, 'backup_ranges': bk, 'backup_sha256': h, 'card_cid': cid}}, blob_file

    def submit(image, descriptor=None, *, flag=True, port='3-1', inputs=None, directory=approvals, approved='coordinator'):
        args = SimpleNamespace(approved_by=approved, storage_write=flag, port=port, image=image,
                               input=list(inputs if inputs is not None else ()), host_tree=None)
        with patch.dict(os.environ, {'SUNSTONE_STORAGE_APPROVALS': str(directory), 'SUNSTONE_RESTORE_POINTS': str(registry)}):
            images.policy({'descriptor': descriptor or {}, 'symbols': {}}, args)
        return args

    blob_path = _write(out / 'bounds.bin', blob_of(ranges, backup_ranges, digest))
    other = _write(out / 'other.bin', blob_path.read_bytes()[:-1] + b'\x01')
    good = {'storage_bounds': {'ranges': ranges, 'backup_ranges': backup_ranges, 'backup_sha256': digest, 'card_cid': cid}}
    # Read-only images and images without an SD core pass with no flags and no records.
    for image in (readonly, nocore, table, blob):
        submit(image, flag=False, approved=None)
    # Everything else needs admission: no flag, no record, no descriptor, wrong input, wrong port.
    for image in (named, renamed, literal, raw):
        no(lambda: submit(image, flag=False, approved=None), 'needs --storage-write')
        no(lambda: submit(image, good, inputs=(blob_path,)), 'No coordinator approval record')
        record()
        no(lambda: submit(image, good, port='3-3', inputs=(blob_path,)), 'another port')
        no(lambda: submit(image, {}, inputs=(blob_path,)), 'storage_bounds')
        no(lambda: submit(image, good), 'first input')
        no(lambda: submit(image, good, inputs=(other,)), 'first input')
        no(lambda: submit(image, good, inputs=(blob_path,), directory=out / 'empty'), 'No coordinator approval record')
        submit(image, good, inputs=(blob_path,))                      # the approval record admits it
        submit(image, good, approved=None, inputs=(blob_path,))       # --approved-by plays no part: the record is the approval
        bounds.approval_path(blob_path.read_bytes(), approvals).unlink()
    # The record must match the descriptor, the port, the image and the partition.
    sha = images.sha(named)
    d, i = case(image='deadbeef')
    no(lambda: submit(named, d, inputs=(i,)), 'does not name this image')
    d, i = case(image=sha)
    submit(named, d, inputs=(i,))
    d, i = case(port='3-3')
    no(lambda: submit(named, d, inputs=(i,)), 'another port')
    d, i = case()
    submit(named, d, inputs=(i,))
    # Write ranges: below the start, overlapping, unsorted, empty, past the end; the last two sectors are admitted.
    for name, rs in {'below the partition': [[32767, 1]], 'overlap': [[40000, 100], [40050, 10]], 'unsorted': [[50000, 1], [40000, 1]],
                     'empty range': [[40000, 0]], 'past the partition end': [[end - 1, 2]], 'empty list': []}.items():
        d, i = case(rs=rs, data=[])
        no(lambda: submit(named, d, inputs=(i,)), '')
    d, i = case(rs=[[end - 2, 2]], data=[[end - 2, 2]])
    submit(named, d, inputs=(i,))
    # Backup ranges: listed sectors must be the file's sectors, in the partition or sector 0, sorted and disjoint.
    for label, bk, text in (('a sector the file lacks', [[0, 1], [32768, 3], [62656, 64]], 'exactly the listed'),
                            ('a sector the list lacks', [[0, 1], [32768, 1], [62656, 64]], 'exactly the listed'),
                            ('outside the partition', [[0, 1], [32768, 2], [62656, 64], [end, 1]], 'outside the partition'),
                            ('sector 1 is not sector 0', [[1, 1], [32768, 2], [62656, 64]], 'outside the partition'),
                            ('unsorted', [[32768, 2], [0, 1], [62656, 64]], 'sorted'),
                            ('empty list', [], 'at least one backup range')):
        d, i = case(bk=bk)
        no(lambda: submit(named, d, inputs=(i,)), text)
    # Metadata inside the write ranges must be backed up; the data exemption cannot cover the boot, FSInfo or FAT area.
    d, i = case(rs=[[32768, 2], [32770, 1]], data=[])
    no(lambda: submit(named, d, inputs=(i,)), 'not backed up')
    d, i = case(data=[[32768, 2]])
    no(lambda: submit(named, d, inputs=(i,)), 'metadata')
    d, i = case(data=[[62720, 64], [65536, 950_000]])
    no(lambda: submit(named, d, inputs=(i,)), 'not inside a write range')
    d, i = case(rs=[[32768, 2], [62656, 64], [65536, 900000]], bk=[[0, 1], [32768, 2], [62656, 32]], data=[[65536, 900000]])
    no(lambda: submit(named, d, inputs=(i,)), 'exactly the listed')
    d, i = case(rs=[[32768, 2], [62656, 64], [65536, 900000]], bk=[[0, 1], [32768, 2], [62656, 64]], data=[[62656, 64], [65536, 900000]])
    submit(named, d, inputs=(i,))                                          # a directory declared as data is the approval's choice
    d, i = case(rs=[[32768, 2], [62688, 32], [65536, 900000]], bk=[[0, 1], [32768, 2], [62656, 64]], data=[[65536, 900000]])
    submit(named, d, inputs=(i,))                                          # half the root cluster, backed up in full
    # Replacement cards: another start and size move both limits.
    moved = card_backup(out / 'moved.bin', start=8192, total=500_000)
    mv = hashlib.sha256(moved.read_bytes()).hexdigest()
    mrs = [[8192, 2], [9000, 100], [507_000, 1]]
    d, i = case(rs=mrs, bk=[[0, 1], [8192, 2], [38080, 64]], h=mv, backup_file=moved, data=[[9000, 100], [507_000, 1]])
    no(lambda: submit(named, d, inputs=(i,)), 'metadata')                  # 9,000 is below the moved card's data area (8,192 + 64 + 29,824)
    mrs = [[8192, 2], [40000, 100], [507_000, 1]]
    d, i = case(rs=mrs, bk=[[0, 1], [8192, 2], [38080, 64]], h=mv, backup_file=moved, data=[[40000, 100], [507_000, 1]])
    submit(named, d, inputs=(i,))
    for label, rs in {'below the moved start': [[8191, 1]], 'past the moved end': [[508_191, 2]],
                      'the old constant start is not special': [[32768, 1_000_000]]}.items():
        d, i = case(rs=rs, bk=[[0, 1], [8192, 2], [38080, 64]], h=mv, backup_file=moved, data=[])
        no(lambda: submit(named, d, inputs=(i,)), '')
    nombr = card_backup(out / 'nombr.bin', mbr_ok=False)
    two = card_backup(out / 'two.bin', extra_entry=0x0b)
    notfat = card_backup(out / 'notfat.bin', ptype=0x83)
    for label, bk_file, bk, text in (('no MBR', nombr, [[32768, 2], [62656, 64]], 'sector 0'), ('two FAT32 entries', two, [[0, 1], [32768, 2], [62656, 64]], 'exactly one'),
                                     ('no FAT32 entry', notfat, [[0, 1], [32768, 2], [62656, 64]], 'exactly one')):
        h2 = hashlib.sha256(bk_file.read_bytes()).hexdigest()
        d, i = case(rs=[[40000, 1]], bk=bk, h=h2, backup_file=bk_file, data=[[40000, 1]])
        no(lambda: submit(named, d, inputs=(i,)), text)
    # Hash and file: a changed file, a short file, a bad hash string, an extra descriptor key.
    flipped = out / 'flipped.bin'
    flipped.write_bytes(bytes([backup.read_bytes()[0] ^ 1]) + backup.read_bytes()[1:])
    d, i = case(backup_file=flipped)
    no(lambda: submit(named, d, inputs=(i,)), 'differs from the approved hash')
    for h in ('', 'ab', digest.upper(), digest[:-1]):
        no(lambda: submit(named, {'storage_bounds': {'ranges': ranges, 'backup_ranges': backup_ranges, 'backup_sha256': h, 'card_cid': cid}},
                          inputs=(blob_path,)), 'Invalid storage_bounds')
    no(lambda: submit(named, {'storage_bounds': {'ranges': ranges, 'backup_ranges': backup_ranges, 'backup_sha256': digest, 'card_cid': cid, 'extra': 1}},
                      inputs=(blob_path,)), 'storage_bounds')
    no(lambda: submit(named, {'storage_bounds': {'ranges': ranges, 'backup_sha256': digest, 'card_cid': cid}}, inputs=(blob_path,)), 'backup_ranges')
    # An unordered backup file is refused even when its hash is right.
    unordered = out / 'unordered.bin'
    recs = backup.read_bytes()
    unordered.write_bytes(recs[520:1040] + recs[:520] + recs[1040:])
    d, i = case(backup_file=unordered, h=hashlib.sha256(unordered.read_bytes()).hexdigest())
    no(lambda: submit(named, d, inputs=(i,)), 'increasing order')
    record()
    submit(named, good, inputs=(blob_path,))

    boot_area_cases(out, named, submit, no, approvals, registry, cid)

    # Lifecycle: the real queue freezes the first input; a changed byte or a swapped blob cannot run.
    queue_root = out / 'queue'
    source = out / 'source.elf'
    source.write_bytes(named.read_bytes())
    parser = argparse.ArgumentParser()
    board.run_options(parser, True)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--worker', default='storage-write')
    parser.add_argument('--hypothesis', default='bounds reach the image unchanged')
    argv = [str(source), '--port', '3-1', '--evidence', str(out / 'run'), '--kind', 'probe',
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
        with patch.dict(os.environ, {'SUNSTONE_STORAGE_APPROVALS': str(approvals), 'SUNSTONE_RESTORE_POINTS': str(registry)}):
            images.policy({'descriptor': good, 'symbols': {}}, args)
        bounds.verify_delivered(args)
        # A swap after admission is caught by verify_delivered ...
        args.input = [other]
        no(lambda: bounds.verify_delivered(args), 'differs from the admitted storage bounds')
        # ... and a swap inside the queue entry stops the claim.
        frozen_input.write_bytes(other.read_bytes())
        assert queue.claim('3-1') is None, 'a queue entry with a swapped input was claimed'
        assert 'Queued input bytes changed' in json.dumps(queue.job(job['id'])), 'the swap was not recorded as a refusal'
        # A resubmission with other bytes is a new entry whose own blob needs its own record.
        second = queue.submit(parser.parse_args(argv + ['--input', str(other), '--hypothesis', 'a different blob']))
        assert Path(second['directory']) != entry and Path(second['options']['input'][0]).read_bytes() == other.read_bytes()
        args = SimpleNamespace(approved_by='coordinator', storage_write=True, port='3-1', image=named,
                               input=[Path(second['options']['input'][0])], host_tree=None)
        with patch.dict(os.environ, {'SUNSTONE_STORAGE_APPROVALS': str(approvals), 'SUNSTONE_RESTORE_POINTS': str(registry)}):
            no(lambda: images.policy({'descriptor': good, 'symbols': {}}, args), 'first input')
    print(f'storage-bounds: classification of 7 images; read-only and no-core pass unchanged; {count[0]} refusals; '
          'complete submission admitted; first input frozen byte for byte and unswappable')


def boot_area_cases(out, named, submit, no, approvals, registry, cid):
    """The boot-area class: the first-stage extent below the partition, backed up with sectors 0 and 16..511."""
    card = '2750485344333247120000029d014c47'
    stranger = 'fedcba9876543210fedcba9876543210'

    def mbr(start):
        sector = bytearray(512)
        sector[446 + 4] = 0x0c
        struct.pack_into('<II', sector, 446 + 8, start, 61065216)
        sector[510:512] = b'\x55\xaa'
        return bytes(sector)

    def backup_file(name, start=2048, extent=None, extra=()):
        content = {0: mbr(start)}
        content.update({s: bytes([s & 0xff]) * 512 for s in range(16, 512)})
        content.update(extent or {})
        content.update({s: bytes(512) for s in extra})
        path = out / name
        path.write_bytes(b''.join(struct.pack('<Q', s) + content[s] for s in sorted(content)))
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    backup, backup_sha = backup_file('boot-area-backup.bin')
    candidate = _write(out / 'first-stage.fex', bytes((i * 7) & 0xff for i in range(481 * 512)))
    candidate_sha = hashlib.sha256(candidate.read_bytes()).hexdigest()

    def manifest(name, sectors=481, sha=candidate_sha):
        path = _write(out / name, json.dumps({'schema': 'sunstone-card-fel-image-v1', 'first_stage': {
            'bytes': 512 * sectors, 'sha256': sha, 'first_sector': 16, 'sectors': sectors}}).encode())
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    stage, stage_sha = manifest('first-stage.json')
    def extent(data, first=16):
        return _write(out / f'extent-{hashlib.sha256(data).hexdigest()[:12]}-{first}.bin', bounds.write_extent_input(first, data))
    for name, path, sha in (('3-3', backup, backup_sha),):
        bounds.register_restore_point(card, name, path, sha, registry)
    serial = [0]

    def case(rs=((16, 481),), bk=bounds.BOOT_AREA_BACKUP, h=backup_sha, cand=candidate_sha, card_id=card, port='3-3',
             backup_path=backup, manifest_path=stage, manifest_sha=stage_sha, direction='install', original=None,
             record_port=None, record_card=None, record=True, extra_keys=None):
        rs = [list(r) for r in rs]
        blob = bounds.encode_input([tuple(r) for r in rs], [tuple(r) for r in bk], h, card_id)
        if record:
            entry = {'ranges': rs, 'backup_ranges': bk, 'backup_sha256': h, 'candidate_sha256': cand,
                     'card_cid': record_card or card_id, 'port': record_port or port, 'image': bounds.ANY_IMAGE,
                     'backup': str(backup_path), 'first_stage_manifest': str(manifest_path),
                     'first_stage_manifest_sha256': manifest_sha, 'direction': direction}
            if original:
                entry.update(original_backup=str(original[0]), original_backup_sha256=original[1])
            bounds.approval_path(blob, approvals).write_text(json.dumps(entry))
        serial[0] += 1
        blob_path = _write(out / f'boot-area-{serial[0]}.bin', blob)
        descriptor = {'storage_boot_area': {'ranges': rs, 'backup_ranges': bk, 'backup_sha256': h, 'card_cid': card_id,
                                            'candidate_sha256': cand}}
        descriptor.update(extra_keys or {})
        return descriptor, blob_path

    def run(descriptor, blob_path, inputs=None, port='3-3'):
        return submit(named, descriptor, port=port, inputs=inputs or (blob_path, extent(candidate.read_bytes())))

    d, i = case()
    args = run(d, i)                                                   # the install is admitted
    assert args.storage_candidate == candidate_sha
    bounds.verify_delivered(args)
    args.input = [i, extent(bytes(481 * 512))]
    no(lambda: bounds.verify_delivered(args), 'delivered candidate differs')
    # The restore direction: the candidate is the extent's bytes in an earlier backup.
    original_extent = {s: bytes([0xa5]) * 512 for s in range(16, 497)}
    original = backup_file('boot-area-original.bin', extent=original_extent)
    restore = _write(out / 'restore.bin', b''.join(original_extent[s] for s in range(16, 497)))
    restore_sha = hashlib.sha256(restore.read_bytes()).hexdigest()
    d, i = case(cand=restore_sha, direction='restore', original=original)
    run(d, i, inputs=(i, extent(restore.read_bytes())))
    d, i = case(cand=restore_sha, direction='restore', original=(backup, backup_sha))
    no(lambda: run(d, i, inputs=(i, extent(restore.read_bytes()))), 'not the extent\'s bytes in the original backup')
    d, i = case(cand=restore_sha, direction='install')
    no(lambda: run(d, i, inputs=(i, extent(restore.read_bytes()))), 'not the manifest\'s first stage')
    d, i = case(cand=restore_sha, direction=None)
    no(lambda: run(d, i, inputs=(i, extent(restore.read_bytes()))), 'names no direction')
    # The extent: past the backed-up sectors and sector 2048, over the partition, or another extent than the manifest's.
    d, i = case(rs=((16, 2040),))
    no(lambda: run(d, i), 'outside the backed-up sectors')
    d, i = case(rs=((1, 481),))
    no(lambda: run(d, i), 'outside the backed-up sectors')
    low, low_sha = backup_file('boot-area-low-partition.bin', start=400)
    d, i = case(h=low_sha, backup_path=low)
    no(lambda: run(d, i), 'overlaps the FAT32 partition')
    big = _write(out / 'first-stage-482.fex', bytes(482 * 512))
    d, i = case(rs=((16, 482),), cand=hashlib.sha256(big.read_bytes()).hexdigest())
    no(lambda: run(d, i, inputs=(i, extent(big.read_bytes()))), 'differs from the first-stage extent')
    d, i = case(rs=((16, 2),), manifest_sha='0' * 64)
    no(lambda: run(d, i, inputs=(i, extent(bytes(1024)))), 'candidate differs')
    d, i = case(manifest_sha='0' * 64)
    no(lambda: run(d, i), 'manifest differs from the approved hash')
    # The backup: exactly sector 0 and sectors 16..511, in the file and in the ranges.
    d, i = case(bk=[[16, 496]])
    no(lambda: run(d, i), 'exactly sector 0 and sectors 16..511')
    d, i = case(bk=[[0, 1], [16, 496], [2048, 1]])
    no(lambda: run(d, i), 'exactly sector 0 and sectors 16..511')
    wide, wide_sha = backup_file('boot-area-wide.bin', extra=(2048,))
    d, i = case(h=wide_sha, backup_path=wide)
    no(lambda: run(d, i), 'exactly the listed backup sectors')
    # The candidate, the mode word and the inputs.
    d, i = case(cand='1' * 64)
    no(lambda: run(d, i), 'candidate differs from storage_boot_area')
    no(lambda: run(*case(), inputs=(case()[1], extent(candidate.read_bytes(), first=17))), 'raw-extent mode for the extent')
    no(lambda: run(*case(), inputs=(case()[1], _write(out / 'short-extent.bin', bounds.write_extent_input(16, candidate.read_bytes()[:-512])))), 'not the extent\'s length')
    no(lambda: run(*case(), inputs=(case()[1],)), 'takes the bounds blob and the raw-extent mode input')
    # The record: another port, another card, absent; the card's restore point; one class per descriptor.
    d, i = case(record_port='3-1')
    no(lambda: run(d, i), 'another port')
    d, i = case(record_card=stranger)
    no(lambda: run(d, i), 'another card')
    d, i = case(record=False, h='2' * 64)
    no(lambda: run(d, i), 'No coordinator approval record')
    d, i = case(card_id=stranger)
    no(lambda: run(d, i), 'No restore point')
    d, i = case()
    no(lambda: run(d, i, port='3-1'), 'another port')
    for other in ({'storage_bounds': {}}, {'storage_scratch': {}}):
        d, i = case(extra_keys=other)
        no(lambda: run(d, i), 'one of storage_bounds, storage_scratch and storage_boot_area')
    d, i = case()
    d['storage_boot_area']['extra'] = 1
    no(lambda: run(d, i), 'storage_boot_area needs')
    print('storage-bounds: boot-area install and restore admitted; extent, backup, candidate, record and class refusals hold')


def _write(path, data):
    path.write_bytes(data)
    return path


if __name__ == '__main__':
    main()
