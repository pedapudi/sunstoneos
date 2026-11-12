#!/usr/bin/env python3
"""Prepare an eight-core image, or run one protected launch attempt.

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
import struct
import subprocess
import sys
import time
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host_evidence import evidence_directory, new_directory, unique_directory
FEL_HASH = '4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e'
ROM_HASH = 'a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa'
REQUEST_MAGIC = 0x415337334c4e4331
CONTRACT = 0x42004000
REGIONS = [('launcher', 0x52000, 0xc00), ('request', 0x52c00, 128),
           ('report', 0x52d00, 128), ('stack', 0x53000, 0x1000), ('scratch', 0x71000, 128)]
# One sunxi-fel call writes a whole extent and reads it back. The recorded
# full-speed link moved about 0.57 MB/s; budget less than half of that for each
# byte written or read, on top of the fixed limit for small requests.
FEL_REQUEST_SECONDS = 15
FEL_BYTES_PER_SECOND = 250000
FEL_TIMEOUT_LIMIT = 300


def require(ok, message):
    if not ok: raise ValueError(message)


def transfer_timeout(moved):
    """Bound one request that moves this many bytes; refuse one that cannot finish."""
    timeout = FEL_REQUEST_SECONDS + -(-moved // FEL_BYTES_PER_SECOND)
    require(timeout <= FEL_TIMEOUT_LIMIT, 'FEL request exceeds the bounded transfer time.')
    return timeout


def fel_timeout(arguments):
    """Bound one request by the bytes it moves; refuse one that cannot finish."""
    arguments = [str(x) for x in arguments]; moved = 0
    for index, value in enumerate(arguments):
        if value == 'write': moved += Path(arguments[index + 2]).stat().st_size
        elif value == 'read': moved += int(arguments[index + 2])
    return transfer_timeout(moved)


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
    """Return the port's FEL node and its identity, or (None, None).

    session_node already lstats the node and maps the expected disconnect to
    absence. The node can still disappear before the identity lstat here;
    treat only that absence as an absent node.
    """
    candidate = access.session_node(config)
    if candidate is None: return None, None
    try: return candidate, access.identity(candidate)
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ENODEV): return None, None
        raise




output_directory = evidence_directory
artifact_directory = new_directory


class DramNotReady(ValueError):
    """The complete test word differed and its original bytes were restored."""


def clean_dram_refusal(record, error):
    return isinstance(error, DramNotReady) and not any(record.get(key) for key in
        ('dirty', 'dram_probe_dirty', 'transport_failed', 'launch_attempted'))


def verify_dram(transaction, evidence, record=None, save=lambda: None):
    """Test eight bytes in the reserved result page and restore them before preload.

    Transport errors propagate immediately. A successful transfer with different
    bytes permits restoration, then refuses launch. No historical result is used.
    """
    if record is None: record = {}
    address = '0x42000000'
    before = evidence / 'dram-before.bin'
    record['dram_probe_dirty'] = True
    try:
        save()
    except BaseException:
        # No request has started, so the failed initial journal cannot hide a write.
        record['dram_probe_dirty'] = False
        raise
    transaction('read', address, 8, before)
    original = before.read_bytes()
    require(len(original) == 8, 'Incomplete DRAM test backup.')
    sync_file(before)
    # Complement every saved bit so unchanged or stale reads cannot pass.
    pattern = bytes(value ^ 255 for value in original)
    with sealed_source(pattern) as (source, descriptor):
        tested = evidence / 'dram-tested.bin'
        transaction('write', address, source, 'read', address, 8, tested, pass_fds=(descriptor,))
    observed = tested.read_bytes()
    require(len(observed) == 8, 'Incomplete DRAM test readback; state is uncertain.')
    matches = observed == pattern
    with sealed_source(original) as (source, descriptor):
        restored = evidence / 'dram-restored.bin'
        transaction('write', address, source, 'read', address, 8, restored, pass_fds=(descriptor,))
    require(restored.read_bytes() == original, 'DRAM test restoration differs; launch refused.')
    record['dram_probe_dirty'] = False
    try:
        save()
    except BaseException:
        record['dram_probe_dirty'] = True
        raise
    if not matches:
        raise DramNotReady('DRAM write/read comparison differs; launch refused.')


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
    kind, machine, version, entry, phoff, shoff, _, ehsize, phsize, phnum, shsize, shnum, shstrndx = header
    require((kind, machine, version, entry, ehsize, phsize, shsize) == (2, 183, 1, 0x41000000, 64, 56, 64), 'ELF contract differs.')
    require(0 < phnum < 64 and 0 < shnum < 4096 and phoff + phnum * phsize <= len(data) and shoff + shnum * shsize <= len(data), 'Invalid ELF tables.')
    symbols = {}
    symbol_info = {}
    usb_symbols = {'__uncached_pool_start', '__uncached_pool_end', 'usb_memory_extent_valid', 'os_usb_service_init'}
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
            if label in {'a733_smp_build_id', '__a733_smp_result', '__a733_smp_contract', '__a733_smp_contract_end'} | usb_symbols:
                require(label not in symbols, 'Duplicate image contract symbol.'); symbols[label] = (value, length)
                symbol_info[label] = (info, index)
    headers = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize) for i in range(phnum)]
    usb_start, usb_end = 0x42200000, 0x42a00000
    usb_section = None
    if symbols.get('__uncached_pool_start', (0,))[0] == usb_start:
        # apps/os/usb-memory-link.patch reserves this class-only arena. It is
        # unloaded memory, not a larger executable or upload reservation.
        require(0 < shstrndx < shnum, 'Missing USB arena section names.')
        strings = sections[shstrndx]
        require(strings[1] == 3 and strings[4] + strings[5] <= len(data), 'Malformed USB arena section names.')
        names = data[strings[4]:strings[4] + strings[5]]
        matches = [i for i, s in enumerate(sections) if s[0] < len(names) and names[s[0]:].split(b'\0', 1)[0] == b'.uncached_pool']
        require(len(matches) == 1, 'Missing or duplicate USB arena section.')
        index = matches[0]; usb_section = sections[index]
        require(usb_section[1:4] == (8, 3, usb_start) and usb_section[5] == usb_end - usb_start and
                symbols.get('__uncached_pool_end') == (usb_end, 0) and symbols['__uncached_pool_start'] == (usb_start, 0) and
                all(symbol_info[name][1] == index for name in ('__uncached_pool_start', '__uncached_pool_end')),
                'USB arena section or linker bounds differ.')
        for name in ('usb_memory_extent_valid', 'os_usb_service_init'):
            value, length = symbols.get(name, (0, 0)); info, section_index = symbol_info.get(name, (0, 0))
            require(info & 15 == 2 and length and 0 < section_index < shnum and
                    sections[section_index][1] == 1 and sections[section_index][2] & 4 and
                    sections[section_index][3] <= value < value + length <= sections[section_index][3] + sections[section_index][5] and
                    any(h[0] == 1 and h[1] & 1 and h[3] <= value < value + length <= h[3] + h[5] for h in headers),
                    'USB arena requires its linked service and memory validator.')
        require(all(i == index or not s[2] & 2 or not s[5] or s[3] + s[5] <= usb_start or usb_end <= s[3]
                    for i, s in enumerate(sections)), 'USB arena overlaps an allocated section.')
    segments = []
    usb_segments = 0
    for ptype, flags, offset, address, physical, filesz, memsz, alignment in headers:
        if ptype != 1: continue
        arena = bool(usb_section and (address, memsz, filesz, flags, offset) ==
                     (usb_start, usb_end - usb_start, 0, 6, usb_section[4]))
        require(address == physical and (0x41000000 <= address < address + memsz <= 0x42200000 or arena) and
                filesz <= memsz and offset + filesz <= len(data) and flags & 3 != 3, 'Invalid load segment.')
        require(not (flags & 1) or address + memsz <= 0x41200000, 'Code exceeds reserved block.')
        require(not filesz or address + filesz <= 0x42000000, 'Image file bytes overlap result or contract.')
        require(all(address + memsz <= other['address'] or other['address'] + other['memory_bytes'] <= address for other in segments), 'ELF load allocations overlap.')
        segments.append({'address': address, 'memory_bytes': memsz, 'data': data[offset:offset + filesz]})
        usb_segments += arena
    require(usb_section is None or usb_segments == 1, 'Missing USB arena load reservation.')
    require(any(s['address'] <= entry < s['address'] + len(s['data']) for s in segments), 'Entry has no file bytes.')
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
    segments, build_address, build_id = image_parts(image)
    output = artifact_directory(output)
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
    shutil.copy2(image, output / 'image.elf')
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
              'files': {f.name: digest(f) for f in output.iterdir() if f.is_file()}, 'physical_access': False, 'output': str(output)}
    save_json(output / 'prepared.json', record)
    return record


def validate_bundle(bundle):
    prepared = json.loads((bundle / 'prepared.json').read_text())
    require(prepared.get('schema') == 2 and prepared.get('physical_access') is False, 'Unsupported prepared bundle.')
    sizes = {}
    required_files = {'image.elf', 'launch.bin', 'request.bin', 'contract.bin', 'result-zero.bin', 'report-zero.bin'} | {item['name'] for item in prepared['segments']}
    require(required_files <= prepared['files'].keys(), 'Native transfer inputs are incomplete.')
    for name in required_files:
        expected = prepared['files'][name]
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
    require_upload_bounds(prepared)
    return prepared








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


def uploads(prepared):
    """Preload order: image segments, then result, contract, request, report and launcher."""
    return ([(item['name'],item['address']) for item in prepared['segments']] +
            [('result-zero.bin',0x42000000),('contract.bin',CONTRACT),('request.bin',0x52c00),('report-zero.bin',0x52d00),('launch.bin',0x52000)])


def require_upload_bounds(prepared):
    """Refuse a bundle with an upload whose write and readback exceed the FEL bound."""
    for name,_ in uploads(prepared):
        require(name in prepared['_sizes'], 'Upload lacks a validated identity.')
        transfer_timeout(2 * prepared['_sizes'][name])


def preload(transaction, bundle, prepared, evidence):
    """No enabling writes: all transfers finish and match before the sole exe."""
    # Validation already checks this for bundles it admits; repeat it before
    # the first transfer for callers with their own bundle validation.
    require_upload_bounds(prepared)
    for name,address,length in REGIONS:
        backup = evidence / f'before-{name}.bin'
        transaction('read', hex(address), length, backup)
        require(backup.stat().st_size == length, 'SRAM backup is incomplete.')
        sync_file(backup)
        prepared.setdefault('_backup_hashes', {})[name] = digest(backup)
    for name,address in uploads(prepared):
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


def report_hold(message):
    # A closed console must not end the hold and release the board flock.
    try: print(message,file=sys.stderr,flush=True)
    except (OSError,ValueError): pass


def hold_ownership():
    report_hold('Launch outcome unresolved. Board flock remains held; coordinator recovery intervention is required.')
    while True:
        try: time.sleep(1)
        except KeyboardInterrupt: report_hold('Ownership retained. Stop this process only after independent coordinator recovery review.')


def stop_after_failure(record, save, quarantine, error, hold=hold_ownership):
    if clean_dram_refusal(record, error):
        record.update(failure=str(error), prelaunch_refusal='dram-not-ready',
                      ownership_release_permitted=True)
        save()
        return
    # Evidence or quarantine failure must not release ownership of a live image.
    try:
        record['failure'] = str(error)
        save()
        quarantine()
    finally:
        if record.get('launch_attempted') or record.get('dram_probe_dirty'): hold()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__); sub = parser.add_subparsers(dest='operation',required=True)
    prep = sub.add_parser('prepare'); prep.add_argument('--image',type=Path,required=True); prep.add_argument('--image-manifest',type=Path,help=argparse.SUPPRESS); prep.add_argument('--output',type=Path,required=True)
    prep.add_argument('--clang',default='clang-21'); prep.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    live = sub.add_parser('launch')
    for name in ('bundle','fel','evidence'): live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--confirm-bridge-stable',action='store_true')
    args = parser.parse_args(argv)
    if args.operation == 'prepare':
        result=prepare(args.image,args.image_manifest,args.output.resolve(),args.clang,args.lld); print(json.dumps(result,indent=2)); return 0
    sys.path.insert(0, str(ROOT/'tools'))
    import board
    raise SystemExit(board.legacy_run(args, image=None))


if __name__=='__main__':
    try: sys.exit(main())
    except (OSError,ValueError,RuntimeError,struct.error) as error: sys.exit(f'Stopped: {error}')
