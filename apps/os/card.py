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


NPU_WORK_US = 60000000
GPU_RESET = 'watchdog-reset-retires-gpu-before-reload-and-reuse'
GPU_NPU_CLOCK = 'cold-concurrent-gpu-npu-cross-pll-qualified'
GPU_CONDITIONS = (host.GPU_CONDITIONS - {'manual-whole-board-power-removal-and-no-reuse'}) | {
    GPU_RESET, 'cold-entry-excludes-prior-gpu-writers',
    'retain-until-qualified-cold-reset', 'stop-after-first-failure'}


def condition_bytes(name, ref, directory, frozen):
    require(isinstance(ref, dict) and set(ref) == {'path', 'sha256'} and isinstance(ref['path'], str),
            'Invalid cold condition reference.')
    data = package.read_regular(directory / ref['path'], 16 << 20)
    require(sha(data) == ref['sha256'], 'Cold condition evidence changed.')
    frozen[name] = data
    return data


PERF_CONDITIONS = {'runtime-operating-point-transitions-qualified',
    'thermal-interruption-and-exhausted-budget-qualified',
    'uncertain-transition-retired-before-cold-reuse'}
PERF_CROSS = 'concurrent-gpu-npu-runtime-cross-pll-qualified'


def performance_configuration(elf, manifest, raw):
    """Actual linked mode row ([MHz, mV] points) and finite CPU authority."""
    perf = manifest.get('perf_modes', False)
    require(type(perf) is bool, 'Performance capability must be boolean.')
    if not perf:
        return None
    require(manifest.get('perf_operation') == 8 and manifest.get('perf_boot_mode') in ('high', 'low'),
            'Resident performance capability differs.')
    rows = [s for s in elf.symbols if s['name'] == 'a733_perf_modes' and s['shndx'] != 0]
    require(len(rows) == 1 and rows[0]['size'] == 88, 'Expected the actual single-bin performance table.')
    column, name, *values = struct.unpack('<IH2x20I', elf.read(rows[0]['value'], 88))
    require((column, name) == (4, 0x0400), 'Resident performance bin is unsupported.')
    blocks = ('cpu-l', 'cpu-b', 'dsu', 'gpu', 'npu')
    points = {mode: {block: values[base + i*2:base + i*2 + 2] for i, block in enumerate(blocks)}
              for mode, base in (('high', 0), ('low', 10))}
    g = host.grant_fields(raw)
    require(g is not None and g['clusters'] == 3 and 5 <= g['changes'] <= 64 and
            list(g['max_mhz']) == [points['high'][b][0] for b in blocks[:2]],
            'Resident performance needs both CPU maxima and a bounded boot/thermal grant.')
    gpu = host.os_link.gpu_configuration(elf, required=manifest.get('gpu_linear', False))
    if gpu:
        expected = {'version': 1, 'boot_mode': manifest['perf_boot_mode']}
        for mode in ('high', 'low'):
            expected[mode + '_hz'] = points[mode]['gpu'][0] * 1000000
            expected[mode + '_uv'] = points[mode]['gpu'][1] * 1000
        require(gpu.get('performance') == expected, 'GPU runtime points differ from the linked performance table.')
    owned = list(blocks[:3]) + (['gpu'] if manifest.get('gpu_linear') else []) + (['npu'] if host.npu_session(raw) else [])
    return {'version': 1, 'boot_mode': manifest['perf_boot_mode'], 'operation': 8,
            'bin_column': column, 'bin_name': name, 'points': points, 'owned': owned,
            'cpu_changes_per_boot': g['changes'], 'thermal_restore_reserve': 2}


