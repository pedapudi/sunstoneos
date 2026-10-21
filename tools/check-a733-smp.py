#!/usr/bin/env python3
"""Prepare an eight-core image, or run one qualified protected launch attempt.

Preparation is host-only. Launch cannot establish scheduler success: no result
channel survives the independent watchdog reset under the current contract.
"""
import argparse
from contextlib import contextmanager
import errno
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import sqlite3
import struct
import subprocess
import sys
import time
ROOT = Path(__file__).resolve().parents[1]
FEL_HASH = '4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e'
ROM_HASH = 'a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa'
REQUEST_MAGIC = 0x415337334c4e4331
CONTRACT = 0x42004000
REGIONS = [('launcher', 0x52000, 0xc00), ('request', 0x52c00, 128),
           ('report', 0x52d00, 128), ('stack', 0x53000, 0x1000), ('scratch', 0x71000, 128)]
PREREQUISITES = {'independent-watchdog-rom-return', 'stable-protected-fel', 'ddr-reservation', 'smp-launcher-software'}
# One sunxi-fel call writes a whole extent and reads it back. The recorded
# full-speed link moved about 0.57 MB/s; budget less than half of that for each
# byte written or read, on top of the fixed limit for small requests.
FEL_REQUEST_SECONDS = 15
FEL_BYTES_PER_SECOND = 250000
FEL_TIMEOUT_LIMIT = 300


def require(ok, message):
    if not ok: raise ValueError(message)


def fel_timeout(arguments):
    """Bound one request by the bytes it moves; refuse one that cannot finish."""
    arguments = [str(x) for x in arguments]; moved = 0
    for index, value in enumerate(arguments):
        if value == 'write': moved += Path(arguments[index + 2]).stat().st_size
        elif value == 'read': moved += int(arguments[index + 2])
    timeout = FEL_REQUEST_SECONDS + -(-moved // FEL_BYTES_PER_SECOND)
    require(timeout <= FEL_TIMEOUT_LIMIT, 'FEL request exceeds the bounded transfer time.')
    return timeout


def fel_request(fel, node, arguments, entry, pass_fds=()):
    """Run one sunxi-fel request; journal its bound and monotonic start and end."""
    timeout = fel_timeout(arguments)
    entry.update(timeout_seconds=timeout, started_monotonic=time.monotonic())
    try:
        result = subprocess.run([str(fel), '-d', f'{int(node.parent.name)}:{int(node.name)}', *map(str, arguments)],
                                capture_output=True, text=True, timeout=timeout, pass_fds=pass_fds)
    finally:
        entry['finished_monotonic'] = time.monotonic()
    entry.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
    require(result.returncode == 0, 'FEL request failed.')
    return result


def present_instance(access, config):
    """Return the port's FEL node and identity from one lstat, or (None, None).

    session_node maps the expected disconnect to absence, but the node can still
    disappear before a second lstat. Treat only that absence as an absent node.
    """
    candidate = access.session_node(config)
    if candidate is None: return None, None
    try: return candidate, access.identity(candidate)
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ENODEV): return None, None
        raise


def require_queue_port(database, port):
    """A board queue authorizes only the physical port recorded at its creation."""
    db = sqlite3.connect(Path(database).resolve().as_uri() + '?mode=ro', uri=True)
    try: row = db.execute("SELECT value FROM settings WHERE key='port'").fetchone()
    finally: db.close()
    require(row is not None and row[0] == port, f'Queue {database} is not bound to locked USB port {port}.')


def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save_json(path, value):
    temporary = path.with_suffix('.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
    temporary.replace(path)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)


def artifact(item):
    require(isinstance(item, dict) and set(item) == {'path', 'sha256'}, 'Invalid artifact reference.')
    path = Path(item['path'])
    require(path.is_absolute() and path.is_file() and digest(path) == item['sha256'], 'Artifact identity differs.')
    return path


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result); return result


