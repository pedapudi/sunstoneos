#!/usr/bin/env python3
"""Build and check the typed board session of a SunstoneOS image, with no device access and no old queue entry.

    session.py build IMAGE --out DIR [--manifest M] [--npu-declaration D] [--input FILE ...]
                             [--decoder FILE] [--device-profile usb-host|radio-scan]
    session.py check DIR_OR_SESSION [--image IMAGE] [--admit [--lifetime returning|resident]] [--json]

build writes the complete session of IMAGE to DIR/template/session.bin, the input files to DIR/template/input-N.bin, and DIR/session.json, which
records what each field came from. The template is what `board.py submit --template DIR/template` takes; the runner draws a fresh epoch,
generation and lease and re-derives the live thermal fields when it launches. --decoder copies the application's decoder (for example the one
apps/npu-graphs/host.py writes) to DIR/decoder.py, the file that `board.py submit --decoder` takes. Every field is read from the image or from tools/session-facts.json:

  image identity, manifest digest   the image's build ID (tools/check-a733-smp.py) and its sunstone_manifest section
  devices, flags, workload          the device classes the manifest declares (required or optional) that the session can grant: the GPU, when the
                                    image links the GPU service. It has no session record, so the bit and the image's service are the whole
                                    grant. --device-profile radio-scan selects the passive scan producer and its pinned sources.
                                    USB host selection requires a reviewed profile that is not yet pinned. Other classes need facts
                                    this file does not pin (see DEVICE_FACTS); an optional one is left out
                                    and named in session.json, a required one is an error that names what is missing
  which service records             the thermal record always; the CPU clock grant only with --clock-grant (and --perf), for a manifest
                                    that declares cpu.clock_grants (cpu.perf); the NPU service when the application declares the NPU
                                    (ss_application_npu; the NPU is a service, not a device bit); a DMA service is refused
  NPU flags                         the image's ss_application_npu (the manifest's npu.observe)
  NPU pool request                  --npu-pool BASE:BYTES (default 0:0: the allocator chooses)
  NPU declaration                   the image's ss_application_npu (the manifest's npu section); --npu-declaration D (a manifest with an
                                    npu section, or a bare slots/in_flight/graphs/links object) and --manifest M must agree with it
  inputs                            --input FILE for each manifest input region, in order (their digests go into the session)
  board and evidence facts          tools/session-facts.json: the board prefix, thermal and grant records and the NPU authority words of
                                    the profile the image needs ("npu" for an application that declares the NPU, else "control")
  passive radio scan               --device-profile radio-scan requires the radio declaration and linked scan service. The existing
                                    runner producer supplies the profile and four verified firmware uploads. session.json records
                                    their addresses, sizes and hashes; no association input is read

A field that none of these determines is an error that names it; nothing is guessed. The encoder and decoder are apps/os/host.py, the code the
runner (tools/board_images.py), the producer (apps/os/prepare.py) and the host admission use. check decodes a session with that decoder and
prints its fields; with --image it also compares them with the image, and with --admit it runs the image's own EL3 admission on the session
(tools/host_admission.py) and prints the verdict.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
FACTS = ROOT / 'tools/session-facts.json'
SCHEMA = 'sunstone-session-v1'


class SessionError(ValueError):
    """A field of the session cannot be determined or does not hold; the message names it."""


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def records():
    """apps/os/host.py: the session encoder and decoder that the runner, the producer and the host admission share."""
    return load('session_tool_records', ROOT / 'apps/os/host.py')


# struct a733_board_session (include/a733_board_application.h), os_session's thermal tail (apps/os/os.h), a733_cpu_clock_grant_record
# (include/a733_cpu_clock.h) and os_npu_session (apps/os/npu-session.h), little endian, as (name, struct format).
PREFIX = [('magic', 'Q'), ('version', 'Q'), ('bytes', 'Q'), ('epoch', 'Q'), ('generation', 'Q'), ('lease', 'Q'),
          ('image', '32s'), ('qualification', '32s'), ('minimum_reload_us', 'Q'), ('maximum_recovery_us', 'Q'), ('guard_us', 'Q'),
          ('reset_by_us', 'Q'), ('rtc_control', 'I'), ('rtc_divider', 'I'), ('rtc_source', 'I'), ('reserved0', 'I'),
          ('deadline_us', 'Q'), ('reload_before_ticks', 'Q'), ('reload_after_ticks', 'Q'), ('topology', 'Q'), ('cold_master', 'Q'),
          ('memory_authority', 'Q'), ('recovery_authority', 'Q'), ('wiring', '32s'), ('memory', '32s'), ('recovery', '32s'),
          ('sid_authority', 'Q'), ('sid_evidence', '32s'), ('radio_profile_address', 'Q'), ('radio_profile_bytes', 'Q'),
          ('radio_profile_digest', '32s'), ('workload', 'I'), ('usb_topology', 'I'), ('latch_identity', 'Q'), ('latch_owner', 'Q'),
          ('latch_known_mask', 'I'), ('latch_value', 'I'), ('latch_m_identity', 'Q'), ('latch_m_owner', 'Q'),
          ('latch_m_known_mask', 'I'), ('latch_m_value', 'I'), ('pmic_owner', 'Q'), ('usb0_owner', 'Q'), ('typec_owner', 'Q'),
          ('usb2_owner', 'Q'), ('fabric', '3I'), ('fabric_hz', '3I'), ('usb_supply_uv', '5I'), ('usb0_cold_reference', 'I'),
          ('usb2_revision', 'I'), ('pck_client', 'I'), ('source_min_mv', 'I'), ('source_max_mv', 'I'), ('source_ma', 'I'),
          ('zero_threshold_max_mv', 'I'), ('fusb_device_id', 'I'), ('pmic_pins', '6s'), ('typec_pins', '12s'), ('reserved', '6s'),
          ('_pad', '4x')]
THERMAL = [('magic', 'Q'), ('version', 'Q'), ('bytes', 'Q'), ('thermal_owner', 'Q'), ('thermal_authority', 'Q'),
           ('thermal_initial', '32s'), ('thermal_electrical', '32s'), ('thermal_clocks', '32s'), ('thermal_supervision', '32s'),
           ('apb1', 'I'), ('apb1_hz', 'I'), ('configure_adc_clock', 'B'), ('alarm_mask', 'B'), ('critical_mask', 'B'),
           ('reserved0', 'B'), ('limits', '15H'), ('reserved', '2s'), ('cpu_critical_millicelsius', 'I')]
GRANT = [('magic', 'Q'), ('version', 'Q'), ('bytes', 'Q'), ('owner', 'Q'), ('epoch', 'Q'), ('generation', 'Q'), ('clusters', 'I'),
         ('max_mhz', '2I'), ('changes', 'I'), ('perf', 'I'), ('reserved', '28s')]
NPU = [('magic', 'Q'), ('version', 'Q'), ('bytes', 'Q'), ('owner', 'Q'), ('command_admission', 'Q'), ('supervisor', 'Q'),
       ('pck_client', 'I'), ('flags', 'I'), ('reserved', 'I'), ('graph_region', 'I'), ('pool_base', 'Q'), ('pool_bytes', 'Q'),
       ('admission', '32s'), ('clock', '32s'), ('power', '32s'), ('supervision', '32s'), ('ddr', '32s'),
       ('slots', 'I'), ('in_flight', 'I'), ('graphs', 'I'), ('links', 'I'), ('graph_bytes', 'I'), ('tensor_bytes', 'I')]
LAYOUTS = {'prefix': (PREFIX, 592), 'thermal': (THERMAL, 216), 'cpu_grant': (GRANT, 96), 'npu': (NPU, 264)}
SESSION_MAGIC, SESSION_VERSION = 0x4153373342535332, 2          # the board prefix (apps/board/prepare.py SESSION_MAGIC)
GRANT_MAGIC = 0x4153373343505531                                # A733_CPU_CLOCK_GRANT_MAGIC
OS_MAGIC = 0x415337334f534131                                   # the thermal record's magic (A733_OS_SESSION_MAGIC)
# What each record takes from the image or the build rather than from the facts file.
DERIVED = {'prefix': {'magic', 'version', 'bytes', 'epoch', 'generation', 'lease', 'image', '_pad'},
           'thermal': {'magic', 'version', 'bytes'},
           'cpu_grant': {'magic', 'version', 'bytes', 'epoch', 'generation', 'clusters', 'max_mhz', 'perf', 'reserved'},
           'npu': {'magic', 'version', 'bytes', 'flags', 'reserved', 'graph_region', 'pool_base', 'pool_bytes',
                   'slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'}}
# Symbols that tell which services a session grants: every OS image links the NPU service, and an application that declares
# the NPU in its manifest records the declaration (tools/sunstone-manifest.py).
SYMBOLS = {'npu': 'ss_application_npu'}
REVIEWED = ROOT / 'tools/cpu-reviewed-points.json'
# Device classes a session can grant, and the symbol that shows the image links the service behind each. The GPU has no session
# extent; the decoder (apps/os/host.py) and the EL3 admission (apps/os/session.c) check only the bit, the workload word of the board
# prefix (2 for a session without USB storage, which the control profile carries) and, in the image, that the GPU service is linked.
GRANTABLE = {'gpu': 'os_gpu_queue_start_entry', 'usb-device': 'board_usb0_task'}
DEVICE_PROFILES = {
    'usb-host': ('usb-keyboard', ('os_usb_service_init', 'os_usb_quiesce_start')),
    'radio-scan': ('radio', ('os_radio_start', 'os_radio_result_descriptor')),
}
RADIO_SCAN_SOURCES = ('tools/radio_scan_prepare.py', 'docs/designs/radio-vendor-power-sequence.md')
# What the other classes need and the facts file does not pin: a grant of any of them is refused or left out.
DEVICE_FACTS = {'usb-storage': 'the USB workload facts (workload 0, latch, Type-C and USB2 owners)',
                'usb-keyboard': 'the USB workload facts (workload 0 or 4, latch, Type-C and USB2 owners)',
                'audio-i2s': 'the audio extent and the DMA0 authority',
                'audio-headset': 'the Bluetooth profile digest and the headset extent',
                'usb-mouse': 'the USB class services, which the returning lifetime excludes',
                'usb-serial': 'the USB class services, which the returning lifetime excludes',
                'usb-camera': 'the USB class services, which the returning lifetime excludes',
                'radio': 'an explicit scan-only profile or association admission'}


def devices_for(manifest, symbols, names, device_profile=None):
    """(granted bits, {name: reason} of the optional classes left out) for a manifest and the symbols of its image.

    names maps each class to its bit (tools/sunstone-manifest.py DEVICES). A class the manifest does not declare is not granted.
    A declared class is granted when the facts allow it and the image links its service; a required class that cannot be granted
    is an error, an optional one is reported."""
    declared = manifest['devices_required'] | manifest['devices_optional']
    selected = DEVICE_PROFILES.get(device_profile)
    if device_profile is not None and selected is None:
        raise SessionError('device profile: unsupported selection %s' % device_profile)
    if selected and not declared & names[selected[0]]:
        raise SessionError('device profile: %s requires a declared %s device' % (device_profile, selected[0]))
    granted, left_out = 0, {}
    for name, bit in names.items():
        if not declared & bit:
            continue
        required_symbols = selected[1] if selected and name == selected[0] else (GRANTABLE[name],) if name in GRANTABLE else ()
        missing = [symbol for symbol in required_symbols if symbol not in symbols]
        if required_symbols and not missing:
            granted |= bit
            continue
        reason = ('the image links no %s service (symbol %s)' % (name, ', '.join(missing)) if required_symbols
                  else 'tools/session-facts.json pins no facts for %s' % DEVICE_FACTS.get(name, 'it'))
        if manifest['devices_required'] & bit:
            raise SessionError('devices: the manifest requires %s and %s' % (name, reason))
        left_out[name] = reason
    return granted, left_out


# Services whose records the facts do not pin, refused when the image links them. Every image links the I2S service and
# the radio station; a session grants them only with the audio-i2s or radio device, which devices_for leaves out or refuses.
UNSUPPORTED = {'os_dma_profile': 'DMA service record and its physical authority'}


def quiesce_actors(plan, manifest=None):
    """Owner names required by the session records, granted devices and card inputs.

    This is an inventory, not launch admission. A synthetic plan can cover an
    OS grant whose hardware facts the session builder cannot yet supply.
    Optional manifest devices enter the inventory only when actually granted.
    """
    tool = load('session_quiesce_manifest', ROOT / 'tools/sunstone-manifest.py')
    grants = {row[0] for row in tool.os_service_rows('OS_GRANT_ROWS')}
    by_record = {extent.removeprefix('SS_EXTENT_').lower(): grant
                 for grant, extent, *_ in tool.os_service_rows('OS_RECORD_ROWS')}
    by_record.update({name.lower(): name for name in grants})
    actors = {'THERMAL_OWNER', 'PMIC_OWNER', 'TERMINAL_OWNER'}
    for record in plan.get('services', ()):
        if record == 'thermal':
            continue
        if record not in by_record:
            raise SessionError('quiesce: no service grant for record %s' % record)
        actors.add(by_record[record])
    device_owners = {'audio-i2s': 'AUDIO', 'audio-headset': 'HEADSET', 'usb-device': 'USB0'}
    devices = plan.get('devices', 0)
    known_devices = 0
    for bit in tool.DEVICES.values():
        known_devices |= bit
    if devices & ~known_devices:
        raise SessionError('quiesce: unknown granted device bits %#x' % (devices & ~known_devices))
    for name, bit in tool.DEVICES.items():
        if not devices & bit:
            continue
        owner = device_owners.get(name, 'USB' if name.startswith('usb-') else name.upper().replace('-', '_'))
        actors.add(owner)
    workload = plan.get('workload', plan.get('prefix', {}).get('workload', 2))
    if workload in (0, 1, 4):
        actors.add('USB')
    if workload in (1, 3):
        actors.add('RADIO')
    manifest = manifest if manifest is not None else plan.get('manifest', {})
    if (plan.get('storage') or {'os_sd_owner_task', 'os_sd_quiesce_register'} & set(plan.get('symbols', {})) or
            any(row.get('source_bytes', 0) for row in manifest.get('large_regions', ()))):
        actors.add('STORAGE')
    return sorted(actors)


def width(fmt):
    return struct.calcsize('<' + fmt)


for _name, (_layout, _size) in LAYOUTS.items():
    assert sum(width(f) for _, f in _layout) == _size, _name


def offset(kind, name):
    """The byte offset of field NAME in record KIND, from its layout above."""
    at = 0
    for field, fmt in LAYOUTS[kind][0]:
        if field == name:
            return at
        at += width(fmt)
    raise KeyError('%s has no field %s' % (kind, name))


def count(fmt):
    return int(fmt[:-1]) if fmt[:-1].isdigit() and fmt[-1] != 's' and fmt[-1] != 'x' else 1


def pack(kind, values):
    """The bytes of one record from a dictionary of named values; a missing or unexpected name is an error naming it."""
    layout, size = LAYOUTS[kind]
    names = {name for name, fmt in layout if not name.startswith('_')}
    for name in sorted(names - set(values)):
        raise SessionError('%s field %s is not determined: no source gives it a value' % (kind, name))
    for name in sorted(set(values) - names):
        raise SessionError('%s has no field %s' % (kind, name))
    out = bytearray()
    for name, fmt in layout:
        if name.startswith('_'):
            out += bytes(width(fmt))
            continue
        value = values[name]
        if fmt[-1] == 's':
            data = bytes.fromhex(value) if isinstance(value, str) else bytes(value)
            if len(data) != width(fmt):
                raise SessionError('%s field %s is %d bytes, not %d' % (kind, name, len(data), width(fmt)))
            out += data
        else:
            items = value if isinstance(value, (list, tuple)) else [value]
            if len(items) != count(fmt):
                raise SessionError('%s field %s takes %d values' % (kind, name, count(fmt)))
            try:
                out += struct.pack('<%d%s' % (count(fmt), fmt[-1]), *items)
            except struct.error as error:
                raise SessionError('%s field %s: %s' % (kind, name, error)) from None
    assert len(out) == size
    return bytes(out)


def unpack(kind, raw):
    """The named values of one record; byte fields as hex strings, arrays as lists."""
    layout, size = LAYOUTS[kind]
    if len(raw) != size:
        raise SessionError('%s record is %d bytes, not %d' % (kind, len(raw), size))
    result, at = {}, 0
    for name, fmt in layout:
        n = width(fmt)
        if not name.startswith('_'):
            if fmt[-1] == 's':
                result[name] = bytes(raw[at:at + n]).hex()
            else:
                values = struct.unpack_from('<%d%s' % (count(fmt), fmt[-1]), raw, at)
                result[name] = list(values) if count(fmt) > 1 else values[0]
        at += n
    return result


def facts(path=FACTS):
    """tools/session-facts.json: {'profiles': {name: {record: {field: value}}}}, each record holding exactly the fields not derived."""
    data = json.loads(Path(path).read_text())
    if data.get('schema') != 'sunstone-session-facts-v1':
        raise SessionError('%s is not a session facts file' % path)
    return data


def profile_values(profile, kind, derived):
    """The full value dictionary of a record: the profile's facts and the derived fields, refusing an overlap."""
    given = dict(profile.get(kind, {}))
    clash = set(given) & set(derived)
    if clash:
        raise SessionError('%s fields %s are derived from the image, not facts' % (kind, ', '.join(sorted(clash))))
    return {**given, **derived}