def performance_policy(policy, configuration, directory, frozen):
    if configuration is None:
        require('performance' not in policy, 'Runtime performance authority supplied without service.')
        return
    perf = policy.get('performance')
    conditions = PERF_CONDITIONS | ({PERF_CROSS} if {'gpu', 'npu'} <= set(configuration['owned']) else set())
    if 'gpu' in configuration['owned']:
        conditions.add(host.GPU_PERF_CONDITION)
    require(isinstance(perf, dict) and set(perf) == {'configuration', 'conditions'} and
            perf['configuration'] == configuration and isinstance(perf['conditions'], dict) and
            set(perf['conditions']) == conditions, 'Cold runtime performance policy differs.')
    for condition, ref in perf['conditions'].items():
        fact = json.loads(condition_bytes('performance_' + condition, ref, directory, frozen))
        require(fact.get('schema') == 'a733-os-card-performance-condition-v1' and
                fact.get('scope') == 'resident-runtime-operating-points' and
                fact.get('condition') == condition and fact.get('configuration') == configuration and
                fact.get('reviewed') is True and all(fact.get(k) == policy[k] for k in
                    ('identity', 'sid_authority', 'image_sha256', 'stage2_sha256', 'canonical_session_sha256', 'evidence_class')),
                'Cold runtime performance condition names another capability or lifetime.')
        physical = policy['evidence_class'] == 'board-records'
        require(fact.get('physical_authority') is physical and fact.get('modeled') is (not physical),
                'Cold runtime performance evidence class differs.')
        require(isinstance(fact.get('evidence'), list) and fact['evidence'], 'Runtime performance evidence is missing.')
        for i, item in enumerate(fact['evidence']):
            condition_bytes('performance_' + condition + '_' + str(i), item, directory, frozen)


def gpu_policy(policy, config, raw, directory, frozen):
    if config is None:
        require('gpu' not in policy, 'GPU cold authority supplied without GPU.')
        return
    gpu = policy.get('gpu')
    require(isinstance(gpu, dict) and set(gpu) == {'configuration', 'conditions'} and
            gpu['configuration'] == config, 'Cold GPU configuration differs from the actual linked image.')
    combined = host.npu_session(raw)
    # Same fixed identities as dispatch.h and the native pre-provider guard.
    owners = list(struct.unpack_from('<4Q', raw, 456))
    owners += [struct.unpack_from('<Q', raw, offset)[0] for offset in (416, 440, 616)]
    grant = host.grant_fields(raw)
    if grant: owners.append(grant['owner'])
    if combined:
        owners.append(struct.unpack_from('<Q', raw, 832)[0])
        require(struct.unpack_from('<I', raw, 856)[0] != 7, 'Cold NPU PCK client aliases GPU.')
    require(not any(owner in (0x4f53475055464d31, 0x4f534731) for owner in owners) and
            struct.unpack_from('<I', raw, 540)[0] != 7, 'Cold GPU owner/client aliases another provider.')
    conditions = GPU_CONDITIONS | ({GPU_NPU_CLOCK} if combined else set())
    require(isinstance(gpu['conditions'], dict) and set(gpu['conditions']) == conditions,
            'Cold GPU condition set differs.')
    for condition, ref in gpu['conditions'].items():
        fact = json.loads(condition_bytes('gpu_' + condition, ref, directory, frozen))
        require(fact.get('reviewed') is True and all(fact.get(k) == policy[k] for k in
                ('identity', 'sid_authority', 'image_sha256', 'stage2_sha256', 'evidence_class')),
                'Cold GPU condition is not bound to this board, SID, image and stage2.')
        if policy['evidence_class'] == 'board-records':
            require(fact.get('physical_authority') is True and fact.get('modeled') is False,
                    'Cold GPU condition lacks physical authority.')
        else:
            require(fact.get('modeled') is True and fact.get('physical_authority') is False,
                    'Cold GPU model condition must be explicitly nondeployable.')
        if condition == GPU_NPU_CLOCK:
            require(fact.get('schema') == 'a733-os-card-gpu-npu-clock-v1' and
                    fact.get('scope') == 'simultaneous-powered-cold-lifetime' and
                    fact.get('gpu') == {'core_hz': config['core_hz'], 'supply_uv': config['supply_uv']} and
                    fact.get('npu') == {'mhz': policy['npu']['mhz'], 'supply_uv': 800000} and
                    fact.get('npu_pll_enable_with_gpu_active') is True and
                    fact.get('gpu_pll_start_stop_with_npu_active') is True and
                    fact.get('cpu_ddr_clocks_remain_admitted') is True,
                    'Cold combined GPU/NPU clock authority differs.')
            clock = json.loads(frozen['npu_clock'])
            shared = clock.get('conditions', {}).get(GPU_NPU_CLOCK)
            require(isinstance(shared, dict) and shared.get('sha256') == ref['sha256'],
                    'Cold GPU and NPU clock records must reference the same evidence.')
        else:
            require(fact.get('schema') == 'a733-os-card-gpu-condition-v1' and
                    fact.get('condition') == condition, 'Cold GPU condition record differs.')
        if condition == GPU_RESET:
            require(all(fact.get(k) is True for k in ('after_uncertain_loss', 'before_stage2_reload',
                    'before_reserved_memory_reuse', 'firmware_and_bus_transactions_retired',
                    'pll_gpu0_disabled', 'gpu_gates_closed', 'gpu_bus_reset_asserted')) and
                    fact.get('initial_supply_uv') in (800000, 960000),
                    'Cold GPU reset must retire uncertain writes before reuse and restore the admitted entry profile.')
        require(isinstance(fact.get('evidence'), list) and fact['evidence'], 'Cold GPU source evidence is missing.')
        for i, item in enumerate(fact['evidence']):
            condition_bytes('gpu_' + condition + '_' + str(i), item, directory, frozen)



