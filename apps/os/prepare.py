#!/usr/bin/env python3
"""Offline preparation of one supervised integrated OS session (docs/os-image.md).

No hardware access. The bundle holds the linked image, its link-check record as
the image manifest, the reviewed entry record with every evidence byte it cites,
the 808-byte session (992 bytes with audio), the SRAM launcher built for the OS
contract, and the three fixed recognition inputs. The reviewed entry record is
mandatory: it carries the board's supervised entry facts, the thermal owner's
facts, the DDR reservation of the recognition inputs and, for the audio build,
the I2S receive and DMA0 authority. The session identities come from the
reviewed template, so the DMA0 authority can name them; only the linked build
identity is written here.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


mimus = module('os_mimus_board_preparation', ROOT / 'apps/mimus-board/prepare.py')
thermal = module('os_thermal_preparation', ROOT / 'apps/thermal/prepare.py')
security_authority = module('os_dma_security', ROOT / 'tools/dma-nonsecure-access.py')
board, host = mimus.board, mimus.host
base = board.base
require, digest, save_json = base.require, base.digest, base.save_json
SCHEMA = 'a733-os-supervised-bundle-v1'
COMBINED_SCHEMA = 'a733-os-supervised-combined-v1'
ENTRY_SCHEMA = 'a733-os-supervised-entry-v1'
AUDIO_SCHEMA = 'a733-os-audio-entry-v1'
EXECUTION_PROFILE = 'os-supervised-v1'
CONTRACT_MAGIC = 0x415337334f534331
REQUEST_MAGIC = 0x415337334f534c31
SESSION_MAGIC = 0x415337334f534131
AUDIO_MAGIC = 0x415337334f415531
SESSION_ADDRESS = 0x41f00000
BOARD_BYTES, OS_BYTES, AUDIO_BYTES = 592, 808, 184
# Offsets in the audio extension, which starts at OS_BYTES (apps/os/os.h).
AUDIO_EVIDENCE, AUDIO_DMA, AUDIO_DMA_BYTES, AUDIO_TAIL = 832, 864, 104, 968
AUDIO_CONDITIONS = {
    'dma0-cpu-and-remote-users-excluded', 'fifo-producers-quiescent-and-prior-dma-drained',
    'dma-route-and-irq143-disabled-inactive-clear', 'audio-parent-and-shared-clock-ownership',
    'i2s0-pins-bank-voltage-and-microphone-wiring', 'device-mappings-and-cache-alias-exclusion',
    'manual-whole-board-power-removal', 'retain-all-resources-until-power-removal',
    'stop-after-first-failure'}


def image_manifest_check(manifest, raw):
    """The OS link check of these exact bytes; returns whether audio is built in."""
    audio = manifest.get('audio')
    require(not manifest.get('radio') and not manifest.get('headset'),
            'Preparation does not yet stage the radio or Bluetooth profile and firmware.')
    require(manifest.get('passed') is True and manifest.get('target') == 'sunstone-a733-os' and
            manifest.get('lifetime') == 'supervised' and type(audio) is bool and
            manifest.get('image_sha256') == hashlib.sha256(raw).hexdigest(),
            'Passed supervised OS link check of this image required.')
    require([a.get('name') for a in manifest.get('applications', [])] ==
            ['mimus', 'thermal'] + (['audio'] if audio else []) and
            [i.get('intid') for i in manifest.get('interrupts', [])] == [71] + ([143] if audio else []),
            'OS start list or interrupt table differs from the link check.')
    return audio


def session_check(raw, audio):
    """Python statement of apps/os/admission.c os_session_valid for the bundle."""
    require(len(raw) == OS_BYTES + (AUDIO_BYTES if audio else 0), 'OS session size differs from the image.')
    require(board.session_check(raw[:BOARD_BYTES], usb_reference_modes=(1,)) in (0, 4),
            'The OS image admits board workloads 0 and 4 only.')
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    require(epoch and generation and lease, 'Reviewed session identities required.')
    require(struct.unpack_from('<3Q', raw, BOARD_BYTES) == (SESSION_MAGIC, 1, len(raw)), 'Invalid OS session header.')
    owners = struct.unpack_from('<4Q', raw, 456)
    owner, authority = struct.unpack_from('<2Q', raw, 616)
    require(owner and authority and owner not in owners, 'Invalid thermal owner.')
    # The thermal fields follow the thermal application's rules (apps/thermal/prepare.py).
    require(all(any(raw[o:o + 32]) for o in (632, 664, 696, 728)), 'Missing thermal physical evidence.')
    apb, hz, configure, alarm, critical, reserved = struct.unpack_from('<2I4B', raw, 760)
    rate, div = {0: 24000000, 3: 600000000}.get(apb >> 24, 0), (apb & 31) + 1
    require(not apb & ~0x0300001f and rate and rate % div == 0 and hz == rate // div and configure in (0, 1) and
            alarm == critical == 31 and not reserved and raw[802:808] == bytes(6),
            'Unsupported thermal clock or control profile.')
    for i in range(5):
        hot, hysteresis, limit = struct.unpack_from('<3H', raw, 772 + 6 * i)
        require(0 < limit <= hot < hysteresis <= 4095, 'Invalid thermal thresholds.')
    if not audio:
        return
    a = raw[OS_BYTES:]
    require(struct.unpack_from('<3Q', a) == (AUDIO_MAGIC, 1, AUDIO_BYTES) and any(a[24:56]),
            'Invalid OS audio header or missing audio evidence.')
    fabric, rates = raw[AUDIO_DMA:AUDIO_DMA + 12], raw[AUDIO_DMA + 12:AUDIO_DMA + 24]
    dma_owner, handoff, deadline = struct.unpack_from('<3Q', raw, AUDIO_DMA + 48)
    controller, evidence, established, owned, service = struct.unpack_from('<2QI2H', raw, AUDIO_DMA + 80)
    audio_cold, samples = struct.unpack_from('<2I', raw, AUDIO_TAIL)
    require(dma_owner and dma_owner not in owners + (owner,), 'Invalid audio DMA owner.')
    require(handoff == evidence == lease and not deadline, 'Audio DMA handoff must name the session lease.')
    require((controller, established, owned, service) == (0x04601000, 0x5f, 0xffff, 1),
            'Audio requires the supervised single-channel DMA0 handoff.')
    require(fabric == raw[488:500] and rates == raw[500:512], 'Audio DMA and board fabric profiles differ.')
    require(audio_cold <= 1 and 16000 <= samples <= 80000 and raw[985:992] == bytes(7),
            'Unsupported audio capture profile.')


def capture_profile(raw):
    """The session identities and, with audio, the capture profile."""
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    profile = {'epoch': epoch, 'generation': generation, 'lease': lease}
    if len(raw) > OS_BYTES:
        profile['audio_cold'], profile['capture_samples'] = struct.unpack_from('<2I', raw, AUDIO_TAIL)
    return profile


def request_bytes(address, identity, raw):
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    request, contract = board.request_bytes(address, identity, epoch, generation, lease, raw)
    request = struct.pack('<Q', REQUEST_MAGIC) + request[8:]
    contract = bytearray(contract)
    struct.pack_into('<Q', contract, 184, len(raw))
    return request, bytes(contract)


def build_launcher(out, session_bytes, clang, lld):
    """The board's SRAM launcher with the OS request, contract and session size."""
    text = (ROOT / 'platform/a733-smp-supervised/launch.c').read_text()
    (out / 'base-supervised-launch.c').write_text(text)
    text = board.adapt_launcher(text, {
        '0x4153373353554c33': '0x415337334f534c31', 'REQUEST[1]==3': 'REQUEST[1]==2',
        'CONTRACT->version==3': 'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':
            'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==%du' % session_bytes,
        '20*A733_SMP_COUNTER_HZ': 'A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ'})
    (out / 'board-launch-base.c').write_text(text)
    for name, path in [('launch.c', ROOT / 'apps/board/launch.c'), ('launch.S', ROOT / 'platform/a733-smp-observe/launch.S'),
                       ('launch.ld', ROOT / 'platform/a733-smp-observe/launch.ld'),
                       ('boot.h', ROOT / 'platform/a733-smp-cached/boot.h'),
                       ('a733_board_application.h', ROOT / 'include/a733_board_application.h')]:
        shutil.copy2(path, out / name)
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall',
             '-Wextra', '-Werror', '-DSUNSTONE_BOARD_SUPERVISED=1', f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC:#x}ULL',
             '-I' + str(out)]
    commands = [[clang, *flags, '-c', out / name, '-o', out / (name + '.o')] for name in ('launch.c', 'launch.S')]
    commands += [[lld, '-T', out / 'launch.ld', out / 'launch.c.o', out / 'launch.S.o', '-o', out / 'launch.elf'],
                 ['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', out / 'launch.elf', out / 'launch.bin']]
    for command in commands:
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        with (out / 'launcher-build.log').open('a') as log:
            log.write(result.stdout + result.stderr)
        require(result.returncode == 0, 'OS launcher build failed; inspect the retained log.')
    save_json(out / 'launcher-commands.json', [[str(x) for x in c] for c in commands])
    require(0 < (out / 'launch.bin').stat().st_size <= 0xc00, 'OS launcher exceeds reserved SRAM.')


