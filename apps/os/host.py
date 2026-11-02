#!/usr/bin/env python3
"""Offline SunstoneOS manifest, typed-session and result records.

Application payloads remain opaque. Device evidence and transport ownership
are validated by the caller before it uses these records for a board launch.
"""
import hashlib
import importlib.util
import io
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


manifest_tool = module('ss_host_manifest', ROOT / 'tools/sunstone-manifest.py')
PREFIX_BYTES, MAX_BYTES = 592, 4096
SESSION_MAGIC, SESSION_VERSION = 0x324e4f4953534553, 1
BIND_SID = 1
HEADER = struct.Struct('<QQIIII32s')
EXTENT = struct.Struct('<II')
INPUT = struct.Struct('<Q32s')
THERMAL, AUDIO, NPU, CPU_GRANT, JPEG, USB_MEMORY, INPUTS = 1, 2, 3, 4, 5, 6, 16
DMA = 7
SERVICE_BYTES = {THERMAL: 216, AUDIO: 184, NPU: 272, CPU_GRANT: 96, DMA: 160}
SERVICE_MAGIC = {THERMAL: 0x415337334f534131, AUDIO: 0x415337334f415531,
                 NPU: 0x415337334f4e5031, CPU_GRANT: 0x4153373343505531,
                 DMA: 0x41533733444d4131}
RESULT_MAGIC, RESULT_VERSION, RESULT_FINISHED = 0x31544c5553455241, 1, 1
RESULT_FAILED = 2
# include/sunstone_session.h struct ss_failure
FAILURE = struct.Struct('<Q4IQ2IQ')
FAILURE_MAGIC, FAILURE_VERSION = 0x3130454c49414653, 1
FAILURE_CONTEXTS = {1: 'running', 2: 'startup', 3: 'fault'}
RESULT = struct.Struct('<7Q')
FRAME_MAGIC, RESULT_OP = 0x424d494d, 0x80
FRAME = struct.Struct('<8I2Q16s')
REQUEST = struct.Struct('<BBHIQ')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def image_manifest(image):
    """Read the linked section; JSON supplied beside an image is not authority."""
    raw = Path(image).read_bytes() if isinstance(image, (str, Path)) else bytes(image)
    elf = ELFFile(io.BytesIO(raw))
    sections = [s for s in elf.iter_sections() if s.name == 'sunstone_manifest']
    require(len(sections) <= 1, 'Ambiguous application manifest section.')
    section = sections[0] if sections else None
    packed = section.data() if section else b''
    require(not section or section['sh_type'] == 'SHT_PROGBITS' and section['sh_flags'] & 2 and not section['sh_flags'] & 1,
            'Application manifest must be immutable allocated data.')
    manifests = manifest_tool.unpack(packed) if packed else []
    require(len(manifests) <= 1, 'One application per image required.')
    return {'sha256': hashlib.sha256(packed).hexdigest(), 'manifest': manifests[0] if manifests else None,
            'section': packed, 'elf': elf}


def regions(manifest, kinds):
    return [dict(r) for r in (manifest or {}).get('regions', []) if r['kind'] in kinds]


NPU_DECLARED_OFFSET = 60  # struct os_npu_session, declared


def with_npu_declaration(extent, word):
    """The NPU service extent with the application's declaration (tools/sunstone-manifest.py, npu_word) in place."""
    require(len(extent) == SERVICE_BYTES[NPU], 'NPU extent size differs.')
    data = bytearray(extent)
    struct.pack_into('<I', data, NPU_DECLARED_OFFSET, word)
    return bytes(data)


def input_facts(manifest, inputs):
    declared = regions(manifest, (manifest_tool.KIND['input'],))
    require(len(inputs) == len(declared), 'Input count differs from manifest.')
    facts, uploads = bytearray(), []
    for index, (region, data) in enumerate(zip(declared, inputs)):
        data = bytes(data)
        digest = hashlib.sha256(data).digest()
        pinned = bytes.fromhex(region['sha256'])
        require(len(data) <= region['bytes'], 'Input length exceeds manifest region.')
        require(not any(pinned) or len(data) == region['bytes'] and digest == pinned,
                'Input differs from pinned manifest digest or length.')
        facts.extend(INPUT.pack(len(data), digest))
        uploads.append({'index': index, 'address': region['address'], 'bytes': len(data),
                        'sha256': digest.hex()})
    return bytes(facts), uploads


