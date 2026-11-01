#!/usr/bin/env python3
"""Return layout, one returning run and result decoding for CPU-return OS images.

A CPU-return image hands the board back to the USB boot ROM after its
application calls ss_finish. Its watchdog marker names the WDT0 mode that the
host arms (include/sunstone_crumb.h), and its result record ss_result_record
holds the application's result (apps/os/host.py). geometry() reads the linked
return layout, run_once() performs one launch inside the returning transport
(tools/a733-returning-host.py), and the application decodes its own payload.

The transport owner retains board locks, admission, recovery and rail
restoration obligations.
"""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('return_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
require = host.require
thermal_capture = host.module('return_host_thermal', ROOT / 'apps/os/cpu-return-thermal.py')
cpu = host.module('return_host_qualification', ROOT / 'apps/cpu-runtime-qualification/host.py')
base, digest = cpu.base, cpu.digest
PROFILE = 'os-cpu-return-v1'
CHUNK = 4 * 1024 * 1024
# The forced-FEL boot ROM words at 0x20, 0x24, 0x28, 0x3c and 0x40.
ROM_ENTRY = {0x20: 0xeb00008f, 0x24: 0xe3a00911, 0x28: 0xe3a0193d, 0x3c: 0xe3a0605c, 0x40: 0xea00004d}
# Image RAM below the session page; every run backs it up and restores it.
IMAGE_RAM = (0x41000000, 0x1200000)
# The copied result record's evidence name, which follow-up tools read.
OUTPUT = 'recorded-output'


def linked_symbols(image):
    """Defined global symbols of the image: name -> (address, bytes)."""
    elf = ELFFile(io.BytesIO(Path(image).read_bytes()))
    table = elf.get_section_by_name('.symtab')
    require(table is not None, 'CPU return image has no symbol table.')
    found = {}
    for s in table.iter_symbols():
        if s['st_shndx'] != 'SHN_UNDEF' and s['st_info']['bind'] == 'STB_GLOBAL':
            require(s.name not in found, 'Ambiguous global CPU return symbol.')
            found[s.name] = (int(s['st_value']), int(s['st_size']))
    return found


def layout(image):
    """The monitor's return report and cleanup stacks, and the cached writable range."""
    symbols = linked_symbols(image)
    def at(name):
        require(name in symbols, 'CPU return symbol missing: ' + name)
        return symbols[name][0]
    monitor = [at('__monitor_bss_start'), at('__monitor_bss_end')]
    report, stacks = at('a733_qualification_report'), at('a733_qualification_stacks')
    require(0x42005000 <= monitor[0] < monitor[1] <= at('__a733_smp_boot_stack_top') <= 0x421f0000 and
            symbols['a733_qualification_report'][1] == cpu.REPORT_BYTES and
            symbols['a733_qualification_stacks'][1] == 32768 and
            monitor[0] <= report < report + cpu.REPORT_BYTES <= monitor[1] and
            monitor[0] <= stacks < stacks + 32768 <= monitor[1] and
            (report + cpu.REPORT_BYTES <= stacks or stacks + 32768 <= report),
            'CPU return Secure Device allocations differ.')
    require(0x41200000 <= at('__bss_start') <= at('__bss_end') <= at('__heap_end') <= 0x41c00000,
            'CPU return cached writable range differs.')
    return {'report_address': report, 'report_bytes': cpu.REPORT_BYTES,
            'cleanup_stacks': [stacks, stacks + 32768], 'monitor': monitor,
            'cached_writable': [0x41200000, (at('__heap_end') + 63) & ~63]}


def geometry(image):
    """The return layout of an image whose application finishes with ss_finish.

    Besides layout(): the result record, the manifest's WORK and OUTPUT
    regions to back up, the thermal window to copy when the application
    records one (os_thermal_record_begin and os_thermal_record_end) and, in an
    image that admits the host-armed watchdog, its marker."""
    image = Path(image)
    result = layout(image)
    output = host.result_geometry(image)
    require(0x41200000 <= output['address'] < output['address'] + output['bytes'] <= 0x42000000,
            'Copied result geometry differs.')
    result.update(application_regions=host.backup_regions(image), output_address=output['address'],
                  output_bytes=output['bytes'])
    if 'os_thermal_record_end' in linked_symbols(image):
        result['thermal'] = thermal_capture.geometry(image)
    watchdog = host.watchdog_marker(image)
    if watchdog is not None:
        result['watchdog'] = watchdog
    return result


def decode_report(raw, epoch):
    """The monitor's report of a normal return: every CPU captured, cleaned and off."""
    require(len(raw) == cpu.REPORT_BYTES, 'Incomplete CPU return report.')
    magic, version, size, actual, deadline, state, error, core = struct.unpack_from('<8Q', raw)
    require((magic, version, size, actual, state, error, core) ==
            (cpu.MAGIC, 3, cpu.REPORT_BYTES, epoch, 4, 0, 0) and deadline,
            'No successful CPU return report of this session.')
    for i in range(8):
        offset = cpu.CPU_OFFSET + i * cpu.CPU_BYTES
        seen, captured, cleaned, restored, request, ack, error = struct.unpack_from('<7Q', raw, offset)
        drained = struct.unpack_from('<Q', raw, offset + 104)[0]
        off = struct.unpack_from('<Q', raw, offset + 768)[0]
        require((seen, captured, drained, cleaned, restored) == (epoch,) * 5 and not error and
                (request, ack, off) == ((epoch,) * 3 if i else (0, 0, 0)),
                f'CPU {i} normal shutdown incomplete.')
    return {'epoch': epoch, 'state': state, 'secondaries_off': 7}


def decode_record(copied, p, result, record, evidence):
    """Default decoder: the OS result record's payload, written to result.bin."""
    q = p['qualification']
    payload = host.decode_result(copied[OUTPUT], p['epoch'], p['generation'],
                                 q['output_bytes'] - host.RESULT.size)
    (evidence / 'result.bin').write_bytes(payload)
    base.sync_file(evidence / 'result.bin')
    result.update(payload_bytes=len(payload), payload_sha256=hashlib.sha256(payload).hexdigest())


def run_once(tx, launch, ready, bundle, p, evidence, record, journal, *, extents=None, captures=(),
             decode=decode_record):
    """One launch inside the transport's run(): back up, upload, launch, copy and restore.

    p is the validated bundle: the transport's upload plan (segments, files,
    _sizes), the session identities, 'qualification' from geometry() and
    '_inputs', the INPUT uploads as {'name', 'path', 'address', 'bytes',
    'sha256'}. extents replaces the application's WORK and OUTPUT regions as
    the DRAM ranges backed up besides image RAM and the inputs. captures
    adds (name, address, bytes) copies after the result record.
    decode(copied, p, result, record, evidence) decodes the copied result
    record, copied[OUTPUT], into result before DRAM restoration. Any failure
    raises with DRAM unrestored, so the transport retains the instance."""
    require(not record.get('launch_attempted'), 'One CPU return attempt only.')
    ready()
    q = p['qualification']
    # Back up every modified DRAM range before any upload. Returned ROM owns
    # SRAM; only these copied DRAM bytes may be restored after native return.
    if extents is None:
        extents = [(r['address'], r['bytes']) for r in q['application_regions']]
    extents = [IMAGE_RAM, *extents] + [(item['address'], item['bytes']) for item in p['_inputs']]
    backups = []
    record.update(dirty=True, dram_restored=False, cpu_rail_restoration_pending=True)
    if '_approved_rail_helper' in p:
        record.update(cpu_clock_entry_status='under-test', approved_rail_helper=p['_approved_rail_helper'])
    journal()
    for address, size in extents:
        for offset in range(0, size, CHUNK):
            n = min(CHUNK, size - offset)
            path = evidence / f'before-dram-{len(backups)}.bin'
            tx('read', hex(address + offset), n, path)
            require(path.stat().st_size == n, 'Incomplete CPU return RAM backup.')
            base.sync_file(path)
            backups.append((address + offset, n, path, digest(path)))
    record['dram_backups'] = [{'address': a, 'bytes': n, 'file': path.name, 'sha256': h}
                              for a, n, path, h in backups]
    journal()
    words = [int(x, 16) for x in tx(*[v for a in ROM_ENTRY for v in ('readl', hex(a))]).split()]
    require(words == list(ROM_ENTRY.values()), 'Forced-FEL ROM entry differs.')
    base.preload(tx, bundle / 'board', p, evidence)
    for item in p['_inputs']:
        data = Path(item['path']).read_bytes()
        readback = evidence / ('readback-' + item['name'])
        require(len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256'],
                'Application input changed.')
        with base.sealed_source(data) as (source, descriptor):
            tx('write', hex(item['address']), source, 'read', hex(item['address']), len(data), readback,
               pass_fds=(descriptor,))
        require(readback.read_bytes() == data, 'Application input readback differs.')
        base.sync_file(readback)
    ready()
    require(int(tx('readl', '0x02050014').strip(), 16) & 1 == 0, 'Watchdog enabled before native execution.')
    record.update(backup_sha256=p['_backup_hashes'].copy())
    journal()
    launch()
    require('soc=00001903(A733)' in tx('version'), 'Returned service is not A733 ROM.')
    copied = {}
    reads = [('return-report', q['report_address'], cpu.REPORT_BYTES),
             (OUTPUT, q['output_address'], q['output_bytes']), *captures]
    if 'thermal' in q:
        reads.extend(thermal_capture.captures(q['thermal']))
    for name, address, size in reads:
        path = evidence / (name + '.bin')
        tx('read', hex(address), size, path)
        require(path.stat().st_size == size, 'Incomplete copied CPU return output.')
        base.sync_file(path)
        copied[name] = path.read_bytes()
    result = decode_report(copied['return-report'], p['epoch'])
    decode(copied, p, result, record, evidence)
    if 'thermal' in q:
        record['thermal_capture_sha256'] = {name: hashlib.sha256(copied[name]).hexdigest()
                                            for name, _, _ in thermal_capture.captures(q['thermal'])}
        # Telemetry decoding cannot strand restored, responsive hardware with
        # raised CPU rails. The mandatory return proof above remains separate.
        try:
            result['thermal'] = thermal_capture.decode(q['thermal'], copied, p['epoch'], p['generation'])
            record['thermal_capture_status'] = 'decoded'
        except (ValueError, KeyError, struct.error) as error:
            record['thermal_capture_status'] = 'malformed'
            record['thermal_capture_error'] = str(error)
    record.update(report_sha256=hashlib.sha256(copied['return-report']).hexdigest(),
                  output_sha256=hashlib.sha256(copied[OUTPUT]).hexdigest())
    journal()
    for _, n, path, expected in backups:
        require(path.stat().st_size == n and digest(path) == expected, 'Saved CPU return RAM changed.')
    for i, (address, n, path, expected) in enumerate(backups):
        data = path.read_bytes()
        require(len(data) == n and hashlib.sha256(data).hexdigest() == expected, 'Saved RAM changed before restore.')
        readback = evidence / f'restored-dram-{i}.bin'
        with base.sealed_source(data) as (source, descriptor):
            tx('write', hex(address), source, 'read', hex(address), n, readback, pass_fds=(descriptor,))
        require(readback.read_bytes() == data, 'DRAM restoration readback differs.')
        base.sync_file(readback)
    record.pop('mode', None)
    record.update(result=result, complete=True, physical_access=True, dirty=False,
                  dram_restored=True, rom_usable=True, ownership_release_permitted=False,
                  cpu_rail_restoration_pending=True,
                  result_limit='RAM continuity only; verify boot clocks and restore CPU rails before shared reuse.')
    journal()
    return result


def main():
    parser = argparse.ArgumentParser(description='Decode a copied OS result record into its payload.')
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True)
    parser.add_argument('--epoch', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--generation', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    info = host.image_manifest(args.image)
    payload = host.decode_result(args.record.read_bytes(), args.epoch, args.generation, info['manifest']['output_limit'])
    with args.output.open('xb') as stream:
        stream.write(payload)
    print(json.dumps({'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()}))


if __name__ == '__main__':
    main()