def read_symbols(image):
    """{name: (address, size)} of the defined symbols and an ELF reader for their file bytes."""
    from elftools.elf.elffile import ELFFile
    elf = ELFFile(open(image, 'rb'))
    table = elf.get_section_by_name('.symtab')
    if table is None:
        raise SessionError('%s has no symbol table' % image)
    return {s.name: (int(s['st_value']), int(s['st_size'])) for s in table.iter_symbols() if s['st_shndx'] != 'SHN_UNDEF'}, elf


def symbol_bytes(elf, symbols, name):
    address, size = symbols[name]
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD' and segment['p_vaddr'] <= address and address + size <= segment['p_vaddr'] + segment['p_filesz']:
            offset = address - segment['p_vaddr']
            return segment.data()[offset:offset + size]
    raise SessionError('symbol %s has no bytes in the image file' % name)


def declaration_of(path):
    """(slots, in_flight, graphs, links, graph_bytes, tensor_bytes) of a manifest's npu section or a bare declaration object."""
    raw = json.loads(Path(path).read_text())
    if 'npu' in raw:
        manifest = load('session_tool_manifest', ROOT / 'tools/sunstone-manifest.py')
        n = manifest.load(path)['npu']
    else:
        extra = set(raw) - {'slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'}
        if extra:
            raise SessionError('npu declaration %s has unknown keys %s' % (path, sorted(extra)))
        n = {k: raw.get(k, 0) for k in ('slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes')}
    values = tuple(n[k] for k in ('slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'))
    if not (values[0] and values[1] and values[2]) or not all(type(v) is int and 0 <= v < 1 << 32 for v in values):
        raise SessionError('npu declaration needs slots, in_flight and graphs of at least 1, each below 2^32')
    return values