def session_decode(raw, manifest=None, manifest_sha256=None):
    raw = bytes(raw)
    require(PREFIX_BYTES + HEADER.size <= len(raw) <= MAX_BYTES, 'Invalid typed session size.')
    magic, version, size, count, devices, flags, digest = HEADER.unpack_from(raw, PREFIX_BYTES)
    require((magic, version, size) == (SESSION_MAGIC, SESSION_VERSION, len(raw)), 'Invalid typed session header.')
    require(not flags & ~BIND_SID and not devices & ~sum(manifest_tool.DEVICES.values()), 'Unknown session devices or flags.')
    extents, offsets, offset = {}, {}, PREFIX_BYTES + HEADER.size
    for _ in range(count):
        require(offset + EXTENT.size <= len(raw), 'Truncated session extent header.')
        kind, size = EXTENT.unpack_from(raw, offset)
        offset += EXTENT.size
        require(kind < 32 and kind not in extents, 'Duplicate or invalid session extent type.')
        padded = (size + 7) & ~7
        require(offset + padded <= len(raw), 'Truncated session extent payload.')
        require(not any(raw[offset + size:offset + padded]), 'Nonzero session extent padding.')
        offsets[kind], extents[kind] = offset, raw[offset:offset + size]
        offset += padded
    require(offset == len(raw), 'Unconsumed typed session bytes.')
    for kind, size in SERVICE_BYTES.items():
        require(kind not in extents or len(extents[kind]) == size, 'Invalid service extent size.')
    require(THERMAL in extents, 'Missing thermal service extent.')
    legacy_bytes = PREFIX_BYTES + sum(SERVICE_BYTES[kind] for kind in extents if kind in SERVICE_BYTES)
    for kind in SERVICE_BYTES:
        if kind in extents:
            size = legacy_bytes if kind == THERMAL else SERVICE_BYTES[kind]
            require(struct.unpack_from('<3Q', extents[kind]) == (SERVICE_MAGIC[kind], 1, size),
                    'Invalid service record header.')
    if USB_MEMORY in extents:
        value=extents[USB_MEMORY]
        require(len(value)==64 and struct.unpack_from('<4Q',value)==(1,0x42200000,0x800000,0x7a000000),
                'Invalid USB memory profile geometry.')
    if JPEG in extents:
        value=extents[JPEG]
        require(len(value)==856 and value[:PREFIX_BYTES]==raw[:PREFIX_BYTES], 'JPEG extent prefix differs.')
        require(struct.unpack_from('<3Q',value,PREFIX_BYTES)==(0x415337334a504132,4,856), 'Invalid camera JPEG extent header.')
        require(not any(value[768:832]), 'Camera JPEG extent must not fabricate fixture or reference identity.')
    if manifest_sha256 is not None:
        require(digest.hex() == manifest_sha256, 'Session manifest digest differs from image.')
    if manifest is not None or manifest_sha256 is not None:
        m = manifest or {}
        required, optional = m.get('devices_required', 0), m.get('devices_optional', 0)
        require(devices & required == required and not devices & ~(required | optional),
                'Session device grant differs from manifest.')
        capture = devices & 12
        require(capture in (0, 4, 8), 'Multiple capture devices granted.')

        require(devices & 1 or not capture and flags & BIND_SID,
                'Session without storage requires SID binding and no capture.')
        workload = struct.unpack_from('<I', raw, 400)[0]
        expected = 0 if devices & 0xe2 else 0 if devices & 1 else 2
        require(workload == expected or workload == 4 and devices & 3 == 3 or workload == 1 and expected == 0,
                'Board workload differs from device grant.')
        facts = extents.get(INPUTS, b'')
        declared = regions(manifest, (manifest_tool.KIND['input'],))
        require(len(facts) == INPUT.size * len(declared), 'INPUTS extent count differs from manifest.')
        for index, region in enumerate(declared):
            size, actual = INPUT.unpack_from(facts, index * INPUT.size)
            pinned = bytes.fromhex(region['sha256'])
            require(size <= region['bytes'], 'Input length exceeds manifest region.')
            require(not any(pinned) or size == region['bytes'] and actual == pinned,
                    'INPUTS extent differs from pinned manifest digest or length.')
        if capture == manifest_tool.DEVICES['audio-i2s']:
            require(AUDIO in extents, 'Capture grant requires audio service extent.')
            samples = struct.unpack_from('<I', extents[AUDIO], 164)[0]
            require(m['audio']['min_frames'] <= samples <= m['audio']['max_frames'],
                    'Capture length differs from manifest.')
    return {'devices': devices, 'flags': flags, 'manifest_sha256': digest.hex(),
            'extents': extents, 'offsets': offsets}


def session_bytes(prefix, services, manifest, manifest_sha256, devices, flags=0, inputs=()):
    """Build a typed session from existing board and service record producers."""
    require(len(prefix) == PREFIX_BYTES, 'Board prefix size differs.')
    facts, _ = input_facts(manifest, inputs)
    records = dict(services)
    require(INPUTS not in records, 'INPUTS must come from the input producer.')
    if facts:
        records[INPUTS] = facts
    body = bytearray()
    for kind, data in records.items():
        data = bytes(data)
        body.extend(EXTENT.pack(kind, len(data)))
        body.extend(data)
        body.extend(bytes((-len(data)) % 8))
    raw = bytes(prefix) + HEADER.pack(SESSION_MAGIC, SESSION_VERSION, PREFIX_BYTES + HEADER.size + len(body),
                                    len(records), devices, flags, bytes.fromhex(manifest_sha256)) + body
    session_decode(raw, manifest, manifest_sha256)
    return raw