def image_parts(path):
    """Parse the small required ELF64 subset without loading executable code."""
    data = path.read_bytes()
    require(len(data) >= 64 and data[:7] == b'\x7fELF\x02\x01\x01', 'Expected little-endian ELF64.')
    header = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    kind, machine, version, entry, phoff, shoff, _, ehsize, phsize, phnum, shsize, shnum, _ = header
    require((kind, machine, version, entry, ehsize, phsize, shsize) == (2, 183, 1, 0x41000000, 64, 56, 64), 'ELF contract differs.')
    require(0 < phnum < 64 and 0 < shnum < 4096 and phoff + phnum * phsize <= len(data) and shoff + shnum * shsize <= len(data), 'Invalid ELF tables.')
    segments = []
    for i in range(phnum):
        ptype, flags, offset, address, physical, filesz, memsz, alignment = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize)
        if ptype != 1: continue
        require(address == physical and 0x41000000 <= address < address + memsz <= 0x42200000 and
                filesz <= memsz and offset + filesz <= len(data) and flags & 3 != 3, 'Invalid load segment.')
        require(not (flags & 1) or address + memsz <= 0x41200000, 'Code exceeds reserved block.')
        require(not filesz or address + filesz <= 0x42000000, 'Image file bytes overlap result or contract.')
        require(all(address + memsz <= other['address'] or other['address'] + other['memory_bytes'] <= address for other in segments), 'ELF load allocations overlap.')
        segments.append({'address': address, 'memory_bytes': memsz, 'data': data[offset:offset + filesz]})
    require(any(s['address'] <= entry < s['address'] + len(s['data']) for s in segments), 'Entry has no file bytes.')
    symbols = {}
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(shnum)]
    for section in sections:
        if section[1] != 2: continue
        offset, size, link, entsize = section[4], section[5], section[6], section[9]
        require(link < shnum and entsize == 24 and size % 24 == 0 and offset + size <= len(data), 'Malformed symbol table.')
        strings = sections[link]; require(strings[4] + strings[5] <= len(data), 'Malformed string table.')
        names = data[strings[4]:strings[4] + strings[5]]
        for pos in range(offset, offset + size, 24):
            name, info, other, index, value, length = struct.unpack_from('<IBBHQQ', data, pos)
            require(name < len(names), 'Invalid symbol name.')
            end = names.find(b'\0', name); require(end >= 0, 'Unterminated symbol name.')
            label = names[name:end].decode('ascii', errors='replace')
            if label in {'a733_smp_build_id', '__a733_smp_result', '__a733_smp_contract', '__a733_smp_contract_end'}:
                require(label not in symbols, 'Duplicate image contract symbol.'); symbols[label] = (value, length)
    require(symbols.get('__a733_smp_result', (0,))[0] == 0x42000000 and
            symbols.get('__a733_smp_contract', (0,))[0] == CONTRACT and
            symbols.get('__a733_smp_contract_end', (0,))[0] == CONTRACT + 4096, 'Missing linker reservations.')
    address, size = symbols.get('a733_smp_build_id', (0, 0))
    require(size == 32 and 0x41000000 <= address <= 0x411fffe0, 'Invalid image build identity.')
    for segment in segments:
        offset = address - segment['address']
        if 0 <= offset <= len(segment['data']) - 32:
            return segments, address, segment['data'][offset:offset + 32]
    raise ValueError('Build identity is not file-backed.')


def request_bytes(build_address, build_id, epoch, generation, lease):
    require(len(build_id) == 32 and all(type(x) is int and 0 < x < 2**64 for x in (epoch, generation, lease)), 'Invalid launch identity.')
    request = struct.pack('<8Q', REQUEST_MAGIC, 2, 128, epoch, generation, generation ^ (2**64-1), lease, build_address) + build_id + bytes(32)
    contract = struct.pack('<6Q', 0, 1, 224, epoch, generation, generation ^ (2**64-1)) + build_id
    contract += struct.pack('<18Q', 0, 0, 0, 0, lease, 0x41, 1, 2, 0x5aa5a55a, 0xfa50392f, 0x20, 0, *([0]*6))
    require(len(request) == 128 and len(contract) == 224, 'Internal ABI mismatch.')
    return request, contract + bytes(4096 - len(contract))


