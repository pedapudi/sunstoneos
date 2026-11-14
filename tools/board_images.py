"""Offline image/session preparation for tools/board.py; no device access."""
import hashlib
import io
import sys
import json
import os
import re
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
from typing import NamedTuple
from elftools.elf.elffile import ELFFile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import memory_layout

ROOT = Path(__file__).resolve().parents[1]
RUNNER_DRAM = memory_layout.record()['runner_dram']
CACHE = Path.home() / '.cache/sunstoneos'
CONTROL = CACHE/'evidence/wdt-breadcrumbs-1031/control/offline-proof/prepared'
GPU_TREE = Path.home()/'git/sunstoneos-worktrees/gpu-bisect-1031-crumbs'
GPU_TEMPLATE = CACHE/'evidence/sol-gpu-completion-1031/prepared'
CLANG, LLD = Path('/usr/bin/clang-21'), CACHE/'host/usr/bin/ld.lld-21'
NPU_ENTRY_GATES = {0x070101ac: 1, 0x020025c0: 1 << 6, 0x020025e0: 1 << 18}
NPU_PCK4 = (0x07064000, 0x07064008)
DMA_EXTENT, DMA_MAGIC = 7, 0x41533733444d4131
DMA_FACTS = ('dma_bus_enabled', 'dma_reset_deasserted', 'dma_mbus_enabled',
             'dma_exclusive_writers', 'cpu_quiet', 'mcu_quiet', 'producers_quiet',
             'prior_bus_drained', 'irq_masked', 'watchdog_recovery')


class DmaAdmission(NamedTuple):
    session: bytes
    authority: bytes
    identity: tuple
    port: str
    image_sha256: str
    identity_verified: bool


def load(name, path):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def require(ok, message):
    if not ok:
        raise ValueError(message)


# Every OS image links the NPU service; a session grants it only to an application whose manifest declares the NPU, which
# the image records in ss_application_npu (tools/sunstone-manifest.py).
NPU_DECLARED = 'ss_application_npu'


def npu_on_required(info):
    return bool(info.get('finite_npu') or NPU_DECLARED in info.get('symbols', {}))


def validate_npu_on(words):
    """Captured gates and coherent policy/status must establish an already ON NPU."""
    for address, mask in NPU_ENTRY_GATES.items():
        value = words.get(hex(address))
        require(type(value) is int and 0 <= value <= 0xffffffff,
                f'Missing captured NPU entry gate at {address:#x}.')
        require(value & mask == mask, f'NPU entry gate is closed at {address:#x}; PPU access refused.')
    for address in NPU_PCK4:
        value = words.get(hex(address))
        require(type(value) is int and 0 <= value <= 0xffffffff,
                f'Missing captured NPU PCK4 state at {address:#x}.')
    require(all(words[hex(address)] & 15 == 8 for address in NPU_PCK4),
            'NPU PCK4 policy/status must both be ON (8); OFF or uncertain state is refused.')


SESSION = load('runner_session_lifetime', ROOT/'tools/session_lifetime.py')


class AdmissionRefusal(ValueError):
    """The image's own EL3 admission code refuses the session; raised before any device access."""


BOARD_WORDS = 'board-words.json'


def board_words_path():
    return CACHE/BOARD_WORDS


def record_board_words(result, path=None):
    """Refresh the recorded board words from a completed run's result; returns the words it added or changed.

    Two kinds of word are read from the board under the port lock and kept in the result: the NPU entry state
    (`npu_on_admission`, kept when its status is ON and validate_npu_on accepts it) and the CPU, thermal and clock state
    words of the session (`live_words`, kept when the run returned). Each is a fact the board showed, with the run that
    showed it. The offline session build (offline_words) reads them in place of a live read."""
    path = path or board_words_path()
    found = {}
    proof = result.get('npu_on_admission') or {}
    if proof.get('status') == 'ON':
        try:
            validate_npu_on(proof.get('words', {}))
            found.update(proof['words'])
        except (ValueError, KeyError):
            pass
    if result.get('outcome') == 'returned':
        found.update({a: v for a, v in (result.get('live_words') or {}).items() if type(v) is int and 0 <= v <= 0xffffffff})
    if not found:
        return {}
    try:
        record = json.loads(path.read_text())
    except (OSError, ValueError):
        record = {'schema': 'board-words-v1', 'words': {}, 'runs': {}}
    changed = {a: v for a, v in found.items() if record['words'].get(a) != v}
    if changed:
        record['words'].update(found)
        record['runs'].update({a: {'sid': result.get('sid'), 'time': result.get('time'), 'port': result.get('port')} for a in changed})
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix('.tmp')
        temporary.write_text(json.dumps(record, indent=1, sort_keys=True))
        temporary.replace(path)
    return changed


def seed_board_words(entries=None, path=None):
    """Build the recorded words from the finished runs already on this machine (newest last); used when none are recorded."""
    root = entries or CACHE/'board-queue/entries'
    for result in sorted(root.glob('*/run/result.json')):
        try:
            record_board_words(json.loads(result.read_text()), path)
        except (OSError, ValueError):
            continue


def offline_words(path=None):
    """Board words that stand in for a live read when a submission is checked without the board.

    The session's thermal fields derive from them (apb1 and its rate) and the NPU entry check reads the gates and PCK4
    state; admission validates their consistency, not their liveness. The base is the CPU thermal capture of the evidence
    cache; the recorded words (record_board_words, refreshed by the runner from every completed run that read them) are
    laid over it. Returns None when neither is on this machine."""
    path = path or board_words_path()
    if not path.is_file():
        seed_board_words(path=path)
    capture = CACHE/'evidence/sol-resume-20261031/port-3-3/cpu-thermal-state/session.json'
    words = None
    if capture.is_file():
        words = {}
        for transaction in json.loads(capture.read_text())['transactions']:
            command = transaction['command']
            addresses = [int(command[i+1], 16) for i, item in enumerate(command) if item == 'readl']
            words.update({hex(a): int(v, 16) for a, v in zip(addresses, transaction['stdout'].split())})
    try:
        recorded = json.loads(path.read_text())['words']
    except (OSError, ValueError, KeyError):
        recorded = None
    if recorded:
        words = {**(words or {}), **recorded}
    return words


def preflight_admission(image, session, contract, seconds, lifetime='returning'):
    """Run the image's own EL3 admission (tools/host_admission.py) on the session and contract about to be sent.

    A refusal raises AdmissionRefusal naming the failing term. When the host admission cannot be built for this image,
    the result says so and nothing is decided."""
    admission = load('runner_host_admission', ROOT/'tools/host_admission.py')
    try:
        return admission.preflight(image, session, contract, seconds, lifetime=lifetime)
    except admission.AdmissionRefused as error:
        raise AdmissionRefusal('Host admission refuses this session before any board access: %s' % error)
    except admission.Unavailable as error:
        return {'admitted': None, 'unavailable': str(error)}


def credential_session(session, host_admission, radio_device):
    """Refuse a Wi-Fi credential for a session that might not wipe it.

    The wipe runs where the session is a radio session: board_fail wipes for a radio workload (apps/board/main.c,
    a733_board_radio_workload: bit 0 of the workload at offset 400 of the board prefix), the radio owner task, which only
    a radio workload starts, wipes once the application has finished or failed at EL0, and the returning path wipes when
    the session grants the radio (apps/os/cpu-return.c). The session decode admits the radio device only with the radio
    workload and refuses the radio workload without it (apps/os/session.c), and the host admission runs that decode, so
    a credential goes only to a session that carries both and that the image's own admission accepts."""
    # The board prefix's workload (offset 400) and the typed session header's devices (struct ss_session_header after
    # the 592-byte prefix, devices at 24): the bytes the board reads.
    workload = struct.unpack_from('<I', session, 400)[0]
    devices = struct.unpack_from('<I', session, 592 + 24)[0]
    require(devices & radio_device,
            'A Wi-Fi credential goes only to a session that grants the radio device; this session does not.')
    require(workload & 1,
            'A Wi-Fi credential goes only to a radio workload (1 or 3), which wipes it on a failure; this session has '
            'workload %d.' % workload)
    require(host_admission.get('admitted') is True,
            'A Wi-Fi credential goes only to a session that the image\'s own admission accepts on the host; the host '
            'admission is unavailable (%s).' % host_admission.get('unavailable', 'no result'))


def cpu_grant_raises(grant):
    """True when a CPU clock grant record (struct a733_cpu_clock_grant_record, include/a733_cpu_clock.h) admits a
    transition: a cluster in its cluster mask and a change in its change count. Either cluster's transition can raise its
    rail (DCDC5 for CPU-L and the DSU, DCDC3 for CPU-B), and the return lowers only CPU-B, so such a session can end with
    a rail above the boot point. None for a record of another size. The field offsets come from the record's layout in tools/session.py, which
    checks/board-runner.py compares with the header's offsetof."""
    layout = load('board_grant_layout', ROOT/'tools/session.py')
    size = layout.LAYOUTS['cpu_grant'][1]
    if not grant:
        return False
    if len(grant) != size:
        return None             # a record this layout does not describe: the runner restores (board.py rail_restore_choice)
    return all(struct.unpack_from('<I', grant, layout.offset('cpu_grant', name))[0] for name in ('clusters', 'changes'))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def symbols(image):
    elf = ELFFile(io.BytesIO(Path(image).read_bytes()))
    table = elf.get_section_by_name('.symtab')
    result = {}
    for s in table.iter_symbols() if table else ():
        if s['st_shndx'] != 'SHN_UNDEF':
            result[s.name] = (int(s['st_value']), int(s['st_size']))
    return result


