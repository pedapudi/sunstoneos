#!/usr/bin/env python3
"""Prepare the resident recorded OS card bundle on ordinary files only.

Uses the existing monitor package, four-extent slot plan and backup-first
storage sessions. The cold policy identifies evidence; it is not physical
qualification or permission to write a card. See docs/designs/autonomous-card-os.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
from types import SimpleNamespace
import importlib.util

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


host = module('card_os_host', 'apps/os/prepare.py')
slots = module('card_os_slots', 'tools/boot-slots.py')
package = module('card_os_package', 'tools/prepare-microsd-boot.py')
updates = module('card_os_updates', 'tools/storage-update.py')
require = host.require
sha = slots.sha
SCHEMA = 'a733-os-card-bundle-v1'
POLICY = 'a733-os-card-policy-v1'
# Existing session evidence identities; their bytes must be preserved.
EVIDENCE = {'qualification': 80, 'wiring': 216, 'memory': 248, 'recovery': 280,
            'thermal_initial': 632, 'thermal_electrical': 664,
            'thermal_clocks': 696, 'thermal_supervision': 728}


def canonical_session(raw):
    require(len(raw) in (808, 904), 'Recorded card sessions have exactly 808 or 904 bytes.')
    result = bytearray(raw)
    result[24:40] = bytes(16)
    if len(raw) == 904:
        result[840:856] = bytes(16)
        result[872:904] = bytes(32)
    return bytes(result)


def template_check(raw):
    require(len(raw) in (808, 904) and struct.unpack_from('<2Q', raw, 24) == (0, 0) and
            struct.unpack_from('<I', raw, 400)[0] == 2,
            'Cold recorded template requires workload 2 and zero epoch/generation.')
    require(struct.unpack_from('<Q', raw, 312)[0] and any(raw[320:352]), 'Cold SID binding required.')
    if len(raw) == 904:
        require(struct.unpack_from('<2Q', raw, 840) == (0, 0) and
                struct.unpack_from('<I', raw, 856)[0] == 3,
                'Cold CPU grant requires both clusters and zero epoch/generation.')
    # Reuse the native admission's existing host statement with private,
    # representative identities. These bytes never enter the output or hashes.
    representative = bytearray(raw)
    struct.pack_into('<2Q', representative, 24, 1, 1)
    if len(raw) == 904:
        struct.pack_into('<2Q', representative, 840, 1, 1)
    host.session_check(representative, False)


def flat_runtime(image):
    segments, _, build_id = host.base.image_parts(SimpleNamespace(read_bytes=lambda: image))
    initialized = [s for s in segments if s['data']]
    end = max(s['address'] + len(s['data']) for s in initialized)
    require(end <= 0x41f00000, 'Flat runtime would overwrite the session reservation.')
    result = bytearray(end - 0x41000000)
    for s in initialized:
        offset = s['address'] - 0x41000000
        result[offset:offset + len(s['data'])] = s['data']
    return bytes(result), build_id


def monitor_check(elf_bytes, binary):
    """Bind the flat monitor to actual initialized stage-2 ELF sections."""
    elf = host.os_link.Elf(elf_bytes)
    require(struct.unpack_from('<HH', elf.data, 16) == (2, 183), 'Stage 2 must be AArch64 executable ELF.')
    names = {s['name'] for s in elf.symbols if s['shndx'] != 0}
    require({'a733_cold_image', 'a733_stage2_main', 'a733_stage2_report'} <= names,
            'Expected the actual cold stage-2 monitor.')
    data = bytearray()
    for section in elf.sections:
        _, kind, flags, address, offset, length, *_ = section
        if not flags & 2 or not length:
            continue
        require(0x47000000 <= address < address + length <= 0x47400000, 'Stage-2 section leaves its reservation.')
        if kind == 8:  # Native cold entry clears NOLOAD BSS and owns its stack.
            continue
        require(kind == 1 and address + length <= 0x47008000, 'Unexpected initialized monitor section.')
        begin, end = address - 0x47000000, address + length - 0x47000000
        if end > len(data): data += bytes(end - len(data))
        data[begin:end] = elf.data[offset:offset + length]
    require(bytes(data) == binary, 'Stage-2 binary differs from its initialized ELF sections.')
    package.validate_monitor(binary)


def policy_check(policy_path, raw, image, manifest, monitor, model_fixture):
    policy_raw = policy_path.read_bytes()
    p = json.loads(policy_raw)
    require(p.get('schema') == POLICY and p.get('scope') == 'resident-recorded-cold-boot' and
            p.get('image_sha256') == sha(image) and p.get('stage2_sha256') == sha(monitor) and
            p.get('canonical_session_sha256') == sha(canonical_session(raw)) and
            p.get('inference') == manifest['inference'] and
            p.get('sid_authority') == struct.unpack_from('<Q', raw, 312)[0] and
            isinstance(p.get('identity'), str) and p['identity'], 'Cold policy binding differs.')
    require(p.get('evidence_class') in ('board-records', 'model-fixture') and
            (p['evidence_class'] != 'model-fixture' or model_fixture),
            'Modeled evidence requires explicit --model-fixture and never qualifies a physical install.')
    words = p.get('sid_words')
    require(isinstance(words, list) and len(words) == 4 and
            all(type(w) is int and 0 <= w < 2**32 for w in words) and
            hashlib.sha256(struct.pack('<4I', *words)).digest() == raw[320:352], 'Cold SID identity differs.')
    refs = p.get('evidence', {})
    require(set(refs) == set(EVIDENCE) | ({'cpu_b1'} if len(raw) == 904 else set()), 'Cold evidence set differs.')
    frozen = {}
    for name, ref in refs.items():
        require(isinstance(ref, dict) and set(ref) == {'path', 'sha256'} and
                isinstance(ref['path'], str), 'Invalid cold evidence reference.')
        data = package.read_regular(policy_path.parent / ref['path'], 16 << 20)
        require(sha(data) == ref['sha256'], 'Cold evidence changed: ' + name)
        if name in EVIDENCE:
            offset = EVIDENCE[name]
            require(hashlib.sha256(data).digest() == raw[offset:offset + 32], 'Session evidence differs: ' + name)
        frozen[name] = data
    if len(raw) == 904:
        b1, cpu = json.loads(frozen['cpu_b1']), p.get('cpu', {})
        require(b1.get('schema') == host.B1_SCHEMA and b1.get('passed') is True and
                b1.get('identity') == p['identity'] and b1.get('grant_clusters') == 0 and
                b1.get('pattern_lfm_clear') is True and type(b1.get('sid_word')) is int and
                all(type(b1.get(k)) is int and 800 <= b1[k] <= 1050 for k in ('dcdc3_mv', 'dcdc5_mv')),
                'Cold CPU policy requires matching read-only B1 evidence.')
        column = host.bin_column(b1['sid_word'])
        require(column is not None, 'Unsupported SID bin.')
        maxima = [host.supported(c, column)[-1] for c in ('cpu-l', 'cpu-b')]
        g = host.grant_fields(raw)
        require(cpu == {'clusters': 3, 'max_mhz': maxima, 'changes_per_boot': g['changes']} and
                list(g['max_mhz']) == maxima and 0 < g['changes'] <= 64 and
                g['approval'] == hashlib.sha256(policy_raw).digest(), 'Cold CPU policy/grant differs from vendor maxima.')
    else:
        require('cpu' not in p, 'CPU policy supplied without a linked grant.')
    return p, policy_raw, frozen


def prepare(a):
    image, monitor = a.image.read_bytes(), a.stage2.read_bytes()
    monitor_elf, manifest_raw = a.stage2_elf.read_bytes(), a.manifest.read_bytes()
    allocation_raw, card_raw = a.allocation.read_bytes(), a.card.read_bytes()
    manifest = json.loads(manifest_raw)
    elf = host.os_link.Elf(image)
    profile = host.os_link.contract_profile(elf)
    inference = host.os_link.inference_configuration(elf)
    require(manifest.get('passed') is True and manifest.get('target') == 'sunstone-a733-os' and
            manifest.get('lifetime') == profile['lifetime'] == 'resident' and
            manifest.get('image_sha256') == sha(image) and manifest.get('contract_profile') == profile and
            manifest.get('inference') == inference and
            not any(manifest.get(k) for k in ('audio', 'radio', 'headset', 'gpu_linear', 'npu', 'perf_modes')),
            'Expected the linked resident recorded OS profile.')
    runtime, build_id = flat_runtime(image)
    raw = a.session.read_bytes()
    require(len(raw) == profile['session_bytes'] and raw[48:80] == build_id, 'Session differs from the actual image.')
    template_check(raw)
    host.manifest_cpu_clock(manifest, raw)
    monitor_check(monitor_elf, monitor)
    policy, policy_raw, evidence = policy_check(a.policy, raw, image, manifest, monitor, a.model_fixture)
    declared = json.loads((ROOT / 'apps/bluetooth/recognition-models.json').read_text())['models']
    declared += [json.loads((ROOT / 'apps/mimus-board/inputs.json').read_text())['recorded_wav']]
    assets_header = SimpleNamespace(read_text=lambda: 'mimus_asset_address[3]={0x44000000,0x44800000,0x45000000};\n' +
                                   'mimus_asset_bytes[3]={' + ','.join(str(e['bytes']) for e in declared) + '};\n')
    linked = host.os_link.check(SimpleNamespace(read_bytes=lambda: image), assets_header, ROOT / 'apps/mimus/recorded.h',
                               ROOT / 'boot/a733/stage2/loader.c', cpu_clock=len(raw) == 904)
    require(all(manifest.get(k) == v for k, v in linked.items() if k != 'perf_boot_mode' or v is not None),
            'The actual linked services, capability or layout differs from the manifest.')
    assets = [a.base_model.read_bytes(), a.overlay_model.read_bytes(), a.wav.read_bytes()]
    for value, entry in zip(assets, declared):
        require(len(value) == entry['bytes'] and sha(value) == entry['sha256'], 'Recognition input identity differs.')
    linked_hashes = [s for s in elf.symbols if s['name'] in ('mimus_asset_sha256', '_ZL18mimus_asset_sha256') and s['shndx'] != 0]
    expected_hashes = b''.join(bytes.fromhex(e['sha256']) for e in declared)
    require(linked_hashes and all(s['size'] == 96 and elf.read(s['value'], 96) == expected_hashes for s in linked_hashes),
            'The linked recognizer authenticates different inputs.')
    models = assets[0] + bytes(0x800000 - len(assets[0])) + assets[1]
    allocation = json.loads(allocation_raw)
    fd, sectors = slots.open_image(a.backup)
    try:
        slots.check_allocation(allocation, sectors, slots.read_at(fd, 0, 1))
        require(allocation.get('backup') == {'bytes': sectors * 512, 'sha256': updates.hash_fd(fd).hex()},
                'Original backup identity differs from the allocation.')
    finally:
        os.close(fd)
    out = slots.new_directory(a.output)
    for name, data in {'runtime.bin': runtime, 'session-template.bin': raw, 'models.bin': models,
                       'recorded.wav': assets[2], 'stage2.bin': monitor, 'image.elf': image,
                       'stage2.elf': monitor_elf, 'image-manifest.json': manifest_raw,
                       'cold-policy.json': policy_raw, 'allocation.json': allocation_raw,
                       **{'evidence-' + n + '.bin': d for n, d in evidence.items()}}.items():
        slots.write_new(out / name, data)
    contract = {'schema': 1, 'kind': package.INPUT_KIND,
                'monitor': {'format': package.MONITOR_FORMAT, 'path': 'stage2.bin', 'bytes': len(monitor), 'sha256': sha(monitor)}}
    slots.write_new(out / 'monitor-input.json', (json.dumps(contract, indent=2) + '\n').encode())
    package.prepare(SimpleNamespace(input=out / 'monitor-input.json', boot0=a.boot0, packer=a.packer,
                                    output=out / 'boot', restore_variant=None))
    plan = slots.plan(SimpleNamespace(allocation=out / 'allocation.json', card_image=a.card_image,
                      image=str(out / 'runtime.bin') + '@0x41000000', entry=0x41000000,
                      os_session=out / 'session-template.bin',
                      asset=[str(out / 'models.bin') + '@0x44000000', str(out / 'recorded.wav') + '@0x45000000'],
                      container=out / 'boot.manifest.json', output=out / 'slot'))
    updates.plan(out / 'slot/plan.json', out / 'allocation.json', a.card_image,
                 json.loads(card_raw), out / 'storage')
    result = {'schema': SCHEMA, 'physical_qualified': False, 'model_fixture': policy['evidence_class'] == 'model-fixture',
              'inference': inference, 'cpu_vendor_top_policy': len(raw) == 904, 'generation': plan['generation'],
              'slot': plan['slot'], 'runtime_bytes': len(runtime), 'models_bytes': len(models),
              'files': {str(p.relative_to(out)): sha(p.read_bytes()) for p in sorted(out.rglob('*')) if p.is_file()},
              'physical_prerequisites': ['reviewed card allocation and backup-first range sessions',
                'cold entry and remote-writer hardware admission', 'rail, thermal and memory qualification',
                'whole-path autonomous boot and watchdog recovery qualification']}
    slots.write_new(out / 'bundle.json', (json.dumps(result, indent=2) + '\n').encode())
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'manifest', 'stage2', 'stage2-elf', 'session', 'policy', 'base-model',
                 'overlay-model', 'wav', 'allocation', 'backup', 'card-image', 'card', 'boot0', 'packer', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--model-fixture', action='store_true')
    a = p.parse_args()
    try:
        print(json.dumps(prepare(a), indent=2))
    except (AssertionError, ValueError, OSError, KeyError, TypeError, struct.error) as error:
        p.exit(1, 'card-os: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