def validate_admission(record, image_hash, template_hash, raw, audio, cache=None, frozen=False):
    """Every fact the session names is bound to a reviewed record of this image."""
    require(record.get('schema') == ENTRY_SCHEMA, 'Reviewed OS entry record required.')
    profile = capture_profile(raw)
    require(all(record.get(k) == profile[k] for k in ('epoch', 'generation', 'lease')),
            'Reviewed entry names other session identities.')
    # The thermal check runs the board's DDR, platform and manual-recovery
    # checks, then binds the four thermal evidence hashes at their offsets.
    cache = thermal.validate_admission({**record, 'schema': 'a733-thermal-supervised-entry-v1'}, image_hash,
                                       template_hash, raw, cache, frozen, 'thermal-supervised-v1')

    def evidence(item):
        require(isinstance(item, dict) and set(item) == {'path', 'sha256'} and Path(item['path']).is_absolute(),
                'Invalid evidence reference.')
        if item['path'] not in cache:
            require(not frozen, 'Evidence missing from the frozen bundle.')
            cache[item['path']] = Path(item['path']).read_bytes()
        data = cache[item['path']]
        require(hashlib.sha256(data).hexdigest() == item['sha256'], 'Evidence changed.')
        return data
    require(isinstance(record.get('mimus_ddr'), dict), 'Recognition input DDR reservation required.')
    mimus.input_admission(record, image_hash, evidence)
    if not audio:
        require('audio' not in record and 'dma_nonsecure_access' not in record,
                'Audio authority supplied for an image without audio.')
        return cache
    require(isinstance(record.get('audio'), dict) and isinstance(record.get('dma_nonsecure_access'), dict),
            'Reviewed audio record and DMA0 authority required.')
    data = evidence(record['audio'])
    facts = json.loads(data)
    dma_sha256 = hashlib.sha256(raw[AUDIO_DMA:AUDIO_DMA + AUDIO_DMA_BYTES]).hexdigest()
    inherited_sha256 = hashlib.sha256(raw[976:985]).hexdigest()
    require(hashlib.sha256(data).digest() == raw[AUDIO_EVIDENCE:AUDIO_EVIDENCE + 32],
            'Session audio evidence differs from the reviewed audio record.')
    require(facts.get('schema') == AUDIO_SCHEMA and facts.get('identity') == record['identity'] and
            facts.get('port') == record['port'] and facts.get('image_sha256') == image_hash and
            all(facts.get(k) == v for k, v in profile.items()) and facts.get('dma_sha256') == dma_sha256 and
            facts.get('inherited_sha256') == inherited_sha256 and
            set(facts.get('conditions', {})) == AUDIO_CONDITIONS and facts.get('one_attempt') is True and
            facts.get('whole_board_power_removal') is True and
            facts.get('no_resource_reuse_before_power_removal') is True,
            'Reviewed microphone, DMA prior-drain and retained-supervision facts required.')
    for item in facts['conditions'].values():
        evidence(item)
    expected = {k: profile[k] for k in ('epoch', 'generation', 'lease')}
    expected.update(identity=record['identity'], port=record['port'], image_sha256=image_hash,
                    controller=0x04601000, service_channels=1, dma_evidence=profile['lease'],
                    dma_handoff=profile['lease'], configuration_sha256=dma_sha256)
    security_authority.validate(evidence(record['dma_nonsecure_access']), expected, evidence, require)
    return cache