def prepare(image, manifest_path, output, clang, lld):
    require(json.loads(manifest_path.read_text()).get('final_elf') == digest(image), 'Image differs from reviewed manifest.')
    segments, build_address, build_id = image_parts(image)
    output.mkdir(parents=True, exist_ok=False)
    for name in ('launch.S', 'launch.c', 'launch.ld', 'boot.h'):
        shutil.copy2(ROOT / 'platform/a733-smp' / name, output / name)
    commands = []
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(output)]
    def run(command):
        commands.append([str(x) for x in command]); result = subprocess.run(command, capture_output=True, text=True)
        with (output / 'build.log').open('a') as stream: stream.write(result.stdout + result.stderr)
        save_json(output / 'commands.json', commands); require(result.returncode == 0, 'Launcher build failed; inspect build.log.')
    for source, target in [('launch.S', 'entry.o'), ('launch.c', 'launch.o')]: run([clang, *flags, '-c', output / source, '-o', output / target])
    run([lld, '-T', output / 'launch.ld', output / 'entry.o', output / 'launch.o', '-o', output / 'launch.elf'])
    run(['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', output / 'launch.elf', output / 'launch.bin'])
    require(0 < (output / 'launch.bin').stat().st_size <= 0xc00, 'Launcher exceeds SRAM reservation.')
    shutil.copy2(image, output / 'image.elf'); shutil.copy2(manifest_path, output / 'image-input-manifest.json')
    payloads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'; (output / name).write_bytes(segment['data'])
            payloads.append({'name': name, 'address': segment['address']})
    epoch, generation, lease = (secrets.randbits(64) or 1 for _ in range(3))
    request, contract = request_bytes(build_address, build_id, epoch, generation, lease)
    for name, data in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]: (output / name).write_bytes(data)
    record = {'schema': 2, 'image_sha256': digest(image), 'launcher_sha256': digest(output / 'launch.bin'), 'build_id': build_id.hex(),
              'epoch': epoch, 'generation': generation, 'lease': lease, 'segments': payloads,
              'reservations': [{k:v for k,v in s.items() if k != 'data'} for s in segments],
              'files': {f.name: digest(f) for f in output.iterdir() if f.is_file()}, 'physical_access': False}
    save_json(output / 'prepared.json', record)
    return record


def validate_bundle(bundle):
    prepared = json.loads((bundle / 'prepared.json').read_text())
    require(prepared.get('schema') == 2 and prepared.get('physical_access') is False, 'Unsupported prepared bundle.')
    sizes = {}
    for name, expected in prepared['files'].items():
        require(Path(name).name == name, 'Invalid prepared filename.')
        data = (bundle / name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == expected, 'Prepared input changed.')
        sizes[name] = len(data)
    segments, address, identity = image_parts(bundle / 'image.elf')
    expected_segments = []
    for index, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{index}.bin'
            require((bundle / name).read_bytes() == segment['data'], 'Segment bytes differ from ELF.')
            expected_segments.append({'name': name, 'address': segment['address']})
    require(prepared['segments'] == expected_segments and prepared['image_sha256'] == digest(bundle / 'image.elf') and
            prepared['build_id'] == identity.hex(), 'Prepared ELF metadata differs.')
    request, contract = request_bytes(address, identity, prepared['epoch'], prepared['generation'], prepared['lease'])
    for name, expected in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]:
        require((bundle / name).read_bytes() == expected, 'Prepared launch input differs: ' + name)
    require(0 < (bundle / 'launch.bin').stat().st_size <= 0xc00 and prepared['launcher_sha256'] == digest(bundle / 'launch.bin'), 'Invalid launcher identity or size.')
    prepared['_sizes'] = sizes
    return prepared