def describe(image, manifest_path=None, npu_declaration=None, host=None, clock_grant=False, perf=None, device_profile=None):
    """Everything about IMAGE that the session depends on, or a SessionError naming the field that cannot be determined."""
    host = host or records()
    image = Path(image)
    info = host.image_manifest(image)
    manifest = info['manifest']
    if manifest is None:
        raise SessionError('manifest: %s has no sunstone_manifest section, so it is not a typed-session OS image' % image)
    symbols, elf = read_symbols(image)
    for name, what in UNSUPPORTED.items():
        if name in symbols:
            raise SessionError('services: the image links %s (symbol %s), and tools/session-facts.json pins no facts for its %s'
                               % (name.split('_')[1], name, what))
    if 'ss_result_record' not in symbols or 'os_cpu_return_watchdog' not in symbols:
        raise SessionError('image: %s is not a returning OS image (no ss_result_record or os_cpu_return_watchdog)' % image)
    declared = None
    if manifest_path:
        tool = load('session_tool_manifest', ROOT / 'tools/sunstone-manifest.py')
        packed = tool.pack(tool.assign(tool.load(manifest_path)))
        if packed != info['section']:
            raise SessionError('manifest: %s does not pack to the manifest section of %s' % (manifest_path, image))
        declared = tool.load(manifest_path).get('npu')
    names = load('session_tool_devices', ROOT / 'tools/sunstone-manifest.py').DEVICES
    granted, left_out = devices_for(manifest, symbols, names, device_profile)
    plan = {'image': str(image), 'manifest': manifest, 'manifest_sha256': info['sha256'], 'devices': granted, 'devices_left_out': left_out,
            'flags': host.BIND_SID, 'services': ['thermal'], 'symbols': symbols, 'elf': elf}
    if clock_grant or perf:
        # A CPU clock grant is written only on request, and only for an application whose manifest declares that it may use
        # one (and perf). The maxima are the reviewed points; the runner's approval policy sees the record.
        if not manifest['clock_use'] & 1:
            raise SessionError('clock grant: the manifest of %s does not declare cpu.clock_grants' % manifest['name'])
        if perf and not manifest['clock_use'] & 2:
            raise SessionError('perf: the manifest of %s does not declare cpu.perf' % manifest['name'])
        plan['services'].append('cpu_grant')
        plan['perf'] = {'high': 1, 'low': 2}[perf] if perf else 0
    if SYMBOLS['npu'] in symbols:
        plan['services'].append('npu')
        linked = struct.unpack('<8I', symbol_bytes(elf, symbols, SYMBOLS['npu']))
        plan['npu_declaration'] = linked[:6]
        plan['npu_declaration_from'] = '%s (%s)' % (image, SYMBOLS['npu'])
        plan['npu_graph_region'] = linked[6]
        given = [(str(npu_declaration), declaration_of(npu_declaration))] if npu_declaration else []
        if declared:
            given.append((str(manifest_path), tuple(declared[k] for k in ('slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'))))
        for source, values in given:
            if values != linked[:6]:
                raise SessionError('npu declaration: %s declares %s and the image %s' % (source, values, linked[:6]))
        plan['npu_flags'] = linked[7]
    elif npu_declaration or declared:
        raise SessionError('npu declaration: the application of %s declares no NPU, so its session grants none' % image)
    plan['profile'] = 'npu' if 'npu' in plan['services'] else 'control'
    if device_profile and granted & names[DEVICE_PROFILES[device_profile][0]]:
        if 'npu' in plan['services']:
            raise SessionError('device profile: %s has no reviewed NPU combination' % device_profile)
        plan['device_profile'] = device_profile
    return plan


