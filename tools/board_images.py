"""Offline image/session preparation for tools/board.py; no device access."""
import hashlib
import io
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
CONTROL = CACHE/'evidence/wdt-breadcrumbs-1031/control/offline-proof/prepared'
GPU_TREE = Path.home()/'git/sunstoneos-worktrees/gpu-bisect-1031-crumbs'
GPU_TEMPLATE = CACHE/'evidence/sol-gpu-completion-1031/prepared'
CLANG, LLD = Path('/usr/bin/clang-21'), CACHE/'host/usr/bin/ld.lld-21'


def load(name, path):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def require(ok, message):
    if not ok:
        raise ValueError(message)


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
    require(profile in (finite.STARTUP, finite.INFERENCE, finite.RUNTIME, finite.RESTART,
                        finite.SWITCH, finite.SDK, finite.POWER), 'Unsupported finite NPU profile.')
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
    if kind=='probe' and 'sunstone_probe_marker' in names:
        native = load('native_probe_contract', ROOT/'tools/board_probe.py').inspect(image)
        return {'symbols':names,'watchdog':native['watchdog'],'generic':False,'entry_guard':False,
                'descriptor':descriptor,'native_probe':native}
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
                    descriptor=descriptor, finite_npu=True)
    marker = host.watchdog_marker(image) if names else descriptor.get('watchdog')
    require(marker is not None, 'Image does not admit the host-armed watchdog; rebuild with RTC breadcrumbs.')
    require(set(marker) == {'mode', 'session_seconds'} and all(type(v) is int for v in marker.values()) and
            marker['mode'] & 0xff0f == 1 and 1 <= marker['mode'] >> 4 <= 11 and
            1 <= marker['session_seconds'] <= 600, 'Invalid watchdog marker.')
    generic = 'ss_result_record' in names
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
            require('os_npu_client_model' in names, 'NPU kind requires the linked OS NPU service.')
        if generic and 'os_npu_client_model' in names:
            require('npu_pool' in geometry and 'npu_stop' in geometry,
                    'NPU image requires its linked pool and mandatory stop receipt.')
    else:
        require(descriptor.get('purpose')=='watchdog-reset-diagnostic','Raw probes admit only the intentional watchdog-reset diagnostic; use the native ELF contract.')
        require(descriptor.get('schema') == 'sunstone-board-probe-v1', 'Probe requires a hash-bound .board.json descriptor.')
        require(descriptor.get('entry') == 0x41000000 and descriptor.get('report_address') == 0x42000000,
                'Probe entry/report reservations differ.')
    return {'symbols': names, 'watchdog': marker, 'generic': generic,
            'entry_guard': 'os_cpu_return_entry_profile' in names, 'descriptor': descriptor,
            'report_geometry': geometry}


def policy(info, args, session=None):
    """Approval adds authority; it never creates a storage grant or a backup."""
    d = info['descriptor']
    approved = args.approved_by == 'coordinator'
    storage = d.get('storage_write') or args.storage_write
    require(not storage or approved, 'Storage write requires --approved-by coordinator.')
    if storage:
        # Existing session producers own backup-first partition/slot grants.
        # This runner cannot authorize a raw probe by a command-line boolean.
        require(False, 'Storage writes require the existing backup-first storage runner and session grant.')
    reviewed = {'cpu-l': (1794, 1000), 'cpu-b': (2002, 1050),
                'gpu': (1008, 960), 'npu': (852, 800), 'rails': (0, 800)}
    for point in d.get('operating_points', []):
        require(isinstance(point, dict) and set(point) == {'block', 'mhz', 'mv'}, 'Invalid operating point.')
        require(approved or reviewed.get(point['block']) == (point['mhz'], point['mv']),
                'Operating point requires --approved-by coordinator.')
    if session is not None and len(session) >= 96:
        host = load('board_policy_records', (args.host_tree or ROOT) / 'apps/os/host.py')
        if info['generic']:
            grant = host.session_decode(session)['extents'].get(host.CPU_GRANT)
        else:
            grant = session[-96:] if struct.unpack_from('<Q', session, len(session)-96)[0] == 0x4153373343505531 else None
        if grant:
            clusters, little, big, changes = struct.unpack_from('<4I', grant, 48)
            require(0 <= clusters <= 3 and changes <= 64, 'Invalid CPU grant limits.')
            require(approved or ((little == 1794 if clusters & 1 else little == 0) and
                                (big == 2002 if clusters & 2 else big == 0)),
                    'CPU grant outside reviewed operating points requires --approved-by coordinator.')