def prepare(image, link_check, admission, template, assets, output, clang, lld):
    snapshots = {name: Path(path).read_bytes() for name, path in [
        ('image.elf', image), ('image-input-manifest.json', link_check), ('admission.json', admission),
        ('session-template.bin', template)]}
    image_hash = hashlib.sha256(snapshots['image.elf']).hexdigest()
    audio = image_manifest_check(json.loads(snapshots['image-input-manifest.json']), snapshots['image.elf'])
    raw = bytearray(snapshots['session-template.bin'])
    session_check(raw, audio)
    record = json.loads(snapshots['admission.json'])
    cache = validate_admission(record, image_hash, hashlib.sha256(raw).hexdigest(), raw, audio)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    out = output / 'board'
    out.mkdir()
    for name, data in snapshots.items():
        (out / name).write_bytes(data)
    segments, address, identity = base.image_parts(out / 'image.elf')
    raw[48:80] = identity
    build_launcher(out, len(raw), clang, lld)
    uploads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'
            (out / name).write_bytes(segment['data'])
            uploads.append({'name': name, 'address': segment['address']})
    (out / 'session.bin').write_bytes(raw)
    uploads.append({'name': 'session.bin', 'address': SESSION_ADDRESS})
    for name, data in zip(('request.bin', 'contract.bin'), request_bytes(address, identity, raw)):
        (out / name).write_bytes(data)
    for name, count in [('result-zero.bin', 16384), ('report-zero.bin', 128)]:
        (out / name).write_bytes(bytes(count))
    evidence = {}
    for index, (path, data) in enumerate(cache.items()):
        name = f'entry-evidence-{index}.bin'
        (out / name).write_bytes(data)
        evidence[path] = {'name': name, 'sha256': hashlib.sha256(data).hexdigest()}
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    inner = {'schema': SCHEMA, 'physical_access': False, 'execution_profile': EXECUTION_PROFILE, 'audio': audio,
             'image_sha256': image_hash, 'launcher_sha256': digest(out / 'launch.bin'), 'build_id': identity.hex(),
             'epoch': epoch, 'generation': generation, 'lease': lease, 'reservation': record['reservation'],
             'workload': struct.unpack_from('<I', raw, 400)[0], 'session': 'session.bin', 'profile': None,
             'manifest': 'image-input-manifest.json', 'image': 'image.elf', 'segments': uploads,
             'entry_evidence': evidence, 'backup_sizes': {name: n for name, a, n in base.REGIONS},
             'files': {x.name: digest(x) for x in out.iterdir() if x.is_file()}}
    save_json(out / 'prepared.json', inner)
    host.freeze_assets(assets, output / 'assets')
    value = {'schema': COMBINED_SCHEMA, 'physical_access': False, 'audio': audio,
             'board_prepared_sha256': digest(out / 'prepared.json'),
             'assets_sha256': digest(output / 'assets/assets.json')}
    save_json(output / 'prepared.json', value)
    return validate_bundle(output)