def evidence_offsets(raw):
    offsets = dict(EVIDENCE)
    if host.npu_session(raw):
        offsets.update({'npu_' + name: 808 + offset for name, offset in
                        zip(('admission', 'clock', 'power', 'supervision', 'ddr'), (112, 144, 176, 208, 240))})
    return offsets


def npu_summary(raw, boot_mhz=852):
    owner, command, supervisor = struct.unpack_from('<3Q', raw, 832)
    pck, model, jobs = struct.unpack_from('<3I', raw, 856)
    return {'owner': owner, 'command_admission': command, 'supervisor': supervisor,
            'pck_client': pck, 'client_model': model, 'jobs': jobs,
            'catalogue': bytes(raw[888:920]).hex(), 'pool': host.NPU_RANGE,
            'work_us': NPU_WORK_US, 'mhz': boot_mhz, 'supply_uv': 800000}


linked_npu_assets = host.linked_npu_assets


def npu_policy(policy, raw, directory, frozen):
    if not host.npu_session(raw):
        require('npu' not in policy, 'NPU cold authority supplied without NPU.')
        return
    perf = policy.get('performance', {}).get('configuration')
    boot_mhz = perf['points'][perf['boot_mode']]['npu'][0] if perf else 852
    require(policy.get('npu') == npu_summary(raw, boot_mhz), 'Cold NPU owner/catalogue/jobs/pool/operating point differs.')
    rules = host.module('card_npu_rules', ROOT / 'apps/npu-board/prepare.py')
    conditions = {k: set(v) for k, v in rules.CONDITIONS.items()}
    conditions['clock'].remove('supervised-cross-pll-disturbance-risk')
    conditions['clock'].add('cold-boot-cross-pll-disturbance-qualified')
    if policy.get('gpu') is not None: conditions['clock'].add(GPU_NPU_CLOCK)
    conditions['supervision'] = {'watchdog-reset-retires-npu-and-memory',
        'cold-entry-excludes-remote-writers', 'retain-until-qualified-cold-reset', 'stop-after-first-failure'}
    for name in ('admission', 'clock', 'power', 'supervision', 'ddr'):
        fact = json.loads(frozen['npu_' + name])
        require(fact.get('identity') == policy['identity'] and fact.get('image_sha256') == policy['image_sha256'] and
                fact.get('sid_authority') == policy['sid_authority'] and fact.get('evidence_class') == policy['evidence_class'] and
                fact.get('reviewed') is True, 'Cold NPU record is not bound to this board/image/evidence class.')
        if policy['evidence_class'] == 'board-records':
            rules.physical_authority(fact)
        if name == 'ddr':
            require(fact.get('schema') == 'a733-ddr-reservation-v1' and fact.get('ranges') == [host.NPU_RANGE] and
                    fact.get('exclusive') is True and fact.get('published_cache_disabled') is True and
                    fact.get('remote_writers_excluded') is True and fact.get('evidence'), 'Cold NPU DDR reservation differs.')
            for i, ref in enumerate(fact['evidence']): condition_bytes('npu_ddr_' + str(i), ref, directory, frozen)
            continue
        require(fact.get('schema') == 'a733-os-card-npu-' + name + '-v1' and
                isinstance(fact.get('conditions'), dict) and set(fact['conditions']) == conditions[name],
                'Cold NPU condition set differs: ' + name)
        for condition, ref in fact['conditions'].items(): condition_bytes('npu_' + name + '_' + condition, ref, directory, frozen)
        if name == 'admission':
            require(fact.get('ddr_sha256') == policy['evidence']['npu_ddr']['sha256'], 'Cold NPU admission names another pool record.')
        if name == 'power': require(fact.get('supply_uv') == 800000, 'Cold NPU rail must be 800 mV.')
        if name == 'clock':
            word = fact.get('sid_dvfs_word')
            require(type(word) is int and 0 <= word < 2**32 and
                    (((word >> 16) & 255) or ((word >> 8) & 255)) in (0x12, 0x04, 0x05) and
                    fact.get('mhz') == boot_mhz, 'Cold NPU boot point is unsupported by the fixed 800 mV provider.')
        if name == 'supervision':
            require(fact.get('supervisor') == policy['npu']['supervisor'] and fact.get('work_us') == NPU_WORK_US and
                    fact.get('one_attempt_per_boot') is True, 'Cold NPU finite-work supervision differs.')