def launcher(out, size, mode, seconds=60, tree=ROOT):
    """Reuse the board launcher with the OS contract, full session and armed WDT."""
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
    (out / 'board-launch-base.c').write_text(source)
    for name, path in [('launch.c', 'apps/board/launch.c'), ('launch.S', 'platform/a733-smp-observe/launch.S'),
                       ('launch.ld', 'platform/a733-smp-observe/launch.ld'), ('boot.h', 'platform/a733-smp-cached/boot.h'),
                       ('a733_board_application.h', 'include/a733_board_application.h')]:
        shutil.copy2(tree / path, out / name)
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
             '-Os', '-g', '-Wall', '-Wextra', '-Werror', '-DSUNSTONE_BOARD_SUPERVISED=1',
             f'-DA733_BOARD_SUPERVISED_SECONDS={seconds}',
             '-DA733_SMP_CONTRACT_MAGIC=0x415337334f534331ULL', '-I' + str(out)]
    commands = [[str(Path('/usr/bin/clang-21')), *flags, '-c', str(out / n), '-o', str(out / o)]
                for n, o in [('launch.S', 'entry.o'), ('launch.c', 'launch.o')]]
    commands += [[str(CACHE / 'host/usr/bin/ld.lld-21'), '-T', str(out / 'launch.ld'),
                  str(out / 'entry.o'), str(out / 'launch.o'), '-o', str(out / 'launch.elf')],
                 ['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', str(out / 'launch.elf'), str(out / 'launch.bin')]]
    with (out / 'build.log').open('w') as log:
        for cmd in commands:
            subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
    require(0 < (out / 'launch.bin').stat().st_size <= 0xc00, 'Launcher exceeds SRAM reservation.')


def prepare(args, info, live, out):
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
    template = Path(template)
    session_path = template / 'session.bin' if (template/'session.bin').exists() else template/'board/session-template.bin'
    session_path = getattr(args, 'records', None) or session_path
    raw = session_path.read_bytes()
    image_info = records.image_manifest(args.image)
    if struct.unpack_from('<Q', raw, 592)[0] == records.SESSION_MAGIC:
        old = records.session_decode(raw)
        services = {k: v for k, v in old['extents'].items() if k in records.SERVICE_BYTES}
        devices, flags = old['devices'], old['flags']
    else:
        require(len(raw) == 904, 'Supply a typed session bundle for NPU/audio images.')
        services = {records.THERMAL: raw[592:808], records.CPU_GRANT: raw[808:904]}
        devices, flags = 0, records.BIND_SID
    prefix = bytearray(raw[:592])
    epoch, generation, lease = [secrets.randbits(62) | 1 for _ in range(3)]
    struct.pack_into('<3Q', prefix, 24, epoch, generation, lease)
    parts, address, identity = base.image_parts(Path(args.image))
    prefix[48:80] = identity
    if records.CPU_GRANT in services:
        grant = bytearray(services[records.CPU_GRANT])
        if struct.unpack_from('<I', grant, 48)[0]:
            struct.pack_into('<2Q', grant, 32, epoch, generation)
        services[records.CPU_GRANT] = grant
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
    meta = producer.prepare(args.image, prefix, services, inputs, out, devices=devices, flags=flags)
    session = (out/'session.bin').read_bytes()
    policy(info, args, session)
    launcher(out, len(session), info['watchdog']['mode'], info['watchdog']['session_seconds'], tree)
    board = load('runner_request', ROOT/'apps/board/prepare.py')
    request, contract = board.request_bytes(address, identity, epoch, generation, lease, session)
    request = struct.pack('<Q', 0x415337334f534c31) + request[8:]
    contract = bytearray(contract)
    struct.pack_into('<Q', contract, 184, len(session))
    uploads = []
    for i, part in enumerate(parts):
        if part['data']:
            uploads.append((part['address'], write(out/f'segment-{i}.bin', part['data'])))
    uploads += [(u['address'], out/u['name']) for u in meta['uploads']]
    # The launcher checks both sentinel words in this page before entry.
    blank = bytearray(16384)
    struct.pack_into('<I', blank, 0, 0x73307330)
    uploads += [(0x41f00000, out/'session.bin'), (0x42000000, write(out/'result-zero.bin', blank)),
                (0x42004000, write(out/'contract.bin', contract)), (0x52c00, write(out/'request.bin', request)),
                (0x52d00, write(out/'report-zero.bin', bytes(128))), (0x52000, out/'launch.bin')]
    returning = load('prepare_return_geometry', tree/'apps/os/cpu-return-host.py')
    plan = dict(meta, epoch=epoch, generation=generation, uploads=uploads, launch=['exe', '0x52000'],
                watchdog=info['watchdog'], format='generic', geometry=info['report_geometry'],
                return_host=returning, input_regions=meta['uploads'], image_ram=returning.IMAGE_RAM)
    plan['preserve_regions'] = preservation_regions(plan)
    validate_uploads(plan)
    return plan