def symbol_bytes(image, name):
    elf = ELFFile(io.BytesIO(Path(image).read_bytes()))
    addr, size = symbols(image)[name]
    for s in elf.iter_segments():
        if s['p_type'] == 'PT_LOAD' and s['p_vaddr'] <= addr and addr + size <= s['p_vaddr'] + s['p_filesz']:
            return s.data()[addr - s['p_vaddr']:addr - s['p_vaddr'] + size]
    raise ValueError('Symbol has no file bytes: ' + name)


def finite_npu_files(args):
    """Resolve the native session and exact build manifest consumed by admission."""
    template = Path(args.template) if args.template else None
    session = getattr(args, 'records', None) or (template/'session.bin' if template else None)
    require(session is not None, 'NPU requires --records or a template directory with session.bin.')
    manifest = getattr(args, 'manifest', None)
    if manifest is None:
        manifest = template/'manifest.json' if template else Path(args.image).parent/'manifest.json'
        if not manifest.is_file():
            manifest = Path(args.image).parent/'manifest.json'
    return Path(manifest), Path(session)


def finite_npu_info(image, host, finite, manifest):
    """Admit native NPU geometry against its exact watchdog and session contract."""
    watchdog = host.watchdog_marker(image)
    require(watchdog == {'mode': 0xb1, 'session_seconds': 20},
            'NPU requires the exact host-armed watchdog marker (mode 0xb1, 20 seconds).')
    parts, address, build, layout, geometry = finite.image_profile(Path(image).read_bytes(), manifest)
    profile = finite.geometry_profile(geometry)
    profiles = [finite.STARTUP, finite.INFERENCE, finite.RUNTIME, finite.RESTART,
                finite.SWITCH, finite.SDK, finite.POWER,
                getattr(finite, 'LATENCY', 'npu-latency-return-v1')]
    # Older frozen decoders have no cancellation extension. The new decoder
    # validates its immutable command marker and controller-only recovery path.
    if getattr(finite, 'ACTIVE_CANCEL', None) == 'npu-active-cancel-return-v1':
        profiles.append(finite.ACTIVE_CANCEL)
    require(profile in profiles, 'Unsupported finite NPU profile.')
    require(layout[:3] == (456, 224, 232), 'NPU development session layout differs.')
    return {'watchdog': watchdog, 'geometry': geometry, 'session_layout': list(layout),
            'execution_profile': profile, 'image_sha256': sha(image), 'build_id': build.hex()}


def inspect(image, kind, descriptor=None, host_tree=None, manifest=None):
    tree = host_tree or ROOT
    host = load('board_records', tree / 'apps/os/host.py')
    names = symbols(image) if Path(image).read_bytes()[:4] == b'\x7fELF' else {}
    descriptor = descriptor or {}
    if descriptor:
        require(descriptor.get('image_sha256') == sha(image), 'Image descriptor hash differs.')
    if 'a733_card_fel_profile' in names:
        require(kind == 'probe', 'Card FEL recovery uses --kind probe.')
        recovery = load('card_fel_recovery_contract', ROOT/'tools/card_recovery.py').inspect(image)
        return {'symbols': names, 'watchdog': recovery['watchdog'], 'generic': False, 'entry_guard': False,
                'descriptor': descriptor, 'card_fel_recovery': recovery, 'usb_class': False}
    if 'usb_vbus_probe_profile' in names:
        profile = symbol_bytes(image, 'usb_vbus_probe_profile')
        require(profile in [struct.pack('<3Q', 0x3153554256535355, 1, mode) for mode in (0, 1, 10)],
                'Unsupported PL8 VBUS diagnostic profile.')
        require(kind == 'probe' and 'sunstone_probe_marker' in names,
                'PL8 VBUS diagnostic requires the native probe contract.')
    if kind == 'usb-class':
        require('sunstone_probe_marker' in names,
                'USB class images require the native probe contract and real device cleanup; retained OS images cannot return.')
        require('a733_usb2_native_poll' in names,
                'USB class kind requires the linked native USB host controller.')
    if kind in ('probe', 'usb-class') and 'sunstone_probe_marker' in names:
        usb_class = 'a733_usb2_native_poll' in names
        native = load('native_probe_contract', ROOT/'tools/board_probe.py').inspect(
            image, usb_pool=host.usb_pool_geometry if usb_class else None)
        return {'symbols':names,'watchdog':native['watchdog'],'generic':False,'entry_guard':False,
                'descriptor':descriptor,'native_probe':native,'usb_class':usb_class}
    if kind == 'npu' and 'ss_result_record' not in names:
        tree = host_tree or ROOT
        finite = load('inspect_npu_irq', tree/'apps/npu-interrupt/host.py').finite
        if manifest is None:
            manifest = json.loads((Path(image).parent/'manifest.json').read_bytes())
        facts = finite_npu_info(image, host, finite, manifest)
        # Current admission excludes physical OFF even with a frozen geometry parser.
        require(facts.get('execution_profile') != finite.POWER,
                'Unsupported physical NPU OFF profile; use local stop/restart.')
        return dict(facts, symbols=names, generic=False, entry_guard=False,
                    descriptor=descriptor, finite_npu=True,
                    failure_return='npu_failure_return_profile' in names)
    marker = host.watchdog_marker(image) if names else descriptor.get('watchdog')
    require(marker is not None, 'Image does not admit the host-armed watchdog; rebuild with RTC breadcrumbs.')
    require(set(marker) == {'mode', 'session_seconds'} and all(type(v) is int for v in marker.values()) and
            marker['mode'] & 0xff0f == 1 and 1 <= marker['mode'] >> 4 <= 11 and
            1 <= marker['session_seconds'] <= SESSION.MAX_SECONDS, 'Invalid watchdog marker.')
    generic = 'ss_result_record' in names
    dma = 'os_dma_profile' in names
    if dma:
        require(generic and symbol_bytes(image, 'os_dma_profile') == struct.pack('<3Q', DMA_MAGIC, 1, 4096),
                'Unsupported native DMA return profile.')
    geometry = None
    if kind != 'probe':
        require(generic or 'os_cpu_return_output' in names, 'No returning application result in image.')
        base = load('inspect_image_parts', ROOT/'tools/check-a733-smp.py')
        base.image_parts(Path(image))
        if generic:
            host.result_geometry(image)
            returning = load('inspect_return_geometry', tree/'apps/os/cpu-return-host.py')
            geometry = returning.geometry(Path(image))
            if kind == 'gpu-return':
                require('gpu_receipt' in geometry, 'GPU kind requires the linked GPU return receipt.')
        else:
            legacy = load('inspect_legacy', (host_tree or GPU_TREE)/'apps/os/cpu-return-host.py')
            geometry = legacy.geometry(Path(image))
        if kind=='npu':
            require(NPU_DECLARED in names, 'NPU kind requires an application that declares the NPU.')
        if generic and NPU_DECLARED in names:
            require('npu_stop' in geometry, 'NPU image requires its mandatory stop receipt.')
    else:
        require(descriptor.get('purpose')=='watchdog-reset-diagnostic','Raw probes admit only the intentional watchdog-reset diagnostic; use the native ELF contract.')
        require(descriptor.get('schema') == 'sunstone-board-probe-v1', 'Probe requires a hash-bound .board.json descriptor.')
        require(descriptor.get('entry') == 0x41000000 and descriptor.get('report_address') == 0x42000000,
                'Probe entry/report reservations differ.')
    return {'symbols': names, 'watchdog': marker, 'generic': generic,
            'entry_guard': 'os_cpu_return_entry_profile' in names, 'descriptor': descriptor,
            'failure_return': 'os_cpu_return_failure' in names,
            'report_geometry': geometry, 'dma': dma,
            'usb_class': 'os_usb_service_dispatch' in names}


def session_template(args):
    template = Path(args.template or (GPU_TEMPLATE if args.kind == 'gpu-return' else CONTROL))
    path = template/'session.bin' if (template/'session.bin').is_file() else template/'board/session-template.bin'
    return template, Path(getattr(args, 'records', None) or path)