def validate_software(value, bundle):
    require(value.get('schema') == 'a733-smp-launcher-software-v1' and value.get('host_protocol') is True and
            value.get('instruction_model') is True and value.get('physical_access') is False, 'Launcher software checks are missing.')
    for key, name in [('prepared', 'prepared.json'), ('image', 'image.elf'), ('launcher', 'launch.bin')]:
        require(artifact(value[key]) == (bundle / name).resolve(), 'Software evidence covers different launch inputs.')
    require(digest(artifact(value['host_tool'])) == digest(Path(__file__)), 'Software evidence covers another host tool.')
    for item in value.get('evidence', []): artifact(item)
    require(value.get('evidence'), 'Launcher software evidence is missing.')


def validate_qualification(value, port, sysfs):
    require(value.get('schema') == 'a733-independent-reset-v1' and value.get('qualified') is True, 'Independent recovery is not qualified.')
    require(value.get('target') == {'port':port, 'sysfs':sysfs, 'soc':'00001903'}, 'Recovery qualification covers another target.')
    require(value.get('watchdog') == {'mode':0x41, 'configuration':1, 'output':2, 'source':'rtc32k', 'clock_hz':32768}, 'Watchdog interval or clock source differs.')
    cases = value.get('cases', {})
    require(set(cases) == {'clock-source', 'masked-cpu-expiry', 'whole-system-reset', 'protected-rom-return', 'interconnect-stall-recovery'}, 'Recovery evidence does not cover independent reset and source requirements.')
    for item in cases.values(): artifact(item)
    require(value.get('physical_counter') == {'clock_hz':24000000, 'continuous_across_rmr':True}, 'Physical counter rate and RMR continuity are not qualified.')
    require(value.get('rom_sha256') == ROM_HASH, 'Recovery ROM identity differs.')


def claimed(database, test_id, owner, bundle, qualification, input_evidence, ddr):
    db = sqlite3.connect(database.resolve().as_uri() + '?mode=ro', uri=True)
    db.row_factory = sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0], 'Board queue is halted.')
        row = db.execute('SELECT * FROM tests WHERE id=?', (test_id,)).fetchone()
        require(row is not None and row['state'] == 'claimed' and row['owner'] == owner, 'Matching coordinator queue claim required.')
        submission, review = json.loads(row['submission']), json.loads(row['review'])
        require(set(submission['prerequisites']) == PREREQUISITES and set(review['prerequisites']) == PREREQUISITES, 'Queue prerequisite contract differs.')
        reviewed = {name: artifact(item) for name,item in review['prerequisites'].items()}
        require(reviewed['independent-watchdog-rom-return'] == qualification.resolve() and reviewed['stable-protected-fel'] == input_evidence.resolve() and reviewed['ddr-reservation'] == ddr.resolve(), 'Qualification inputs are not the reviewed artifacts.')
        validate_software(json.loads(reviewed['smp-launcher-software'].read_text()), bundle)
        paths = {artifact(item) for item in submission['artifacts']}
        require({(bundle / n).resolve() for n in ('prepared.json','image.elf','launch.bin')} <= paths, 'Queue does not cover exact prepared artifacts.')
    finally: db.close()


def sync_file(path):
    with path.open('rb') as stream: os.fsync(stream.fileno())


@contextmanager
def sealed_source(data):
    """Give FEL a fixed extent that cannot change when an ordinary path changes."""
    descriptor = os.memfd_create('a733-smp-transfer', os.MFD_ALLOW_SEALING | os.MFD_CLOEXEC)
    try:
        view = memoryview(data)
        while view:
            written = os.write(descriptor, view)
            require(written > 0, 'Incomplete immutable transfer source.')
            view = view[written:]
        seals = fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL
        fcntl.fcntl(descriptor, fcntl.F_ADD_SEALS, seals)
        require(fcntl.fcntl(descriptor, fcntl.F_GET_SEALS) == seals, 'Transfer source is not sealed.')
        yield Path(f'/proc/self/fd/{descriptor}'), descriptor
    finally:
        os.close(descriptor)


