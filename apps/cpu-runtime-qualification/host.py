#!/usr/bin/env python3
"""Offline preparation and decoding for the finite current CPU workload.

This tool performs no device or queue operations. Report decoding establishes
only the captured report's contents; ROM continuity and restoration remain the
coordinator's responsibility.
"""
import argparse
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
PROFILE = 'current-finite-cpu-return-v1'


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
            manifest.get('execution_profile') == PROFILE and manifest.get('workload') == '32-rounds-per-core' and
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
    require((magic, version, size, actual, state, error, core) == (MAGIC, 1, REPORT_BYTES, epoch, 4, 0, 0) and deadline,
            'No matching successful native report.')
    cpus = []
    for i in range(8):
        offset = CPU_OFFSET + CPU_BYTES * i
        values = struct.unpack_from('<13Q', raw, offset)
        seen, captured, cleaned, restored, request, acknowledged, error, _, rounds, background, sgis, failure, ticks = values
        drained = struct.unpack_from('<Q', raw, offset + 104)[0]
        off = struct.unpack_from('<Q', raw, offset + 768)[0]
        require((seen, captured, drained, cleaned, restored) == (epoch,) * 5 and error == failure == 0 and
                rounds == 32 and background > 0 and sgis > 0, f'CPU {i} workload/publication incomplete.')
        require((request, acknowledged, off) == ((epoch,) * 3 if i else (0, 0, 0)), f'CPU {i} shutdown incomplete.')
        require(ticks >= 32 if i == 0 else ticks == 0, f'CPU {i} timer count differs from its assigned role.')
        cpus.append({'core': i, 'rounds': rounds, 'background': background, 'sgis': sgis, 'ticks': ticks})
    return {'epoch': epoch, 'state': state, 'cpus': cpus, 'scope': 'copied report only; ROM return and restoration require separate evidence'}


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
    args = parser.parse_args()
    if args.operation == 'prepare':
        prepare(args.image, args.manifest, args.admission, args.output, args.clang, args.lld, args.objcopy)
    else:
        prepared = validate_bundle(args.bundle)
        result = decode_report(args.report.read_bytes(), prepared['epoch'])
        result.update(image_sha256=prepared['image_sha256'], report_sha256=digest(args.report),
                      report_address=prepared['qualification']['report_address'])
        save_json(args.output, result)


if __name__ == '__main__':
    main()