def input_values(plan, inputs, host):
    regions = host.regions(plan['manifest'], (1,))
    if len(inputs) != len(regions):
        raise SessionError('inputs: the manifest declares %d input regions and %d --input files were given (%s)'
                           % (len(regions), len(inputs), ', '.join('%#x:%d' % (r['address'], r['bytes']) for r in regions) or 'none'))
    return [Path(p).read_bytes() for p in inputs]


def device_profile_facts(plan, data):
    """Select the board facts and validate the sources of an explicit device profile."""
    name = plan.get('device_profile')
    if not name:
        return {}
    selected = data.get('device_profiles', {}).get(name)
    if not selected:
        raise SessionError('device profile: tools/session-facts.json pins no reviewed facts for %s' % name)
    expected = 'control' if name == 'radio-scan' else 'usb-host'
    if selected.get('board_profile') != expected:
        raise SessionError('device profile: %s requires board profile %s' % (name, expected))
    if name == 'radio-scan':
        sources = selected.get('sources', {})
        if set(sources) != set(RADIO_SCAN_SOURCES):
            raise SessionError('radio-scan: the producer and reference source hashes are required')
        for source in RADIO_SCAN_SOURCES:
            if hashlib.sha256((ROOT / source).read_bytes()).hexdigest() != sources[source]:
                raise SessionError('radio-scan: source hash differs for %s' % source)
    plan['profile'] = expected
    return selected


