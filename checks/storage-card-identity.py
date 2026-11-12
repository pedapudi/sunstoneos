#!/usr/bin/env python3
"""The bounded-write admission is keyed to the card (tools/storage_bounds.py, drivers/storage/a733_storage_bounds.c).

A write-capable image runs on any port whose card has a recorded restore point; the blob and the approval record name the card's CID, the restore
point must have been captured on the submission's port, and the image's enable refuses when the card in the port reports another CID. Cases:
3-3 admitted with its own blob; 3-3 refused with 3-1's blob (the record is for another port, and a forged record naming 3-1's card on 3-3 is
refused because that card's restore point is on 3-1); refused with no restore point; the same for 3-1; a restore point is never replaced; the
image side refuses any other CID at enable. Mutations of the identity tests must each make the check fail."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from types import SimpleNamespace

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
CID33 = '2750485344333247120000029d014c47'
CID31 = '6d4a7f01534436344720006200a6832e'


def load(path):
    spec = importlib.util.spec_from_file_location('storage_bounds_under_test', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def card_backup(path, start, total=1_000_000):
    mbr = bytearray(512)
    mbr[446 + 4] = 0x0c
    struct.pack_into('<II', mbr, 446 + 8, start, total)
    mbr[510:512] = b'\x55\xaa'
    boot = bytearray(512)
    struct.pack_into('<H', boot, 14, 32)
    boot[16] = 2
    struct.pack_into('<II', boot, 32, total, 14912)
    boot[510:512] = b'\x55\xaa'
    root = start + 32 + 2 * 14912
    path.write_bytes(struct.pack('<Q', 0) + bytes(mbr) + struct.pack('<Q', start) + bytes(boot) + struct.pack('<Q', start + 1) + bytes(512) +
                     b''.join(struct.pack('<Q', root + i) + bytes(512) for i in range(32)))
    return path, root


def scenario(module, out):
    """All the host cases against one copy of the admission code; raises AssertionError on any deviation."""
    out.mkdir()
    approvals = out / 'approvals'
    approvals.mkdir()
    registry = out / 'restore-points.json'
    made = {}
    for name, cid, start in (('3-3', CID33, 2048), ('3-1', CID31, 32768)):
        backup, root = card_backup(out / f'backup-{name}.bin', start)
        digest = hashlib.sha256(backup.read_bytes()).hexdigest()
        ranges = [[start + 1, 1], [root, 32], [root + 64, 1000]]
        backup_ranges = [[0, 1], [start, 2], [root, 32]]
        data = [[root + 64, 1000]]
        bk = [tuple(r) for r in backup_ranges]
        blob = module.encode_input([tuple(r) for r in ranges], bk, digest, cid)
        blob_file = out / f'blob-{name}.bin'
        blob_file.write_bytes(blob)
        record = {'ranges': ranges, 'backup_ranges': backup_ranges, 'data_ranges': data, 'backup_sha256': digest, 'backup': str(backup),
                  'port': name, 'image': module.ANY_IMAGE, 'card_cid': cid}
        module.approval_path(blob, approvals).write_text(json.dumps(record))
        descriptor = {'storage_bounds': {'ranges': ranges, 'backup_ranges': backup_ranges, 'backup_sha256': digest, 'card_cid': cid}}
        made[name] = dict(cid=cid, backup=backup, digest=digest, blob=blob, blob_file=blob_file, record=record, descriptor=descriptor,
                          ranges=ranges, backup_ranges=backup_ranges, data=data)

    def use(path):
        os.environ['SUNSTONE_RESTORE_POINTS'] = str(path)
        module.RESTORE_POINTS = Path(path)

    def admit(port, case, descriptor=None, input_file=None):
        os.environ['SUNSTONE_STORAGE_APPROVALS'] = str(approvals)
        args = SimpleNamespace(approved_by='coordinator', storage_write=True, port=port, input=[str(input_file or made[case]['blob_file'])], host_tree=None)
        return module.admit(descriptor or made[case]['descriptor'], args, 'deadbeef', approvals=approvals)

    def refused(call, text):
        try:
            call()
        except ValueError as e:
            assert text in str(e), (text, str(e))
            return
        raise AssertionError('admitted, expected refusal: ' + text)

    module.APPROVALS = approvals
    # No restore point recorded: refused on either port.
    use(registry)
    refused(lambda: admit('3-3', '3-3'), 'No restore point')
    refused(lambda: admit('3-1', '3-1'), 'No restore point')
    # The first full capture of each card becomes its restore point, on the port it was captured on.
    for name in ('3-3', '3-1'):
        module.register_restore_point(made[name]['cid'], name, made[name]['backup'], made[name]['digest'], registry)
    # 3-3 is admitted with its own blob, and so is 3-1.
    assert admit('3-3', '3-3') == made['3-3']['blob'] and admit('3-1', '3-1') == made['3-1']['blob']
    # 3-3 is refused with 3-1's blob, and the reverse: the record is for another port.
    refused(lambda: admit('3-3', '3-1'), 'another port')
    refused(lambda: admit('3-1', '3-3'), 'another port')
    # A forged record that names 3-1's card on port 3-3 is refused because that card's restore point was captured on 3-1.
    forged = dict(made['3-1']['record'], port='3-3')
    module.approval_path(made['3-1']['blob'], approvals).write_text(json.dumps(forged))
    refused(lambda: admit('3-3', '3-1'), 'was captured on port 3-1')
    # A record that names another card than the descriptor is refused.
    swapped = dict(made['3-3']['record'], card_cid=CID31)
    module.approval_path(made['3-3']['blob'], approvals).write_text(json.dumps(swapped))
    refused(lambda: admit('3-3', '3-3'), 'another card')
    module.approval_path(made['3-3']['blob'], approvals).write_text(json.dumps(made['3-3']['record']))
    # A blob without the card in its last 16 bytes (a v2 blob, or a cut one) is not the delivered input.
    refused(lambda: admit('3-3', '3-3', input_file=_write(out / 'short.bin', made['3-3']['blob'][:-16])), 'first input')
    # A restore point is never replaced, and its file must still match.
    refused(lambda: module.register_restore_point(CID33, '3-3', made['3-3']['backup'], made['3-3']['digest'], registry), 'never replaced')
    refused(lambda: module.register_restore_point(CID33, '3-1', made['3-1']['backup'], made['3-1']['digest'], registry), 'never replaced')
    saved = made['3-3']['backup'].read_bytes()
    made['3-3']['backup'].write_bytes(saved[:-1] + bytes([saved[-1] ^ 1]))
    refused(lambda: admit('3-3', '3-3'), 'differs')
    made['3-3']['backup'].write_bytes(saved)
    assert admit('3-3', '3-3') == made['3-3']['blob']
    scratch_cases(module, out, approvals, made)


def scratch_cases(module, out, approvals, made):
    """A scratch card is admitted on its CID and partition by one record per card; any deviation is refused."""
    registry = out / 'scratch-cards.json'
    part = [2048, 61065216]
    registry.write_text(json.dumps({CID33: {'port': '3-3', 'partition': part}}))
    blob = module.encode_scratch(tuple(part), CID33)
    blob_file = _write(out / 'scratch-blob.bin', blob)
    descriptor = {'storage_scratch': {'card_cid': CID33, 'partition': part}}

    def go(port='3-3', desc=None, input_file=blob_file):
        args = SimpleNamespace(approved_by='coordinator', storage_write=True, port=port, input=[str(input_file)], host_tree=None)
        return module.admit(desc or descriptor, args, 'deadbeef', approvals=approvals)

    def refused(call, text):
        try:
            call()
        except ValueError as e:
            assert text in str(e), (text, str(e))
            return
        raise AssertionError('admitted, expected refusal: ' + text)

    module.SCRATCH_CARDS = registry
    refused(go, 'No coordinator approval record for the scratch card')
    module.card_approval_path(CID33, approvals).write_text(json.dumps({'card_cid': CID33, 'port': '3-3'}))
    assert go() == blob                                                       # the card, its port and its partition: admitted with no backup and no restore point
    assert go() == blob                                                       # and again, with the same record: a record is per card
    refused(lambda: go('3-1'), 'not on port 3-1')
    refused(lambda: go(desc={'storage_scratch': {'card_cid': CID31, 'partition': part}}), 'not listed as a scratch card')
    refused(lambda: go(desc={'storage_scratch': {'card_cid': CID33, 'partition': [2048, 61065217]}}), 'differs from the scratch card')
    refused(lambda: go(input_file=_write(out / 'wide.bin', module.encode_scratch((1, 61067264), CID33))), 'first input')
    module.card_approval_path(CID33, approvals).write_text(json.dumps({'card_cid': CID33, 'port': '3-1'}))
    refused(go, 'another card or port')
    module.card_approval_path(CID33, approvals).write_text(json.dumps({'card_cid': CID33, 'port': '3-3'}))
    refused(lambda: go(desc=dict(descriptor, storage_bounds=made['3-3']['descriptor']['storage_bounds'])), 'one of storage_bounds, storage_scratch and storage_boot_area')


def _write(path, data):
    path.write_bytes(data)
    return path


def image_side(out, source):
    out.mkdir()
    binary = out / 'identity'
    cc = [os.environ.get('CC', '/usr/bin/clang-21'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
          f'-I{ROOT}/include', str(ROOT / 'checks/storage-card-identity.c'), str(source), str(ROOT / 'drivers/storage/storage_digest.c'), '-o', str(binary)]
    built = subprocess.run(cc, capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    original = (ROOT / 'tools/storage_bounds.py').read_text()
    module_path = out / 'storage_bounds_original.py'
    module_path.write_text(original)
    scenario(load(module_path), out / 'cases')
    ran = image_side(out / 'image', ROOT / 'drivers/storage/a733_storage_bounds.c')
    assert ran.returncode == 0, ran.stdout + ran.stderr
    print(ran.stdout.strip())
    # Mutations: each identity test removed must make the check fail.
    killed = []
    for name, old, new in (
            ('no restore point required', "    restore_point(bounds['card_cid'], args.port)\n", ''),
            ('restore point on any port', "    require(point.get('port') == port,", "    require(True or point.get('port') == port,"),
            ('record card not compared', "    require(record.get('card_cid') == bounds['card_cid'], 'The approval record is for another card.')\n", ''),
            ('record port not compared', "    require(record.get('port') == args.port, 'The approval record is for another port.')\n", ''),
            ('scratch port not compared', "    require(entry.get('port') == args.port,", "    require(True or entry.get('port') == args.port,"),
            ('scratch partition not compared', "    require(entry.get('partition') == partition,", "    require(True or entry.get('partition') == partition,"),
            ('scratch card record not required', "    require(record.is_file(), f'No coordinator", "    require(True or record.is_file(), f'No coordinator"),
            ('scratch listing not required', "    require(entry,", "    require(True or entry,"),
            ('restore point replaced', "    require(card_cid not in points, 'This card already has a restore point; it is never replaced.')\n", '')):
        assert original.count(old) == 1, (name, original.count(old))
        mutant = out / ('mutant-' + name.replace(' ', '-') + '.py')
        mutant.write_text(original.replace(old, new))
        try:
            scenario(load(mutant), out / ('cases-' + name.replace(' ', '-')))
        except (AssertionError, ValueError, OSError):
            killed.append(name)
        else:
            raise AssertionError('mutation survived: ' + name)
    source = (ROOT / 'drivers/storage/a733_storage_bounds.c').read_text()
    for name, old, new in (('identity always accepted', "        if (w != cid[i]) return 0;", "        if (0 && w != cid[i]) return 0;"),
                           ('scratch verified without checking shape', "        if (w != 1 || k != 0 || memcmp(blob + 16, zero, 32)) return 0;", "        (void)zero;"),
                           ('identity read before verify', "    if (!b || !cid || b->state != A733_BOUNDS_VERIFIED || !a733_storage_bounds_intact(b)) return 0;", "    if (!b || !cid) return 0;")):
        assert source.count(old) == 1, (name, source.count(old))
        mutant = out / ('mutant-' + name.replace(' ', '-') + '.c')
        mutant.write_text(source.replace(old, new))
        ran = image_side(out / ('image-' + name.replace(' ', '-')), mutant)
        assert ran.returncode != 0, 'mutation survived: ' + name
        killed.append(name)
    print('PASS card identity: 3-3 admitted with its own blob, refused with 3-1\'s (and the reverse), refused without a restore point, a forged record refused, '
          'restore points never replaced, the image refuses another CID; mutations killed: ' + ', '.join(killed))


if __name__ == '__main__':
    main()
