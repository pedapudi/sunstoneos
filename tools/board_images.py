"""Offline image/session preparation for tools/board.py; no device access."""
import hashlib
import io
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'


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


def inspect(image, kind, descriptor=None, host_tree=None):
    host = load('board_records', ROOT / 'apps/os/host.py')
    names = symbols(image) if Path(image).read_bytes()[:4] == b'\x7fELF' else {}
    descriptor = descriptor or {}
    if descriptor:
        require(descriptor.get('image_sha256') == sha(image), 'Image descriptor hash differs.')
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
            cpu = load('inspect_cpu_geometry', ROOT/'apps/cpu-runtime-qualification/host.py')
            geometry = cpu.geometry(Path(image))
        else:
            dev = load('inspect_dev', ROOT/'tools/dev-run.py')
            legacy = load('inspect_legacy', (host_tree or dev.GPU_TREE)/'apps/os/cpu-return-host.py')
            geometry = legacy.geometry(Path(image))
        if kind=='npu':
            require('os_npu_client_model' in names, 'NPU kind requires the linked OS NPU service.')
    else:
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
        host = load('board_policy_records', ROOT / 'apps/os/host.py')
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


def launcher(out, size, mode, seconds=60):
    """Reuse the board launcher with the OS contract, full session and armed WDT."""
    board = load('runner_board_prepare', ROOT / 'apps/board/prepare.py')
    source = (ROOT / 'platform/a733-smp-supervised/launch.c').read_text()
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
        shutil.copy2(ROOT / path, out / name)
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
    dev = load('runner_dev', ROOT / 'tools/dev-run.py')
    base = load('runner_base', ROOT / 'tools/check-a733-smp.py')
    records = load('runner_records', ROOT / 'apps/os/host.py')
    if args.kind == 'probe':
        out.mkdir()
        raw = Path(args.image).read_bytes()
        # Raw probes have an explicit descriptor and may only use the kernel interval.
        require(raw[:4] != b'\x7fELF' and 0 < len(raw) <= 0x100000, 'Probe requires a flat kernel image <= 1 MiB.')
        return {'epoch': 1, 'generation': 1, 'launch': ['reset64', '0x41000000'],
                'uploads': [(0x41000000, Path(args.image)), (0x42000000, write(out/'blank.bin', bytes(16384)))],
                'watchdog': info['watchdog'], 'format': 'probe'}
    template = args.template or (dev.GPU_TEMPLATE if args.kind == 'gpu-return' else dev.CONTROL)
    if not info['generic']:
        tree = args.host_tree or dev.GPU_TREE
        host = load('runner_legacy_host', tree / 'apps/os/cpu-return-host.py')
        meta = dev.prepare(host, args.image, template, out, live)
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
    class Bin:
        pass
    # Reuse the current driver-derived bin map instead of duplicating it.
    text = (ROOT/'drivers/clock/a733_cpu_clock.c').read_text()
    import re
    codes = [int(v, 16) for v in re.search(r'codes\[\]=\{([^}]*)\}', text)[1].split(',')]
    Bin.bin_column = staticmethod(lambda word: codes.index((word>>16&255) or (word>>8&255))+1
                                 if ((word>>16&255) or (word>>8&255)) in codes else None)
    findings, derived = dev.state_check(live['words'], Bin)
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
    producer = load('runner_session_prepare', ROOT/'apps/os/prepare.py')
    meta = producer.prepare(args.image, prefix, services, inputs, out, devices=devices, flags=flags)
    session = (out/'session.bin').read_bytes()
    policy(info, args, session)
    launcher(out, len(session), info['watchdog']['mode'], info['watchdog']['session_seconds'])
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
    return dict(meta, epoch=epoch, generation=generation, uploads=uploads, launch=['exe', '0x52000'],
                watchdog=info['watchdog'], format='generic', geometry=info['report_geometry'])


def write(path, data):
    Path(path).write_bytes(data)
    return Path(path)


def capture(owner, plan, args):
    out = owner.evidence
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
    records = load('runner_result_host', ROOT/'apps/os/cpu-return-host.py')
    result = records.capture_result(owner.tx, args.image, plan['epoch'], plan['generation'], out/'output-record.bin')
    # Copy the secure return report too; the opaque payload alone is not proof
    # of all-core retirement. Its ABI is shared by generic and legacy images.
    addr, size = plan['geometry']['report_address'],plan['geometry']['report_bytes']
    report = owner.read(addr, size, out/'return-report.bin')
    decoded = records.decode_return_report(report, plan['epoch'])
    return {'valid': True, 'payload': result['payload'], 'report': decoded}


def rail_result(values, log):
    """Pinned helper's status, PMIC readback and boot-clock proof."""
    import re
    complete = ('COMPLETE board bundle 33: DCDC5 restored to 800 mV with DCDC3 '
                'already at 800 mV and all boot clock words')
    def row(label):
        rows = re.findall(r'^'+label+r' (.+)$',log,re.M)
        require(len(rows)==1, 'Rail helper inspection absent or ambiguous: '+label)
        return {k:int(v,0) for k,v in (x.split('=') for x in rows[0].split())}
    supply = row('SUPPLY')
    already = (values[:3] == (0x534b4552,5,1) and
               log.splitlines().count('REFUSED board bundle 33: CPU supplies') == 1 and
               supply['dcdc3_uv'] == supply['dcdc5_uv'] == 800000 and
               supply['bin'] == 4 and supply['dcxo_hz'] == 26000000 and
               not re.search(r'^(?:FAIL|STOPPED|VOLT)\b',log,re.M))
    require(already or (values[:3] == (0x534b4552, 5, 0) and log.splitlines().count(complete) == 1 and
            not re.search(r'^(?:FAIL|REFUSED|STOPPED)\b',log,re.M)), 'Rail helper refused or did not complete normally.')
    if already:
        # The pinned helper refuses its write before any transition when both
        # rails are already at the requested point. Inspect its earlier clocks.
        for name,pll,mux,mhz in [('CPU-L',0xf8802600,0x03010000,494),('CPU-B',0xf8802700,0x03000000,1014),
                                ('DSU',0xf8802600,0x03010105,494)]:
            f = row(name+' before')
            require(f['pll']==pll and f['mux']==mux and f['mhz']==mhz and
                    not(f['pat0']&(3<<29) or f['pat1']>>31 or f['lfm']>>31), 'Rail helper boot clocks differ.')
        return {'dcdc3_uv':800000,'dcdc5_uv':800000,'boot_clock_domains':3,'already_normalized':True}
    require(supply['dcdc3_uv']==800000 and supply['dcdc5_uv']==1000000 and supply['bin']==4 and
            supply['dcxo_hz']==26000000, 'Rail helper entry supplies/bin/reference differ.')
    volt = row('VOLT')
    require(set(volt)=={'dcdc5_selector','dcdc5_uv'} and volt['dcdc5_uv']==800000 and
            volt['dcdc5_selector']&127==30, 'Rail helper voltage readback differs.')
    for name,pll,mux,mhz in [('CPU-L',0xf8802600,0x03010000,494),('CPU-B',0xf8802700,0x03000000,1014),
                            ('DSU',0xf8802600,0x03010105,494)]:
        for phase in ('before','after'):
            f = row(name+' '+phase)
            require(f['pll']==pll and f['mux']==mux and f['mhz']==mhz and
                    not(f['pat0']&(3<<29) or f['pat1']>>31 or f['lfm']>>31), 'Rail helper boot clocks differ.')
    return {'dcdc3_uv':800000,'dcdc5_uv':800000,'boot_clock_domains':3}
