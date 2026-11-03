#!/usr/bin/env python3
"""Build and check the typed board session of a SunstoneOS image, with no device access and no old queue entry.

    session.py build IMAGE --out DIR [--manifest M] [--npu-declaration D] [--input FILE ...] [--devices MASK]
                             [--decoder FILE]
    session.py check DIR_OR_SESSION [--image IMAGE] [--admit] [--json]

build writes the complete session of IMAGE to DIR/template/session.bin, the input files to DIR/template/input-N.bin, and DIR/session.json, which
records what each field came from. The template is what `board.py submit --template DIR/template` takes; the runner draws a fresh epoch,
generation and lease and re-derives the live thermal fields when it launches. --decoder copies the application's decoder (for example the one
apps/npu-graphs/host.py writes) to DIR/decoder.py, the file that `board.py submit --decoder` takes. Every field is read from the image or from tools/session-facts.json:

  image identity, manifest digest   the image's build ID (tools/check-a733-smp.py) and its sunstone_manifest section
  devices, flags, workload          the manifest's required devices; --devices adds optional ones (only 0 has pinned board facts)
  which service records             what the image links: the thermal record always, the CPU clock grant (a733_cpu_clock_grant_load),
                                    the NPU service (os_npu_catalogue_digest); an audio, DMA or radio service is refused
  NPU catalogue, client model, pool the image's os_npu_catalogue_digest, os_npu_client_model and __os_npu_pool_start/end
  NPU declaration                   --npu-declaration D (a manifest with an npu section, or a bare slots/in_flight/graphs/links object),
                                    or the npu section of --manifest M; the image does not carry it
  inputs                            --input FILE for each manifest input region, in order (their digests go into the session)
  board and evidence facts          tools/session-facts.json: the board prefix, thermal and grant records and the NPU authority words of
                                    the profile the image needs ("npu" for an image with the NPU service, else "control")

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
         ('max_mhz', '2I'), ('changes', 'I'), ('approval', '32s')]
NPU = [('magic', 'Q'), ('version', 'Q'), ('bytes', 'Q'), ('owner', 'Q'), ('command_admission', 'Q'), ('supervisor', 'Q'),
       ('pck_client', 'I'), ('client_model', 'I'), ('jobs', 'I'), ('reserved', 'I'), ('pool_start', 'Q'), ('pool_bytes', 'Q'),
       ('catalogue', '32s'), ('admission', '32s'), ('clock', '32s'), ('power', '32s'), ('supervision', '32s'), ('ddr', '32s'),
       ('slots', 'I'), ('in_flight', 'I'), ('graphs', 'I'), ('links', 'I'), ('graph_bytes', 'I'), ('tensor_bytes', 'I')]
LAYOUTS = {'prefix': (PREFIX, 592), 'thermal': (THERMAL, 216), 'cpu_grant': (GRANT, 96), 'npu': (NPU, 296)}
SESSION_MAGIC, SESSION_VERSION = 0x4153373342535332, 2          # the board prefix (apps/board/prepare.py SESSION_MAGIC)
GRANT_MAGIC = 0x4153373343505531                                # A733_CPU_CLOCK_GRANT_MAGIC
OS_MAGIC = 0x415337334f534131                                   # the thermal record's magic (A733_OS_SESSION_MAGIC)
# What each record takes from the image or the build rather than from the facts file.
DERIVED = {'prefix': {'magic', 'version', 'bytes', 'epoch', 'generation', 'lease', 'image', '_pad'},
           'thermal': {'magic', 'version', 'bytes'},
           'cpu_grant': {'magic', 'version', 'bytes', 'epoch', 'generation'},
           'npu': {'magic', 'version', 'bytes', 'client_model', 'reserved', 'pool_start', 'pool_bytes', 'catalogue',
                   'slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'}}
# Symbols that tell which services an OS image links (the CMake options SUNSTONE_A733_OS_*).
SYMBOLS = {'cpu_grant': 'a733_cpu_clock_grant_load', 'npu': 'os_npu_catalogue_digest'}
UNSUPPORTED = {'os_audio_task': 'audio service record (devices audio-i2s or audio-headset)',
               'os_dma_profile': 'DMA service record and its physical authority',
               'os_radio_start': 'radio profile, SID evidence and radio latch facts of the board prefix (workload 1)',
               'usb_memory_extent_valid': 'USB memory extent (USB class devices)'}


def width(fmt):
    return struct.calcsize('<' + fmt)


for _name, (_layout, _size) in LAYOUTS.items():
    assert sum(width(f) for _, f in _layout) == _size, _name


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


def describe(image, manifest_path=None, npu_declaration=None, devices=None, host=None):
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
    granted = manifest['devices_required'] | (devices or 0)
    if granted & ~(manifest['devices_required'] | manifest['devices_optional']):
        raise SessionError('devices: %#x is not among the manifest\'s required %#x and optional %#x devices'
                           % (devices, manifest['devices_required'], manifest['devices_optional']))
    if granted:
        raise SessionError('devices: the session grants %#x, and tools/session-facts.json pins board facts only for a session that grants none' % granted)
    plan = {'image': str(image), 'manifest': manifest, 'manifest_sha256': info['sha256'], 'devices': granted,
            'flags': host.BIND_SID, 'services': ['thermal'], 'symbols': symbols, 'elf': elf}
    if SYMBOLS['cpu_grant'] in symbols:
        plan['services'].append('cpu_grant')
    if SYMBOLS['npu'] in symbols:
        plan['services'].append('npu')
        if npu_declaration:
            plan['npu_declaration'] = declaration_of(npu_declaration)
            plan['npu_declaration_from'] = str(npu_declaration)
        elif declared:
            plan['npu_declaration'] = tuple(declared[k] for k in ('slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'))
            plan['npu_declaration_from'] = str(manifest_path)
        else:
            raise SessionError('npu declaration (slots, in_flight, graphs, links): the image does not carry it; pass --npu-declaration D '
                               '(a manifest with an npu section, or a JSON object of those fields) or --manifest M with an npu section')
        pool = host.npu_pool_symbols(image)
        if pool is None:
            raise SessionError('npu pool: the image links the NPU service but no __os_npu_pool_start/__os_npu_pool_end')
        plan['npu_pool'] = pool
        plan['npu_client_model'] = struct.unpack('<I', symbol_bytes(elf, symbols, 'os_npu_client_model')[:4])[0]
        plan['npu_catalogue'] = symbol_bytes(elf, symbols, 'os_npu_catalogue_digest')
    plan['profile'] = 'npu' if 'npu' in plan['services'] else 'control'
    return plan


def input_values(plan, inputs, host):
    regions = host.regions(plan['manifest'], (1,))
    if len(inputs) != len(regions):
        raise SessionError('inputs: the manifest declares %d input regions and %d --input files were given (%s)'
                           % (len(regions), len(inputs), ', '.join('%#x:%d' % (r['address'], r['bytes']) for r in regions) or 'none'))
    return [Path(p).read_bytes() for p in inputs]


def build_session(plan, inputs, fact_file=FACTS, host=None, entropy=secrets.randbits):
    """The session bytes for a described image, and the identity words it carries."""
    host = host or records()
    base = load('session_tool_image', ROOT / 'tools/check-a733-smp.py')
    _, address, identity = base.image_parts(Path(plan['image']))
    profile = facts(fact_file)['profiles'].get(plan['profile'])
    if profile is None:
        raise SessionError('profile: tools/session-facts.json has no profile %s' % plan['profile'])
    epoch, generation, lease = [entropy(62) | 1 for _ in range(3)]
    prefix = pack('prefix', profile_values(profile, 'prefix', {
        'magic': SESSION_MAGIC, 'version': SESSION_VERSION, 'bytes': 592, 'epoch': epoch, 'generation': generation,
        'lease': lease, 'image': identity}))
    services = {}
    size = 592 + sum(LAYOUTS[k][1] for k in plan['services'])
    thermal = profile_values(profile, 'thermal', {'magic': OS_MAGIC, 'version': 1, 'bytes': size})
    services[host.THERMAL] = pack('thermal', thermal)
    if 'cpu_grant' in plan['services']:
        grant = profile_values(profile, 'cpu_grant', {'magic': GRANT_MAGIC, 'version': 1, 'bytes': 96,
                                                      'epoch': epoch, 'generation': generation})
        if not grant['clusters']:
            grant['epoch'] = grant['generation'] = 0       # a record that grants nothing names no session
        services[host.CPU_GRANT] = pack('cpu_grant', grant)
    if 'npu' in plan['services']:
        slots, in_flight, graphs, links, graph_bytes, tensor_bytes = plan['npu_declaration']
        start, end = plan['npu_pool']
        npu = profile_values(profile, 'npu', {
            'magic': host.SERVICE_MAGIC[host.NPU], 'version': host.SERVICE_VERSION[host.NPU], 'bytes': host.SERVICE_BYTES[host.NPU],
            'client_model': plan['npu_client_model'], 'reserved': 0, 'pool_start': start, 'pool_bytes': end - start,
            'catalogue': plan['npu_catalogue'], 'slots': slots, 'in_flight': in_flight, 'graphs': graphs, 'links': links,
            'graph_bytes': graph_bytes, 'tensor_bytes': tensor_bytes})
        services[host.NPU] = pack('npu', npu)
        services = host.bind_npu_pool(plan['image'], services)
    ordered = {k: services[k] for k in (host.THERMAL, host.CPU_GRANT, host.NPU) if k in services}
    try:
        raw = host.session_bytes(prefix, ordered, plan['manifest'], plan['manifest_sha256'], plan['devices'], plan['flags'], inputs)
    except ValueError as error:
        raise SessionError('session: the decoder refuses it: %s' % error) from None
    return raw, {'epoch': epoch, 'generation': generation, 'lease': lease, 'address': address, 'identity': identity.hex()}


def build(image, out, manifest=None, npu_declaration=None, inputs=(), devices=None, decoder=None, fact_file=FACTS):
    """Write OUT/template/session.bin, input files and session.json; returns the record."""
    host = records()
    plan = describe(image, manifest, npu_declaration, devices, host)
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
              'services': plan['services'], 'devices': plan['devices'], 'flags': plan['flags'], 'session_bytes': len(raw),
              'session_sha256': hashlib.sha256(raw).hexdigest(), **ids,
              'inputs': [{'file': 'template/input-%d.bin' % i, 'bytes': len(v), 'sha256': hashlib.sha256(v).hexdigest()} for i, v in enumerate(values)]}
    if 'npu' in plan['services']:
        record['npu_declaration'] = dict(zip(('slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes'), plan['npu_declaration']))
        record['npu_declaration_from'] = plan['npu_declaration_from']
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
            problems.append('npu: the image links the NPU service and the session has no NPU record')
        if npu and SYMBOLS['npu'] not in symbols:
            problems.append('npu: the session has an NPU record and the image does not link the NPU service')
        if npu and SYMBOLS['npu'] in symbols:
            if bytes.fromhex(npu['catalogue']) != symbol_bytes(elf, symbols, 'os_npu_catalogue_digest'):
                problems.append('npu catalogue: %s differs from the image\'s os_npu_catalogue_digest' % npu['catalogue'])
            if npu['client_model'] != struct.unpack('<I', symbol_bytes(elf, symbols, 'os_npu_client_model')[:4])[0]:
                problems.append('npu client_model: %d differs from the image\'s os_npu_client_model' % npu['client_model'])
            pool = host.npu_pool_symbols(image)
            if pool != (npu['pool_start'], npu['pool_start'] + npu['pool_bytes']):
                problems.append('npu pool: %#x+%#x differs from the image\'s %s' % (npu['pool_start'], npu['pool_bytes'], pool))
        if bool(grant) != (SYMBOLS['cpu_grant'] in symbols):
            problems.append('cpu_grant: the record is %s and the image %s the CPU clock grant'
                            % ('present' if grant else 'absent', 'links' if SYMBOLS['cpu_grant'] in symbols else 'does not link'))
    report['problems'] = problems
    return report


def admit(image, raw, build=None, source=None):
    """The image's own EL3 admission on the session and the launch contract the runner would send; returns its verdict dictionary.

    The admission code is built from the CMake build directory of the image (found from its path, or the cached harness of a queued copy);
    build names that directory for an image copied away from it, and source the source tree whose admission sources are compiled
    (default: the tree the build was configured from)."""
    import re
    located = None
    if build:
        home = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', (Path(build) / 'CMakeCache.txt').read_text(), re.M)
        located = (Path(build).resolve(), Path(source) if source else Path(home.group(1)))
    from_prepare = load('session_tool_prepare', ROOT / 'apps/os/prepare.py')
    admission = load('session_tool_admission', ROOT / 'tools/host_admission.py')
    base = load('session_tool_image', ROOT / 'tools/check-a733-smp.py')
    _, address, identity = base.image_parts(Path(image))
    _, contract = from_prepare.request_bytes(address, identity, raw)
    try:
        return admission.preflight(Path(image), raw, contract, build=located)
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
            print('npu: catalogue %s client_model %d jobs %d pool %#x+%#x declaration slots %d in_flight %d graphs %d links %d '
                  'graph_bytes %d tensor_bytes %d' % (value['catalogue'], value['client_model'], value['jobs'], value['pool_start'],
                                                      value['pool_bytes'], value['slots'], value['in_flight'], value['graphs'],
                                                      value['links'], value['graph_bytes'], value['tensor_bytes']), file=stream)
        elif name == 'cpu_grant':
            print('cpu_grant: clusters %d max_mhz %s changes %d epoch %#x generation %#x' % (
                value['clusters'], value['max_mhz'], value['changes'], value['epoch'], value['generation']), file=stream)
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
    b.add_argument('--devices', type=lambda x: int(x, 0), help='optional manifest devices to grant besides the required ones')
    b.add_argument('--decoder', type=Path, help='the application decoder, copied to OUT/decoder.py')
    c = commands.add_parser('check', help='decode a session and print its fields')
    c.add_argument('path', type=Path, help='a build directory, or a session.bin')
    c.add_argument('--image', type=Path, help='compare the session with this image; default: the image session.json names')
    c.add_argument('--admit', action='store_true', help='run the image\'s EL3 admission on the session (needs the image)')
    c.add_argument('--build', type=Path, help='the CMake build directory of an image that was copied away from it (for --admit)')
    c.add_argument('--source', type=Path, help='with --build: the source tree whose admission sources are compiled (default: the build\'s own tree)')
    c.add_argument('--json', action='store_true', help='print the fields as JSON')
    args = parser.parse_args(argv)
    try:
        if args.command == 'build':
            record = build(args.image, args.out, args.manifest, args.npu_declaration, args.input, args.devices, args.decoder)
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
            report['admission'] = admit(image, raw, args.build, args.source)
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