def dma_admission(args, info, live=None):
    """Authenticate the original DMA record before reseeding any session identity.

    Offline admission binds supplied facts. The runner verifies their original
    USB instance under its lock before resetting, then preserves these bytes.
    """
    if not info.get('generic'):
        return None
    template, path = session_template(args)
    raw = path.read_bytes()
    records = load('runner_dma_records', ROOT/'apps/os/host.py')
    typed = len(raw) >= 600 and struct.unpack_from('<Q', raw, 592)[0] == records.SESSION_MAGIC
    service = records.session_decode(raw)['extents'].get(DMA_EXTENT) if typed else None
    require(bool(service) == bool(info.get('dma')), 'Native DMA profile and service extent differ.')
    if service is None:
        return None
    require(len(service) == 160 and struct.unpack_from('<3Q', service) == (DMA_MAGIC, 1, 160),
            'Invalid DMA service extent.')
    fabric = struct.unpack_from('<6I', service, 56)
    expected_cpu, expected_mcu, desired_cpu, desired_mcu, route_mask = struct.unpack_from('<5I', service, 80)
    owner, handoff, deadline, cold = struct.unpack_from('<3QI', service, 104)
    controller, evidence, established, owned, channels = struct.unpack_from('<2QI2H', service, 136)
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    require(fabric == struct.unpack_from('<6I', raw, 488) and owner and
            handoff == evidence == lease and not deadline and not cold and not route_mask and
            expected_cpu == desired_cpu and expected_mcu == desired_mcu and desired_cpu & 1 and
            controller == 0x04601000 and established == 0x3f and owned == 0xffff and channels == 1,
            'DMA requires the fixed inherited DMA0 configuration and drained ownership.')
    authority_path = template/'dma-authority.json'
    require(authority_path.is_file(), 'DMA requires template/dma-authority.json with physical authority.')
    data = authority_path.read_bytes()
    require(hashlib.sha256(data).digest() == service[24:56], 'DMA authority digest differs from service record.')
    authority = json.loads(data)
    require(isinstance(authority, dict), 'DMA authority must be an object.')
    identity = live.get('identity') if live is not None else authority.get('identity')
    require(isinstance(identity, list) and len(identity) == 5 and all(type(x) is int for x in identity),
            'DMA requires the USB instance identity captured under the board lock.')
    expected = dict(identity=identity, port=args.port, image_sha256=sha(args.image),
                    epoch=epoch, generation=generation, lease=lease, controller=controller,
                    service_channels=channels, dma_evidence=evidence, dma_handoff=handoff,
                    configuration_sha256=hashlib.sha256(service[56:160]).hexdigest())
    security = load('runner_dma_security', ROOT/'tools/dma-nonsecure-access.py')
    security.validate(data, expected, None, require)
    require(all(authority.get(name) is True for name in DMA_FACTS) and
            type(authority.get('dma_autogate')) is int and authority['dma_autogate'] == 4,
            'DMA inherited gates, drain, exclusion or recovery facts are incomplete.')
    return DmaAdmission(raw, data, tuple(identity), args.port, expected['image_sha256'], live is not None)


def bind_dma_identity(admission, args, identity):
    """Bind frozen facts to the original instance under the runner's port lock."""
    require(isinstance(admission, DmaAdmission) and admission.port == args.port and
            admission.image_sha256 == sha(args.image) and tuple(identity) == admission.identity,
            'DMA authority does not cover the original locked USB instance.')
    return admission._replace(identity_verified=True)



def policy(info, args, session=None):
    """Approval adds authority; it never creates a storage grant or a backup."""
    d = info['descriptor']
    approved = args.approved_by == 'coordinator'
    if getattr(args, 'lifetime', 'returning') == 'resident':
        require(info.get('generic') and not info.get('dma') and args.kind == 'cpu-return',
                'Resident FEL qualification requires a generic CPU OS image.')
        require(not preserving(args), 'Resident watchdog reset loses DRAM; --preserve-dram is incompatible.')
        require('os_resident_test_profile' in info.get('symbols', {}) and
                symbol_bytes(args.image, 'os_resident_test_profile') == struct.pack('<3Q', 0x3154534554525353, 1, 0x5000),
                'Resident FEL qualification requires the linked stop-on-finish test profile.')
    require(not getattr(args, 'marker_clear_test', False) or approved, '--marker-clear-test requires --approved-by coordinator.')
    if getattr(args, 'rearm_expiry', None):
        recovery = info.get('card_fel_recovery')
        require(approved, '--rearm-expiry requires --approved-by coordinator.')
        require(isinstance(recovery, dict) and recovery.get('profile', {}).get('entry') == 2,
                '--rearm-expiry runs with the warm-entry SRAM proof image (a733-card-fel-warm.elf).')
        require(not getattr(args, 'boot_once_slot', None) and not getattr(args, 'marker_clear_test', False) and not args.storage_write,
                '--rearm-expiry is incompatible with --boot-once-slot, --marker-clear-test and --storage-write.')
    if getattr(args, 'boot_once_slot', None):
        require(approved, '--boot-once-slot requires --approved-by coordinator.')
        require(getattr(args, 'lifetime', 'returning') == 'resident',
                '--boot-once-slot runs the slot\'s bounded resident image and requires --lifetime resident.')
        require(not getattr(args, 'marker_clear_test', False) and not getattr(args, 'wifi', None) and not args.storage_write,
                '--boot-once-slot is incompatible with --marker-clear-test, --wifi and --storage-write.')
        boot_once_slot(args)
    # The radio stage limit shapes the radio profile; an image without the radio's result channel has none.
    require(not getattr(args, 'radio_stage_limit', 0) or 'os_radio_result_descriptor' in (info.get('symbols') or {}),
            '--radio-stage-limit applies only to the radio scan image.')
    if 'usb_vbus_probe_profile' in info.get('symbols', {}):
        require(approved and args.port == '3-1', 'PL8 VBUS probe requires coordinator approval on port 3-1.')
    if info.get('usb_class'):
        require(approved, 'USB class launch requires --approved-by coordinator after review of source and current-cutoff evidence.')
    storage = d.get('storage_write') or args.storage_write
    # Bounded card writes (tools/storage_bounds.py): classification is default-deny. A read-only image
    # and an image without an SD core pass unchanged; any other image, and any write request, needs
    # the coordinator's approval record for the exact bounds.
    bounds = load('storage_bounds', ROOT/'tools/storage_bounds.py')
    if info.get('card_fel_recovery'):
        require(approved and args.kind == 'probe', 'Card FEL recovery requires --approved-by coordinator.')
        require(not args.input and not getattr(args, 'decoder', None),
                'Card FEL recovery takes no inputs and uses its fixed retained result decoder.')
        require(not storage and not d.get('storage_bounds') and not d.get('storage_scratch') and not d.get('storage_boot_area') and
                bounds.classify(args.image) == 'no-core', 'Card FEL recovery must have no SD core or storage-write authority.')
        require(not d.get('operating_points') and getattr(args, 'restore_rails', None) is not True,
                'Card FEL recovery carries no clock, rail or power transition authority.')
        require(not preserving(args), 'Card FEL recovery uses only SRAM and has no DRAM preservation.')
        load('card_fel_recovery_tool', ROOT/'tools/card_recovery.py').fel_bytes(args.fel)
    if storage or bounds.classify(args.image) == 'write-capable':
        bounds.admit(d, args, sha(args.image))
    # The reviewed operating points are data (tools/cpu-reviewed-points.json, protected), shared with tools/session.py.
    reviewed = {k: tuple(v) for k, v in json.loads((ROOT / 'tools/cpu-reviewed-points.json').read_text())['points'].items()}
    for point in d.get('operating_points', []):
        require(isinstance(point, dict) and set(point) == {'block', 'mhz', 'mv'}, 'Invalid operating point.')
        require(approved or reviewed.get(point['block']) == (point['mhz'], point['mv']),
                'Operating point requires --approved-by coordinator.')
    if session is not None and info['generic']:
        # Only a typed session carries a CPU clock grant, and its extent says so: no image symbol and no trailing bytes.
        host = load('board_policy_records', (args.host_tree or ROOT) / 'apps/os/host.py')
        grant = host.session_decode(session)['extents'].get(host.CPU_GRANT)
        if grant:
            clusters, little, big, changes, perf = struct.unpack_from('<5I', grant, 48)
            require(0 <= clusters <= 3 and changes <= 64 and perf <= 2 and (not perf or clusters == 3), 'Invalid CPU grant limits.')
            require(not any(grant[68:96]), 'A CPU grant has no approval bytes: the approval is recorded by --approved-by, not in the session.')
            require(approved or ((little == reviewed['cpu-l'][0] if clusters & 1 else little == 0) and
                                (big == reviewed['cpu-b'][0] if clusters & 2 else big == 0)),
                    'CPU grant outside reviewed operating points requires --approved-by coordinator.')


