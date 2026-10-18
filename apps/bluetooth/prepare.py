#!/usr/bin/env python3
"""Prepare an offline supervised Bluetooth image with exact retained firmware assets."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bluetooth_board_preparation', ROOT / 'apps/board/prepare.py')
board = importlib.util.module_from_spec(spec)
spec.loader.exec_module(board)
require, digest = board.require, board.digest
SCHEMA = 'a733-bluetooth-supervised-bundle-v1'


def profile_check(data, session):
    require(len(data) == 624 and struct.unpack_from('<QII', data) == (0x3150465442535353, 1, 624), 'Bluetooth profile geometry differs.')
    require(struct.unpack_from('<I', session, 400)[0] == 2 and hashlib.sha256(data).digest() == session[320:352], 'Bluetooth profile/session identity differs.')
    require(struct.unpack_from('<Q', data, 24)[0] == struct.unpack_from('<Q', session, 312)[0] != 0, 'Actual secure SID authority required.')
    require(all(struct.unpack_from('<Q', data, offset)[0] == 0 for offset in (184, 192, 200, 360, 368)), 'Runtime handoffs/deadlines must be absent from immutable input.')
    addresses, sizes = struct.unpack_from('<4Q', data, 512), struct.unpack_from('<4Q', data, 544)
    spans = []
    for address, size in zip(addresses, sizes):
        require(0x43010000 <= address < 0x43100000 and 0 < size <= 0x43100000 - address, 'Firmware span exceeds retained reservation.')
        require(all(address >= a + n or a >= address + size for a, n in spans), 'Firmware spans overlap.')
        spans.append((address, size))
    return spans


def bluetooth_admission(record, profile, session, manifest, image_hash, cache, frozen=False):
    require(manifest.get('target') == 'sunstone-a733-bluetooth' and manifest.get('execution_profile') == 'bluetooth-supervised-v1' and manifest.get('final_elf') == image_hash, 'Actual tested Bluetooth image manifest required.')
    require(record.get('radio_ddr') is not None, 'Complete retained profile/firmware DDR reservation required.')
    board.session_check(session)
    spans = profile_check(profile, session)
    require(isinstance(record.get('payloads'), list) and len(record['payloads']) == 4, 'Exactly four admitted firmware payloads required.')
    for key, offset in [('bluetooth_wiring', 56), ('bluetooth_sid', 88), ('bluetooth_transport', 120)]:
        item = record.get(key)
        require(isinstance(item, dict) and set(item) == {'path', 'sha256'} and Path(item['path']).is_absolute(), 'Exact Bluetooth evidence reference required.')
        if item['path'] not in cache:
            require(not frozen, 'Bluetooth evidence missing from frozen bundle.')
            cache[item['path']] = Path(item['path']).read_bytes()
        require(hashlib.sha256(cache[item['path']]).hexdigest() == item['sha256'] and bytes.fromhex(item['sha256']) == profile[offset:offset + 32], 'Bluetooth authority evidence differs.')
    return spans


def build_launcher(output, clang, lld):
    # Generate the same reviewed supervisor base; only this image's tiny entry
    # validation differs. No timer/reset writes or hardware operation occurs.
    text = (ROOT / 'platform/a733-smp-supervised/launch.c').read_text()
    text = text.replace('0x4153373353554c33', '0x4153373342534c32').replace('REQUEST[1]==3', 'REQUEST[1]==2').replace('CONTRACT->version==3', 'CONTRACT->version==2')
    text = text.replace('CONTRACT->reserved[0] && !CONTRACT->reserved[1]', 'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct a733_board_session)')
    text = text.replace('20*A733_SMP_COUNTER_HZ', 'A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ')
    (output / 'board-launch-base.c').write_text(text)
    for name, source in [('launch.c', ROOT / 'apps/bluetooth/launch.c'), ('launch.S', ROOT / 'platform/a733-smp-observe/launch.S'),
                         ('launch.ld', ROOT / 'platform/a733-smp-observe/launch.ld'), ('boot.h', ROOT / 'platform/a733-smp-cached/boot.h'),
                         ('a733_board_application.h', ROOT / 'include/a733_board_application.h')]:
        shutil.copyfile(source, output / name)
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall', '-Wextra', '-Werror',
             '-DSUNSTONE_BOARD_SUPERVISED=1', '-DA733_SMP_CONTRACT_MAGIC=0x4153373342534332ULL', '-I' + str(output)]
    commands = [[clang, *flags, '-c', output / name, '-o', output / obj] for name, obj in [('launch.S', 'entry.o'), ('launch.c', 'launch.o')]]
    commands += [[lld, '-T', output / 'launch.ld', output / 'entry.o', output / 'launch.o', '-o', output / 'launch.elf'],
                 ['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', output / 'launch.elf', output / 'launch.bin']]
    for command in commands:
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        with (output / 'launcher.log').open('a') as log:
            log.write(result.stdout + result.stderr)
        require(result.returncode == 0, 'Bluetooth SRAM launcher failed; preserve the stopped build.')
    (output / 'launcher-commands.json').write_text(json.dumps([[str(x) for x in c] for c in commands], indent=2) + '\n')
    require(0 < (output / 'launch.bin').stat().st_size <= 0xc00, 'SRAM launcher extent differs.')


def prepare(image, manifest, admission, template, profile, payloads, output, clang, lld):
    image, manifest, admission, template, profile, output = map(Path, (image, manifest, admission, template, profile, output))
    image_data, manifest_data, admission_data, template_data, profile_data = [p.read_bytes() for p in (image, manifest, admission, template, profile)]
    image_hash = hashlib.sha256(image_data).hexdigest()
    proof = json.loads(manifest_data)
    record = json.loads(admission_data)
    cache = board.validate_admission(record, image_hash, hashlib.sha256(template_data).hexdigest())
    spans = bluetooth_admission(record, profile_data, template_data, proof, image_hash, cache)
    snapshots = [Path(item['path']).read_bytes() for item in payloads]
    require([(int(item['address']), len(data)) for item, data in zip(payloads, snapshots)] == spans, 'Exactly four profile firmware spans are required.')
    require(payloads == record.get('payloads') and all(hashlib.sha256(data).hexdigest() == item['sha256'] for item, data in zip(payloads, snapshots)), 'Firmware differs from reviewed admission.')
    epoch, generation, lease = (secrets.randbits(64) or 1 for _ in range(3))
    session = bytearray(template_data);struct.pack_into('<3Q', session, 24, epoch, generation, lease);session[48:80] = bytes.fromhex(image_hash)
    output.mkdir(parents=True, exist_ok=False)
    build_launcher(output, clang, lld)
    for name, data in [('image.elf', image_data), ('image-manifest.json', manifest_data), ('admission.json', admission_data),
                       ('session-template.bin', template_data), ('session.bin', session), ('profile.bin', profile_data)]:
        (output / name).write_bytes(data)
    segments, entry, identity = board.base.image_parts(output / 'image.elf')
    uploads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin';(output / name).write_bytes(segment['data']);uploads.append({'name': name, 'address': segment['address']})
    uploads += [{'name': 'session.bin', 'address': 0x41f00000}, {'name': 'profile.bin', 'address': 0x43000000}]
    for index, ((address, size), data) in enumerate(zip(spans, snapshots)):
        name = f'firmware-{index}.bin';(output / name).write_bytes(data);uploads.append({'name': name, 'address': address})
    request, contract = board.request_bytes(entry, identity, epoch, generation, lease, session)
    for name, data in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]:
        (output / name).write_bytes(data)
    evidence = {}
    for index, (path, data) in enumerate(cache.items()):
        name = f'entry-evidence-{index}.bin';(output / name).write_bytes(data);evidence[path] = {'name': name, 'sha256': hashlib.sha256(data).hexdigest()}
    result = {'schema': SCHEMA, 'physical_access': False, 'image_sha256': image_hash, 'build_id': identity.hex(),
              'epoch': epoch, 'generation': generation, 'lease': lease, 'segments': uploads, 'entry_evidence': evidence,
              'launcher_sha256': digest(output / 'launch.bin'), 'backup_sizes': {n: s for n, a, s in board.base.REGIONS},
              'files': {p.name: digest(p) for p in output.iterdir() if p.is_file()}}
    (output / 'prepared.json').write_text(json.dumps(result, indent=2) + '\n')
    return validate_bundle(output)


def validate_bundle(bundle):
    bundle = Path(bundle);metadata = (bundle / 'prepared.json').read_bytes();p = json.loads(metadata)
    require(p.get('schema') == SCHEMA and p.get('physical_access') is False, 'Unsupported Bluetooth bundle.')
    for name, expected in p['files'].items():
        require(Path(name).name == name and digest(bundle / name) == expected, 'Prepared Bluetooth bytes changed.')
    require(digest(bundle / 'image.elf') == p['image_sha256'] and digest(bundle / 'launch.bin') == p['launcher_sha256'], 'Bluetooth image/launcher differs.')
    raw = (bundle / 'session.bin').read_bytes();template = bytearray((bundle / 'session-template.bin').read_bytes())
    struct.pack_into('<3Q', template, 24, p['epoch'], p['generation'], p['lease']);template[48:80] = bytes.fromhex(p['image_sha256'])
    require(raw == template, 'Session derivation differs.')
    cache = {path: (bundle / item['name']).read_bytes() for path, item in p['entry_evidence'].items()}
    require(all(hashlib.sha256(cache[path]).hexdigest() == item['sha256'] for path, item in p['entry_evidence'].items()), 'Frozen evidence differs.')
    admitted = json.loads((bundle / 'admission.json').read_text())
    board.validate_admission(admitted, p['image_sha256'], digest(bundle / 'session-template.bin'), cache, True)
    spans = bluetooth_admission(admitted, (bundle / 'profile.bin').read_bytes(), raw,
        json.loads((bundle / 'image-manifest.json').read_text()), p['image_sha256'], cache, True)
    segments, address, identity = board.base.image_parts(bundle / 'image.elf')
    require(identity.hex() == p['build_id'], 'Build identity differs.')
    for name, data in zip(('request.bin', 'contract.bin'), board.request_bytes(address, identity, p['epoch'], p['generation'], p['lease'], raw)):
        require((bundle / name).read_bytes() == data, 'Launch request/contract differs.')
    expected = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin';require((bundle / name).read_bytes() == segment['data'], 'Image segment differs.')
            expected.append({'name': name, 'address': segment['address']})
    expected += [{'name': 'session.bin', 'address': 0x41f00000}, {'name': 'profile.bin', 'address': 0x43000000}]
    for index, ((address, size), item) in enumerate(zip(spans, admitted['payloads'])):
        name = f'firmware-{index}.bin';require(len((bundle / name).read_bytes()) == size and digest(bundle / name) == item['sha256'] and int(item['address']) == address, 'Firmware changed.')
        expected.append({'name': name, 'address': address})
    require(len(admitted['payloads']) == 4 and p['segments'] == expected and
        p['backup_sizes'] == {n: s for n, a, s in board.base.REGIONS} and
        0 < (bundle / 'launch.bin').stat().st_size <= 0xc00, 'Unsealed upload map or SRAM reservation.')
    p['_prepared_sha256'] = hashlib.sha256(metadata).hexdigest()
    return p


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['image', 'manifest', 'admission', 'template', 'profile', 'payloads', 'output', 'clang', 'lld']:
        parser.add_argument('--' + name, type=Path, required=True)
    a = parser.parse_args()
    prepare(a.image, a.manifest, a.admission, a.template, a.profile, json.loads(a.payloads.read_text()), a.output, a.clang, a.lld)
