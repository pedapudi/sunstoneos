#!/usr/bin/env python3
"""Offline preparation of one supervised integrated OS session (docs/os-image.md).

No hardware access. The bundle holds the linked image, its link-check record as
the image manifest, the reviewed entry record with every evidence byte it cites,
the 808-byte session (992 bytes with audio, and 96 more with the CPU clock
grant record), the SRAM launcher built for the OS contract, and the three
fixed recognition inputs. A grant that names a cluster needs a reviewed
approval record bound to one board, this image, this session and that
board's read-only B1 record (docs/designs/cpu-a76-dvfs.md). The reviewed entry record is
mandatory: it carries the board's supervised entry facts, the thermal owner's
facts, the DDR reservation of the recognition inputs and, for the audio build,
the I2S receive and DMA0 authority. Radio and headset builds also freeze their
authenticated profiles and pinned firmware in the existing radio reservation.
The session identities come from the
reviewed template, so the DMA0 authority can name them; only the linked build
identity is written here.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
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
os_link = module('os_link_configuration', ROOT / 'checks/os-image-link.py')
radio_assets = module('os_radio_assets', ROOT / 'apps/os/radio-assets.py')
cpu_return_clock = module('os_cpu_return_clock_review', ROOT / 'apps/os/cpu-return-clock-review.py')
gpu_return_rules = module('os_gpu_return_rules', ROOT / 'apps/os/gpu-return.py')
bluetooth = module('os_bluetooth_preparation', ROOT / 'apps/bluetooth/prepare.py')
board, host = mimus.board, mimus.host
base = board.base
require, digest, save_json = base.require, base.digest, base.save_json
SCHEMA = 'a733-os-supervised-bundle-v1'
COMBINED_SCHEMA = 'a733-os-supervised-combined-v1'
ENTRY_SCHEMA = 'a733-os-supervised-entry-v1'
AUDIO_SCHEMA = 'a733-os-audio-entry-v1'
EXECUTION_PROFILE = 'os-supervised-v1'
GPU_CONDITIONS = {
    'gpu-and-remote-users-excluded-prior-work-drained',
    'gpu-top5-core6-initial-state-and-local-authority',
    'dcdc4-800-to-960mv-and-pll-gpu0-1008mhz-authority',
    'shared-pmic-and-register-ownership', 'irq95-route-disabled-inactive-clear',
    'gpu-device-mappings-and-memory-alias-exclusion',
    'manual-whole-board-power-removal-and-no-reuse'}
GPU_PERF_CONDITION = 'gpu-runtime-600-1008mhz-and-exclusive-dcdc4-800-960mv-authority'
GPU_NPU_CONDITION = 'concurrent-gpu-npu-lifetime-and-cross-pll-risk'
GPU_NPU_SCHEMA = 'a733-os-gpu-npu-supervised-entry-v1'
CONTRACT_MAGIC = 0x415337334f534331
REQUEST_MAGIC = 0x415337334f534c31
SESSION_MAGIC = 0x415337334f534131
AUDIO_MAGIC = 0x415337334f415531
SESSION_ADDRESS = 0x41f00000
BOARD_BYTES, OS_BYTES, AUDIO_BYTES = 592, 808, 184
NPU_BYTES, NPU_MAGIC = 272, 0x415337334f4e5031
NPU_RANGE = [0x47400000, 0x47c00000]
# Offsets in the audio extension, which starts at OS_BYTES (apps/os/os.h).
AUDIO_EVIDENCE, AUDIO_DMA, AUDIO_DMA_BYTES, AUDIO_TAIL = 832, 864, 104, 968
AUDIO_CONDITIONS = {
    'dma0-cpu-and-remote-users-excluded', 'fifo-producers-quiescent-and-prior-dma-drained',
    'dma-route-and-irq143-disabled-inactive-clear', 'audio-parent-and-shared-clock-ownership',
    'i2s0-pins-bank-voltage-and-microphone-wiring', 'device-mappings-and-cache-alias-exclusion',
    'manual-whole-board-power-removal', 'retain-all-resources-until-power-removal',
    'stop-after-first-failure'}
# CPU clock grant record (include/a733_cpu_clock.h), after the thermal and
# audio facts in the SUNSTONE_A733_OS_CPU_CLOCK build.
GRANT_MAGIC, GRANT_BYTES, GRANT_CHANGES = 0x4153373343505531, 96, 64
APPROVAL_SCHEMA = 'a733-os-cpu-clock-approval-v1'
# The performance-mode build's grant names both clusters at the bin's high
# points (docs/designs/performance-modes.md).
PERF_APPROVAL_SCHEMA = 'a733-os-perf-modes-approval-v1'
B1_SCHEMA = 'a733-os-cpu-clock-b1-v1'
# TOP is the default path of docs/designs/cpu-top-opp.md: both clusters at
# their bin's highest points; the OS owner then sets the DSU by the vendor rule.
STEPS = {'A1': ('cpu-b', 1196), 'A2': ('cpu-b', None), 'A3': ('cpu-l', 1196), 'TOP': ('both', None)}


def driver_text():
    return (ROOT / 'drivers/clock/a733_cpu_clock.c').read_text()


def bin_codes():
    """The driver's codes[] for columns 1..8; code 0 selects the envelope."""
    return [int(v, 16) for v in re.search(r'codes\[\]=\{([^}]*)\}', driver_text())[1].split(',')]