def result_geometry(image):
    info = image_manifest(image)
    require(info['manifest'] is not None, 'Result requires an application manifest.')
    table = info['elf'].get_section_by_name('.symtab')
    found = [s for s in table.iter_symbols() if s.name == 'ss_result_record' and s['st_shndx'] != 'SHN_UNDEF'] if table else []
    size = RESULT.size + info['manifest']['output_limit']
    require(len(found) == 1 and found[0]['st_info']['bind'] == 'STB_GLOBAL' and found[0]['st_size'] == size,
            'Result symbol differs from manifest capacity.')
    symbol = found[0]
    index, address = symbol['st_shndx'], int(symbol['st_value'])
    require(symbol['st_info']['type'] == 'STT_OBJECT' and type(index) is int and
            0 < index < info['elf'].num_sections(), 'Result symbol must name a data object.')
    section = info['elf'].get_section(index)
    require(section['sh_type'] in ('SHT_PROGBITS', 'SHT_NOBITS') and section['sh_flags'] & 3 == 3 and
            section['sh_addr'] <= address and address + size <= section['sh_addr'] + section['sh_size'],
            'Result object is outside allocated writable data.')
    require(0x41000000 <= address < address + size <= 0x41f00000,
            'Result object is outside native image RAM.')
    require(any(p['p_type'] == 'PT_LOAD' and p['p_flags'] & 2 and p['p_vaddr'] <= address and
                address + size <= p['p_vaddr'] + p['p_memsz'] for p in info['elf'].iter_segments()),
            'Result object has no writable load segment.')
    return {'address': address, 'bytes': size}


def decode_result(raw, epoch, generation, output_limit=None):
    raw = bytes(raw)
    require(len(raw) >= RESULT.size, 'Truncated application result.')
    magic, version, capacity, actual, actual_generation, status, used = RESULT.unpack_from(raw)
    require((magic, version, capacity, actual, actual_generation, status) ==
            (RESULT_MAGIC, RESULT_VERSION, len(raw), epoch, generation, RESULT_FINISHED),
            'Application result is incomplete or belongs to another session.')
    require(output_limit is None or len(raw) == RESULT.size + output_limit,
            'Application result capacity differs from manifest.')
    require(used <= len(raw) - RESULT.size, 'Application result payload exceeds capacity.')
    return raw[RESULT.size:RESULT.size + used]


def decode_failure(raw, epoch, generation, output_limit=None):
    """The record of a failure return: result status FAILED and an ss_failure payload.

    The generation is zero when the failure preceded the sealed session.
    Returns the decoded fields and the payload bytes."""
    raw = bytes(raw)
    require(len(raw) >= RESULT.size, 'Truncated failure result.')
    magic, version, capacity, actual, actual_generation, status, used = RESULT.unpack_from(raw)
    require((magic, version, capacity, actual, status) == (RESULT_MAGIC, RESULT_VERSION, len(raw), epoch, RESULT_FAILED) and
            actual_generation in (generation, 0), 'Failure result is absent or belongs to another session.')
    require(output_limit is None or len(raw) == RESULT.size + output_limit, 'Failure result capacity differs from manifest.')
    require(used == FAILURE.size and used <= len(raw) - RESULT.size, 'Failure record has the wrong length.')
    payload = raw[RESULT.size:RESULT.size + used]
    magic, version, context, site, value, caller, core, reserved, counter = FAILURE.unpack(payload)
    require((magic, version, reserved) == (FAILURE_MAGIC, FAILURE_VERSION, 0) and context in FAILURE_CONTEXTS and core < 8,
            'Failure record is malformed.')
    return {'context': FAILURE_CONTEXTS[context], 'site': site, 'value': value, 'caller': caller, 'core': core,
            'counter': counter, 'generation_bound': actual_generation != 0}, payload