def build_session(plan, inputs, fact_file=FACTS, host=None, entropy=secrets.randbits):
    """The session bytes for a described image, and the identity words it carries."""
    host = host or records()
    base = load('session_tool_image', ROOT / 'tools/check-a733-smp.py')
    _, address, identity = base.image_parts(Path(plan['image']))
    data = facts(fact_file)
    selected = device_profile_facts(plan, data)
    profile = data['profiles'].get(plan['profile'])
    if profile is None:
        raise SessionError('profile: tools/session-facts.json has no profile %s' % plan['profile'])
    epoch, generation, lease = [entropy(62) | 1 for _ in range(3)]
    prefix = pack('prefix', profile_values(profile, 'prefix', {
        'magic': SESSION_MAGIC, 'version': SESSION_VERSION, 'bytes': 592, 'epoch': epoch, 'generation': generation,
        'lease': lease, 'image': identity}))
    if plan.get('device_profile') == 'radio-scan':
        scan = load('session_tool_radio_scan', ROOT / RADIO_SCAN_SOURCES[0])
        prefix = bytearray(prefix)
        try:
            _, plan['uploads'] = scan.apply(prefix, bytes.fromhex(selected['sources'][RADIO_SCAN_SOURCES[1]]), stage_limit=0)
        except (OSError, ValueError) as error:
            raise SessionError('radio-scan: %s' % error) from None
        prefix = bytes(prefix)
        plan['device_profile_sources'] = selected['sources']
    services = {}
    size = 592 + sum(LAYOUTS[k][1] for k in plan['services'])
    thermal = profile_values(profile, 'thermal', {'magic': OS_MAGIC, 'version': 1, 'bytes': size})
    services[host.THERMAL] = pack('thermal', thermal)
    if 'cpu_grant' in plan['services']:
        points = json.loads(REVIEWED.read_text())['points']
        grant = profile_values(profile, 'cpu_grant', {'magic': GRANT_MAGIC, 'version': 1, 'bytes': 96,
                                                      'epoch': epoch, 'generation': generation, 'clusters': 3,
                                                      'max_mhz': [points['cpu-l'][0], points['cpu-b'][0]],
                                                      'perf': plan.get('perf', 0), 'reserved': bytes(28)})
        if not grant['clusters']:
            grant['epoch'] = grant['generation'] = 0       # a record that grants nothing names no session
        services[host.CPU_GRANT] = pack('cpu_grant', grant)
    if 'npu' in plan['services']:
        slots, in_flight, graphs, links, graph_bytes, tensor_bytes = plan['npu_declaration']
        npu = profile_values(profile, 'npu', {
            'magic': host.SERVICE_MAGIC[host.NPU], 'version': host.SERVICE_VERSION[host.NPU], 'bytes': host.SERVICE_BYTES[host.NPU],
            'flags': plan['npu_flags'], 'reserved': 0, 'graph_region': plan['npu_graph_region'], 'pool_base': plan['npu_pool_request'][0], 'pool_bytes': plan['npu_pool_request'][1],
            'slots': slots, 'in_flight': in_flight, 'graphs': graphs, 'links': links,
            'graph_bytes': graph_bytes, 'tensor_bytes': tensor_bytes})
        services[host.NPU] = pack('npu', npu)
    ordered = {k: services[k] for k in (host.THERMAL, host.CPU_GRANT, host.NPU) if k in services}
    try:
        raw = host.session_bytes(prefix, ordered, plan['manifest'], plan['manifest_sha256'], plan['devices'], plan['flags'], inputs)
    except ValueError as error:
        raise SessionError('session: the decoder refuses it: %s' % error) from None
    return raw, {'epoch': epoch, 'generation': generation, 'lease': lease, 'address': address, 'identity': identity.hex()}