def validate_board(bundle):
    """The board-format part: image, session, launcher and frozen evidence."""
    out = Path(bundle)
    metadata = (out / 'prepared.json').read_bytes()
    p = json.loads(metadata)
    require(p.get('schema') == SCHEMA and p.get('physical_access') is False and
            p.get('execution_profile') == EXECUTION_PROFILE, 'Unsupported OS bundle.')
    required = {'image.elf', 'image-input-manifest.json', 'admission.json', 'session-template.bin', 'session.bin',
                'request.bin', 'contract.bin', 'launch.bin', 'result-zero.bin', 'report-zero.bin'}
    require(required <= set(p['files']) and all(item['name'] in p['files'] for item in p['entry_evidence'].values()),
            'Unsealed OS authority or transport input.')
    for name, expected in p['files'].items():
        require(Path(name).name == name and digest(out / name) == expected, 'Prepared OS input changed.')
    image = (out / 'image.elf').read_bytes()
    require(hashlib.sha256(image).hexdigest() == p['image_sha256'] and digest(out / 'launch.bin') == p['launcher_sha256'],
            'OS image or launcher changed.')
    audio = image_manifest_check(json.loads((out / 'image-input-manifest.json').read_text()), image)
    require(p['audio'] is audio, 'Audio selection changed.')
    sizes = {name: (out / name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes') == {name: n for name, a, n in base.REGIONS} and 0 < sizes['launch.bin'] <= 0xc00,
            'Protected SRAM geometry changed.')
    require((out / 'result-zero.bin').read_bytes() == bytes(16384) and (out / 'report-zero.bin').read_bytes() == bytes(128),
            'Initial report backing changed.')
    template = (out / 'session-template.bin').read_bytes()
    raw = (out / 'session.bin').read_bytes()
    session_check(raw, audio)
    cache = {path: (out / item['name']).read_bytes() for path, item in p['entry_evidence'].items()}
    validate_admission(json.loads((out / 'admission.json').read_text()), p['image_sha256'],
                       hashlib.sha256(template).hexdigest(), template, audio, cache, True)
    segments, address, identity = base.image_parts(out / 'image.elf')
    require(identity.hex() == p['build_id'], 'Build identity changed.')
    derived = bytearray(template)
    derived[48:80] = identity
    require(raw == derived and struct.unpack_from('<3Q', raw, 24) == (p['epoch'], p['generation'], p['lease']) and
            struct.unpack_from('<I', raw, 400)[0] == p['workload'], 'Session differs from the reviewed template.')
    expected = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'
            require((out / name).read_bytes() == segment['data'], 'ELF segment changed.')
            expected.append({'name': name, 'address': segment['address']})
    expected.append({'name': 'session.bin', 'address': SESSION_ADDRESS})
    require(p['segments'] == expected, 'Unsealed or changed upload.')
    for name, data in zip(('request.bin', 'contract.bin'), request_bytes(address, identity, raw)):
        require((out / name).read_bytes() == data, 'Launch request or contract changed.')
    p['_sizes'] = sizes
    p['_prepared_sha256'] = hashlib.sha256(metadata).hexdigest()
    return p


def validate_bundle(output):
    output = Path(output)
    value = json.loads((output / 'prepared.json').read_text())
    require(value.get('schema') == COMBINED_SCHEMA and value.get('physical_access') is False, 'Bundle schema differs.')
    require(digest(output / 'board/prepared.json') == value['board_prepared_sha256'] and
            digest(output / 'assets/assets.json') == value['assets_sha256'], 'Bundle identity differs.')
    inner = validate_board(output / 'board')
    host.validate_assets(output / 'assets')
    require(value['audio'] is inner['audio'], 'Audio selection differs.')
    return value


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'link-check', 'admission', 'session-template', 'base', 'overlay', 'wav', 'output',
                 'clang', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    a = parser.parse_args()
    prepare(a.image, a.link_check, a.admission, a.session_template, [a.base, a.overlay, a.wav], a.output, a.clang, a.lld)
