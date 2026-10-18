#!/usr/bin/env python3
"""Prepare native recorded/paced recognition and durably capture its output.

Physical execution belongs to the exclusive board coordinator. The connected
launch function accepts its already protected transaction and reconnect hooks.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('native_mimus_launch', ROOT/'tools/check-a733-storage-launch.py')
shared = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(shared)
require, digest, save_json = shared.require, shared.digest, shared.save_json
TARGET = 'sunstone-a733-mimus-native'
REQUEST_MAGIC = 0x415337334d494c31


def session_bytes(config, image, assets, paced, epoch, generation, lease, nonce=None):
    minimum, maximum, guard, reset, deadline = (config[k] for k in
        ('minimum_reload_us', 'maximum_recovery_us', 'guard_us', 'reset_by_us', 'deadline_us'))
    require(all(shared.u64(x) and x <= (2**64-1)//24 for x in (minimum, maximum, guard, reset, deadline)) and
            minimum > guard+1000000 and maximum >= minimum and reset > maximum and
            guard < deadline <= reset-maximum, 'Invalid absolute watchdog bounds.')
    require(len(assets) >= 128 and len(assets) <= 32*1024*1024 and assets[:8] == b'SUNMIM01', 'Invalid asset image.')
    offset = 128
    for size in struct.unpack_from('<3Q', assets, 8):
        require(size and offset <= len(assets) and size <= len(assets)-offset, 'Asset span exceeds authenticated bytes.')
        offset = (offset+size+63)&~63
        require(offset <= len(assets), 'Asset padding exceeds authenticated bytes.')
    require(len(config['owners']) == 2 and len(set(config['owners'])) == 2 and
            all(shared.u64(x) for x in (*config['owners'], config['topology'])), 'Invalid provider identities.')
    b = bytearray(704)
    struct.pack_into('<5Q', b, 8, 1, 704, epoch, generation, lease)
    b[48:80] = bytes.fromhex(image)
    b[80:112] = bytes.fromhex(config['qualification_sha256'])
    struct.pack_into('<4Q', b, 112, minimum, maximum, guard, reset)
    struct.pack_into('<3I', b, 144, *config['rtc'])
    struct.pack_into('<3Q', b, 160, 1, int(paced), len(assets))
    b[184:216] = secrets.token_bytes(32) if nonce is None else nonce
    b[216:248] = hashlib.sha256(assets).digest()
    struct.pack_into('<Q', b, 256, deadline)
    struct.pack_into('<Q', b, 416, config['topology'])
    b[424:456] = bytes.fromhex(config['topology_artifact'])
    struct.pack_into('<4I', b, 488, *config['usb_supply_uv'])
    struct.pack_into('<2Q', b, 512, *config['owners'])
    struct.pack_into('<3I', b, 528, *config['fabric'])
    struct.pack_into('<3I', b, 540, *config['fabric_hz'])
    struct.pack_into('<I', b, 556, config['usb_cold_reference'])
    require(len(config['pmic_expected']) == 2 and all(len(p) == 3 for p in config['pmic_expected']), 'Invalid PMIC pin route.')
    struct.pack_into('<6B', b, 632, *(v for p in config['pmic_expected'] for v in p))
    require(len(b) == 704 and any(b[80:112]) and any(b[424:456]), 'Missing recovery or topology evidence identity.')
    return bytes(b)


def prepare(image, assets_path, configuration, output, paced, clang, lld):
    segments, address, identity = shared.image_parts(image)
    config = json.loads(configuration.read_text())
    assets = assets_path.read_bytes()
    epoch, generation, lease = (secrets.randbits(64) or 1 for _ in range(3))
    session = session_bytes(config, digest(image), assets, paced, epoch, generation, lease)
    output.mkdir(parents=True, exist_ok=False)
    shared.build_launcher(output, ROOT/'apps/mimus/session.h',
                          (clang, lld, '-DSUNSTONE_MIMUS_NATIVE=1', '-I'+str(ROOT/'apps/mimus')))
    shutil.copy2(image, output/'image.elf')
    shutil.copy2(configuration, output/'configuration.json')
    (output/'assets.bin').write_bytes(assets)
    payloads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'
            (output/name).write_bytes(segment['data'])
            payloads.append(dict(name=name, address=segment['address']))
    payloads.append(dict(name='assets.bin', address=0x43000000))
    request, contract = shared.request_bytes(address, identity, epoch, generation, lease, session)
    request = struct.pack('<Q', REQUEST_MAGIC)+request[8:]
    for name, data in [('session.bin', session+bytes(4096-704)), ('request.bin', request),
                       ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]:
        (output/name).write_bytes(data)
    record = dict(schema='a733-native-mimus-launch-1', target=TARGET, epoch=epoch, generation=generation,
                  lease=lease, image_sha256=digest(image), build_id=identity.hex(), paced=bool(paced),
                  segments=payloads, configuration_sha256=digest(configuration), assets_sha256=digest(assets_path),
                  host_sha256=digest(Path(__file__)), shared_host_sha256=digest(Path(shared.__file__)),
                  base_host_sha256=digest(Path(shared.base.__file__)), physical_access=False,
                  files={p.name:digest(p) for p in output.iterdir() if p.is_file()})
    save_json(output/'prepared.json', record)
    return record


def validate_bundle(bundle):
    record = json.loads((bundle/'prepared.json').read_text())
    require(record.get('schema') == 'a733-native-mimus-launch-1' and record.get('target') == TARGET and
            record.get('physical_access') is False, 'Wrong native recognition bundle.')
    require(record['host_sha256'] == digest(Path(__file__)) and
            record['shared_host_sha256'] == digest(Path(shared.__file__)) and
            record['base_host_sha256'] == digest(Path(shared.base.__file__)), 'Launcher tools changed.')
    for name, expected in record['files'].items():
        require(Path(name).name == name and digest(bundle/name) == expected, 'Prepared artifact changed.')
    segments, address, identity = shared.image_parts(bundle/'image.elf')
    expected = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'
            require((bundle/name).read_bytes() == segment['data'], 'Runtime segment bytes differ from ELF.')
            expected.append(dict(name=name, address=segment['address']))
    expected.append(dict(name='assets.bin', address=0x43000000))
    require(record['segments'] == expected and record['build_id'] == identity.hex() and
            record['image_sha256'] == digest(bundle/'image.elf'), 'Runtime segment layout or identity differs.')
    raw = (bundle/'session.bin').read_bytes()
    config = json.loads((bundle/'configuration.json').read_text())
    session = session_bytes(config, digest(bundle/'image.elf'), (bundle/'assets.bin').read_bytes(),
                            record['paced'], record['epoch'], record['generation'], record['lease'], raw[184:216])
    request, contract = shared.request_bytes(address, identity, record['epoch'], record['generation'], record['lease'], session)
    request = struct.pack('<Q', REQUEST_MAGIC)+request[8:]
    require(raw == session+bytes(4096-704) and (bundle/'request.bin').read_bytes() == request and
            (bundle/'contract.bin').read_bytes() == contract, 'Application wire records differ from prepared inputs.')
    require((bundle/'result-zero.bin').read_bytes() == bytes(16384) and
            (bundle/'report-zero.bin').read_bytes() == bytes(128), 'Initial result records are not zero.')
    required = {'image.elf', 'assets.bin', 'session.bin', 'request.bin', 'contract.bin', 'result-zero.bin',
                'report-zero.bin', 'launch.bin', 'configuration.json', *(x['name'] for x in expected)}
    require(required <= record['files'].keys(), 'Prepared bundle omits required artifacts.')
    return record


def capture(exchange, output, maximum_exchanges=262144):
    """exchange(request) returns one full USB response under coordinator locks.

    Every request acknowledges only data fsynced by the preceding iteration.
    The final acknowledgement must itself complete before success is recorded.
    """
    offset = 0
    output.mkdir(parents=True, exist_ok=False)
    with (output/'recognition.log').open('xb') as stream:
        for _ in range(maximum_exchanges):
            response = exchange(struct.pack('<2I', offset, offset))
            require(len(response) >= 16, 'Short native capture response.')
            total, finished, returned, size = struct.unpack_from('<4I', response)
            require(returned == offset and size <= 1024 and len(response) == 16+size and
                    offset+size <= total <= 256*1024 and finished in (0, 1, 2), 'Invalid native capture response.')
            require(finished != 2, 'Native recognition failed.')
            stream.write(response[16:]); stream.flush(); os.fsync(stream.fileno())
            offset += size
            if finished == 1 and offset == total:
                if size:
                    final = exchange(struct.pack('<2I', offset, offset))
                    require(final == struct.pack('<4I', total, 1, offset, 0), 'Final capture acknowledgement failed.')
                receipt = dict(schema='a733-native-mimus-capture-1', bytes=offset,
                               sha256=digest(output/'recognition.log'), durable=True)
                save_json(output/'complete.json', receipt)
                return receipt
    raise ValueError('Native capture exceeded its declared exchange bound.')


def validate_admission(value, prepared, config, identity, port, sysfs):
    binding = dict(target=dict(soc='00001903', port=port, sysfs=sysfs, instance=identity),
                   image_sha256=prepared['image_sha256'], build_id=prepared['build_id'],
                   epoch=prepared['epoch'], generation=prepared['generation'], lease=prepared['lease'],
                   session_sha256=prepared['files']['session.bin'], assets_sha256=prepared['files']['assets.bin'],
                   configuration_sha256=prepared['files']['configuration.json'],
                   dram=[[0x41000000, 0x42200000], [0x43000000, 0x45000000], [0x46000000, 0x47000000]],
                   sram=shared.REGIONS)
    binding = json.loads(json.dumps(binding))
    require(value.get('schema') == 'a733-native-mimus-admission-1' and value.get('binding') == binding and
            value.get('physical_qualification') is True and value.get('ownership_retained') is True,
            'Current native recognition physical admission is absent or mismatched.')
    conditions = {'dram-cpu-coherence', 'remote-dma-alias-exclusion', 'cold-controller-irq-exclusion',
                  'board-topology-supplies', 'qualified-reload-bounds', 'sram-workspace',
                  'independent-watchdog-rom-return', 'stable-protected-fel'}
    require(set(value.get('conditions', {})) == conditions, 'Missing native physical prerequisites.')
    for name, reference in value['conditions'].items():
        condition = shared.read_record(reference)
        require(condition.get('condition') == name and condition.get('binding') == binding and
                condition.get('physical_access') is True and condition.get('established') is True and
                condition.get('observations'), 'Unqualified native physical condition: '+name)
        for observation in condition['observations']:
            shared.base.artifact(observation)
        if name == 'qualified-reload-bounds':
            require(condition.get('bounds') == {k:config[k] for k in
                    ('minimum_reload_us', 'maximum_recovery_us', 'guard_us', 'reset_by_us', 'rtc')},
                    'Qualified reload bounds differ from native session.')
        if name == 'sram-workspace':
            require(condition.get('preservation_restoration_qualified') is True, 'Expanded SRAM restoration is unqualified.')


def connected_launch(transaction, wait_runtime, wait_rom, ready, bundle, evidence, record, save,
                     admission, identity, port, sysfs):
    """Called only by the qualified coordinator holding board and port locks."""
    prepared = validate_bundle(bundle)
    validate_admission(admission, prepared, json.loads((bundle/'configuration.json').read_text()), identity, port, sysfs)
    shared.preload(transaction, bundle, prepared, evidence)
    words = [int(x, 16) for x in transaction(*[a for _ in range(4) for a in ('readl', '0x03000024')]).split()]
    require(len(words) == 4 and all(0 <= x <= 0xffffffff and not x&0x100 for x in words), 'FEL input changed.')
    require(int(transaction('readl', '0x02050014').strip(), 16)&1 == 0, 'Watchdog became active before native execution.')
    ready()
    record.update(launch_attempted=True, execution_intent=dict(entry=shared.ENTRY,
                  epoch=prepared['epoch'], generation=prepared['generation'], lease=prepared['lease']))
    save()
    transaction('exe', hex(shared.ENTRY))
    exchange = wait_runtime()
    require(exchange is not None, 'Native recognition did not enumerate.')
    record['capture'] = capture(exchange, evidence/'capture'); save()
    wait_rom()
    record.update(complete=True, recovery_limit='ROM return and hardware reset cause require independent physical evidence.')
    save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'assets', 'configuration', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--paced', action='store_true')
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--lld', default='ld.lld')
    args = parser.parse_args()
    print(json.dumps(prepare(args.image, args.assets, args.configuration, args.output,
                             args.paced, args.clang, args.lld), indent=2))


if __name__ == '__main__':
    main()