def launcher(out, size, mode, seconds=60, tree=ROOT, magic=0x415337334f534331):
    """Reuse the board launcher with the OS contract of the given magic, full session and armed WDT."""
    board = load('runner_board_prepare', tree / 'apps/board/prepare.py')
    source = (tree / 'platform/a733-smp-supervised/launch.c').read_text()
    source = board.adapt_launcher(source, {
        '0x4153373353554c33': '0x415337334f534c31', 'REQUEST[1]==3': 'REQUEST[1]==2',
        'CONTRACT->version==3': 'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':
            f'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]=={size}u',
        '20*A733_SMP_COUNTER_HZ': 'A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ',
        '!(rd(0x02050014)&1) && !(rd(0x02050010)&0x100) &&':
            f'(rd(0x02050014)&0xffff)=={mode:#x} && (rd(0x02050010)&0xffff)==1 && (rd(0x02050018)&0xffff)==2 &&'})
    if magic == 0x415337334f524331:
        require(mode == 0x41, 'Resident entry requires the four-second WDT0 mode.')
        source = board.adapt_launcher(source, {
            'start<=UINT64_MAX-A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ':
                'start<=UINT64_MAX-4*A733_SMP_COUNTER_HZ',
            'CONTRACT->diagnostic_deadline=start+A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ;':
                'CONTRACT->diagnostic_deadline=UINT64_MAX;',
            'CONTRACT->expires_at=start+A733_BOARD_SUPERVISED_SECONDS*A733_SMP_COUNTER_HZ;':
                'CONTRACT->expires_at=start+4*A733_SMP_COUNTER_HZ;\n'
                '    CONTRACT->watchdog_mode=0x41;CONTRACT->watchdog_configuration=1;CONTRACT->watchdog_output=2;'})
        source = board.adapt_launcher(source, {
            '!rd(0x070501c0) && !rd(0x070501d4),6);':
                '!rd(0x070501c0) && !rd(0x070501c4) && !rd(0x070501d4) && !rd(0x07090108),6);',
            'uint64_t start=ticks();require(start<=UINT64_MAX-4*A733_SMP_COUNTER_HZ,8);':
                'uint64_t start=ticks();require(start<=UINT64_MAX-4*A733_SMP_COUNTER_HZ,8);\n'
                '    *(volatile uint32_t *)(uintptr_t)0x0205000c=0x14af;\n'
                '    __asm__ volatile("dsb sy":::"memory");\n'
                '    require((rd(0x02050014)&0xffff)==0x41 && (rd(0x02050010)&0xffff)==1 &&\n'
                '        (rd(0x02050018)&0xffff)==2,6);\n'
                '    within(start);'}, counts={'!rd(0x070501c0) && !rd(0x070501d4),6);':2})
    (out / 'board-launch-base.c').write_text(source)
    for name, path in [('launch.c', 'apps/board/launch.c'), ('launch.S', 'platform/a733-smp-observe/launch.S'),
                       ('launch.ld', 'platform/a733-smp-observe/launch.ld'), ('boot.h', 'platform/a733-smp-cached/boot.h'),
                       ('a733_board_application.h', 'include/a733_board_application.h')]:
        shutil.copy2(tree / path, out / name)
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
             '-Os', '-g', '-Wall', '-Wextra', '-Werror', '-DSUNSTONE_BOARD_SUPERVISED=1',
             f'-DA733_BOARD_SUPERVISED_SECONDS={seconds}',
             f'-DA733_SMP_CONTRACT_MAGIC={magic:#x}ULL', '-I' + str(out)]
    commands = [[str(Path('/usr/bin/clang-21')), *flags, '-c', str(out / n), '-o', str(out / o)]
                for n, o in [('launch.S', 'entry.o'), ('launch.c', 'launch.o')]]
    commands += [[str(CACHE / 'host/usr/bin/ld.lld-21'), '-T', str(out / 'launch.ld'),
                  str(out / 'entry.o'), str(out / 'launch.o'), '-o', str(out / 'launch.elf')],
                 ['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', str(out / 'launch.elf'), str(out / 'launch.bin')]]
    with (out / 'build.log').open('w') as log:
        for cmd in commands:
            subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
    require(0 < (out / 'launch.bin').stat().st_size <= 0xc00, 'Launcher exceeds SRAM reservation.')