def opp_rows(cluster):
    """The driver's 26 MHz table rows: (MHz, mV per column) for 'little' or 'big'."""
    text = driver_text()
    body = re.search(r'static const struct a733_cpu_opp ' + cluster + r'\[\]=\{(.*?)\n\};', text, re.S)[1]
    return [(int(m[1]), [int(v) for v in m[2].split(',')]) for m in re.finditer(r'\{(\d+),\{([\d,]+)\}\}', body)]


def bin_column(sid_word):
    """The driver's bin map: a column index, 9 for the code-0 envelope, None if refused."""
    code, codes = (sid_word >> 16) & 255 or (sid_word >> 8) & 255, bin_codes()
    return 9 if not code else codes.index(code) + 1 if code in codes else None


def supported(cluster, column):
    """MHz points that the column (or the envelope) supports, ascending."""
    rows = opp_rows('little' if cluster == 'cpu-l' else 'big')
    return [mhz for mhz, mv in rows if (all(mv) if column == 9 else mv[column])]


def session_layout(raw):
    """(audio, grant): the session sizes that apps/os/os.h defines."""
    extra = len(raw) - OS_BYTES
    if extra in (NPU_BYTES, NPU_BYTES + GRANT_BYTES):
        return False, extra != NPU_BYTES
    require(extra in (0, AUDIO_BYTES, GRANT_BYTES, AUDIO_BYTES + GRANT_BYTES), 'Unknown OS session size.')
    return extra >= AUDIO_BYTES, extra in (GRANT_BYTES, AUDIO_BYTES + GRANT_BYTES)


def grant_fields(raw):
    """The grant record's fields, or None without one."""
    if not session_layout(raw)[1]:
        return None
    g = raw[len(raw) - GRANT_BYTES:]
    magic, version, size, owner, epoch, generation = struct.unpack_from('<6Q', g)
    clusters, little, big, changes = struct.unpack_from('<4I', g, 48)
    return {'magic': magic, 'version': version, 'bytes': size, 'owner': owner, 'epoch': epoch,
            'generation': generation, 'clusters': clusters, 'max_mhz': (little, big), 'changes': changes,
            'approval': bytes(g[64:96])}


def manifest_cpu_clock(manifest, raw):
    """The link check's record of a CPU clock build must match the session size."""
    cpu_clock = manifest.get('cpu_clock')
    require(type(cpu_clock) is bool and session_layout(raw)[1] is cpu_clock,
            'The session size and the link check disagree on the CPU clock build.')
    # Missing capabilities mean an older linked image. A zero trip remains
    # compatible; a nonzero trip requires the native setter to be linked.
    trip, report = manifest.get('cpu_critical_trip', False), manifest.get('thermal_report', False)
    require(type(trip) is bool and type(report) is bool and (not report or cpu_clock and trip),
            'Invalid thermal image capabilities.')
    require(not struct.unpack_from('<I', raw, 804)[0] or trip,
            'The linked image does not support a session CPU critical trip.')
    npu = npu_session(raw)
    require(manifest.get('npu', False) is npu, 'NPU image/session profile differs.')
    # The performance-mode build admits both clusters or a read-only grant.
    perf = manifest.get('perf_modes', False)
    g = grant_fields(raw)
    require(type(perf) is bool and (not perf or (cpu_clock and manifest.get('perf_operation') == 8 and manifest.get('perf_boot_mode') in ('high', 'low'))) and
            (not perf or g is not None and g['clusters'] in (0, 3)),
            'The performance-mode record and the CPU grant disagree.')
    if npu:
        profile = manifest.get('npu_profile', {})
        require(profile.get('range') == NPU_RANGE and profile.get('catalogue') == bytes(raw[888:920]).hex() and
                profile.get('client_model') == struct.unpack_from('<I', raw, 860)[0],
                'NPU linked reservation/catalogue/client differs.')


def npu_session(raw):
    """True when the session carries the NPU extension, with or without the grant."""
    return len(raw) - OS_BYTES in (NPU_BYTES, NPU_BYTES + GRANT_BYTES)


def same_inference(value, expected):
    return value == expected and (expected is None or
            isinstance(value, dict) and all(type(v) is int for v in value.values()))