def preload(transaction, bundle, prepared, evidence):
    """No enabling writes: all transfers finish and match before the sole exe."""
    for name,address,length in REGIONS:
        backup = evidence / f'before-{name}.bin'
        transaction('read', hex(address), length, backup)
        require(backup.stat().st_size == length, 'SRAM backup is incomplete.')
        sync_file(backup)
        prepared.setdefault('_backup_hashes', {})[name] = digest(backup)
    uploads = [(item['name'],item['address']) for item in prepared['segments']]
    uploads += [('result-zero.bin',0x42000000),('contract.bin',CONTRACT),('request.bin',0x52c00),('report-zero.bin',0x52d00),('launch.bin',0x52000)]
    for name,address in uploads:
        require(int(transaction('readl','0x02050014').strip(),16)&1 == 0, 'Watchdog enabled during preload.')
        source = bundle / name; destination = evidence / ('readback-' + name)
        require(name in prepared['files'] and name in prepared['_sizes'], 'Upload lacks a validated identity.')
        size = prepared['_sizes'][name]
        data = source.read_bytes()
        require(len(data) == size and hashlib.sha256(data).hexdigest() == prepared['files'][name], 'Upload changed after validation.')
        with sealed_source(data) as (frozen, descriptor):
            transaction('write',hex(address),frozen,'read',hex(address),size,destination,pass_fds=(descriptor,))
        require(destination.stat().st_size == size and digest(destination) == prepared['files'][name], 'Upload differs from admitted bytes; launch refused.')
        sync_file(destination)


def launch_once(transaction, wait_return, bundle, prepared, evidence, record, save, ready=lambda: None):
    preload(transaction,bundle,prepared,evidence)
    words = [int(x,16) for x in transaction(*[arg for _ in range(4) for arg in ('readl','0x03000024')]).split()]
    require(len(words)==4 and all(0<=x<=0xffffffff and not x&0x100 for x in words), 'Physical FEL input is not consistently low.')
    require(int(transaction('readl','0x02050014').strip(),16)&1 == 0, 'Watchdog became enabled before execution.')
    ready()
    record['execution_intent'] = {'entry':0x52000,'epoch':prepared['epoch'],'generation':prepared['generation'],'native_elapsed_limit_ticks':24000}
    record['backup_sha256'] = prepared['_backup_hashes'].copy()
    record['launch_attempted'] = True; save()
    transaction('exe','0x52000')
    outcome = wait_return()
    if outcome == 'refused':
        record.update(launch_protocol_complete=True, scheduler_result='unobserved', complete=False, clean_refusal=True,
                      result_limit='Fresh refusal report from the original ROM instance; diagnostic entry was refused before device mutation.')
        save(); return
    record.update(launch_protocol_complete=True, scheduler_result='unobserved', complete=False,
                  result_limit='A distinct ROM instance does not prove watchdog cause, launcher entry or scheduler completion. No old DRAM or SRAM result was read.')
    save()


def observe_refusal(transaction, evidence, prepared, record, save):
    """One report probe; a successful response must precede further inspection."""
    path = evidence / 'refusal-report.bin'
    transaction('read','0x52d00',128,path)
    data = path.read_bytes()
    require(len(data) == 128, 'Incomplete original-instance refusal report.')
    words = struct.unpack('<16Q',data)
    require(words[0] in (1,2,3,4,5,6,13) and words[1:3] == (0,0) and
            words[3:6] == (prepared['epoch'],prepared['generation'],prepared['lease']) and
            words[6] == 0x4153373352454631 and not any(words[7:]), 'No matching clean-refusal report; ownership retained.')
    sync_file(path)
    require(int(transaction('readl','0x02050014').strip(),16)&1 == 0, 'Refusal returned with an active watchdog.')
    record['refusal_reason'] = words[0]; save()
    for name,address,length in REGIONS:
        source = evidence / f'before-{name}.bin'; destination = evidence / f'restored-{name}.bin'
        data = source.read_bytes()
        expected = prepared.get('_backup_hashes', {}).get(name)
        require(len(data) == length and hashlib.sha256(data).hexdigest() == expected, 'SRAM backup changed; restoration refused.')
        with sealed_source(data) as (frozen, descriptor):
            transaction('write',hex(address),frozen,'read',hex(address),length,destination,pass_fds=(descriptor,))
        require(destination.stat().st_size == length and digest(destination) == expected, 'SRAM restoration differs; ownership retained.')
        sync_file(destination)
    record['sram_restored'] = True; save()
    return 'refused'