def build(image, out, manifest=None, npu_declaration=None, inputs=(), decoder=None, fact_file=FACTS, npu_pool=(0, 0),
          clock_grant=False, perf=None, device_profile=None):
    """Write OUT/template/session.bin, input files and session.json; returns the record."""
    host = records()
    plan = describe(image, manifest, npu_declaration, host, clock_grant, perf, device_profile)
    plan['npu_pool_request'] = tuple(npu_pool)
    values = input_values(plan, inputs, host)
    raw, ids = build_session(plan, values, fact_file, host)
    out = Path(out)
    template = out / 'template'
    template.mkdir(parents=True, exist_ok=True)
    (template / 'session.bin').write_bytes(raw)
    for number, data in enumerate(values):
        (template / ('input-%d.bin' % number)).write_bytes(data)
    record = {'schema': SCHEMA, 'image': str(Path(image).resolve()), 'image_sha256': hashlib.sha256(Path(image).read_bytes()).hexdigest(),
              'manifest_sha256': plan['manifest_sha256'], 'application': plan['manifest']['name'], 'profile': plan['profile'],
              'facts': str(Path(fact_file).resolve().relative_to(ROOT)) if Path(fact_file).resolve().is_relative_to(ROOT) else str(fact_file),
              'facts_sha256': hashlib.sha256(Path(fact_file).read_bytes()).hexdigest(),
              'services': plan['services'], 'quiesce_actors': quiesce_actors(plan), 'devices': plan['devices'], 'devices_left_out': plan['devices_left_out'], 'flags': plan['flags'], 'session_bytes': len(raw),
              'session_sha256': hashlib.sha256(raw).hexdigest(), **ids,
              'inputs': [{'file': 'template/input-%d.bin' % i, 'bytes': len(v), 'sha256': hashlib.sha256(v).hexdigest()} for i, v in enumerate(values)]}
    if 'npu' in plan['services']:
        record['npu_declaration'] = dict(zip(('slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'), plan['npu_declaration']))
        record['npu_declaration_from'] = plan['npu_declaration_from']
    if plan.get('device_profile'):
        record['device_profile'] = plan['device_profile']
        record['device_profile_sources'] = plan.get('device_profile_sources', {})
    if plan.get('uploads'):
        record['uploads'] = []
        for address, name, data in plan['uploads']:
            (template / name).write_bytes(data)
            record['uploads'].append({'file': 'template/' + name, 'address': address, 'bytes': len(data),
                                      'sha256': hashlib.sha256(data).hexdigest()})
    for name, source, target in (('decoder', decoder, 'decoder.py'),):
        if source:
            shutil.copyfile(source, out / target)
            record[name] = {'file': target, 'source': str(Path(source).resolve()), 'sha256': hashlib.sha256((out / target).read_bytes()).hexdigest()}
    (out / 'session.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def find_session(path):
    path = Path(path)
    for candidate in (path, path / 'template/session.bin', path / 'session.bin'):
        if candidate.is_file() and candidate.suffix == '.bin':
            return candidate
    raise SessionError('no session.bin at %s (looked in template/ and the directory itself)' % path)


def inspect(raw, image=None, host=None):
    """Decode a session and return its fields; with an image, also compare them with it and list the disagreements."""
    host = host or records()
    raw = bytes(raw)
    info = host.image_manifest(image) if image else {'manifest': None, 'sha256': None}
    try:
        decoded = host.session_decode(raw, info['manifest'] if image else None, info['sha256'] if image else None)
    except ValueError as error:
        raise SessionError('decode: %s' % error) from None
    prefix = unpack('prefix', raw[:592])
    report = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(), 'prefix': {k: prefix[k] for k in (
        'epoch', 'generation', 'lease', 'image', 'qualification', 'workload', 'usb_topology')},
        'manifest_sha256': decoded['manifest_sha256'], 'devices': decoded['devices'], 'flags': decoded['flags'], 'extents': {}}
    problems = []
    names = {host.THERMAL: 'thermal', host.CPU_GRANT: 'cpu_grant', host.NPU: 'npu'}
    for kind, data in decoded['extents'].items():
        if kind in names:
            report['extents'][names[kind]] = unpack(names[kind], data)
        elif kind == host.INPUTS:
            report['extents']['inputs'] = [{'bytes': n, 'sha256': d.hex()} for n, d in struct.iter_unpack(host.INPUT.format, data)]
        else:
            report['extents']['kind %d' % kind] = {'bytes': len(data)}
    grant = report['extents'].get('cpu_grant')
    if grant and grant['clusters'] and (grant['epoch'], grant['generation']) != (prefix['epoch'], prefix['generation']):
        problems.append('cpu_grant: epoch and generation differ from the board prefix')
    if image:
        symbols, elf = read_symbols(image)
        base = load('session_tool_image', ROOT / 'tools/check-a733-smp.py')
        _, address, identity = base.image_parts(Path(image))
        report['address'] = address
        if bytes.fromhex(prefix['image']) != identity:
            problems.append('prefix image: %s differs from the image build ID %s' % (prefix['image'], identity.hex()))
        npu = report['extents'].get('npu')
        if SYMBOLS['npu'] in symbols and not npu:
            problems.append('npu: the application declares the NPU and the session has no NPU record')
        if npu and SYMBOLS['npu'] not in symbols:
            problems.append('npu: the session has an NPU record and the application declares no NPU')
        if npu and SYMBOLS['npu'] in symbols:
            if npu['flags'] != struct.unpack('<8I', symbol_bytes(elf, symbols, SYMBOLS['npu']))[7]:
                problems.append('npu flags: %#x differ from those the application declares (ss_application_npu)' % npu['flags'])
            if bool(npu['pool_base']) != bool(npu['pool_bytes']):
                problems.append('npu pool request: base %#x and bytes %#x are both set or both zero' % (npu['pool_base'], npu['pool_bytes']))
        manifest = host.image_manifest(image)['manifest'] or {}
        if grant and not manifest.get('clock_use', 0) & 1:
            problems.append('cpu_grant: the record is present and the manifest does not declare cpu.clock_grants')
        if grant and grant['perf'] and not manifest.get('clock_use', 0) & 2:
            problems.append('cpu_grant: the record carries perf and the manifest does not declare cpu.perf')
    report['problems'] = problems
    return report