def image_inference(manifest, raw):
    """Bind the host capability to the actual adapter record, never a JSON count alone."""
    try:
        config = os_link.inference_configuration(os_link.Elf(raw), legacy=True)
    except (AssertionError, ValueError, IndexError, struct.error) as error:
        require(False, 'Invalid linked inference configuration: ' + str(error))
    require(same_inference(manifest.get('inference'), config), 'Inference manifest differs from the linked adapter.')
    if config and config['workers'] == 8:
        require(manifest.get('lifetime') == 'supervised' and
                not any(manifest.get(k) for k in ('audio', 'radio', 'headset')),
                'Eight workers require the supervised recorded OS profile.')
    return config


def inference_admission(record, config):
    if config and config['workers'] == 8:
        require(same_inference(record.get('inference'), config),
                'Reviewed entry must name the exact eight-worker placement and priorities.')
    elif 'inference' in record:
        require(same_inference(record['inference'], config), 'Reviewed inference configuration differs.')


def linked_npu_assets(elf):
    """Authenticate the actual embedded model and vectors, using linked pointers."""
    def one(name):
        rows = [s for s in elf.symbols if s['name'] == name and s['shndx'] != 0]
        require(len(rows) == 1, 'NPU linked symbol is absent or ambiguous: ' + name)
        return rows[0]
    def value(name):
        symbol = one(name)
        require(symbol['size'] == 4, 'NPU linked count size differs.')
        return struct.unpack('<I', elf.read(symbol['value'], 4))[0]
    model = value('os_npu_client_model')
    catalogue = json.loads((ROOT / 'tools/npu-session-models.json').read_text())['models']
    require(model < len(catalogue), 'NPU linked catalogue entry differs.')
    selected = catalogue[model]
    for symbol, entries in [('os_npu_models', [selected['model']]),
                            ('os_npu_inputs', [v['reference'] for v in selected['inputs']]),
                            ('os_npu_references', [v['reference'] for v in selected['outputs']])]:
        table = one(symbol)
        require(table['size'] == 16 * len(entries), 'NPU linked vector table size differs.')
        for i, expected in enumerate(entries):
            pointer, size = struct.unpack('<2Q', elf.read(table['value'] + 16*i, 16))
            require(size == expected['bytes'] and hashlib.sha256(elf.read(pointer, size)).hexdigest() == expected['sha256'],
                    'NPU embedded asset differs from the authenticated catalogue.')
    require(value('os_npu_input_count') == len(selected['inputs']) and
            value('os_npu_output_count') == len(selected['outputs']), 'NPU linked vector counts differ.')


def image_gpu(manifest, raw):
    """Read the bounded caller and immutable GPU inputs from the actual linked image."""
    enabled = manifest.get('gpu_linear', False)
    require(type(enabled) is bool, 'GPU profile must be boolean.')
    try:
        elf = os_link.Elf(raw)
        config = os_link.gpu_configuration(elf, required=enabled)
        contract = os_link.contract_profile(elf) if enabled else None
    except (AssertionError, ValueError, IndexError, struct.error) as error:
        require(False, 'Invalid linked GPU configuration: ' + str(error))
    require(bool(config) is enabled and
            json.dumps(manifest.get('gpu_profile'), sort_keys=True) == json.dumps(config, sort_keys=True),
            'GPU manifest differs from the linked configuration.')
    if enabled:
        cpu_clock = manifest.get('cpu_clock')
        require(type(cpu_clock) is bool, 'GPU CPU-clock feature must be boolean.')
        npu = manifest.get('npu', False)
        require(type(npu) is bool, 'GPU NPU feature must be boolean.')
        expected_bytes = OS_BYTES + (NPU_BYTES if npu else 0) + (GRANT_BYTES if cpu_clock else 0)
        require(contract == {'lifetime': 'supervised', 'session_bytes': expected_bytes} and
                manifest.get('contract_profile') == contract,
                'Supervised GPU requires the actual linked supervised contract and session size.')
        require(manifest.get('lifetime') == 'supervised' and
                not any(manifest.get(k) for k in ('audio', 'radio', 'headset')),
                'Supervised GPU requires recorded recognition without audio or radio devices.')
        perf = manifest.get('perf_modes', False)
        require(type(perf) is bool and perf is ('performance' in config),
                'GPU performance capability differs from the linked implementation.')
        if perf:
            require(cpu_clock and manifest.get('perf_operation') == 8 and
                    manifest.get('perf_boot_mode') == config['performance']['boot_mode'],
                    'GPU performance boot mode or command differs.')
    return config