def prepare(args, info, live, out, *, dma_binding=None):
    if npu_on_required(info):
        validate_npu_on(live.get('words', {}))
    if info.get('finite_npu'):
        tree = args.host_tree or ROOT
        finite = load('prepare_npu_irq', tree/'apps/npu-interrupt/host.py').finite
        helper = load('prepare_npu_development', tree/'apps/npu-finite/development.py')
        require(not args.input and not getattr(args, 'decoder', None),
                'Finite NPU images use linked inputs and their native result decoder.')
        manifest, session = finite_npu_files(args)
        meta = helper.prepare(finite, Path(args.image), manifest, session, live, out,
                              str(CLANG), str(LLD), str(args.objcopy))
        helper.validator(finite, finite.watchdog_marker)(out)
        uploads = [(part['address'], out/part['name']) for part in meta['segments']]
        uploads += [(0x42000000,out/'result-zero.bin'),(0x42004000,out/'contract.bin'),
                    (0x52c00,out/'request.bin'),(0x52d00,out/'report-zero.bin'),(0x52000,out/'launch.bin')]
        return dict(meta, uploads=uploads, launch=['exe','0x52000'], format='npu', finite=finite)
    if info.get('native_probe'):
        return load('prepare_native_probe', ROOT/'tools/board_probe.py').prepare(args.image,info['native_probe'],args.input,out)
    if info.get('card_fel_recovery'):
        return load('prepare_card_fel_recovery', ROOT/'tools/card_recovery.py').prepare(info['card_fel_recovery'],args.input,out)
    dev = load('runner_dev', ROOT / 'tools/board_state.py')
    base = load('runner_base', ROOT / 'tools/check-a733-smp.py')
    tree = args.host_tree or ROOT
    records = load('runner_records', tree / 'apps/os/host.py')
    if args.kind == 'probe':
        out.mkdir()
        raw = Path(args.image).read_bytes()
        # Raw probes have an explicit descriptor and may only use the kernel interval.
        require(raw[:4] != b'\x7fELF' and 0 < len(raw) <= 0x100000, 'Probe requires a flat kernel image <= 1 MiB.')
        return {'epoch': 1, 'generation': 1, 'launch': ['reset64', '0x41000000'],
                'uploads': [(0x41000000, Path(args.image)), (0x42000000, write(out/'blank.bin', bytes(16384)))],
                'watchdog': info['watchdog'], 'format': 'probe'}
    template = args.template or (GPU_TEMPLATE if args.kind == 'gpu-return' else CONTROL)
    if not info['generic']:
        tree = args.host_tree or GPU_TREE
        host = load('runner_legacy_host', tree / 'apps/os/cpu-return-host.py')
        meta = prepare_legacy(host, args.image, template, out, live)
        require(not meta['state_findings'], '; '.join(meta['state_findings']))
        raw = (out / 'board/session.bin').read_bytes()
        policy(info, args, raw)
        uploads = [(s['address'], out/'board'/s['name']) for s in meta['segments']]
        uploads += [(a['address'], out/'assets'/a['name']) for a in host.os_image.host.validate_assets(out/'assets')['assets']]
        uploads += [(0x42000000, out/'board/result-zero.bin'), (0x42004000, out/'board/contract.bin'),
                    (0x52c00, out/'board/request.bin'), (0x52d00, out/'board/report-zero.bin'), (0x52000, out/'board/launch.bin')]
        return dict(meta, uploads=uploads, launch=['exe', '0x52000'], format='legacy', host=host,
                    geometry=host.geometry(Path(args.image)))
    # A typed-session bundle can carry NPU service facts and arbitrary manifest inputs.
    # The reviewed legacy control template also supplies board/thermal/CPU facts
    # for the OS-only hello-compute control; no application asset is copied.
    template, session_path = session_template(args)
    admission = dma_binding if dma_binding is not None else dma_admission(args, info, live)
    if admission is not None:
        require(isinstance(admission, DmaAdmission) and admission.identity_verified and info.get('dma') and
                admission.port == args.port and admission.image_sha256 == sha(args.image),
                'DMA preparation requires frozen authority bound to the original USB instance.')
    dma_authority = admission.authority if admission is not None else None
    raw = admission.session if admission is not None else session_path.read_bytes()
    image_info = records.image_manifest(args.image)
    if struct.unpack_from('<Q', raw, 592)[0] == records.SESSION_MAGIC:
        old = records.session_decode(raw)
        services = {k: v for k, v in old['extents'].items() if k != records.INPUTS}
        devices, flags = old['devices'], old['flags']
    else:
        require(len(raw) == 904, 'Supply a typed session bundle for NPU/audio images.')
        services = {records.THERMAL: raw[592:808], records.CPU_GRANT: raw[808:904]}
        devices, flags = 0, records.BIND_SID
    # The session grants the NPU exactly when the application declares it; only then does the host read the NPU records.
    npu_session = records.NPU in services
    require(npu_session == (NPU_DECLARED in (info.get('symbols') or {})),
            'The session carries an NPU record exactly when the application declares the NPU: the record is %s and the '
            'application %s.' % ('present' if npu_session else 'absent',
                                 'declares it' if NPU_DECLARED in (info.get('symbols') or {}) else 'does not'))
    geometry = info['report_geometry']
    prefix = bytearray(raw[:592])
    epoch, generation, lease = [secrets.randbits(62) | 1 for _ in range(3)]
    struct.pack_into('<3Q', prefix, 24, epoch, generation, lease)
    parts, address, identity = base.image_parts(Path(args.image))
    prefix[48:80] = identity
    radio_profile = None
    if 'os_radio_result_descriptor' in (info.get('symbols') or {}):
        # The scanning radio image: a workload 3 session that grants the radio device, a scan-only radio profile and
        # the firmware it names.
        devices |= records.manifest_tool.DEVICES['radio']
        scan = load('radio_scan_prepare', ROOT/'tools/radio_scan_prepare.py')
        reference = (ROOT/'docs/designs/radio-vendor-power-sequence.md').read_bytes()
        radio_profile = scan.apply(prefix, hashlib.sha256(reference).digest(),
                                   stage_limit=getattr(args, 'radio_stage_limit', 0))
    if records.JPEG in services:
        # The JPEG service embeds the same board prefix. Rebind its session
        # identity together with the outer prefix, preserving its evidence.
        jpeg = bytearray(services[records.JPEG])
        jpeg[:records.PREFIX_BYTES] = prefix
        services[records.JPEG] = jpeg
    if records.CPU_GRANT in services:
        grant = bytearray(services[records.CPU_GRANT])
        if struct.unpack_from('<I', grant, 48)[0]:
            struct.pack_into('<2Q', grant, 32, epoch, generation)
        # A session prepared from a template written before the approval bytes left the record (a 32-byte digest after the
        # change count) carries no perf and no reserved bytes: the digest is cleared, and the runner records the approval.
        if struct.unpack_from('<I', grant, 64)[0] > 2 or any(grant[68:96]):
            grant[64:96] = bytes(32)
        services[records.CPU_GRANT] = grant
    if DMA_EXTENT in services:
        dma = bytearray(services[DMA_EXTENT])
        struct.pack_into('<Q', dma, 112, lease)
        struct.pack_into('<Q', dma, 144, lease)
        services[DMA_EXTENT] = dma
    findings, derived = dev.state_check(live['words'])
    require(not findings, '; '.join(findings))
    thermal = bytearray(services[records.THERMAL])
    struct.pack_into('<2I', thermal, 168, derived['apb1'], derived['apb1_hz'])
    thermal[176] = derived['configure_adc_clock']
    services[records.THERMAL] = thermal
    inputs = [p.read_bytes() for p in args.input]
    if not inputs and image_info['manifest']['name'] == 'hello-compute':
        inputs = [b'SunstoneOS board control\n']
    elif not inputs:
        inputs = [(template/f'input-{i}.bin').read_bytes() for i, _ in enumerate(records.regions(image_info['manifest'], (1,)))]
    producer = load('runner_session_prepare', tree/'apps/os/prepare.py')
    meta = producer.prepare(args.image, prefix, services, inputs, out, devices=devices, flags=flags,
                            npu_pool=records.parse_npu_pool(getattr(args, 'npu_pool', None)))
    if dma_authority is not None:
        write(out/'dma-authority.json', dma_authority)
    session = (out/'session.bin').read_bytes()
    policy(info, args, session)
    lifetime = getattr(args, 'lifetime', 'returning')
    # The linked profile supplies authority; a command-line selection cannot add a lifetime.
    magic = load('runner_host_admission', ROOT/'tools/host_admission.py').contract_magic(
        symbol_bytes(args.image, 'os_contract_profile'), lifetime)
    watchdog = dict(info['watchdog'])
    if lifetime == 'resident':
        watchdog['mode'] = 0x41
    launcher(out, len(session), watchdog['mode'], watchdog['session_seconds'], tree, magic)
    board = load('runner_request', ROOT/'apps/board/prepare.py')
    request, contract = board.request_bytes(address, identity, epoch, generation, lease, session)
    request = struct.pack('<Q', 0x415337334f534c31) + request[8:]
    contract = bytearray(contract)
    struct.pack_into('<Q', contract, 184, len(session))
    host_admission = preflight_admission(args.image, session, bytes(contract), watchdog['session_seconds'], lifetime)
    uploads = []
    for i, part in enumerate(parts):
        if part['data']:
            uploads.append((part['address'], write(out/f'segment-{i}.bin', part['data'])))
    uploads += [(u['address'], out/u['name']) for u in meta['uploads']]
    radio_regions = []
    if radio_profile is not None:
        for radio_address, radio_name, radio_data in radio_profile[1]:
            uploads.append((radio_address, write(out/radio_name, radio_data)))
            radio_regions.append({'address': radio_address, 'bytes': len(radio_data)})
    # The launcher checks both sentinel words in this page before entry.
    blank = bytearray(16384)
    struct.pack_into('<I', blank, 0, 0x73307330)
    uploads += [(0x41f00000, out/'session.bin'), (0x42000000, write(out/'result-zero.bin', blank)),
                (0x42004000, write(out/'contract.bin', contract)), (0x52c00, write(out/'request.bin', request)),
                (0x52d00, write(out/'report-zero.bin', bytes(128))), (0x52000, out/'launch.bin')]
    returning = load('prepare_return_geometry', tree/'apps/os/cpu-return-host.py')
    plan = dict(meta, epoch=epoch, generation=generation, uploads=uploads, launch=['exe', '0x52000'],
                host_admission=host_admission, watchdog=watchdog, lifetime=lifetime, format='generic', geometry=geometry,
                return_host=returning, input_regions=list(meta['uploads']) + radio_regions, image_ram=returning.IMAGE_RAM)
    if getattr(args, 'wifi', None):
        credential_session(session, host_admission, records.manifest_tool.DEVICES['radio'])
        # The credential is a separate input, uploaded by the runner from memory (Live.deliver_credential); it is not
        # a file of the bundle. Only its extent is known here, so the preservation list can cover it.
        wifi = load('runner_wifi_credential', ROOT/'tools/wifi-credential.py')
        plan['credential'] = {'address': wifi.RECORD_ADDRESS, 'bytes': wifi.RECORD_BYTES}
    plan['cpu_grant_raises'] = cpu_grant_raises(services.get(records.CPU_GRANT))
    plan['preserve_regions'] = preservation_regions(plan)
    validate_uploads(plan)
    return plan


def boot_once_slot(args):
    """Verify that the submitted image is the runtime the slot manifest names, and return the record the result carries.

    The manifest is the plan of tools/boot-slots.py (or the card build's manifest): its kind 1 extent holds the flat runtime
    image, whose SHA-256 must equal the hash of the flat runtime built from the submitted ELF (apps/os/card.py). The run then
    names exactly what the card boots; no byte of the card is read or written.

    It first requires a measured watchdog recovery for every stage of the chain (tools/boot_chain.py): a stage that hangs
    with no WDT0 reset leaves the board off USB until the card is removed."""
    problems = load('boot_once_chain', ROOT/'tools/boot_chain.py').unproven()
    require(not problems, 'Boot-once refused: a stage of the card boot chain has no measured watchdog recovery. ' +
            ' '.join(problems))
    path = Path(args.boot_once_slot)
    raw = path.read_bytes()
    try:
        manifest = json.loads(raw)
    except ValueError as error:
        raise ValueError('The boot-once slot manifest is not JSON: %s' % error) from None
    extents = manifest.get('extents') if isinstance(manifest, dict) else None
    require(isinstance(extents, list) and extents and manifest.get('kind', 'a733-boot-slot-plan') == 'a733-boot-slot-plan',
            'The boot-once slot manifest has no boot slot extents.')
    runtime = [e for e in extents if isinstance(e, dict) and e.get('kind') == 1]
    require(len(runtime) == 1 and re.fullmatch(r'[0-9a-f]{64}', str(runtime[0].get('sha256', ''))),
            'The boot-once slot manifest needs exactly one runtime extent (kind 1) with a SHA-256.')
    flat = load('boot_once_runtime', ROOT/'apps/os/card.py').flat_runtime(args.image)
    digest = hashlib.sha256(flat).hexdigest()
    require(digest == runtime[0]['sha256'],
            'The submitted image flattens to runtime %s, but the slot manifest names %s; the card would boot another image.'
            % (digest, runtime[0]['sha256']))
    return {'manifest_sha256': hashlib.sha256(raw).hexdigest(), 'slot': manifest.get('slot'),
            'generation': manifest.get('generation'), 'entry': manifest.get('entry'),
            'runtime_sha256': digest, 'runtime_bytes': len(flat),
            'extents': [{k: e.get(k) for k in ('name', 'kind', 'load', 'bytes', 'sha256')} for e in extents]}


def preserving(args):
    """True when the run saves and restores DRAM. A caller without the option keeps the safe behavior."""
    return bool(getattr(args, 'preserve_dram', True))