def canonical_session(raw):
    require(len(raw) in (808, 904, 1080, 1176), 'Recorded card session size is unsupported.')
    result = bytearray(raw)
    result[24:40] = bytes(16)
    if host.session_layout(raw)[1]:
        result[-64:-48] = bytes(16)
        result[-32:] = bytes(32)
    return bytes(result)


def template_check(raw):
    require(len(raw) in (808, 904, 1080, 1176) and struct.unpack_from('<2Q', raw, 24) == (0, 0) and
            struct.unpack_from('<I', raw, 400)[0] == 2,
            'Cold recorded template requires workload 2 and zero epoch/generation.')
    require(struct.unpack_from('<Q', raw, 312)[0] and any(raw[320:352]), 'Cold SID binding required.')
    if host.session_layout(raw)[1]:
        require(struct.unpack_from('<2Q', raw, len(raw) - 64) == (0, 0) and
                struct.unpack_from('<I', raw, len(raw) - 48)[0] == 3,
                'Cold CPU grant requires both clusters and zero epoch/generation.')
    # Reuse the native admission's existing host statement with private,
    # representative identities. These bytes never enter the output or hashes.
    representative = bytearray(raw)
    struct.pack_into('<2Q', representative, 24, 1, 1)
    if host.session_layout(raw)[1]:
        struct.pack_into('<2Q', representative, len(raw) - 64, 1, 1)
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
    offsets = evidence_offsets(raw)
    require(set(refs) == set(offsets) | ({'cpu_b1'} if host.session_layout(raw)[1] else set()), 'Cold evidence set differs.')
    frozen = {}
    for name, ref in refs.items():
        require(isinstance(ref, dict) and set(ref) == {'path', 'sha256'} and
                isinstance(ref['path'], str), 'Invalid cold evidence reference.')
        data = package.read_regular(policy_path.parent / ref['path'], 16 << 20)
        require(sha(data) == ref['sha256'], 'Cold evidence changed: ' + name)
        if name in offsets:
            offset = offsets[name]
            require(hashlib.sha256(data).digest() == raw[offset:offset + 32], 'Session evidence differs: ' + name)
        frozen[name] = data
    if host.session_layout(raw)[1]:
        b1, cpu = json.loads(frozen['cpu_b1']), p.get('cpu', {})
        require(b1.get('schema') == host.B1_SCHEMA and b1.get('passed') is True and
                b1.get('identity') == p['identity'] and b1.get('grant_clusters') == 0 and
                b1.get('pattern_lfm_clear') is True and type(b1.get('sid_word')) is int and
                all(type(b1.get(k)) is int and 800 <= b1[k] <= 1050 for k in ('dcdc3_mv', 'dcdc5_mv')),
                'Cold CPU policy requires matching read-only B1 evidence.')
        column = host.bin_column(b1['sid_word'])
        require(column is not None and (not manifest.get('perf_modes') or column == 4), 'Unsupported SID bin.')
        maxima = [host.supported(c, column)[-1] for c in ('cpu-l', 'cpu-b')]
        g = host.grant_fields(raw)
        require(cpu == {'clusters': 3, 'max_mhz': maxima, 'changes_per_boot': g['changes']} and
                list(g['max_mhz']) == maxima and 0 < g['changes'] <= 64 and
                g['approval'] == hashlib.sha256(policy_raw).digest(), 'Cold CPU policy/grant differs from vendor maxima.')
    else:
        require('cpu' not in p, 'CPU policy supplied without a linked grant.')
    configuration = performance_configuration(host.os_link.Elf(image), manifest, raw)
    performance_policy(p, configuration, policy_path.parent, frozen)
    npu_policy(p, raw, policy_path.parent, frozen)
    config = host.os_link.gpu_configuration(host.os_link.Elf(image), required=manifest.get('gpu_linear', False))
    require(manifest.get('gpu_profile') == config, 'Cold GPU manifest differs from the linked image.')
    gpu_policy(p, config, raw, policy_path.parent, frozen)
    return p, policy_raw, frozen