def gpu_admission(record, config, evidence, raw):
    grant = grant_fields(raw)
    if config is None:
        require('gpu' not in record, 'GPU authority supplied for an image without GPU.')
        return
    # Fixed native GPU identities in apps/os/dispatch.h; session and
    # pre-provider checks exclude the CPU and NPU owners.
    require(grant is None or grant['owner'] not in (0x4f53475055464d31, 0x4f534731),
            'CPU clock owner aliases a GPU owner.')
    value = record.get('gpu')
    require(isinstance(value, dict) and set(value) == {'configuration', 'conditions'} and
            json.dumps(value['configuration'], sort_keys=True) == json.dumps(config, sort_keys=True),
            'Reviewed GPU entry must name the exact linked configuration.')
    combined = npu_session(raw)
    conditions = GPU_CONDITIONS | ({GPU_NPU_CONDITION} if combined else set()) | ({GPU_PERF_CONDITION} if config.get('performance') else set())
    require(isinstance(value['conditions'], dict) and set(value['conditions']) == conditions,
            'Incomplete GPU physical entry conditions.')
    for item in value['conditions'].values():
        evidence(item)
    if combined:
        owner, pck = struct.unpack_from('<Q', raw, 832)[0], struct.unpack_from('<I', raw, 856)[0]
        require(owner not in (0x4f53475055464d31, 0x4f534731) and pck != 7,
                'NPU owner or PCK client aliases the GPU.')
        facts = json.loads(evidence(value['conditions'][GPU_NPU_CONDITION]))
        require(facts.get('schema') == GPU_NPU_SCHEMA and facts.get('scope') == 'simultaneous-powered-lifetime' and
                facts.get('reviewed') is True and facts.get('physical_authority') is True and facts.get('modeled') is False and
                all(facts.get(k) == record[k] for k in ('identity', 'port', 'image_sha256', 'epoch', 'generation', 'lease')),
                'Combined GPU/NPU authority names another scope or lifetime.')
        require(facts.get('gpu') == {'core_hz': config['core_hz'], 'supply_uv': config['supply_uv']} and
                facts.get('npu') == {'mhz': 852, 'supply_uv': 800000} and
                facts.get('npu_pll_enable_with_gpu_active') is True and
                facts.get('gpu_pll_start_stop_with_npu_active') is True and
                facts.get('cpu_ddr_clocks_remain_admitted') is True and facts.get('evidence'),
                'Combined GPU/NPU clock authority is incomplete.')
        for item in facts['evidence']:
            evidence(item)


def image_manifest_check(manifest, raw, *, cpu_return=False):
    """The OS link check of these exact bytes; returns whether audio is built in."""
    names = {s['name'] for s in os_link.Elf(raw).symbols if s['shndx'] != 0}
    require(type(cpu_return) is bool and manifest.get('cpu_return', False) is cpu_return and
            ('os_cpu_return_output' in names) is cpu_return,
            'CPU return requires its dedicated host adapter.')
    if cpu_return:
        elf = os_link.Elf(raw)
        markers = [s for s in elf.symbols if s['shndx'] != 0 and s['name'] == 'os_cpu_return_entry_profile']
        require(len(markers) == 1 and markers[0]['size'] == 32 and
                struct.unpack('<4Q', elf.read(markers[0]['value'], 32)) == cpu_return_clock.MARKER and
                {'os_cpu_return_entry_guard', 'os_cpu_return_thermal_ready'} <= names,
                'CPU return initial-state guard capability differs.')
    image_inference(manifest, raw)
    gpu = image_gpu(manifest, raw)
    require(not gpu or ('return' in gpu) is cpu_return, 'GPU return capability differs from the selected lifetime.')
    audio = manifest.get('audio')
    # A link check from before the NPU profile has no 'npu' key: NPU is off.
    npu = manifest.get('npu', False)
    require(type(npu) is bool, 'The link check records the NPU profile as a boolean.')
    if npu:
        linked_npu_assets(os_link.Elf(raw))
    flags = radio_assets.features(manifest, raw, os_link.Elf, require)
    require(not (audio and flags['headset']), 'Microphone and headset profiles cannot be combined.')
    require(manifest.get('passed') is True and manifest.get('target') == 'sunstone-a733-os' and
            manifest.get('lifetime') == 'supervised' and type(audio) is bool and
            manifest.get('image_sha256') == hashlib.sha256(raw).hexdigest(),
            'Passed supervised OS link check of this image required.')
    require([a.get('name') for a in manifest.get('applications', [])] ==
            ['mimus', 'thermal'] + (['npu'] if npu else []) + (['audio'] if audio else []) +
            (['radio'] if flags['radio'] else []) + (['headset'] if flags['headset'] else []) + (['gpu-linear'] if gpu else []) and
            [i.get('intid') for i in manifest.get('interrupts', [])] == [71] + ([97] if npu else []) + ([143] if audio else []) + ([95] if gpu else []),
            'OS start list or interrupt table differs from the link check.')
    return audio