def preservation_regions(plan):
    """Union the linked image/monitor, manifest, input and device DRAM extents."""
    geo = plan['geometry']
    extents = [plan['image_ram'], (geo['report_address'], geo['report_bytes']),
               (geo['output_address'], geo['output_bytes'])]
    extents += [(geo[name][0], geo[name][1] - geo[name][0])
                for name in ('monitor', 'cached_writable')]
    extents += [(r['address'], r['bytes']) for r in geo['application_regions']]
    extents += [(r['address'], r['bytes']) for r in plan['input_regions']]
    if plan.get('credential'):
        extents.append((plan['credential']['address'], plan['credential']['bytes']))
    merged = []
    for address, size in sorted(extents):
        require(type(address) is int and type(size) is int and size > 0 and
                RUNNER_DRAM[0] <= address < address + size <= RUNNER_DRAM[1],
                'Preservation extent %#x..%#x is outside the DRAM the runner can address (%#x..%#x, the reach of a '
                '32-bit FEL address); a region there is marked scratch in the manifest or is not preserved.' % (
                    address, address + size, RUNNER_DRAM[0], RUNNER_DRAM[1]))
        high = address + size
        if merged and address <= merged[-1][0] + merged[-1][1]:
            low, count = merged[-1]
            merged[-1] = (low, max(low + count, high) - low)
        else:
            merged.append((address, size))
    return merged


def preserve_estimate(plan):
    """(bytes, seconds): what backing up and restoring the plan's preservation regions transfers, and its time."""
    total = sum(size for _, size in plan['preserve_regions'])
    return total, memory_layout.preserve_seconds(total)


def require_budget(plan, budget_seconds):
    """Refuse a preserving plan whose estimated transfer exceeds the budget the submission names."""
    total, seconds = preserve_estimate(plan)
    if budget_seconds is not None:
        require(seconds <= budget_seconds,
                'The preserving plan moves %d MiB and takes about %.0f s (%d MiB read and written at %d kB/s); '
                'the submission allows %.0f s. Mark the regions that need no preservation scratch or raise the budget.' % (
                    total >> 20, seconds, total >> 20, memory_layout.record()['fel_bytes_per_second'] // 1000,
                    budget_seconds))
    return total, seconds


def validate_uploads(plan):
    regions = preservation_regions(plan)
    require(plan['preserve_regions'] == regions, 'Preservation plan omits a linked DRAM range.')
    for address, source in plan['uploads']:
        size = Path(source).stat().st_size
        require(size > 0 and (any(low <= address and address + size <= low + count
                                 for low, count in regions) or
                             0x52000 <= address < address + size <= 0x52d80),
                'Upload is outside protected DRAM or launcher SRAM.')


def backup_dram(owner, regions, label='dram'):
    """Read bounded chunks before uploads and retain their hashes durably."""
    backups = []
    for address, size in regions:
        for offset in range(0, size, 4 * 1024 * 1024):
            count = min(4 * 1024 * 1024, size - offset)
            path = owner.evidence/f'{label}-before-{len(backups)}.bin'
            data = owner.read(address + offset, count, path)
            require(len(data) == count and path.stat().st_size == count, 'Incomplete DRAM backup.')
            with path.open('rb') as stream:
                os.fsync(stream.fileno())
            backups.append({'address': address + offset, 'bytes': count, 'file': str(path),
                            'sha256': hashlib.sha256(data).hexdigest()})
    return backups


def restore_dram(owner, backups, regions):
    """Validate every saved byte and extent before the first restoration write."""
    expected = [(a + o, min(4 * 1024 * 1024, n - o)) for a, n in regions
                for o in range(0, n, 4 * 1024 * 1024)]
    require([(b['address'], b['bytes']) for b in backups] == expected,
            'DRAM backup inventory omits or overlaps a protected range.')
    for item in backups:
        path = Path(item['file'])
        require(path.stat().st_size == item['bytes'] and sha(path) == item['sha256'], 'Saved DRAM changed.')
    for item in backups:
        owner.upload(item['address'], Path(item['file']), expected_sha256=item['sha256'])


# Upload verification by an on-board digest (docs/board-runner.md, "Verify uploads by digest"). The helper
# boot/a733/upload-digest.S runs in the launcher's SRAM reservation, which every plan that executes 0x52000 owns, before
# the launcher, request and report are uploaded there. Its request block starts DIGEST_PARAMS bytes after the helper and
# ends at the end of the reservation; the range count it accepts follows from that size.
LAUNCHER_SRAM = (0x52000, 0x52d80)
DIGEST_SOURCE = 'boot/a733/upload-digest.S'
DIGEST_PARAMS = 0x800
DIGEST_MAGIC, DIGEST_DONE = 0x31474455, 0x454e4f44
DIGEST_MAX_RANGES = (LAUNCHER_SRAM[1] - LAUNCHER_SRAM[0] - DIGEST_PARAMS - 16) // 40
DIGEST_STATUS = {0xe0000001: 'request magic differs', 0xe0000002: 'no SHA-256 instructions (ID_ISAR5.SHA2 is 0)',
                 0xe0000003: 'too many ranges', 0xe0000004: 'range address is not a multiple of 4'}
VERIFY_MODES = ('readback', 'digest')


class UploadFault(ValueError):
    """An upload whose on-board bytes differ from the host's: a transport fault, not an image result."""
    def __init__(self, message, detail):
        super().__init__(message)
        self.detail = detail