def admit(image, raw, build=None, lifetime='returning'):
    """The image's own EL3 admission on the session and the launch contract the runner would send; returns its verdict dictionary.

    The admission code is compiled from the image's own source tree, the one its CMake build directory was configured from
    (tools/host_admission.py refuses a tree that has moved or an image that is not that build's). lifetime selects the contract the
    admission is run with: an image that admits only the resident lifetime is refused under the returning one. An image under its build directory
    needs no argument; build names the directory for an image copied away from it. A queued copy finds the harness built at submission."""
    import re
    located = None
    if build:
        home = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', (Path(build) / 'CMakeCache.txt').read_text(), re.M)
        located = (Path(build).resolve(), Path(home.group(1)))
    from_prepare = load('session_tool_prepare', ROOT / 'apps/os/prepare.py')
    admission = load('session_tool_admission', ROOT / 'tools/host_admission.py')
    base = load('session_tool_image', ROOT / 'tools/check-a733-smp.py')
    _, address, identity = base.image_parts(Path(image))
    _, contract = from_prepare.request_bytes(address, identity, raw)
    try:
        return admission.preflight(Path(image), raw, contract, build=located, lifetime=lifetime)
    except admission.AdmissionRefused as error:
        return {'admitted': False, 'refusal': str(error), 'detail': error.detail, 'term': error.term, 'sub': error.sub}
    except admission.Unavailable as error:
        raise SessionError('admission: not decided: %s' % error) from None