def session_check(raw, audio, flags=None):
    """Python statement of apps/os/admission.c os_session_valid for the bundle."""
    require(session_layout(raw)[0] is audio, 'OS session size differs from the image.')
    workload = board.session_check(raw[:BOARD_BYTES], usb_reference_modes=(1,))
    flags = flags or {'radio': False, 'headset': False}
    require(workload in ((1,) if flags['radio'] else (0, 4) if audio or flags['headset'] else (0, 2, 4)),
            'Workload differs from the recorded, radio or headset image.')
    if flags['radio']:
        identity, latch_owner, known, value = struct.unpack_from('<2Q2I', raw, 432)
        require(identity and latch_owner and known in (0, 63) and not value & ~known and
                struct.unpack_from('<Q', raw, 312)[0] and any(raw[320:352]),
                'Radio PM ownership or SID authority absent.')
    else:
        require(not any(raw[352:400] + raw[432:456]), 'Non-radio session carries radio profile or PM authority.')
    if workload == 2:
        # Native a733_board_session_valid requires no host/latch/Type-C authority.
        require(not any(raw[408:432] + raw[472:488] + raw[528:532] + raw[536:564] + raw[570:582]),
                'USB0-only recognition must carry no USB-host authority or supply.')
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
            alarm == critical == 31 and not reserved and raw[802:804] == bytes(2),
            'Unsupported thermal clock or control profile.')
    require(struct.unpack_from('<I', raw, 804)[0] <= 110000,
            'CPU critical trip must not exceed the vendor 110000 millicelsius.')
    for i in range(5):
        hot, hysteresis, limit = struct.unpack_from('<3H', raw, 772 + 6 * i)
        require(0 < limit <= hot < hysteresis <= 4095, 'Invalid thermal thresholds.')
    g = grant_fields(raw)
    if g is not None:
        # a733_cpu_clock_grant_load and the owner rule of apps/os/admission.c.
        dma_owner = struct.unpack_from('<Q', raw, AUDIO_DMA + 48)[0] if audio else 0
        npu_owner = struct.unpack_from('<Q', raw, OS_BYTES + 24)[0] if npu_session(raw) else 0
        require((g['magic'], g['version'], g['bytes']) == (GRANT_MAGIC, 1, GRANT_BYTES) and g['owner'] and
                g['owner'] not in owners + (owner, dma_owner, npu_owner) and not g['clusters'] & ~3,
                'Invalid CPU clock grant.')
        if not g['clusters']:
            require(not (g['epoch'] or g['generation'] or any(g['max_mhz']) or g['changes'] or any(g['approval'])),
                    'A grant that names no cluster carries nothing else.')
        else:
            require((g['epoch'], g['generation']) == (epoch, generation) and 0 < g['changes'] <= GRANT_CHANGES and
                    any(g['approval']), 'The CPU clock grant must name this session and an approval.')
    if npu_session(raw):
        npu_session_check(raw)
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


def npu_session_check(raw):
    npu_rules = module('os_npu_preparation', ROOT / 'apps/npu-board/prepare.py')
    n = raw[OS_BYTES:OS_BYTES + NPU_BYTES]
    require(len(n) == NPU_BYTES and struct.unpack_from('<3Q', n) == (NPU_MAGIC, 1, NPU_BYTES),
            'Invalid OS NPU extension.')
    owner, command, supervisor = struct.unpack_from('<3Q', n, 24)
    pck, model, jobs, reserved = struct.unpack_from('<4I', n, 48)
    owners = struct.unpack_from('<4Q', raw, 456) + (struct.unpack_from('<Q', raw, 616)[0],
              struct.unpack_from('<Q', raw, 416)[0], struct.unpack_from('<Q', raw, 440)[0])
    require(owner and command and supervisor and owner not in owners and pck < 16 and
            pck != struct.unpack_from('<I', raw, 540)[0] and model < 5 and 1 <= jobs <= 8 and not reserved,
            'NPU identity or bounded caller selection differs.')
    require(struct.unpack_from('<2Q', n, 64) == (NPU_RANGE[0], NPU_RANGE[1] - NPU_RANGE[0]) and
            all(any(n[i:i+32]) for i in range(80, 272, 32)), 'Missing NPU reservation or evidence.')
    # The image links only the client's model (apps/os/npu-catalogue.py).
    catalogue = json.loads((ROOT / 'tools/npu-session-models.json').read_text())['models']
    expected = hashlib.sha256(bytes.fromhex(catalogue[model]['model']['sha256'])).digest()
    require(n[80:112] == expected, 'NPU catalogue differs.')
    npu_rules.fabric_check(raw)