def build_upload_digest(out):
    """Assemble the digest helper at the launcher's address; return its flat binary."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm']
    commands = [[str(CLANG), *flags, '-c', str(ROOT/DIGEST_SOURCE), '-o', str(out/'upload-digest.o')],
                [str(LLD), f'-Ttext={LAUNCHER_SRAM[0]:#x}', '-e', '_start', str(out/'upload-digest.o'),
                 '-o', str(out/'upload-digest.elf')],
                ['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', str(out/'upload-digest.elf'), str(out/'upload-digest.bin')]]
    with (out/'upload-digest.log').open('w') as log:
        for command in commands:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    binary = out/'upload-digest.bin'
    require(0 < binary.stat().st_size <= DIGEST_PARAMS, 'The digest helper overlaps its request block.')
    return binary


def upload_verification(plan, mode):
    """Which uploads the digest helper verifies and which are read back, in plan order.

    The helper needs the launcher's SRAM reservation, so the digest path applies to a plan that executes 0x52000 and
    uploads into that reservation; it verifies the DRAM uploads whose address is a multiple of 4 and that no later upload
    overlaps. The uploads into the reservation are written after the helper has run and are read back, as are all uploads
    of any other plan or in readback mode."""
    require(mode in VERIFY_MODES, f'Unknown upload verification {mode!r}.')
    uploads = list(plan.get('uploads', []))
    inside = lambda address: LAUNCHER_SRAM[0] <= address < LAUNCHER_SRAM[1]
    result = {'mode': 'readback', 'digest': [], 'readback': uploads, 'reason': 'readback requested'}
    if mode == 'readback':
        return result
    if plan.get('launch') != ['exe', hex(LAUNCHER_SRAM[0])] or not any(inside(a) for a, _ in uploads):
        return dict(result, reason='the plan does not own the launcher SRAM')
    sizes = [Path(source).stat().st_size for _, source in uploads]
    digest = []
    for index, ((address, source), size) in enumerate(zip(uploads, sizes)):
        later = [(a, n) for (a, _), n in zip(uploads[index+1:], sizes[index+1:])]
        if (not inside(address) and address % 4 == 0 and size > 0 and
                not any(a < address + size and address < a + n for a, n in later)):
            digest.append((address, source))
    if not digest:
        return dict(result, reason='no upload qualifies for the digest')
    return {'mode': 'digest', 'digest': digest, 'readback': [u for u in uploads if u not in digest], 'reason': None}


def digest_request(ranges):
    """The request block for [(address, bytes), ...]: magic, count, status 0, ID_ISAR5 0, the ranges, zeroed digests."""
    require(0 < len(ranges) <= DIGEST_MAX_RANGES, 'Digest request range count is outside the request block.')
    return (struct.pack('<4I', DIGEST_MAGIC, len(ranges), 0, 0) + b''.join(struct.pack('<2I', a, n) for a, n in ranges) +
            bytes(32 * len(ranges)))


def digest_reply(request, reply):
    """(status, ID_ISAR5, [digest, ...]) from the block read back after the helper ran. Every byte the host wrote, other
    than the status, ID_ISAR5 and digest fields the helper fills, must read back unchanged."""
    count = struct.unpack_from('<I', request, 4)[0]
    require(len(reply) == len(request), 'Digest reply is the wrong size.')
    ranges = 16 + 8 * count
    if reply[:8] != request[:8] or reply[16:ranges] != request[16:ranges]:
        raise UploadFault('The digest request block read back differently; the helper result is not trusted.',
                          {'site': 'upload-digest-request', 'expected': request[:ranges].hex(), 'observed': reply[:ranges].hex()})
    status, isar5 = struct.unpack_from('<2I', reply, 8)
    return status, isar5, [reply[ranges + 32 * i:ranges + 32 * i + 32] for i in range(count)]


def write(path, data):
    Path(path).write_bytes(data)
    return Path(path)


def capture(owner, plan, args):
    if plan.get('format') == 'card-fel-recovery':
        return load('capture_card_fel_recovery', ROOT/'tools/card_recovery.py').capture(owner, plan)
    out = owner.evidence
    if plan['format'] == 'npu':
        finite = plan['finite']
        reports = [owner.read(address, size, out/(name+'.bin')) for name,address,size in
                   [('monitor',0x42000000,128),('platform',plan['geometry']['log'],16384),
                    ('receipt',plan['geometry']['receipt'],88)]]
        decoded = finite.validate_reports(*reports,plan)
        if plan['geometry'].get('stopped_capture'):
            finite.stopped.capture(owner.tx, finite.base, plan, out, owner.record, owner.journal, decoded)
        # Validated retirement permits restoration even after numerical mismatch.
        if preserving(args):
            backup = plan['dram_backup']
            require(sha(backup) == plan['dram_backup_sha256'], 'Saved NPU DRAM changed.')
            owner.upload(finite.DRAM[0], backup)
            owner.record['dram_restored'] = True
            owner.journal()
        return {'valid': decoded['status'] == 0, 'payload': decoded['log'].encode(), 'report': decoded}
    if plan['format']=='native-probe':
        probe=load('capture_native_probe',ROOT/'tools/board_probe.py')
        decoded=probe.receipt(owner.read(probe.RECEIPT,128,out/'probe-receipt.bin'),plan)
        payload=owner.read(plan['marker']['payload_address'],decoded['payload_bytes'],out/'probe-payload.bin') if decoded['payload_bytes'] else b''
        require(probe.fnv(payload)==decoded['payload_hash'],'Probe payload checksum differs.')
        return {'valid':decoded['status']==0,'payload':payload,'report':decoded}
    if plan['format'] == 'probe':
        data = owner.read(0x42000000, 16384, out/'return-report.bin')
        values = struct.unpack_from('<16Q', data)
        require(values[3] <= 16384-257, 'Invalid probe log length.')
        payload = data[256:256+values[3]]
        (out/'output.bin').write_bytes(payload)
        return {'valid': values[:3] == (0x534b4552, 5, 0), 'payload': payload,
                'report': {'magic': values[0], 'stage': values[1], 'status': values[2]}}
    if plan['format'] == 'legacy':
        host, geo = plan['host'], plan['geometry']
        report = owner.read(geo['report_address'], geo['report_bytes'], out/'return-report.bin')
        decoded = host.decode_report(report, plan['epoch'])
        raw = owner.read(geo['output_address'], geo['output_bytes'], out/'output-record.bin')
        payload = host.decode_output(raw, plan['epoch'], plan['generation']).encode()
        if 'gpu' in geo:
            raw = owner.read(geo['gpu']['report_address'], 64, out/'gpu-report.bin')
            decoded['gpu'] = host.os_image.gpu_return_rules.decode(raw, plan['epoch'], plan['generation'], require,
                allow_untouched=geo['gpu'].get('untouched_report_version') == 2)
        return {'valid': True, 'payload': payload, 'report': decoded}
    records = plan['return_host']
    geo = plan['geometry']
    reads = [('return-report', geo['report_address'], geo['report_bytes']),
             ('output-record', geo['output_address'], geo['output_bytes'])]
    if 'gpu_receipt' in geo:
        gpu = geo['gpu_receipt']
        reads.append(('gpu-report', gpu['address'], gpu['bytes']))
    if 'thermal' in geo:
        reads.extend(records.thermal_capture.captures(geo['thermal']))
    if 'npu_stop' in geo:
        reads.append(('npu-stop', geo['npu_stop']['address'], geo['npu_stop']['bytes']))
    if 'cpu_rails' in geo:
        reads.append(('cpu-rails', geo['cpu_rails']['address'], geo['cpu_rails']['bytes']))
    for name, place in geo.get('cpu_clock', {}).items():
        reads.append(('cpu-clock-' + name.replace('_', '-'), place['address'], place['bytes']))
    for name, key in (('device-ranges-head', 'device_ranges_head'), ('return-walk', 'return_walk'), ('npu-guard', 'npu_guard'),
                      ('npu-register', 'npu_register'), ('npu-prepare', 'npu_prepare')):
        if key in geo:
            reads.append((name, geo[key]['address'], geo[key]['bytes']))
    if 'radio_result' in geo:
        reads.append(('radio-descriptor', geo['radio_result']['address'], geo['radio_result']['bytes']))
    # Preserve every returned object before decoding any of them. A malformed
    # application result must not hide the native retirement or GPU receipt.
    copied = {name: owner.read(address, size, out/(name + '.bin'))
              for name, address, size in reads}
    require(owner.record.get('normal_return') and not owner.record.get('dram_lost'),
            'DRAM restoration requires a verified normal return.')
    if 'device-ranges-head' in copied:
        # The first page of the published list is in DRAM that the OS chose at run time, so its address comes from the copied head.
        head = struct.unpack('<Q', copied['device-ranges-head'])[0]
        if head:
            records.require_listable_head(head)   # a ValueError naming the head, as every runner requirement
            copied['device-ranges-page'] = owner.read(head, 4096, out/'device-ranges-page.bin')
    if hasattr(records, 'mandatory_return'):
        decoded = records.mandatory_return(copied, geo, plan['epoch'], plan['generation'],
                                           failure_return=bool(owner.record.get('failure_return')))
    else:
        require('npu_stop' not in geo, 'NPU retirement parser is missing.')
        decoded = records.decode_return_report(copied['return-report'], plan['epoch'])
    if hasattr(records, 'decode_published'):
        try:
            decoded.update(records.decode_published(copied, geo))
        except (ValueError, struct.error) as error:     # a malformed publication does not hide the native retirement
            decoded['device_ranges_error'] = str(error)
    owner.record['native_retirement_validated'] = True
    owner.journal()
    try:
        valid, payload = True, b''
        if 'npu_stop' in decoded:
            decoded['npu_capture_sha256'] = hashlib.sha256(copied['npu-stop']).hexdigest()
            try:
                decoded['npu_report'] = records.npu_capture.decode(
                    copied['npu-stop'], plan['epoch'], decoded['npu_stop'].get('generation', plan['generation']),
                    never_started=decoded['npu_stop']['finished'] == 2)
                valid = not (decoded['npu_report']['mismatches'] or decoded['npu_report']['refused'])
                decoded['npu_capture_status'] = 'decoded'
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded.update(npu_capture_status='malformed', npu_error=str(error))
        if 'radio_result' in geo:
            # The record is as long as its scan list: read it whole from where the descriptor says.
            decoded['radio_descriptor_sha256'] = hashlib.sha256(copied['radio-descriptor']).hexdigest()
            try:
                channel = load('radio_result_channel', ROOT/'tools/radio_result_channel.py')
                decoded['radio'], raw_radio = channel.read_record(
                    lambda address, size, name: owner.read(address, size, out/(name + '.bin')),
                    copied['radio-descriptor'], records.RAM)
                decoded['radio_capture_sha256'] = hashlib.sha256(raw_radio).hexdigest()
                decoded['radio_capture_status'] = 'decoded'
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded.update(radio_capture_status='malformed', radio_error=str(error))
        if 'cpu_rails' in geo:
            decoded['cpu_rail_capture_sha256'] = hashlib.sha256(copied['cpu-rails']).hexdigest()
            try:
                decoded['cpu_rails'] = records.decode_rails(copied['cpu-rails'])
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded['cpu_rail_error'] = str(error)
        if 'cpu_clock' in geo:
            try:
                decoded['cpu_clock'] = records.decode_clock(*(copied['cpu-clock-' + name.replace('_', '-')]
                                                              for name in ('record', 'dsu_mhz', 'pattern_bits', 'parked_max_us')))
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded['cpu_clock_error'] = str(error)
        decoded['output_capture_sha256'] = hashlib.sha256(copied['output-record']).hexdigest()
        try:
            info = records.host.image_manifest(args.image)
            if owner.record.get('failure_return'):
                # The image returned after its first failure. The record is
                # the failure result, never an application result.
                valid = False
                try:
                    decoded['failure'], payload = records.host.decode_failure(
                        copied['output-record'], plan['epoch'], plan['generation'], info['manifest']['output_limit'])
                    decoded['output_capture_status'] = 'failure'
                except (ValueError, KeyError, struct.error) as error:
                    decoded.update(output_capture_status='failure-record-absent', output_error=str(error))
            else:
                payload = records.host.decode_result(copied['output-record'], plan['epoch'], plan['generation'],
                                                      info['manifest']['output_limit'])
                decoded['output_capture_status'] = 'decoded'
        except (ValueError, KeyError, struct.error) as error:
            valid = False
            decoded.update(output_capture_status='malformed', output_error=str(error))
        if 'gpu_receipt' in geo:
            decoded['gpu_capture_sha256'] = hashlib.sha256(copied['gpu-report']).hexdigest()
            try:
                decoded['gpu'] = records.gpu_capture.decode_captured(copied['gpu-report'], geo['gpu_receipt'],
                    plan['epoch'], plan['generation'],
                    failure_return=bool(owner.record.get('failure_return') and decoded.get('failure')))
                require(args.kind != 'gpu-return' or decoded['gpu'].get('gpu_bisect') or
                        decoded['gpu'].get('completed_gpu_work'),
                        'GPU kind requires completed GPU work or an image-bound diagnostic receipt.')
                decoded['gpu_capture_status'] = 'decoded'
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded.update(gpu_capture_status='malformed', gpu_error=str(error))
        if 'thermal' in geo:
            decoded['thermal_capture_sha256'] = {name: hashlib.sha256(copied[name]).hexdigest()
                                                 for name, _, _ in records.thermal_capture.captures(geo['thermal'])}
            try:
                decoded['thermal'] = records.thermal_capture.decode(geo['thermal'], copied,
                                                                    plan['epoch'], plan['generation'])
                decoded['thermal_capture_status'] = 'decoded'
                if decoded['thermal'].get('classification') != 'complete':
                    valid = False
                    decoded['thermal_error'] = 'Thermal window is '+str(decoded['thermal'].get('classification'))
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded.update(thermal_capture_status='malformed', thermal_error=str(error))
        else:
            decoded['thermal_capture_status'] = 'absent'
        return {'valid': valid, 'payload': payload, 'report': decoded}
    finally:
        require(plan['preserve_regions'] == preservation_regions(plan),
                'Preservation plan changed after launch.')
        if preserving(args):
            restore_dram(owner, plan['dram_backups'], plan['preserve_regions'])
            owner.record.update(dram_restored=True, dram_dirty=False)
            owner.journal()





def symbol(elf, name):
    """A defined symbol by exact name, including C++ internal-linkage names."""
    found = [s for s in elf.symbols if s['shndx'] != 0 and (s['name'] == name or s['name'].endswith(
        str(len(name)) + name))]
    return found[0] if len(found) == 1 else None


def prepare_legacy(host, image, template, output, live):
    """Board bundle from the ELF, a template bundle's session template and assets."""
    o = host.os_image
    data = Path(image).read_bytes()
    elf = o.os_link.Elf(data)
    require(symbol(elf, 'os_cpu_return_output') is not None, 'Image is not a CPU-return image.')
    watchdog = o.watchdog_marker(elf)
    require(watchdog is not None, 'Image carries no host-armed watchdog marker; a hang would strand the board.')
    raw = bytearray((Path(template) / 'board/session-template.bin').read_bytes())
    # Fresh identities: no record of an earlier run (DRAM result, report)
    # can match this session. The grant record names the same session.
    epoch, generation, lease = (secrets.randbits(62) | 1 for _ in range(3))
    struct.pack_into('<3Q', raw, 24, epoch, generation, lease)
    grant = o.grant_fields(raw)
    if grant and grant['clusters']:
        struct.pack_into('<2Q', raw, len(raw) - o.GRANT_BYTES + 32, epoch, generation)
    findings, derived = load('legacy_state', ROOT/'tools/board_state.py').state_check(live['words'])
    if derived['apb1_hz']:
        struct.pack_into('<2I', raw, 760, derived['apb1'], derived['apb1_hz'])
    if derived['configure_adc_clock'] is not None:
        raw[768] = derived['configure_adc_clock']
    o.session_check(raw, False)
    output.mkdir(parents=True)
    board = output / 'board'
    board.mkdir()
    (board / 'image.elf').write_bytes(data)
    segments, address, identity = o.base.image_parts(board / 'image.elf')
    raw[48:80] = identity
    o.build_launcher(board, len(raw), str(CLANG), str(LLD), watchdog['mode'])
    uploads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            (board / f'segment-{index}.bin').write_bytes(segment['data'])
            uploads.append({'name': f'segment-{index}.bin', 'address': segment['address']})
    (board / 'session.bin').write_bytes(raw)
    uploads.append({'name': 'session.bin', 'address': o.SESSION_ADDRESS})
    for name, value in zip(('request.bin', 'contract.bin'), o.request_bytes(address, identity, raw)):
        (board / name).write_bytes(value)
    (board / 'result-zero.bin').write_bytes(bytes(16384))
    (board / 'report-zero.bin').write_bytes(bytes(128))
    shutil.copytree(Path(template) / 'assets', output / 'assets')
    assets = o.host.validate_assets(output / 'assets')['assets']
    linked = symbol(elf, 'mimus_asset_sha256')
    if linked is not None:
        require(elf.read(linked['value'], 96) == b''.join(bytes.fromhex(a['sha256']) for a in assets),
                'Recognition inputs differ from the hashes linked into the image.')
    names = ['launch.bin', 'session.bin', 'request.bin', 'contract.bin', 'result-zero.bin', 'report-zero.bin',
             'image.elf'] + [u['name'] for u in uploads if u['name'].startswith('segment-')]
    meta = {'schema': 'sunstoneos-dev-bundle-v1', 'image_sha256': hashlib.sha256(data).hexdigest(),
            'build_id': identity.hex(), 'epoch': epoch, 'generation': generation, 'lease': lease,
            'workload': struct.unpack_from('<I', raw, 400)[0], 'segments': uploads,
            'files': {n: sha(board / n) for n in names}, 'template': str(Path(template).resolve()),
            'derived_session': derived, 'state_findings': findings, 'watchdog': watchdog}
    (output/'dev-bundle.json').write_text(json.dumps(meta,indent=2)+'\n')
    return meta