def read_result(exchange, image, epoch, generation):
    """Read one complete OS result over the caller-owned framed transport.

    Return None when the first frame reports an unfinished application. Each
    exchange runs once; transport errors propagate to the owner without retry.
    """
    info = image_manifest(image)
    require(info['manifest'] is not None, 'Result requires an application manifest.')
    limit = info['manifest']['output_limit']
    total, raw = RESULT.size + limit, bytearray()
    while len(raw) < total:
        offset, count = len(raw), min(1024, total - len(raw))
        frame = bytes(exchange(REQUEST.pack(RESULT_OP, 0, count, offset, 0)))
        require(len(frame) >= FRAME.size, 'Truncated result frame.')
        magic, version, state, size, capacity, actual_offset, storage, op, actual_epoch, actual_generation, tag = FRAME.unpack_from(frame)
        require((magic, version, capacity, actual_offset, storage, op, actual_epoch, actual_generation, tag) ==
                (FRAME_MAGIC, 1, total, offset, 0, RESULT_OP, epoch, generation, bytes(16)),
                'Result frame identity or geometry differs.')
        require(state in (0, 1) and len(frame) == FRAME.size + size, 'Invalid result frame state or size.')
        if not state:
            require(not raw and not size, 'Result became unavailable during capture.')
            return None
        require(size == count, 'Incomplete result frame payload.')
        raw.extend(frame[FRAME.size:])
    return decode_result(raw, epoch, generation, limit)


WATCHDOG_MARKER, WATCHDOG_VERSION, CRUMB_RTC = 0x31544457534f5243, 2, 0x07090000


def watchdog_marker(image):
    """{'mode', 'session_seconds'} of a returning image's watchdog, or None (apps/os/cpu-return.c).

    The host arms that WDT0 mode before launch. The image restarts it while its
    session is live, disarms it on its only normal return and records
    breadcrumbs in RTC words (include/sunstone_crumb.h)."""
    raw = Path(image).read_bytes() if isinstance(image, (str, Path)) else bytes(image)
    elf = ELFFile(io.BytesIO(raw))
    table = elf.get_section_by_name('.symtab')
    found = [s for s in table.iter_symbols() if s.name == 'os_cpu_return_watchdog' and
             s['st_shndx'] != 'SHN_UNDEF'] if table else []
    if not found:
        return None
    require(len(found) == 1 and found[0]['st_size'] == 40 and type(found[0]['st_shndx']) is int,
            'Watchdog marker geometry differs.')
    section, address = elf.get_section(found[0]['st_shndx']), int(found[0]['st_value'])
    offset = address - section['sh_addr']
    data = section.data()[offset:offset + 40]
    require(section['sh_type'] == 'SHT_PROGBITS' and len(data) == 40, 'Watchdog marker has no image bytes.')
    magic, version, mode, rtc, seconds = struct.unpack('<5Q', data)
    require((magic, version, rtc) == (WATCHDOG_MARKER, WATCHDOG_VERSION, CRUMB_RTC) and mode & 0xff0f == 1 and
            1 <= mode >> 4 <= 11 and 1 <= seconds <= 600, 'Watchdog marker differs.')
    return {'mode': mode, 'session_seconds': seconds}


def usb_pool_geometry(elf):
    """Validate the exact linked USB arena; this establishes no physical authority."""
    pool = elf.get_section_by_name('.uncached_pool')
    if pool is not None and pool['sh_size'] and pool['sh_addr'] >= 0x42200000:
        low, size = pool['sh_addr'], pool['sh_size']
        table = elf.get_section_by_name('.symtab')
        bounds = {name: [s for s in table.iter_symbols() if s.name == name and
                        s['st_shndx'] != 'SHN_UNDEF'] if table else []
                  for name in ('__uncached_pool_start', '__uncached_pool_end')}
        require((low, size) == (0x42200000, 0x800000) and pool['sh_type'] == 'SHT_NOBITS' and
                pool['sh_flags'] & 7 == 3 and all(len(found) == 1 and
                found[0]['st_info']['bind'] == 'STB_GLOBAL' and found[0]['st_value'] == expected
                for found, expected in zip(bounds.values(), (low, low + size))) and
                sum(p['p_type'] == 'PT_LOAD' and p['p_flags'] == 6 and not p['p_filesz'] and
                    p['p_vaddr'] == p['p_paddr'] == low and p['p_memsz'] == size
                    for p in elf.iter_segments()) == 1, 'USB backup pool lacks its exact linked allocation.')
        return {'address': low, 'bytes': size}
    return None


def backup_regions(image, uploads=()):
    """Union uploaded spans with the manifest WORK and OUTPUT regions."""
    info = image_manifest(image)
    spans = [(r['address'], r['address'] + r['bytes']) for r in
             regions(info['manifest'], (manifest_tool.KIND['work'], manifest_tool.KIND['output']))]
    spans.extend((r['address'], r['address'] + r['bytes']) for r in uploads if r['bytes'])
    pool = usb_pool_geometry(info['elf'])
    if pool:
        spans.append((pool['address'], pool['address'] + pool['bytes']))
    merged = []
    for low, high in sorted(spans):
        require(0 <= low < high <= 1 << 64, 'Invalid backup region.')
        if merged and low <= merged[-1][1]:
            merged[-1][1] = max(high, merged[-1][1])
        else:
            merged.append([low, high])
    return [{'address': low, 'bytes': high - low} for low, high in merged]
