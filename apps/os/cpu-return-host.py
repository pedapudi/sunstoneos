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
import sys
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import memory_layout
LAYOUT = memory_layout.record()
spec = importlib.util.spec_from_file_location('return_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
require = host.require
thermal_capture = host.module('return_host_thermal', ROOT / 'apps/os/cpu-return-thermal.py')
gpu_capture = host.module('return_host_gpu', ROOT / 'apps/os/gpu-return.py')
npu_capture = host.module('return_host_npu', ROOT / 'apps/os/npu-report.py')
cpu = host.module('return_host_qualification', ROOT / 'apps/cpu-runtime-qualification/host.py')
base, digest = cpu.base, cpu.digest
PROFILE = 'os-cpu-return-v1'
CHUNK = 4 * 1024 * 1024
# The forced-FEL boot ROM words at 0x20, 0x24, 0x28, 0x3c and 0x40.
ROM_ENTRY = {0x20: 0xeb00008f, 0x24: 0xe3a00911, 0x28: 0xe3a0193d, 0x3c: 0xe3a0605c, 0x40: 0xea00004d}
# Image RAM below the session page; every run backs it up and restores it.
IMAGE_RAM = memory_layout.extent(LAYOUT['image']['image_ram'])
# The cached writable RAM: data, bss, stacks and heap, below the monitor interval.
RAM = tuple(LAYOUT['image']['ram'])
# The copied result record's evidence name, which follow-up tools read.
OUTPUT = 'recorded-output'



# The NPU pool (docs/designs/npu-resources.md, "The pool"). Either the extent every image has had so far, 8 MiB at
# 0x47400000, or a pool sized from the session's declaration: a multiple of 2 MiB between 10 and 48 MiB that ends at
# 0x47000000, the bottom of the monitor interval. Both lie in the write-back RAM that EL0 reaches (0x40000000 to
# 0x48000000), above the application regions' lowest address 0x44000000, and clear of the image RAM below 0x42200000.
NPU_POOL_LEGACY = tuple(LAYOUT['npu_pool']['legacy'])
NPU_POOL_TOP, NPU_POOL_MIN, NPU_POOL_MAX, NPU_POOL_BLOCK = (LAYOUT['npu_pool'][k] for k in ('top', 'min', 'max', 'block'))


def npu_pool_extent_valid(low, high):
    """True for the legacy extent and for a sized one."""
    if (low, high) == NPU_POOL_LEGACY:
        return True
    size = high - low
    return high == NPU_POOL_TOP and NPU_POOL_MIN <= size <= NPU_POOL_MAX and size % NPU_POOL_BLOCK == 0

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
    require(LAYOUT['monitor']['bss_first'] <= monitor[0] < monitor[1] <= at('__a733_smp_boot_stack_top') <= LAYOUT['monitor']['tables'] and
            symbols['a733_qualification_report'][1] == cpu.REPORT_BYTES and
            symbols['a733_qualification_stacks'][1] == 32768 and
            monitor[0] <= report < report + cpu.REPORT_BYTES <= monitor[1] and
            monitor[0] <= stacks < stacks + 32768 <= monitor[1] and
            (report + cpu.REPORT_BYTES <= stacks or stacks + 32768 <= report),
            'CPU return Secure Device allocations differ.')
    require(RAM[0] <= at('__bss_start') <= at('__bss_end') <= at('__heap_end') <= LAYOUT['image']['writable_ceiling'],
            'CPU return cached writable range differs.')
    return {'report_address': report, 'report_bytes': cpu.REPORT_BYTES,
            'cleanup_stacks': [stacks, stacks + 32768], 'monitor': monitor,
            'cached_writable': [RAM[0], (at('__heap_end') + 63) & ~63]}


def geometry(image):
    """The return layout of an image whose application finishes with ss_finish.

    Besides layout(): the result record, the manifest's WORK and OUTPUT
    regions to back up, the thermal window to copy when the application
    records one (os_thermal_record_begin and os_thermal_record_end) and, in an
    image that admits the host-armed watchdog, its marker."""
    image = Path(image)
    result = layout(image)
    output = host.result_geometry(image)
    require(RAM[0] <= output['address'] < output['address'] + output['bytes'] <= RAM[1],
            'Copied result geometry differs.')
    result.update(application_regions=host.backup_regions(image), output_address=output['address'],
                  output_bytes=output['bytes'])
    symbols = linked_symbols(image)
    if 'os_cpu_return_rail_uv' in symbols:
        address, size = symbols['os_cpu_return_rail_uv']
        low, high = result['cached_writable']
        require(size == 8 and address % 4 == 0 and low <= address < address + size <= high and
                symbols['__bss_start'][0] <= address < address + size <= symbols['__bss_end'][0],
                'CPU rail readback object differs from writable image BSS.')
        result['cpu_rails'] = {'address': address, 'bytes': size}
    pool_names = ('__os_npu_pool_start', '__os_npu_pool_end')
    present = [name in symbols for name in pool_names]
    require(present[0] == present[1], 'Partial NPU pool geometry.')
    if all(present):
        low, high = [symbols[name][0] for name in pool_names]
        require(npu_pool_extent_valid(low, high), 'NPU pool extent differs.')
        manifest = host.image_manifest(image)['manifest']
        for region in manifest['regions'] if manifest else []:
            a, n = region['address'], region['bytes']
            require(a + n <= low or a >= high, 'Application region overlaps the NPU pool.')
        require('os_npu_published_report' in symbols, 'NPU stop producer absent.')
        address, size = symbols['os_npu_published_report']
        begin, end = result['cached_writable']
        require(size == npu_capture.REPORT_BYTES and begin <= address < address + size <= end,
                'NPU stop report outside preserved image RAM.')
        result['npu_pool'] = {'address': low, 'bytes': high-low}
        result['npu_stop'] = {'address': address, 'bytes': size}
    else:
        require('os_npu_published_report' not in symbols, 'NPU stop producer without owned pool.')
    thermal_producers = [name in symbols for name in ('os_thermal_record_begin', 'os_thermal_record_end')]
    require(thermal_producers[0] == thermal_producers[1], 'Partial thermal producer presence.')
    if all(thermal_producers):
        result['thermal'] = thermal_capture.geometry(image)
    gpu = gpu_capture.geometry(image)
    if gpu is not None:
        result['gpu_receipt'] = gpu
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


def decode_npu_stop(raw, epoch, generation, failure_generation=None):
    """Mandatory native local-stop publication; application output is separate.

    failure_generation is None unless the image returned after a failure
    (mandatory_return). Then it is the generation of the failure record: a
    report with finished=2 (apps/os/npu.h) says that the NPU service never
    ran, so nothing was powered, submitted or retained, and carries that
    generation. Without a failure return such a report is refused."""
    require(len(raw) == npu_capture.REPORT_BYTES, 'Incomplete native NPU stop report.')
    report = dict(zip(npu_capture.FIELDS, struct.unpack_from(npu_capture.LAYOUT, raw)))
    never = report['finished'] == 2
    require(not never or failure_generation is not None,
            'A never-started NPU report is valid only with a failure return.')
    require((report['magic'], report['version'], report['bytes'], report['epoch'], report['generation']) ==
            (npu_capture.MAGIC, 1, npu_capture.REPORT_BYTES, epoch, failure_generation if never else generation),
            'Native NPU stop layout or session differs.')
    if never:
        require((report['stopped'], report['retained']) == (1, 0) and not any(
                report[key] for key in npu_capture.FIELDS[6:] if key not in ('retained', 'stopped')) and
                not any(raw[72:]), 'Native NPU never-started report carries results or retention.')
    else:
        require((report['finished'], report['stopped'], report['retained']) == (1, 1, 0),
                'Native NPU local stop incomplete or faulted.')
    # Functional mismatch/refusal does not revoke local quiescence. Its copied
    # operands survive restoration and are classified by the application.
    return {key: report[key] for key in ('finished', 'stopped', 'retained', 'mismatches', 'refused', 'jobs', 'jobs_selected',
                                         'generation')}


def mandatory_return(copied, qualification, epoch, generation, failure_return=False):
    """Validate CPU and optional NPU retirement before restoring saved RAM.

    failure_return: the image returned after its first failure. The NPU stop
    report may then be the never-started report, bound to the failure
    record's epoch and generation (decode_npu_stop)."""
    result = decode_report(copied['return-report'], epoch)
    if 'npu_stop' in qualification:
        failure_generation = None
        if failure_return:
            failure, _ = host.decode_failure(copied.get(OUTPUT, copied.get('output-record')), epoch, generation)
            failure_generation = generation if failure['generation_bound'] else 0
        result['npu_stop'] = decode_npu_stop(copied['npu-stop'], epoch, generation, failure_generation)
    return result


def decode_record(copied, p, result, record, evidence):
    """Default decoder: the OS result record's payload, written to result.bin."""
    q = p['qualification']
    payload = host.decode_result(copied[OUTPUT], p['epoch'], p['generation'],
                                 q['output_bytes'] - host.RESULT.size)
    (evidence / 'result.bin').write_bytes(payload)
    base.sync_file(evidence / 'result.bin')
    result.update(payload_bytes=len(payload), payload_sha256=hashlib.sha256(payload).hexdigest())


def decode_rails(raw):
    """The clock owner's last confirmed PMIC readbacks; zero means unread.

    These readings precede the external helper's rail normalization. START
    can close before any inspection and reports both rails as unread.
    """
    dcdc5, dcdc3 = struct.unpack('<2I', raw)
    return {'dcdc5_uv': dcdc5 or None, 'dcdc3_uv': dcdc3 or None,
            'source': 'last confirmed PMIC readbacks at CPU clock close'}


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
    record, copied[OUTPUT], into result. Payload and device telemetry errors
    are retained for classification after DRAM restoration. A failed transport
    request or mandatory CPU return proof stops immediately."""
    require(not record.get('launch_attempted'), 'One CPU return attempt only.')
    ready()
    q = p['qualification']
    # Back up every modified DRAM range before any upload. Returned ROM owns
    # SRAM; only these copied DRAM bytes may be restored after native return.
    if extents is None:
        extents = [(r['address'], r['bytes']) for r in q['application_regions']]
    if 'npu_pool' in q:
        pool = q['npu_pool']
        require(npu_pool_extent_valid(pool['address'], pool['address'] + pool['bytes']), 'NPU backup extent differs.')
        extents = [*extents, (pool['address'], pool['bytes'])]
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
    if 'npu_stop' in q:
        reads.append(('npu-stop', q['npu_stop']['address'], q['npu_stop']['bytes']))
    if 'cpu_rails' in q:
        reads.append(('cpu-rails', q['cpu_rails']['address'], q['cpu_rails']['bytes']))
    if 'gpu_receipt' in q:
        reads.append(('gpu-report', q['gpu_receipt']['address'], q['gpu_receipt']['bytes']))
    if 'thermal' in q:
        reads.extend(thermal_capture.captures(q['thermal']))
    for name, address, size in reads:
        path = evidence / (name + '.bin')
        tx('read', hex(address), size, path)
        require(path.stat().st_size == size, 'Incomplete copied CPU return output.')
        base.sync_file(path)
        copied[name] = path.read_bytes()
    result = mandatory_return(copied, q, p['epoch'], p['generation'])
    if 'cpu_rails' in q:
        result['cpu_rails'] = decode_rails(copied['cpu-rails'])
        record['cpu_rail_capture_sha256'] = hashlib.sha256(copied['cpu-rails']).hexdigest()
    deferred_error = None
    try:
        decode(copied, p, result, record, evidence)
    except (ValueError, KeyError, struct.error) as error:
        deferred_error = error
        result['output_error'] = str(error)
        record['output_capture_status'] = 'malformed'
        record['output_capture_error'] = str(error)
    if 'gpu_receipt' in q:
        record['gpu_capture_sha256'] = hashlib.sha256(copied['gpu-report']).hexdigest()
        try:
            result['gpu'] = gpu_capture.decode_captured(copied['gpu-report'], q['gpu_receipt'],
                                                        p['epoch'], p['generation'])
            record['gpu_capture_status'] = 'decoded'
        except (ValueError, KeyError, struct.error) as error:
            deferred_error = deferred_error or error
            result['gpu_error'] = str(error)
            record['gpu_capture_status'] = 'malformed'
            record['gpu_capture_error'] = str(error)
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
            deferred_error = deferred_error or error
            result['thermal_error'] = str(error)
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
    # Existing consumers provide their own decoder and rely on its exception
    # to reject a run. Preserve that contract after restoring all saved RAM.
    if decode is not decode_record and deferred_error is not None:
        raise deferred_error
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


# Compatibility names for the single-owner board runner.
decode_return_report = decode_report

def capture_result(tx, image, epoch, generation, output):
    geometry = host.result_geometry(image)
    output = Path(output)
    require(not output.exists(), "Result capture destination already exists.")
    tx("read", hex(geometry["address"]), geometry["bytes"], output)
    raw = output.read_bytes()
    info = host.image_manifest(image)
    payload = host.decode_result(raw, epoch, generation, info["manifest"]["output_limit"])
    return {"payload": payload, "record_sha256": hashlib.sha256(raw).hexdigest()}