def show(report, stream=sys.stdout):
    p = report['prefix']
    print('session %d bytes sha256 %s' % (report['bytes'], report['sha256']), file=stream)
    print('epoch %#x generation %#x lease %#x' % (p['epoch'], p['generation'], p['lease']), file=stream)
    print('image identity %s' % p['image'], file=stream)
    print('manifest sha256 %s' % report['manifest_sha256'], file=stream)
    print('devices %#x flags %d workload %d' % (report['devices'], report['flags'], p['workload']), file=stream)
    for name, value in report['extents'].items():
        if name == 'inputs':
            for i, item in enumerate(value):
                print('input %d: %d bytes sha256 %s' % (i, item['bytes'], item['sha256']), file=stream)
        elif name == 'npu':
            print('npu: flags %#x pool request %#x+%#x declaration slots %d in_flight %d graphs %d links %d '
                  'graph_bytes %d tensor_bytes %d' % (value['flags'], value['pool_base'],
                                                      value['pool_bytes'], value['slots'], value['in_flight'], value['graphs'],
                                                      value['links'], value['graph_bytes'], value['tensor_bytes']), file=stream)
        elif name == 'cpu_grant':
            print('cpu_grant: clusters %d max_mhz %s changes %d perf %d epoch %#x generation %#x' % (
                value['clusters'], value['max_mhz'], value['changes'], value['perf'], value['epoch'], value['generation']), file=stream)
        elif name == 'thermal':
            print('thermal: owner %#x apb1 %d (%d Hz) size field %d' % (value['thermal_owner'], value['apb1'], value['apb1_hz'], value['bytes']),
                  file=stream)
        else:
            print('%s: %s' % (name, value), file=stream)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    b = commands.add_parser('build', help='write the session template of an image')
    b.add_argument('image', type=Path)
    b.add_argument('--out', type=Path, required=True, help='output directory; template/session.bin goes under it')
    b.add_argument('--manifest', type=Path, help='the application manifest JSON; it must pack to the image\'s manifest section')
    b.add_argument('--npu-declaration', type=Path, help='manifest with an npu section, or a JSON object of slots, in_flight, graphs, links')
    b.add_argument('--input', type=Path, action='append', default=[], help='a manifest input region\'s content, in region order (repeat)')
    b.add_argument('--clock-grant', action='store_true', help='grant the CPU clock at the reviewed points (the manifest must declare cpu.clock_grants)')
    b.add_argument('--perf', choices=('high', 'low'), help='with --clock-grant: the performance-mode service and its boot mode (the manifest must declare cpu.perf)')
    b.add_argument('--decoder', type=Path, help='the application decoder, copied to OUT/decoder.py')
    b.add_argument('--device-profile', choices=tuple(DEVICE_PROFILES),
                   help='reviewed facts for a declared device: USB keyboard host or passive radio scan; requires its linked service')
    b.add_argument('--npu-pool', type=lambda text: tuple(int(x, 0) for x in text.split(':')), default=(0, 0), metavar='BASE:BYTES',
                   help='a placement request for the NPU reservation (default 0:0, the allocator chooses; the ledger refuses an extent that is not free)')
    c = commands.add_parser('check', help='decode a session and print its fields')
    c.add_argument('path', type=Path, help='a build directory, or a session.bin')
    c.add_argument('--image', type=Path, help='compare the session with this image; default: the image session.json names')
    c.add_argument('--admit', action='store_true', help='run the image\'s EL3 admission on the session (needs the image)')
    c.add_argument('--build', type=Path, help='the CMake build directory of an image that was copied away from it (for --admit)')
    c.add_argument('--lifetime', choices=('returning', 'resident'), default='returning',
                   help='the lifetime contract --admit runs the admission with (default returning); a resident-only image needs resident')
    c.add_argument('--json', action='store_true', help='print the fields as JSON')
    args = parser.parse_args(argv)
    try:
        if args.command == 'build':
            record = build(args.image, args.out, args.manifest, args.npu_declaration, args.input, args.decoder,
                           npu_pool=args.npu_pool, clock_grant=args.clock_grant, perf=args.perf, device_profile=args.device_profile)
            print('wrote %s (%d bytes, profile %s, services %s)' % (args.out / 'template/session.bin', record['session_bytes'],
                                                                   record['profile'], ', '.join(record['services'])))
            print('submit with: board.py submit %s --template %s%s%s ...' % (
                args.image, args.out / 'template', ' --decoder %s' % (args.out / 'decoder.py') if args.decoder else '', ''))
            return 0
        session = find_session(args.path)
        image = args.image
        record = args.path / 'session.json' if args.path.is_dir() else None
        if image is None and record and record.is_file():
            image = Path(json.loads(record.read_text())['image'])
        raw = session.read_bytes()
        report = inspect(raw, image)
        if args.admit:
            if image is None:
                raise SessionError('admit: --admit needs the image (--image, or a directory with session.json)')
            report['admission'] = admit(image, raw, args.build, args.lifetime)
        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            show(report)
            for problem in report['problems']:
                print('problem: %s' % problem)
            if 'admission' in report:
                print('admitted: %s' % str(report['admission']['admitted']).lower())
                if not report['admission']['admitted']:
                    print('refused: %s' % report['admission'].get('refusal'))
        failed = report['problems'] or ('admission' in report and not report['admission']['admitted'])
        return 1 if failed else 0
    except (SessionError, ValueError, OSError, KeyError) as error:
        print('session: %s' % error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
