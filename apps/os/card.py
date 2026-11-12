#!/usr/bin/env python3
"""Offline typed-session checks used by backup-first card packaging.

Card allocation, backup identity and writes remain in tools/boot-slots.py and
its storage update path. This module grants no storage or physical authority.
"""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('card_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
require = host.require


def canonical_session(raw):
    decoded = host.session_decode(raw)
    result = bytearray(raw)
    result[24:40] = bytes(16)
    offset = decoded['offsets'].get(host.CPU_GRANT)
    if offset is not None:
        result[offset + 32:offset + 48] = bytes(16)
    return bytes(result)


POLICY = 'sunstone-card-boot-policy-v1'
BUNDLE = 'sunstone-card-boot-bundle-v1'
SCOPE = 'resident-cold-boot'
EVIDENCE = {'qualification': 80, 'wiring': 216, 'memory': 248, 'recovery': 280}
THERMAL_EVIDENCE = dict(zip(('thermal_initial', 'thermal_electrical', 'thermal_clocks', 'thermal_supervision'),
                            (40, 72, 104, 136)))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def sha(data):
    return hashlib.sha256(data).hexdigest()


def metadata(raw):
    """Reject ambiguous JSON before binding its exact bytes."""
    def unique(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate metadata field: ' + key)
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=unique)


def vendor_cpu_points(sid_word):
    """Derive bin and maxima from the native driver's pinned vendor tables."""
    source = (ROOT / 'drivers/clock/a733_cpu_clock.c').read_text()
    codes = [int(v, 16) for v in re.search(r'codes\[\]=\{([^}]*)\}', source)[1].split(',')]
    code = (sid_word >> 16) & 255 or (sid_word >> 8) & 255
    require(not code or code in codes, 'CPU B1 SID bin is unsupported.')
    column = codes.index(code) + 1 if code else 9
    points = []
    for name in ('little', 'big'):
        rows = re.search(r'\b' + name + r'\[\]=\{(.*?)\n\};', source, re.S)
        require(rows is not None, 'Native CPU vendor table is missing.')
        supported = []
        for mhz, voltages in re.findall(r'\{(\d+),\{([^}]*)\}\}', rows[1]):
            mv = [int(value) for value in voltages.split(',')]
            require(len(mv) == 9, 'Native CPU vendor table geometry changed.')
            value = mv[column] if column < 9 else max(mv) if all(mv) else 0
            if value:
                supported.append({'mhz': int(mhz), 'mv': value})
        require(supported, 'CPU bin has no supported vendor point.')
        points.append(max(supported, key=lambda row: row['mhz']))
    return {'bin_column': column, 'points': points, 'source_sha256': sha(source.encode())}


def monitor_check(elf_bytes, binary, package):
    """Bind a flat stage-2 monitor to its initialized ELF sections."""
    from elftools.elf.elffile import ELFFile
    import io
    elf = ELFFile(io.BytesIO(elf_bytes))
    require(elf['e_type'] == 'ET_EXEC' and elf['e_machine'] == 'EM_AARCH64' and elf['e_entry'] == 0x47000000,
            'Stage 2 must be an AArch64 executable ELF.')
    symbols = elf.get_section_by_name('.symtab')
    names = {s.name for s in symbols.iter_symbols() if s['st_shndx'] != 'SHN_UNDEF'} if symbols else set()
    require({'a733_cold_image', 'a733_stage2_main', 'a733_stage2_report'} <= names,
            'Expected the cold stage-2 monitor.')
    entry = [s for s in symbols.iter_symbols() if s.name == 'a733_cold_image' and s['st_shndx'] != 'SHN_UNDEF']
    require(len(entry) == 1 and entry[0]['st_value'] == 0x47000000, 'Stage-2 entry symbol differs from its load address.')
    flat = bytearray()
    occupied = []
    for section in elf.iter_sections():
        address, size = int(section['sh_addr']), int(section['sh_size'])
        if not section['sh_flags'] & 2 or not size:
            continue
        require(0x47000000 <= address < address + size <= 0x47400000,
                'Stage-2 allocation leaves its reservation.')
        require(all(address + size <= low or high <= address for low, high in occupied),
                'Stage-2 allocations overlap.')
        occupied.append((address, address + size))
        if section['sh_type'] == 'SHT_NOBITS':
            continue
        require(section['sh_type'] == 'SHT_PROGBITS' and address + size <= 0x47008000,
                'Stage-2 initialized section exceeds its boundary.')
        end = address + size - 0x47000000
        flat.extend(bytes(max(0, end - len(flat))))
        data = section.data()
        require(len(data) == size, 'Truncated stage-2 section.')
        flat[address - 0x47000000:end] = data
    require(bytes(flat) == binary, 'Stage-2 flat binary differs from its ELF.')
    package.validate_monitor(binary)


def cold_profile(raw, image, linked):
    decoded = template_check(raw, image)
    module('cold_board_prefix', 'apps/board/prepare.py').session_check(raw[:host.PREFIX_BYTES], usb_reference_modes=(1,))
    require(struct.unpack_from('<3Q', raw) == (0x4153373342535332, 2, host.PREFIX_BYTES) and
            struct.unpack_from('<Q', raw, 40)[0] and raw[48:80].hex() == linked['build_id'],
            'Cold board prefix differs from the linked image.')
    require(decoded['devices'] == 0 and decoded['flags'] == host.BIND_SID and
            struct.unpack_from('<I', raw, 400)[0] == 2,
            'Card profile requires workload 2, SID binding and no device grants.')
    require(set(decoded['extents']) <= {host.THERMAL, host.CPU_GRANT, host.INPUTS},
            'Card profile supports only thermal, CPU and input records.')
    require(raw[80:112] == raw[280:312], 'Qualification and recovery identities differ.')
    # Pure service terms of apps/os/admission.c; physical authority remains external.
    thermal = decoded['extents'][host.THERMAL]
    thermal_owner, thermal_authority = struct.unpack_from('<2Q', thermal, 24)
    owners = struct.unpack_from('<4Q', raw, 456)
    apb, hz = struct.unpack_from('<2I', thermal, 168)
    parent, divisor = apb >> 24, (apb & 31) + 1
    source = {0: 24000000, 3: 600000000}.get(parent, 0)
    require(thermal_owner and thermal_authority and thermal_owner not in owners and
            all(any(thermal[offset:offset + 32]) for offset in THERMAL_EVIDENCE.values()) and
            thermal[176] <= 1 and thermal[177:179] == bytes((31, 31)) and not thermal[179] and
            not any(thermal[210:212]) and struct.unpack_from('<I', thermal, 212)[0] <= 110000 and
            not apb & ~0x0300001f and source and not source % divisor and hz == source // divisor,
            'Cold thermal ownership, clocks or masks differ from native admission.')
    trips = struct.unpack_from('<15H', thermal, 180)
    require(all(0 < low < high <= 4095 and 0 < delta <= low for low, high, delta in zip(trips[::3], trips[1::3], trips[2::3])),
            'Cold thermal trip bounds differ from native admission.')
    grant = decoded['extents'].get(host.CPU_GRANT)
    if grant is not None:
        require(linked['manifest']['clock_use'] & 1, 'The manifest does not declare CPU clock grants.')
        owner, = struct.unpack_from('<Q', grant, 24)
        clusters, little, big, changes = struct.unpack_from('<4I', grant, 48)
        require(owner and clusters == 3 and little and big and 1 <= changes <= 64 and not any(grant[64:96]),
                'Cold CPU grant requires both clusters, a finite transition budget and zero perf/reserved bytes.')
        require(owner != thermal_owner and owner not in owners, 'Cold CPU owner aliases another provider.')
    return decoded


def asset_check(manifest, decoded, assets):
    """Every input fact has one exact asset slice; every other asset byte is zero."""
    require(len(assets) <= 2, 'A card slot has room for at most two prepacked assets.')
    declarations = host.regions(manifest, (host.manifest_tool.KIND['input'],))
    facts = decoded['extents'].get(host.INPUTS, b'')
    require(len(facts) == len(declarations) * host.INPUT.size, 'Input fact count differs.')
    covered = [[] for _ in assets]
    load_base, load_limit = host.memory_layout.record()['boot_slot_load']
    reserved = ((0x41000000, 0x42200000), (0x47000000, 0x47400000))
    for index, (load, data) in enumerate(assets):
        require(type(load) is int and data and load_base <= load < load + len(data) <= load_limit,
                'Asset leaves the slot load interval.')
        require(all(load + len(data) <= low or high <= load for low, high in reserved),
                'Asset overlaps OS or stage-2 memory.')
        require(all(load + len(data) <= other or other + len(value) <= load for other, value in assets[:index]),
                'Asset memory extents overlap.')
    for index, region in enumerate(declarations):
        length, digest = host.INPUT.unpack_from(facts, index * host.INPUT.size)
        require(length > 0, 'Card inputs must carry nonempty data.')
        address = region['address']
        matches = [(i, address - load) for i, (load, data) in enumerate(assets)
                   if load <= address and address + length <= load + len(data)]
        require(len(matches) == 1, 'Each declared input must be covered by one asset.')
        i, offset = matches[0]
        require(hashlib.sha256(assets[i][1][offset:offset + length]).digest() == digest,
                'Asset slice differs from its authenticated input fact.')
        covered[i].append((offset, offset + length))
    for (_, data), spans in zip(assets, covered):
        require(spans, 'Asset covers no declared input.')
        end = 0
        for low, high in sorted(spans):
            require(low >= end and not any(data[end:low]), 'Asset input slices overlap or undeclared padding is nonzero.')
            end = high
        require(not any(data[end:]), 'Undeclared asset tail is nonzero.')


def policy_check(policy_raw, raw, image_sha, stage2_sha, linked, decoded, card, read):
    """Validate the current policy ABI; policy hashes never enter CPU reserved bytes."""
    p = metadata(policy_raw)
    require(isinstance(p, dict), 'Cold policy must be an object.')
    grant = decoded['extents'].get(host.CPU_GRANT)
    required = {'schema', 'scope', 'evidence_class', 'identity', 'target', 'image_sha256', 'stage2_sha256',
                'manifest_sha256', 'canonical_session_sha256', 'workers', 'sid_words', 'sid_authority', 'evidence'}
    require(set(p) == required | ({'cpu'} if grant is not None else set()), 'Unexpected cold policy fields.')
    require(p['schema'] == POLICY and p['scope'] == SCOPE and p['image_sha256'] == image_sha and
            p['stage2_sha256'] == stage2_sha and p['manifest_sha256'] == decoded['manifest_sha256'] and
            p['canonical_session_sha256'] == sha(canonical_session(raw)) and
            type(p['workers']) is int and p['workers'] == linked['manifest']['workers'] and p['target'] == card['target'] and
            isinstance(p['identity'], str) and p['identity'], 'Cold policy binding differs.')
    require(p['evidence_class'] in ('board-records', 'model-fixture'), 'Unsupported cold evidence class.')
    words = p['sid_words']
    require(isinstance(words, list) and len(words) == 4 and all(type(w) is int and 0 <= w < 2**32 for w in words) and
            hashlib.sha256(struct.pack('<4I', *words)).digest() == raw[320:352] and
            type(p['sid_authority']) is int and p['sid_authority'] == struct.unpack_from('<Q', raw, 312)[0],
            'Cold policy SID binding differs.')
    offsets = dict(EVIDENCE)
    offsets.update({name: decoded['offsets'][host.THERMAL] + offset for name, offset in THERMAL_EVIDENCE.items()})
    require(isinstance(p['evidence'], dict) and set(p['evidence']) == set(offsets) | ({'cpu_b1'} if grant else set()),
            'Cold policy evidence set differs.')
    frozen = {}
    for name, reference in p['evidence'].items():
        require(isinstance(reference, dict) and set(reference) == {'path', 'sha256'} and
                isinstance(reference['path'], str) and reference['path'], 'Invalid evidence reference.')
        data = read(reference['path'])
        require(sha(data) == reference['sha256'], 'Evidence file changed: ' + name)
        if name in offsets:
            require(hashlib.sha256(data).digest() == raw[offsets[name]:offsets[name] + 32],
                    'Session evidence differs: ' + name)
        frozen[name] = data
    if grant is not None:
        b1 = metadata(frozen['cpu_b1'])
        require(b1.get('schema') == 'a733-os-cpu-clock-b1-v1' and b1.get('passed') is True and
                b1.get('identity') == p['identity'] and b1.get('port') == card['target']['port'] and
                b1.get('grant_clusters') == 0 and b1.get('pattern_lfm_clear') is True and
                type(b1.get('sid_word')) is int and 0 <= b1['sid_word'] < 2**32 and
                all(type(b1.get(k)) is int and 800 <= b1[k] <= 1050 for k in ('dcdc3_mv', 'dcdc5_mv')),
                'CPU grant requires matching read-only B1 evidence.')
        vendor = vendor_cpu_points(b1['sid_word'])
        clusters, little, big, changes = struct.unpack_from('<4I', grant, 48)
        expected = {'clusters': clusters, 'max_mhz': [row['mhz'] for row in vendor['points']],
                    'vendor_mv': [row['mv'] for row in vendor['points']], 'bin_column': vendor['bin_column'],
                    'changes_per_boot': changes}
        require(p['cpu'] == expected and [little, big] == expected['max_mhz'],
                'CPU policy or grant differs from the SID bin vendor maxima.')
    return p, frozen


def template_check(raw, image):
    info = host.image_manifest(image)
    decoded = host.session_decode(raw, info['manifest'], info['sha256'])
    require(struct.unpack_from('<2Q', raw, 24) == (0, 0), 'Cold session requires zero epoch and generation.')
    if decoded['flags'] & host.BIND_SID:
        require(struct.unpack_from('<Q', raw, 312)[0] and any(raw[320:352]), 'Cold SID binding required.')
    grant = decoded['extents'].get(host.CPU_GRANT)
    if grant is not None:
        require(struct.unpack_from('<2Q', grant, 32) == (0, 0), 'Cold CPU grant requires zero epoch and generation.')
    return decoded


def flat_runtime(image):
    """Validate allocated sections and flatten only initialized runtime bytes.

    Load segments can span linker gaps and the NOBITS session reservation.
    The named monitor reservations also have no file bytes to upload.
    """
    import io
    from elftools.elf.elffile import ELFFile
    raw = Path(image).read_bytes() if isinstance(image, (str, Path)) else bytes(image)
    elf = ELFFile(io.BytesIO(raw))
    require(elf['e_type'] == 'ET_EXEC' and elf['e_machine'] == 'EM_AARCH64' and elf['e_entry'] == 0x41000000,
            'Runtime must be an AArch64 executable at the slot entry.')
    loads = [p for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
    require(any(p['p_flags'] & 1 and p['p_vaddr'] <= 0x41000000 and
                0x41000004 <= p['p_vaddr'] + p['p_filesz'] for p in loads),
            'Runtime entry has no initialized executable instruction.')
    allocated = [s for s in elf.iter_sections() if s['sh_flags'] & 2 and s['sh_size']]
    session = [s for s in allocated if s.name == '.storage_session']
    require(len(session) <= 1, 'Duplicate session reservation.')
    reservations = {'.storage_session': (0x41f00000, 0x41f01000),
                    '.result': (0x42000000, 0x42004000),
                    '.contract': (0x42004000, 0x42005000),
                    '.monitor_bss': (0x42005000, 0x421f0000),
                    '.boot_stack': (0x42005000, 0x421f0000),
                    '.tables': (0x421f0000, 0x42200000)}
    occupied = []
    for section in allocated:
        low, high = int(section['sh_addr']), int(section['sh_addr'] + section['sh_size'])
        if section.name in reservations:
            start, end = reservations[section.name]
            valid = (section['sh_type'] == 'SHT_NOBITS' and section['sh_flags'] & 3 == 3 and
                     not section['sh_flags'] & 4 and start <= low < high <= end)
            if section.name in ('.storage_session', '.result', '.contract'):
                valid = valid and (low, high) == (start, end)
            elif section.name == '.monitor_bss':
                valid = valid and low == start
            elif section.name == '.boot_stack':
                valid = valid and low % 4096 == 0 and high - low == 16384
            elif section.name == '.tables':
                valid = valid and low == start
            require(valid, 'Invalid fixed runtime reservation: ' + section.name)
        else:
            require(0x41000000 <= low < high <= 0x41f00000,
                    'Allocated section overlaps reserved memory: ' + section.name)
        require(all(high <= start or end <= low for start, end in occupied),
                'Overlapping allocated runtime sections.')
        occupied.append((low, high))
        require(any(p['p_vaddr'] <= low and high <= p['p_vaddr'] + p['p_memsz'] for p in loads),
                'Allocated runtime section has no load segment: ' + section.name)
        if section['sh_type'] != 'SHT_NOBITS':
            require(any(p['p_vaddr'] <= low and high <= p['p_vaddr'] + p['p_filesz'] and
                        section['sh_offset'] - p['p_offset'] == low - p['p_vaddr'] for p in loads),
                    'Initialized runtime section has no file backing: ' + section.name)
    occupied = []
    require(loads, 'Runtime has no load segment.')
    for p in loads:
        low, high = int(p['p_vaddr']), int(p['p_vaddr'] + p['p_memsz'])
        require(0 <= p['p_filesz'] <= p['p_memsz'], 'Invalid runtime load size.')
        if high == low:
            continue
        image_end = 0x41f01000 if session else 0x41f00000
        require((0x41000000 <= low < high <= image_end) or
                (not p['p_filesz'] and 0x42000000 <= low < high <= 0x42200000),
                'Runtime load overlaps reserved memory.')
        require(not p['p_filesz'] or low + p['p_filesz'] <= 0x41f00000,
                'Initialized runtime bytes overlap reserved memory.')
        require(all(high <= start or end <= low for start, end in occupied), 'Overlapping load memory.')
        occupied.append((low, high))
    segments = [p for p in loads if p['p_filesz']]
    require(segments, 'Runtime has no initialized load segment.')
    end = max(int(p['p_vaddr'] + p['p_filesz']) for p in segments)
    result = bytearray(end - 0x41000000)
    for p in segments:
        address, data = int(p['p_vaddr']), p.data()
        require(len(data) == p['p_filesz'], 'Truncated runtime load bytes.')
        offset = address - 0x41000000
        result[offset:offset + len(data)] = data
    return bytes(result)


class OrdinaryCard:
    """Stream a held ordinary source and detect mutation during helper reuse."""
    def __init__(self, path, package):
        self.path = Path(os.path.abspath(path))
        self.package = package
        self.fd = self.open()
        try:
            self.before = os.fstat(self.fd)
            require(stat.S_ISREG(self.before.st_mode) and self.before.st_size > 0 and self.before.st_size % 512 == 0,
                    'Card source must be a nonempty ordinary file of whole sectors.')
            self.bytes = self.before.st_size
            self.sectors = self.bytes // 512
            self.sha256 = self.hash()
        except BaseException:
            os.close(self.fd)
            raise

    def open(self):
        directory = self.package.directory(self.path.parent)
        try:
            return os.open(self.path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
        finally:
            os.close(directory)

    @staticmethod
    def identity(info):
        return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)

    def hash(self):
        digest = hashlib.sha256()
        for offset in range(0, self.bytes, 1 << 20):
            count = min(1 << 20, self.bytes - offset)
            data = os.pread(self.fd, count, offset)
            require(len(data) == count, 'Card source produced a short read.')
            digest.update(data)
        self.check()
        return digest.hexdigest()

    def check(self):
        require(self.identity(os.fstat(self.fd)) == self.identity(self.before), 'Card source changed while planning.')
        current = self.open()
        try:
            require(self.identity(os.fstat(current)) == self.identity(self.before), 'Card source path changed while planning.')
        finally:
            os.close(current)

    def read(self, first, blocks):
        require(0 <= first and 0 < blocks <= self.sectors - first, 'Card read leaves source bounds.')
        data = os.pread(self.fd, blocks * 512, first * 512)
        require(len(data) == blocks * 512, 'Card range produced a short read.')
        return data

    def close(self):
        os.close(self.fd)


def publish(staging, output):
    """Publish one complete directory without replacing any existing destination."""
    import ctypes
    for path in [p for p in staging.rglob('*') if p.is_dir()] + [staging]:
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    library = ctypes.CDLL(None, use_errno=True)
    rename = library.renameat2
    rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(staging), -100, os.fsencode(output), 1):  # RENAME_NOREPLACE
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(output))
    fd = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def prepare(args):
    """Freeze a verified offline bundle and backup-first plans; never open a device."""
    import contextlib
    import tempfile
    package = module('card_package', 'tools/prepare-microsd-boot.py')
    slots = module('card_slots', 'tools/boot-slots.py')
    updates = module('card_updates', 'tools/storage-update.py')
    checker = module('card_link', 'tools/sunstone-image-link.py')
    # These inputs are snapshotted once. Helpers consume only the frozen copies.
    read = lambda path: package.read_regular(path, slots.LOAD_LIMIT - slots.LOAD_BASE)
    inputs = {name: read(getattr(args, name)) for name in
              ('image', 'link_check', 'session', 'policy', 'stage2', 'stage2_elf', 'allocation', 'card', 'boot0')}
    assets = [(load, read(path)) for path, load in (slots.parse_load(value) for value in args.asset)]
    require(len(assets) <= 2, 'A card slot has room for at most two prepacked assets.')
    boot0 = inputs['boot0']
    require(len(boot0) == package.BOOT0_BYTES and sha(boot0) == package.BOOT0_HASH, 'Pinned boot0 identity differs.')
    packer = read(args.packer) if getattr(args, 'packer', None) else None
    if packer is not None:
        require(len(packer) == package.PACKER_BYTES and sha(packer) == package.PACKER_HASH, 'Pinned packer identity differs.')
    # Failed work remains in a private hidden sibling. Only a complete bundle gets the requested name.
    output = Path(os.path.abspath(args.output))
    parent = package.directory(output.parent)
    os.close(parent)
    require(not os.path.lexists(output), 'Bundle destination already exists.')
    out = Path(tempfile.mkdtemp(prefix='.' + output.name + '.partial-', dir=output.parent))
    slots.write_new(out / 'image.elf', inputs['image'])
    linked = checker.check(out / 'image.elf')
    report = metadata(inputs['link_check'])
    require(report.get('passed') is True and all(report.get(k) == v for k, v in linked.items()),
            'Link report differs from the actual ELF checker result.')
    elf = checker.Elf(inputs['image'])
    names = {symbol['name']: symbol for symbol in elf.symbols if symbol['shndx'] != 0}
    require('os_resident_test_profile' not in names, 'Stop-on-finish diagnostic cannot be a production card image.')
    profile = names.get('os_contract_profile')
    require(profile is not None and profile['size'] >= 16 and
            struct.unpack_from('<Q', elf.read(profile['value'], profile['size']), 8)[0] == 0x415337334f524331,
            'The linked OS does not admit the resident lifetime.')
    require(linked['manifest'] is not None, 'Card bundle requires one linked application manifest.')
    decoded = cold_profile(inputs['session'], inputs['image'], linked)
    asset_check(linked['manifest'], decoded, assets)
    monitor_check(inputs['stage2_elf'], inputs['stage2'], package)
    runtime = flat_runtime(inputs['image'])
    allocation, card = metadata(inputs['allocation']), metadata(inputs['card'])
    p, evidence = policy_check(inputs['policy'], inputs['session'], sha(inputs['image']), sha(inputs['stage2']),
                              linked, decoded, card, lambda path: read(Path(args.policy).parent / path))
    require(p['evidence_class'] != 'model-fixture' or args.model_fixture,
            'Modeled evidence requires explicit --model-fixture.')
    with contextlib.ExitStack() as stack:
        original = OrdinaryCard(args.backup, package)
        stack.callback(original.close)
        current = OrdinaryCard(args.card_image, package)
        stack.callback(current.close)
        slots.check_allocation(allocation, original.sectors, original.read(0, 1))
        slots.check_allocation(allocation, current.sectors, current.read(0, 1))
        require(allocation.get('backup') == {'bytes': original.bytes, 'sha256': original.sha256},
                'Original full backup differs from the allocation.')
        require(original.bytes == current.bytes, 'Original backup and current card capacity differ.')
        updates.card_record(card, current.sectors)
        frozen = {'runtime.bin': runtime, 'session-template.bin': inputs['session'],
                  'stage2.bin': inputs['stage2'], 'stage2.elf': inputs['stage2_elf'], 'link-check.json': inputs['link_check'],
                  'cold-policy.json': inputs['policy'], 'allocation.json': inputs['allocation'], 'card.json': inputs['card'],
                  'boot0.bin': boot0, **{'evidence-' + name + '.bin': data for name, data in evidence.items()}}
        frozen.update({'asset-' + str(index) + '.bin': data for index, (_, data) in enumerate(assets)})
        if packer is not None:
            frozen['packer.bin'] = packer
        for name, data in frozen.items():
            slots.write_new(out / name, data)
        monitor = {'schema': 1, 'kind': package.INPUT_KIND,
                   'monitor': {'format': package.MONITOR_FORMAT, 'path': 'stage2.bin',
                               'bytes': len(inputs['stage2']), 'sha256': sha(inputs['stage2'])}}
        slots.write_new(out / 'monitor-input.json', (json.dumps(monitor, indent=2) + '\n').encode())
        package.prepare(SimpleNamespace(input=out / 'monitor-input.json', boot0=out / 'boot0.bin',
                                        packer=out / 'packer.bin' if packer is not None else None,
                                        restore_variant=None, output=out / 'boot'))
        original.check(); current.check()
        slot = slots.plan(SimpleNamespace(allocation=out / 'allocation.json', card_image=current.path,
                         image=str(out / 'runtime.bin') + '@0x41000000', entry=0x41000000,
                         os_session=out / 'session-template.bin', legacy_os_session=False,
                         asset=[str(out / ('asset-' + str(i) + '.bin')) + '@' + hex(load)
                                for i, (load, _) in enumerate(assets)],
                         container=out / 'boot.manifest.json', output=out / 'slot'))
        original.check(); current.check()
        storage = updates.plan(out / 'slot/plan.json', out / 'allocation.json', current.path, card, out / 'storage')
        original.check(); current.check()
        result = {'schema': BUNDLE, 'physical_qualified': False, 'evidence_class': p['evidence_class'],
                  'policy_sha256': sha(inputs['policy']), 'board_identity': p['identity'], 'target': card['target'],
                  'card_cid': card['cid'], 'original_backup': {'bytes': original.bytes, 'sha256': original.sha256},
                  'current_card_image': {'bytes': current.bytes, 'sha256': current.sha256},
                  'allocation_sha256': sha(inputs['allocation']), 'image_sha256': sha(inputs['image']),
                  'stage2_sha256': sha(inputs['stage2']), 'manifest_sha256': decoded['manifest_sha256'],
                  'workers': linked['manifest']['workers'], 'canonical_session_sha256': sha(canonical_session(inputs['session'])),
                  'cpu_authority': host.CPU_GRANT in decoded['extents'],
                  'slot': slot['slot'], 'generation': slot['generation'], 'extents': slot['extents'],
                  'storage_sessions': len(storage['sessions']),
                  'evidence_files': {name: 'evidence-' + name + '.bin' for name in evidence},
                  'files': {str(path.relative_to(out)): {'bytes': path.stat().st_size, 'sha256': sha(read(path))}
                            for path in sorted(out.rglob('*')) if path.is_file()},
                  'physical_prerequisites': [
                      'Card capture provenance, CID and board identity verified independently',
                      'Coordinator approval and a reviewed physical writer transport for every storage session',
                      'Verified range backups before the backup-first storage sessions',
                      'Cold predecessor, remote writers, cache and CPU ownership admitted before loading',
                      'Native OS session admission after stage-2 publication',
                      'Memory, rail, thermal, autonomous boot and watchdog recovery qualification',
                      'Recovery for interrupted boot0 and monitor-package installation']}
        slots.write_new(out / 'bundle.json', (json.dumps(result, indent=2) + '\n').encode())
        for path in out.rglob('*'):
            if path.is_file():
                path.chmod(0o400)
        publish(out, output)
    return result