def npu_admission(record, image_hash, raw, evidence):
    npu_rules = module('os_npu_preparation', ROOT / 'apps/npu-board/prepare.py')
    npu_rules.physical_authority(record)
    refs = record.get('npu')
    require(isinstance(refs, dict) and set(refs) == {'admission', 'clock', 'power', 'supervision', 'ddr'},
            'Complete OS NPU authority required.')
    n = raw[OS_BYTES:]
    identity = {key: record[key] for key in ('identity', 'port')}
    def fact(key, offset):
        data = evidence(refs[key])
        require(hashlib.sha256(data).digest() == n[offset:offset+32], 'NPU evidence digest differs: ' + key)
        f = json.loads(data)
        npu_rules.physical_authority(f)
        require(all(f.get(k) == v for k, v in identity.items()) and f.get('image_sha256') == image_hash and
                all(f.get(k) == struct.unpack_from('<Q', raw, 24+8*i)[0]
                    for i, k in enumerate(('epoch', 'generation', 'lease'))), 'NPU authority names another lifetime.')
        return f
    ddr = fact('ddr', 240)
    require(ddr.get('schema') == 'a733-ddr-reservation-v1' and ddr.get('ranges') == [NPU_RANGE] and
            ddr.get('exclusive') is True and ddr.get('published_cache_disabled') is True and
            ddr.get('remote_writers_excluded') is True and ddr.get('evidence'), 'NPU private DDR reservation differs.')
    for item in ddr['evidence']:
        evidence(item)
    for key, offset in zip(('admission', 'clock', 'power', 'supervision'), (112, 144, 176, 208)):
        f = fact(key, offset)
        require(f.get('schema') == 'a733-npu-board-' + key + '-v1' and f.get('reviewed') is True and
                set(f.get('conditions', {})) == npu_rules.CONDITIONS[key], 'Incomplete NPU ' + key + ' authority.')
        for item in f['conditions'].values():
            evidence(item)
        if key == 'admission':
            require(f.get('ddr_sha256') == refs['ddr']['sha256'], 'NPU admission covers another DDR reservation.')
        if key == 'power':
            require(f.get('supply_uv') == 800000, 'NPU rail differs from fixed operating point.')
        if key == 'supervision':
            require(f.get('supervisor') == struct.unpack_from('<Q', n, 40)[0] and f.get('one_attempt') is True,
                    'NPU one-attempt supervision differs.')


def capture_profile(raw):
    """The session identities and, with audio, the capture profile."""
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    profile = {'epoch': epoch, 'generation': generation, 'lease': lease}
    if session_layout(raw)[0]:
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


def validate_admission(record, image_hash, template_hash, raw, audio, cache=None, frozen=False, perf_modes=False, inference=None, gpu=None, cpu_return=False):
    """Every fact the session names is bound to a reviewed record of this image."""
    require(record.get('schema') == ENTRY_SCHEMA, 'Reviewed OS entry record required.')
    inference_admission(record, inference)
    profile = capture_profile(raw)
    require(all(record.get(k) == profile[k] for k in ('epoch', 'generation', 'lease')),
            'Reviewed entry names other session identities.')
    # The thermal check runs the board's DDR, platform and manual-recovery
    # checks, then binds the four thermal evidence hashes at their offsets.
    cache = thermal.validate_admission({**record, 'schema': 'a733-thermal-supervised-entry-v1'}, image_hash,
                                       template_hash, raw, cache, frozen,
                                       'thermal-cpu-return-supervised-v1' if cpu_return else 'thermal-supervised-v1')

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
    gpu_admission(record, gpu, evidence, raw)
    if cpu_return and gpu:
        require('return' in gpu, 'GPU return capability absent.')
        gpu_return_rules.validate(json.loads(evidence(record.get('normal_return_review'))),gpu,evidence,require)
    if npu_session(raw):
        npu_admission(record, image_hash, raw, evidence)
    else:
        require('npu' not in record, 'NPU authority supplied for an image without NPU.')
    cpu_clock_admission(record, image_hash, raw, profile, evidence, perf_modes, cpu_return=cpu_return)
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


