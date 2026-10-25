#!/usr/bin/env python3
"""Prepare, run once, or decode the finite current CPU workload.

Physical execution requires root-supplied admission, a live queue claim and
current DDR/survey continuity. Failures retain ownership for explicit recovery.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('qualification_supervised', ROOT / 'tools/check-a733-smp-supervised.py')
supervised = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(supervised)
require, digest, save_json = supervised.require, supervised.digest, supervised.save_json
MAGIC = 0x3154525133333741
REPORT_BYTES, CPU_OFFSET, CPU_BYTES = 8880, 688, 1024
PROFILE = 'current-finite-cpu-idle-wake-v3'
# Retained-failure reasons that runtime.c publishes in the report's error word
# and in the failing CPU's error word.
REASONS = {
    1: 'capture entered in the wrong state or after the deadline',
    2: 'capture epoch, repeat or arrival mismatch',
    3: 'exit fault slot differs',
    4: 'workload rounds, background progress, SGIs, failure or ticks differ',
    5: 'capture barrier timed out or another CPU failed',
    6: 'owned timer PPIs did not quiesce',
    7: 'owned timer PPIs remained pending or enabled',
    8: 'idle/wake proof missing',
    9: 'drained barrier timed out or another CPU failed',
    20: 'shutdown entered after the deadline or a failure',
    21: 'secondary waited too long for its shutdown request',
    22: 'CPU0 waited too long for a secondary restore',
    23: 'CPU0 waited too long for a power-down acknowledgement',
    24: 'secondary power status did not reach OFF',
    25: 'secondary power register restore differs',
    26: 'distributor write did not complete before restore',
    27: 'distributor disable did not complete',
    28: 'SPI disable did not complete',
    29: 'SPI enable restore differs',
    30: 'distributor control restore differs',
    31: 'shutdown before capture or cleaning',
}


def reason(code):
    return f'reason {code} ({REASONS.get(code, "unknown")})'


def geometry(image):
    """Read the actual linked allocations, including NOLOAD monitor storage."""
    segments, build_address, build_id = supervised.image_parts(image)
    data = image.read_bytes()
    header = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    shoff, shsize, shnum = header[5], header[10], header[11]
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(shnum)]
    wanted = {'a733_qualification_report', 'a733_qualification_stacks', '__monitor_bss_start',
              '__monitor_bss_end', '__bss_start', '__bss_end', '__a733_smp_boot_stack_top',
              'a733_fault_pending', 'a733_fault_capture', 'a733_qualification_captured'}
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        offset, size, link, entry = section[4], section[5], section[6], section[9]
        require(link < shnum and entry == 24 and size % entry == 0 and offset + size <= len(data), 'Invalid symbol table.')
        strings = sections[link]
        names = data[strings[4]:strings[4] + strings[5]]
        for pos in range(offset, offset + size, entry):
            name, _, _, index, address, length = struct.unpack_from('<IBBHQQ', data, pos)
            end = names.find(b'\0', name)
            require(0 <= name < len(names) and end >= name, 'Invalid symbol name.')
            label = names[name:end].decode('ascii', errors='replace')
            if label in wanted:
                require(label not in symbols and index != 0, 'Missing or duplicate qualification symbol.')
                symbols[label] = (address, length)
    require(set(symbols) == wanted, 'Current runtime/fault symbols missing.')
    monitor_start = symbols['__monitor_bss_start'][0]
    monitor_end = symbols['__monitor_bss_end'][0]
    require(0x42005000 <= monitor_start < monitor_end <= symbols['__a733_smp_boot_stack_top'][0] <= 0x421f0000,
            'Monitor and boot stacks exceed Device reservation.')
    report, size = symbols['a733_qualification_report']
    stacks, stack_size = symbols['a733_qualification_stacks']
    require(size == REPORT_BYTES and stack_size == 8 * 4096 and
            monitor_start <= report < report + size <= monitor_end and
            monitor_start <= stacks < stacks + stack_size <= monitor_end and
            (report + size <= stacks or stacks + stack_size <= report), 'Report/cleanup stacks differ from native ABI.')
    writable = [[s[3], s[3] + s[5]] for s in sections if s[2] & 3 == 3 and s[5] and s[3] < 0x42000000]
    require(writable and all(0x41200000 <= lo < hi <= 0x42000000 for lo, hi in writable), 'Invalid cached writable section.')
    start, end = min(x[0] for x in writable), max(x[1] for x in writable)
    require(start <= symbols['__bss_start'][0] <= symbols['a733_fault_pending'][0] <
            symbols['a733_fault_pending'][0] + symbols['a733_fault_pending'][1] <= symbols['__bss_end'][0] <= end,
            'Fault pending storage lies outside cached writable extent.')
    require(any(s['address'] <= 0x42001000 and 0x42001400 <= s['address'] + s['memory_bytes'] for s in segments),
            'Current eight fault slots are absent from load reservation.')
    return {'report_address': report, 'report_bytes': size, 'cleanup_stacks': [stacks, stacks + stack_size],
            'cached_writable': [start & ~63, (end + 63) & ~63], 'monitor': [monitor_start, monitor_end],
            'fault_slots': [0x42001000, 0x42001400], 'reservation': [0x41000000, 0x42200000],
            'build_address': build_address, 'build_id': build_id.hex()}


def manifest_check(image, manifest):
    g = geometry(image)
    require(manifest.get('target') == 'a733-cpu-runtime-qualification' and
            manifest.get('execution_profile') == PROFILE and manifest.get('workload') == '32-rounds-local-timers-idle-wake' and
            manifest.get('final_elf') == digest(image) and manifest.get('build_id') == g['build_id'],
            'Manifest does not bind the current qualification image.')
    return g


def prepare(image, manifest_path, admission_path, output, clang, lld, objcopy):
    g = manifest_check(image, json.loads(manifest_path.read_text()))
    admission = json.loads(admission_path.read_text())
    supervised.validate_admission(admission, digest(image))
    # The shared validator requires the complete 18 MiB allocation. The ELF
    # checks above prove every linked writer fits; they do not grant ownership.
    segments, address, identity = supervised.image_parts(image)
    output.mkdir(parents=True, exist_ok=False)
    for name, source in [('launch.S', ROOT / 'platform/a733-smp-observe/launch.S'),
                         ('launch.ld', ROOT / 'platform/a733-smp-observe/launch.ld'),
                         ('launch.c', ROOT / 'platform/a733-smp-supervised/launch.c'),
                         ('boot.h', ROOT / 'platform/a733-smp-cached/boot.h')]:
        shutil.copy2(source, output / name)
    flags = ['-DA733_SMP_CONTRACT_MAGIC=0x4153373353554333ULL', '--target=arm-none-eabi', '-march=armv8-a',
             '-marm', '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
             '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(output)]
    commands = [[clang, *flags, '-c', output / 'launch.S', '-o', output / 'entry.o'],
                [clang, *flags, '-c', output / 'launch.c', '-o', output / 'launch.o'],
                [lld, '-T', output / 'launch.ld', output / 'entry.o', output / 'launch.o', '-o', output / 'launch.elf'],
                [objcopy, '-O', 'binary', output / 'launch.elf', output / 'launch.bin']]
    for command in commands:
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True)
        with (output / 'build.log').open('a') as stream:
            stream.write(result.stdout + result.stderr)
        require(result.returncode == 0, 'Launcher build failed; preserve build.log.')
    save_json(output / 'commands.json', [[str(x) for x in c] for c in commands])
    require(0 < (output / 'launch.bin').stat().st_size <= 0xc00, 'Launcher exceeds SRAM.')
    for source, name in [(image, 'image.elf'), (manifest_path, 'image-input-manifest.json'), (admission_path, 'admission.json')]:
        shutil.copy2(source, output / name)
    payloads = []
    for i, segment in enumerate(segments):
        if segment['data']:
            name = f'segment-{i}.bin'
            (output / name).write_bytes(segment['data'])
            payloads.append({'name': name, 'address': segment['address']})
    epoch, generation, lease = (secrets.randbits(64) or 1 for _ in range(3))
    request, contract = supervised.request_bytes(address, identity, epoch, generation, lease,
                                                admission['reservation'], bytes.fromhex(digest(admission_path)))
    for name, content in [('request.bin', request), ('contract.bin', contract),
                          ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]:
        (output / name).write_bytes(content)
    record = {'schema': 'a733-smp-supervised-bundle-v1', 'execution_profile': PROFILE,
              'reservation': admission['reservation'], 'admission_sha256': digest(admission_path),
              'image_sha256': digest(image), 'launcher_sha256': digest(output / 'launch.bin'),
              'build_id': identity.hex(), 'epoch': epoch, 'generation': generation, 'lease': lease,
              'segments': payloads, 'reservations': [{k: v for k, v in s.items() if k != 'data'} for s in segments],
              'qualification': g, 'files': {p.name: digest(p) for p in output.iterdir() if p.is_file()},
              'physical_access': False}
    save_json(output / 'prepared.json', record)
    validate_bundle(output)
    return record


def validate_bundle(bundle):
    prepared = supervised.validate_bundle(bundle)
    require(prepared.get('execution_profile') == PROFILE, 'Not a qualification bundle.')
    g = manifest_check(bundle / 'image.elf', json.loads((bundle / 'image-input-manifest.json').read_text()))
    require(prepared.get('qualification') == g, 'Qualification geometry changed.')
    segments, _, _ = supervised.image_parts(bundle / 'image.elf')
    require(prepared.get('reservations') == [{k: v for k, v in s.items() if k != 'data'} for s in segments],
            'Linked reservation metadata changed.')
    return prepared


def decode_report(raw, epoch):
    require(type(epoch) is int and 0 < epoch < 2**64 and len(raw) == REPORT_BYTES, 'Report size or expected epoch differs.')
    magic, version, size, actual, deadline, state, error, core = struct.unpack_from('<8Q', raw)
    # The error word has a meaning only in this session's report of this layout.
    require((magic, version, size, actual) == (MAGIC, 3, REPORT_BYTES, epoch), 'No native report of this session.')
    require(not error, f'Native retained failure: {reason(error)}.')
    require((state, core) == (4, 0) and deadline, 'No matching successful native report.')
    cpus = []
    for i in range(8):
        offset = CPU_OFFSET + CPU_BYTES * i
        values = struct.unpack_from('<13Q', raw, offset)
        seen, captured, cleaned, restored, request, acknowledged, error, _, rounds, background, sgis, failure, ticks = values
        drained = struct.unpack_from('<Q', raw, offset + 104)[0]
        off = struct.unpack_from('<Q', raw, offset + 768)[0]
        require(not error, f'CPU {i} retained failure: {reason(error)}.')
        require((seen, captured, drained, cleaned, restored) == (epoch,) * 5 and failure == 0 and
                rounds == 32 and background > 0 and sgis > 0, f'CPU {i} workload/publication incomplete.')
        require((request, acknowledged, off) == ((epoch,) * 3 if i else (0, 0, 0)), f'CPU {i} shutdown incomplete.')
        require(ticks >= 32 if i == 0 else ticks == 1, f'CPU {i} timer count differs from its assigned role.')
        idle, wakes = struct.unpack_from('<2Q', raw, offset + 112)
        require(idle > 0 and wakes == (7 if i else 6), f'CPU {i} idle/wake proof missing.')
        cpus.append({'idle_completed': idle, 'idle_wakes': wakes, 'core': i, 'rounds': rounds, 'background': background, 'sgis': sgis, 'ticks': ticks})
    return {'epoch': epoch, 'state': state, 'cpus': cpus, 'scope': 'copied report only; ROM return and restoration require separate evidence'}


# The native contract grants 20 seconds; retain the existing 30-second ROM
# discovery allowance after that bound. Neither deadline is renewed.
RETURN_TIMEOUT_SECONDS = 20 + 30
CHUNK_BYTES = 4 * 1024 * 1024
RAM_START, RAM_BYTES = 0x41000000, 0x1200000
base = supervised.base
returning = base.module('qualification_returning', ROOT / 'tools/a733-returning-host.py')
kernel = base.module('qualification_kernel', ROOT / 'tools/check-a733-kernel.py')


def caller_interval(survey):
    # Same source-backed A32 push interval as run-native-startup-probe.py.
    require(survey.get('complete') is True and survey.get('sram_restored') is True and
            survey.get('rom_usable') is True, 'Completed current returning survey required.')
    sp = survey['result']['sp']
    require(type(sp) is int and 0x4003c <= sp <= 0x100000 and sp % 4 == 0, 'Caller SP outside reviewed SRAM.')
    require(all(sp <= address or sp - 60 >= address + size for _, address, size in base.REGIONS),
            'Caller stack overlaps launcher reservations.')
    return sp - 60, 60


def admit(args, prepared, node, identity, port):
    require(isinstance(args.root_owned_claim, str) and args.root_owned_claim.strip(), 'Explicit root-owned claim required.')
    validate_bundle(args.bundle.resolve())
    admission = json.loads(prepared['_frozen']['admission.json'])
    supervised.validate_admission(admission, prepared['image_sha256'], identity, port)
    paths = [args.ddr_evidence, args.platform_survey, *args.warm_return]
    raw = [p.read_bytes() for p in paths]
    kernel.validate_platform_admission(tuple(raw[:2]), node, identity, port,
                                      base.FEL_HASH, base.FEL_HASH, tuple(raw[2:]))
    caller_interval(json.loads(raw[1]))
    ddr = json.loads(supervised.artifact(admission['ddr']).read_text())
    covered = {(item['path'], item['sha256']) for item in ddr['evidence']}
    require(all((str(p.resolve()), hashlib.sha256(data).hexdigest()) in covered for p, data in zip(paths, raw)),
            'Current DDR/survey/warm-return chain is absent from the admitted reservation.')


def run_once(transaction, wait_return, ready, bundle, prepared, evidence, record, journal):
    require(not record.get('launch_attempted'), 'One CPU attempt only.')
    ready()
    # Check immutable ROM code, not the boot-input pin: the native return uses RMR.
    addresses = (0x20, 0x24, 0x28, 0x3c, 0x40)
    words = [int(x, 16) for x in transaction(*[v for a in addresses for v in ('readl', hex(a))]).split()]
    require(words == [0xeb00008f, 0xe3a00911, 0xe3a0193d, 0xe3a0605c, 0xea00004d], 'Direct forced-ROM entry differs.')
    backups = []
    for offset in range(0, RAM_BYTES, CHUNK_BYTES):
        size = min(CHUNK_BYTES, RAM_BYTES - offset)
        path = evidence / f'before-dram-{offset // CHUNK_BYTES}.bin'
        transaction('read', hex(RAM_START + offset), size, path)
        try:
            require(path.stat().st_size == size, 'Incomplete CPU RAM snapshot.')
            base.sync_file(path)
            snapshot_hash = base.digest(path)
        except (OSError, ValueError):
            # Missing, unreadable or incomplete copied bytes leave transport
            # health uncertain. Retain ownership even before the first write.
            record['dirty'] = True
            record['snapshot_incomplete'] = True
            journal()
            raise
        backups.append((RAM_START + offset, size, path, snapshot_hash))
    record.update(dram_backups=[{'address': a, 'bytes': n, 'file': p.name, 'sha256': h} for a, n, p, h in backups],
                  dram_restored=False, sram_restored=False, dirty=True)
    journal()
    base.preload(transaction, bundle, prepared, evidence)
    ready()
    require(int(transaction('readl', '0x02050014').strip(), 16) & 1 == 0, 'Watchdog enabled before CPU execution.')
    record.update(launch_attempted=True, backup_sha256=prepared['_backup_hashes'].copy())
    journal()
    transaction('exe', '0x52000')
    wait_return()
    require('soc=00001903(A733)' in transaction('version'), 'Returned service is not A733 ROM.')
    report = evidence / 'qualification-report.bin'
    transaction('read', hex(prepared['qualification']['report_address']), REPORT_BYTES, report)
    require(report.stat().st_size == REPORT_BYTES, 'Incomplete qualification report.')
    base.sync_file(report)
    result = decode_report(report.read_bytes(), prepared['epoch'])
    record.update(qualification_result=result, report_sha256=base.digest(report))
    journal()
    # Verify every saved byte before the first restoration write. SRAM belongs
    # to the new ROM and is never restored by this adapter.
    for _, size, path, expected in backups:
        require(path.stat().st_size == size and base.digest(path) == expected, 'Saved CPU RAM changed.')
    for index, (address, size, path, expected) in enumerate(backups):
        data = path.read_bytes()
        require(len(data) == size and hashlib.sha256(data).hexdigest() == expected, 'Saved CPU RAM changed before transfer.')
        restored = evidence / f'restored-dram-{index}.bin'
        with base.sealed_source(data) as (source, descriptor):
            transaction('write', hex(address), source, 'read', hex(address), size, restored, pass_fds=(descriptor,))
        require(restored.read_bytes() == data, 'CPU RAM restoration readback differs.')
        base.sync_file(restored)
    # This is the existing native-return branch of the warm-continuity validator;
    # no synthetic kernel-monitor result is substituted for the native report.
    record.pop('mode', None)
    record.update(result=result, physical_access=True, complete=True, dirty=False,
                  dram_restored=True, rom_usable=True, ownership_release_permitted=True)
    journal()
    return result


def run(args):
    def execute(tx, wait, ready, prepared, record, journal):
        paths = [args.ddr_evidence, args.platform_survey, *args.warm_return]
        record['continuity_inputs'] = []
        for index, path in enumerate(paths):
            raw = path.read_bytes()
            saved = args.evidence / f'continuity-{index}.json'
            saved.write_bytes(raw)
            base.sync_file(saved)
            record['continuity_inputs'].append({'path': str(path.resolve()), 'file': saved.name,
                                               'sha256': hashlib.sha256(raw).hexdigest()})
        record['caller_interval'] = list(caller_interval(json.loads(args.platform_survey.read_text())))
        journal()
        return run_once(tx, wait, ready, args.bundle.resolve(), prepared, args.evidence, record, journal)
    return returning.run(args, base=base, profile=PROFILE, validate_bundle=validate_bundle,
                         admit=admit, execute=execute, description='Finite current eight-core workload and validated RAM return',
                         return_timeout=RETURN_TIMEOUT_SECONDS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='operation', required=True)
    prep = sub.add_parser('prepare')
    for name in ['image', 'manifest', 'admission', 'output']:
        prep.add_argument('--' + name, type=Path, required=True)
    for name in ['clang', 'lld', 'objcopy']:
        prep.add_argument('--' + name, required=True)
    report = sub.add_parser('read-report')
    for name in ['bundle', 'report', 'output']:
        report.add_argument('--' + name, type=Path, required=True)
    live = sub.add_parser('run')
    for name in ['bundle', 'fel', 'evidence', 'ddr-evidence', 'platform-survey']:
        live.add_argument('--' + name, type=Path, required=True)
    live.add_argument('--warm-return', type=Path, action='append', default=[])
    live.add_argument('--root-owned-claim', required=True)
    args = parser.parse_args()
    if args.operation == 'prepare':
        prepare(args.image, args.manifest, args.admission, args.output, args.clang, args.lld, args.objcopy)
    elif args.operation == 'run':
        run(args)
    else:
        prepared = validate_bundle(args.bundle)
        result = decode_report(args.report.read_bytes(), prepared['epoch'])
        result.update(image_sha256=prepared['image_sha256'], report_sha256=digest(args.report),
                      report_address=prepared['qualification']['report_address'])
        save_json(args.output, result)


if __name__ == '__main__':
    main()