def prepare(a):
    image, monitor = a.image.read_bytes(), a.stage2.read_bytes()
    monitor_elf, manifest_raw = a.stage2_elf.read_bytes(), a.manifest.read_bytes()
    allocation_raw, card_raw = a.allocation.read_bytes(), a.card.read_bytes()
    manifest = json.loads(manifest_raw)
    elf = host.os_link.Elf(image)
    profile = host.os_link.contract_profile(elf)
    inference = host.os_link.inference_configuration(elf)
    require(type(manifest.get('gpu_linear', False)) is bool, 'GPU profile must be boolean.')
    require(manifest.get('passed') is True and manifest.get('target') == 'sunstone-a733-os' and
            manifest.get('lifetime') == profile['lifetime'] == 'resident' and
            manifest.get('image_sha256') == sha(image) and manifest.get('contract_profile') == profile and
            manifest.get('inference') == inference and
            not any(manifest.get(k) for k in ('audio', 'radio', 'headset')),
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
                               ROOT / 'boot/a733/stage2/loader.c', gpu_linear=manifest.get('gpu_linear', False), cpu_clock=host.session_layout(raw)[1], npu=host.npu_session(raw), perf=manifest.get('perf_modes', False))
    require(all(manifest.get(k) == v for k, v in linked.items() if k != 'perf_boot_mode' or v is not None),
            'The actual linked services, capability or layout differs from the manifest.')
    if host.npu_session(raw):
        linked_npu_assets(elf)
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
              'inference': inference, 'cpu_vendor_top_policy': host.session_layout(raw)[1], 'npu': policy.get('npu'), 'performance': policy.get('performance', {}).get('configuration'),
              'gpu': linked['gpu_profile'], 'generation': plan['generation'],
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