def cpu_clock_admission(record, image_hash, raw, profile, evidence, perf_modes=False, *, cpu_return=False):
    """A grant that names a cluster is bound to one reviewed approval of one
    step on one board, which cites that board's read-only B1 record."""
    g = grant_fields(raw)
    if g is None or not g['clusters']:
        require('cpu_clock_approval' not in record, 'CPU clock approval supplied without a grant.')
        return
    require(isinstance(record.get('cpu_clock_approval'), dict), 'Reviewed CPU clock approval record required.')
    data = evidence(record['cpu_clock_approval'])
    a = json.loads(data)
    if perf_modes:
        require(g['clusters'] == 3, 'Performance modes require both clusters.')
        perf_admission(record, image_hash, profile, evidence, g, data, a)
        return
    if cpu_return and a.get('schema') in cpu_return_clock.UNDER_TEST_SCHEMAS:
        cpu_return_clock.validate(a, data, record, image_hash, raw, profile, g, evidence, require, os_link.Elf)
        require(bin_column(a['historical_sid_word']) == 4, 'Historical SID does not select the guarded bin.')
        return
    require(a.get('schema') == APPROVAL_SCHEMA and a.get('identity') == record['identity'] and
            a.get('port') == record['port'] and a.get('image_sha256') == image_hash and
            all(a.get(k) == profile[k] for k in ('epoch', 'generation', 'lease')) and
            isinstance(a.get('approval_text'), str) and a['approval_text'].strip() and
            isinstance(a.get('approval_date'), str) and a.get('step') in STEPS,
            'The approval record names another board, image, session or step.')
    cluster, fixed = STEPS[a['step']]
    b1 = json.loads(evidence(a.get('b1')))
    # B1 ran this build with a grant that named no cluster, and passed.
    require(b1.get('schema') == B1_SCHEMA and b1.get('passed') is True and b1.get('grant_clusters') == 0 and
            b1.get('identity') == record['identity'] and
            b1.get('port') == record['port'] and b1.get('pattern_lfm_clear') is True and
            all(isinstance(b1.get(k), int) and 800 <= b1[k] <= 1050 for k in ('dcdc3_mv', 'dcdc5_mv')) and
            isinstance(b1.get('sid_word'), int), "The approval must cite this board's B1 record.")
    column = bin_column(b1['sid_word'])
    require(column is not None, 'B1 recorded a fuse code without a table column.')
    if cluster == 'both':
        # Each cluster at the top row of the board's column.
        highest = {'cpu-l': supported('cpu-l', column)[-1], 'cpu-b': supported('cpu-b', column)[-1]}
        require(a.get('cluster') == 'both' and a.get('highest_mhz') == highest and
                isinstance(a.get('changes'), int) and 0 < a['changes'] <= GRANT_CHANGES,
                "The approval's step, clusters or top points do not fit this board's bin.")
        clusters, maxima = 3, (highest['cpu-l'], highest['cpu-b'])
    else:
        index = 0 if cluster == 'cpu-l' else 1
        points = supported(cluster, column)
        highest = fixed if fixed else points[-1]
        require(a.get('cluster') == cluster and a.get('highest_mhz') == highest and highest in points and
                isinstance(a.get('changes'), int) and 0 < a['changes'] <= GRANT_CHANGES,
                "The approval's step, cluster or highest point does not fit this board's bin.")
        clusters, maxima = 1 << index, ((highest, 0) if index == 0 else (0, highest))
    require(g['clusters'] == clusters and g['max_mhz'] == maxima and g['changes'] == a['changes'] and
            g['approval'] == hashlib.sha256(data).digest(), 'The session grant differs from the approval record.')


def b1_column(record, evidence, a):
    """The bin column of the board's read-only B1 record that the approval cites."""
    b1 = json.loads(evidence(a.get('b1')))
    # B1 ran this build with a grant that named no cluster, and passed.
    require(b1.get('schema') == B1_SCHEMA and b1.get('passed') is True and b1.get('grant_clusters') == 0 and
            b1.get('identity') == record['identity'] and
            b1.get('port') == record['port'] and b1.get('pattern_lfm_clear') is True and
            all(isinstance(b1.get(k), int) and 800 <= b1[k] <= 1050 for k in ('dcdc3_mv', 'dcdc5_mv')) and
            isinstance(b1.get('sid_word'), int), "The approval must cite this board's B1 record.")
    column = bin_column(b1['sid_word'])
    require(column == 4, 'B1 recorded a fuse code without a table column.')
    return column


def perf_admission(record, image_hash, profile, evidence, g, data, a):
    """The performance-mode grant: both clusters up to the bin's high points,
    bound to one reviewed approval of the mode service on one board."""
    require(a.get('schema') == PERF_APPROVAL_SCHEMA and a.get('identity') == record['identity'] and
            a.get('port') == record['port'] and a.get('image_sha256') == image_hash and
            all(a.get(k) == profile[k] for k in ('epoch', 'generation', 'lease')) and
            isinstance(a.get('approval_text'), str) and a['approval_text'].strip() and
            isinstance(a.get('approval_date'), str),
            'The performance-mode approval names another board, image or session.')
    column = b1_column(record, evidence, a)
    high = (supported('cpu-l', column)[-1], supported('cpu-b', column)[-1])
    require(a.get('high_mhz') == {'cpu-l': high[0], 'cpu-b': high[1]} and isinstance(a.get('changes'), int) and
            0 < a['changes'] <= GRANT_CHANGES, "The approval's high points do not fit this board's bin.")
    require(g['max_mhz'] == high and g['changes'] == a['changes'] and g['approval'] == hashlib.sha256(data).digest(),
            'The session grant differs from the performance-mode approval.')