def hold_ownership():
    print('Launch outcome unresolved. Board flock and queue claim remain held; coordinator recovery intervention is required.',file=sys.stderr,flush=True)
    while True:
        try: time.sleep(1)
        except KeyboardInterrupt: print('Ownership retained. Stop this process only after independent coordinator recovery review.',file=sys.stderr,flush=True)


def stop_after_failure(record, save, quarantine, error, hold=hold_ownership):
    # Evidence or quarantine failure must not release ownership of a live image.
    try:
        record['failure'] = str(error)
        save()
        quarantine()
    finally:
        if record['launch_attempted']: hold()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__); sub = parser.add_subparsers(dest='operation',required=True)
    prep = sub.add_parser('prepare'); prep.add_argument('--image',type=Path,required=True); prep.add_argument('--image-manifest',type=Path,required=True); prep.add_argument('--output',type=Path,required=True)
    prep.add_argument('--clang',default='clang-21'); prep.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    live = sub.add_parser('launch')
    for name in ('bundle','fel','qualification','input-evidence','ddr-contract','evidence','database'): live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--claim',required=True); live.add_argument('--owner',required=True); live.add_argument('--confirm-bridge-stable',action='store_true',required=True)
    args = parser.parse_args(argv)
    if args.operation == 'prepare':
        result=prepare(args.image,args.image_manifest,args.output.resolve(),args.clang,args.lld); print(json.dumps(result,indent=2)); return 0
    bundle=args.bundle.resolve(); prepared=validate_bundle(bundle)
    require(digest(args.fel)==FEL_HASH, 'FEL executable differs from reviewed bytes.')
    claimed(args.database,args.claim,args.owner,bundle,args.qualification,args.input_evidence,args.ddr_contract)
    qualification=json.loads(args.qualification.read_text()); input_record=json.loads(args.input_evidence.read_text()); ddr=json.loads(args.ddr_contract.read_text())
    args.evidence.mkdir(parents=True,exist_ok=False)
    for label, path in [('host-tool.py', Path(__file__)), ('qualification.json', args.qualification), ('input-evidence.json', args.input_evidence), ('ddr-contract.json', args.ddr_contract), ('prepared.json', bundle/'prepared.json')]:
        shutil.copy2(path,args.evidence/label)
    access=module('smp_usb_access',ROOT/'tools/usb-access.py')
    sys.path.insert(0,str(Path.home()/'git/mimus/tools')); import usb_guard
    usb_guard.check_health()
    record={'complete':False,'transactions':[],'claim':args.claim,'owner':args.owner,'bundle_sha256':digest(bundle/'prepared.json'),'launch_attempted':False}
    def save(): save_json(args.evidence/'session.json',record)
    with access.board_lock() as node:
        access.require_session(node); access.require_rom(node); identity=access.identity(node)
        port=access.target(node); port_config={'port':port.name,'sysfs':str(port.resolve())}; failed=False
        require_queue_port(args.database,port.name)
        validate_qualification(qualification,port.name,str(port.resolve()))
        require(input_record.get('complete') is True and input_record.get('asserted') is True and input_record.get('workspace_restored') is True and input_record.get('identity')==identity and input_record.get('port')==port.name and input_record.get('rom_sha256')==ROM_HASH, 'Stable low-input evidence does not cover this live instance.')
        require(ddr.get('schema')=='a733-ddr-reservation-v1' and ddr.get('identity')==identity and ddr.get('port')==port.name and ddr.get('image_sha256')==prepared['image_sha256'] and ddr.get('exclusive') is True and ddr.get('published_cache_disabled') is True and ddr.get('remote_writers_excluded') is True and ddr.get('ranges')==[[0x41000000,0x42200000]], 'DDR reservation/publication contract is incomplete.')
        for item in ddr.get('evidence',[]): artifact(item)
        require(ddr.get('evidence'), 'DDR initialization and reservation evidence is missing.')
        record.update(initial_node=str(node),initial_identity=identity,port=port.name, host_tool_sha256=digest(Path(__file__)), usb_access_sha256=digest(ROOT/'tools/usb-access.py'), usb_guard_sha256=digest(Path(usb_guard.__file__))); save()
        def transaction(*arguments, pass_fds=()):
            nonlocal failed
            require(not failed,'Transport failure forbids all further requests.')
            entry={'arguments':[str(a) for a in arguments]}
            if pass_fds:
                entry['sealed_sources'] = [{'fd':fd,'bytes':os.fstat(fd).st_size,'sha256':hashlib.sha256(os.pread(fd,os.fstat(fd).st_size,0)).hexdigest(),'seals':fcntl.fcntl(fd,fcntl.F_GET_SEALS)} for fd in pass_fds]
            record['transactions'].append(entry); save()
            try:
                require(access.identity(node)==identity,'USB identity changed before a request.')
                result=fel_request(args.fel,node,arguments,entry,pass_fds)
            except (OSError,ValueError,subprocess.TimeoutExpired) as error:
                failed=True; entry['failure']=str(error); save(); raise
            save(); return result.stdout
        def wait_return():
            nonlocal node,identity
            old=identity; deadline=time.monotonic()+20; departure=False
            while time.monotonic()<deadline:
                candidate,new=present_instance(access,port_config)
                if candidate is None or new!=old: departure=True
                if candidate is not None and new!=old and os.access(candidate,os.R_OK|os.W_OK):
                    access.require_session(candidate); access.require_rom(candidate); node,identity=candidate,new
                    record.update(returned_node=str(node),returned_identity=identity); save()
                    require('soc=00001903(A733)' in transaction('version'),'Returned ROM is not A733.'); return
                time.sleep(.05)
            candidate,current=present_instance(access,port_config)
            if not departure and candidate is not None and current==old and os.access(candidate,os.R_OK|os.W_OK):
                access.require_session(candidate); access.require_rom(candidate)
                return observe_refusal(transaction,args.evidence,prepared,record,save)
            raise ValueError('No distinct accessible ROM instance in 20 seconds; ownership remains held.')
        try:
            require('soc=00001903(A733)' in transaction('version'),'Target is not A733.')
            with usb_guard.reenumeration('sunstoneos-a733','protected SMP launch and watchdog ROM return',2):
                launch_once(transaction,wait_return,bundle,prepared,args.evidence,record,save,
                    lambda: claimed(args.database,args.claim,args.owner,bundle,args.qualification,args.input_evidence,args.ddr_contract))
        except BaseException as error:
            record['transport_failed'] = failed
            stop_after_failure(record,save,
                lambda: access.quarantine_instance(node,'SMP launch stopped; no further device communication or automatic retry.',identity),error)
            raise
    if record.get('clean_refusal'):
        print('Original ROM reported a clean launch refusal; SRAM was restored. Scheduler result remains unobserved. Queue claim remains with the coordinator.')
    else:
        print('A new accessible ROM instance was observed. Scheduler result remains unobserved; DDR must be reinitialized. Queue claim remains with the coordinator.')
    return 2


if __name__=='__main__':
    try: sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error: sys.exit(f'Stopped: {error}')