def rail_result(values, log):
    dev = load("board_rail_result", ROOT/"tools/board_state.py")
    result = dev.helper_report({"complete": True, "transport_failed": False, "mode": "kernel",
        "result": dict(zip(("magic","stage","status"), values[:3]))}, log)
    require(result["complete"], "Rail helper refused or did not complete normally: "+result.get("reason", ""))
    return dict(result, boot_clock_domains=3, already_normalized=not result["written"])


class RailRestoreRefused(ValueError):
    """The post-ROM rail helper refused or did not complete, and the rails are not shown to be at the restore target."""

    def __init__(self, message, record):
        super().__init__(message)
        self.record = record


def rail_log_facts(log):
    """The supplies and clock rows the helper printed from its own readbacks: {'supply': {...}, 'rows': {...}}."""
    supply = re.findall(r'^SUPPLY (.+)$', log, re.M)
    fields = dict(x.split('=') for x in supply[0].split()) if len(supply) == 1 else {}
    return {'supply': {k: int(v, 0) for k, v in fields.items()}, 'refused': re.findall(r'^REFUSED (.+)$', log, re.M),
            'operations': len(re.findall(r'^VOLT ', log, re.M))}


def post_rail_refused_at_target(values, log):
    """Record of a refusal that is a verified no-op, or None.

    The post-ROM helper is built to take DCDC5 from the 1000 mV an image left it at to 800 mV, and refuses ("CPU supplies") a
    board whose rails are not at that entry point. A board whose rails are already at the restore target (an image that never
    raised them, such as an NPU loader test) is in the state the helper would have produced. The helper's own readbacks show
    it: it refused before any operation (no VOLT line, no COMPLETE), it printed DCDC5 and DCDC3 at 800 mV from the PMIC, and
    the three CPU clock rows it read are the boot words. Nothing else is accepted as a no-op."""
    facts = rail_log_facts(log)
    supply = facts['supply']
    if not (values[0] == 0x534b4552 and facts['refused'] == ['board bundle 33: CPU supplies'] and not facts['operations'] and
            supply.get('dcdc5_uv') == 800000 and supply.get('dcdc3_uv') == 800000 and supply.get('bin') == 4 and
            supply.get('dcxo_hz') == 26000000 and 'COMPLETE' not in log and not re.search(r'^(?:FAIL|STOPPED)\b', log, re.M)):
        return None
    for name, pll, mux, mhz in (('CPU-L', 0xf8802600, 0x03010000, 494), ('CPU-B', 0xf8802700, 0x03000000, 1014),
                                ('DSU', 0xf8802600, 0x03010105, 494)):
        rows = re.findall(r'^' + name + r' before (.+)$', log, re.M)
        if len(rows) != 1:
            return None
        f = {k: int(v, 0) for k, v in (x.split('=') for x in rows[0].split())}
        if not (f['pll'] == pll and f['mux'] == mux and f['mhz'] == mhz):
            return None
    return {'outcome': 'already-at-target', 'helper': 'refused: CPU supplies (entry point differs from the restore target)',
            'dcdc5_uv': 800000, 'dcdc3_uv': 800000, 'written': False}


def post_rail_outcome(values, log):
    """Restored, or a verified no-op; otherwise RailRestoreRefused with the helper's facts."""
    try:
        return dict(post_rail_result(values, log), outcome='restored')
    except ValueError as error:
        noop = post_rail_refused_at_target(values, log)
        if noop:
            return noop
        raise RailRestoreRefused(str(error), {'outcome': 'refused', 'reason': str(error), 'facts': rail_log_facts(log)})


def post_rail_result(values, log):
    """The approved DCDC5-only helper requires DCDC3 already at 800 mV."""
    followup = load('board_post_rail_result', ROOT/'apps/os/cpu-return-followup.py')
    return followup.helper_result({'complete': True, 'transport_failed': False,
        'automatic_access': True, 'mode': 'kernel',
        'result': dict(zip(('magic', 'stage', 'status'), values[:3]))}, log)