def prepare(image, link_check, admission, template, assets, output, clang, lld, *, cpu_return=False):
    snapshots = {name: Path(path).read_bytes() for name, path in [
        ('image.elf', image), ('image-input-manifest.json', link_check), ('admission.json', admission),
        ('session-template.bin', template)]}
    image_hash = hashlib.sha256(snapshots['image.elf']).hexdigest()
    manifest = json.loads(snapshots['image-input-manifest.json'])
    audio = image_manifest_check(manifest, snapshots['image.elf'], cpu_return=cpu_return)
    flags = radio_assets.features(manifest, snapshots['image.elf'], os_link.Elf, require)
    raw = bytearray(snapshots['session-template.bin'])
    session_check(raw, audio, flags)
    manifest_cpu_clock(json.loads(snapshots['image-input-manifest.json']), raw)
    record = json.loads(snapshots['admission.json'])
    cache = validate_admission(record, image_hash, hashlib.sha256(raw).hexdigest(), raw, audio,
                               perf_modes=json.loads(snapshots['image-input-manifest.json']).get('perf_modes', False),
                               inference=image_inference(json.loads(snapshots['image-input-manifest.json']), snapshots['image.elf']),
                               gpu=image_gpu(json.loads(snapshots['image-input-manifest.json']), snapshots['image.elf']),
                               cpu_return=cpu_return)
    radio_uploads = radio_assets.validate(record, raw, flags, cache, board, bluetooth, require)
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
    uploads += [{'name': evidence[path]['name'], 'address': address} for path, address in radio_uploads]
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    inner = {'schema': SCHEMA, 'physical_access': False, 'execution_profile': 'os-cpu-return-v1' if cpu_return else EXECUTION_PROFILE, 'audio': audio,
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
    return validate_bundle(output, cpu_return=cpu_return)


def validate_board(bundle, *, cpu_return=False):
    """The board-format part: image, session, launcher and frozen evidence."""
    out = Path(bundle)
    metadata = (out / 'prepared.json').read_bytes()
    p = json.loads(metadata)
    require(p.get('schema') == SCHEMA and p.get('physical_access') is False and
            p.get('execution_profile') == ('os-cpu-return-v1' if cpu_return else EXECUTION_PROFILE), 'Unsupported OS bundle.')
    required = {'image.elf', 'image-input-manifest.json', 'admission.json', 'session-template.bin', 'session.bin',
                'request.bin', 'contract.bin', 'launch.bin', 'result-zero.bin', 'report-zero.bin'}
    require(required <= set(p['files']) and all(item['name'] in p['files'] for item in p['entry_evidence'].values()),
            'Unsealed OS authority or transport input.')
    for name, expected in p['files'].items():
        require(Path(name).name == name and digest(out / name) == expected, 'Prepared OS input changed.')
    image = (out / 'image.elf').read_bytes()
    require(hashlib.sha256(image).hexdigest() == p['image_sha256'] and digest(out / 'launch.bin') == p['launcher_sha256'],
            'OS image or launcher changed.')
    manifest = json.loads((out / 'image-input-manifest.json').read_text())
    audio = image_manifest_check(manifest, image, cpu_return=cpu_return)
    flags = radio_assets.features(manifest, image, os_link.Elf, require)
    require(p['audio'] is audio, 'Audio selection changed.')
    sizes = {name: (out / name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes') == {name: n for name, a, n in base.REGIONS} and 0 < sizes['launch.bin'] <= 0xc00,
            'Protected SRAM geometry changed.')
    require((out / 'result-zero.bin').read_bytes() == bytes(16384) and (out / 'report-zero.bin').read_bytes() == bytes(128),
            'Initial report backing changed.')
    template = (out / 'session-template.bin').read_bytes()
    raw = (out / 'session.bin').read_bytes()
    session_check(raw, audio, flags)
    manifest_cpu_clock(json.loads((out / 'image-input-manifest.json').read_text()), raw)
    cache = {path: (out / item['name']).read_bytes() for path, item in p['entry_evidence'].items()}
    validate_admission(json.loads((out / 'admission.json').read_text()), p['image_sha256'],
                       hashlib.sha256(template).hexdigest(), template, audio, cache, True,
                       perf_modes=json.loads((out / 'image-input-manifest.json').read_text()).get('perf_modes', False),
                       inference=image_inference(json.loads((out / 'image-input-manifest.json').read_text()), image),
                       gpu=image_gpu(json.loads((out / 'image-input-manifest.json').read_text()), image),
                       cpu_return=cpu_return)
    radio_uploads = radio_assets.validate(json.loads((out / 'admission.json').read_text()),
                                         template, flags, cache, board, bluetooth, require, frozen=True)
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
    expected += [{'name': p['entry_evidence'][path]['name'], 'address': address} for path, address in radio_uploads]
    require(p['segments'] == expected, 'Unsealed or changed upload.')
    for name, data in zip(('request.bin', 'contract.bin'), request_bytes(address, identity, raw)):
        require((out / name).read_bytes() == data, 'Launch request or contract changed.')
    p['_sizes'] = sizes
    p['_prepared_sha256'] = hashlib.sha256(metadata).hexdigest()
    return p


def validate_bundle(output, *, cpu_return=False):
    output = Path(output)
    value = json.loads((output / 'prepared.json').read_text())
    require(value.get('schema') == COMBINED_SCHEMA and value.get('physical_access') is False, 'Bundle schema differs.')
    require(digest(output / 'board/prepared.json') == value['board_prepared_sha256'] and
            digest(output / 'assets/assets.json') == value['assets_sha256'], 'Bundle identity differs.')
    inner = validate_board(output / 'board', cpu_return=cpu_return)
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