def preservation_regions(plan):
    """Union the linked image/monitor, manifest, input and device DRAM extents."""
    geo = plan['geometry']
    extents = [plan['image_ram'], (geo['report_address'], geo['report_bytes']),
               (geo['output_address'], geo['output_bytes'])]
    extents += [(geo[name][0], geo[name][1] - geo[name][0])
                for name in ('monitor', 'cached_writable')]
    extents += [(r['address'], r['bytes']) for r in geo['application_regions']]
    extents += [(r['address'], r['bytes']) for r in plan['input_regions']]
    if 'npu_pool' in geo:
        extents.append((geo['npu_pool']['address'], geo['npu_pool']['bytes']))
    merged = []
    for address, size in sorted(extents):
        require(type(address) is int and type(size) is int and
                0x40000000 <= address < address + size <= 0x80000000,
                'Preservation extent is outside admitted DRAM.')
        high = address + size
        if merged and address <= merged[-1][0] + merged[-1][1]:
            low, count = merged[-1]
            merged[-1] = (low, max(low + count, high) - low)
        else:
            merged.append((address, size))
    return merged


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
        require(sha(item['file']) == item['sha256'], 'Saved DRAM changed before restoration.')
        owner.upload(item['address'], Path(item['file']))


def write(path, data):
    Path(path).write_bytes(data)
    return Path(path)


def capture(owner, plan, args):
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
    # Preserve every returned object before decoding any of them. A malformed
    # application result must not hide the native retirement or GPU receipt.
    copied = {name: owner.read(address, size, out/(name + '.bin'))
              for name, address, size in reads}
    require(owner.record.get('normal_return') and not owner.record.get('dram_lost'),
            'DRAM restoration requires a verified normal return.')
    if hasattr(records, 'mandatory_return'):
        decoded = records.mandatory_return(copied, geo, plan['epoch'], plan['generation'])
    else:
        require('npu_pool' not in geo and 'npu_stop' not in geo, 'NPU retirement parser is missing.')
        decoded = records.decode_return_report(copied['return-report'], plan['epoch'])
    owner.record['native_retirement_validated'] = True
    owner.journal()
    try:
        valid, payload = True, b''
        if 'npu_stop' in decoded:
            decoded['npu_capture_sha256'] = hashlib.sha256(copied['npu-stop']).hexdigest()
            try:
                decoded['npu_report'] = records.npu_capture.decode(copied['npu-stop'], plan['epoch'], plan['generation'])
                valid = not (decoded['npu_report']['mismatches'] or decoded['npu_report']['refused'])
                decoded['npu_capture_status'] = 'decoded'
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded.update(npu_capture_status='malformed', npu_error=str(error))
        if 'cpu_rails' in geo:
            decoded['cpu_rail_capture_sha256'] = hashlib.sha256(copied['cpu-rails']).hexdigest()
            try:
                decoded['cpu_rails'] = records.decode_rails(copied['cpu-rails'])
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded['cpu_rail_error'] = str(error)
        decoded['output_capture_sha256'] = hashlib.sha256(copied['output-record']).hexdigest()
        try:
            info = records.host.image_manifest(args.image)
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
                                                                     plan['epoch'], plan['generation'])
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
                decoded['thermal']['qualification_passed'] = False
                decoded['thermal_capture_status'] = 'decoded'
                if decoded['thermal'].get('classification') != 'complete':
                    valid = False
                    decoded['thermal_error'] = 'Thermal window is '+str(decoded['thermal'].get('classification'))
            except (ValueError, KeyError, struct.error) as error:
                valid = False
                decoded.update(thermal_capture_status='malformed', thermal_error=str(error))
        else:
            decoded['thermal_capture_status'] = 'absent'
        decoded['qualification_passed'] = False
        return {'valid': valid, 'payload': payload, 'report': decoded}
    finally:
        require(plan['preserve_regions'] == preservation_regions(plan),
                'Preservation plan changed after launch.')
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


def post_rail_result(values, log):
    """The approved DCDC5-only helper requires DCDC3 already at 800 mV."""
    followup = load('board_post_rail_result', ROOT/'apps/os/cpu-return-followup.py')
    return followup.helper_result({'complete': True, 'transport_failed': False,
        'automatic_access': True, 'mode': 'kernel',
        'result': dict(zip(('magic', 'stage', 'status'), values[:3]))}, log)
