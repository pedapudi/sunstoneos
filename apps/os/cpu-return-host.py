#!/usr/bin/env python3
"""Copy an opaque application result and derive DRAM backups from its manifest.

The returning transport owner retains board locks, admission, recovery and rail
restoration obligations. Application adapters decode the returned payload.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('return_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
require = host.require


def geometry(image):
    result = host.result_geometry(image)
    record = {'output_address': result['address'], 'output_bytes': result['bytes'],
              'application_regions': host.backup_regions(image)}
    watchdog = host.watchdog_marker(image)
    if watchdog is not None:
        # Host-armed WDT0 and RTC breadcrumbs (include/sunstone_crumb.h).
        record['watchdog'] = watchdog
    return record


def decode_output(raw, epoch, generation, output_limit=None):
    return host.decode_result(raw, epoch, generation, output_limit)


def capture_result(tx, image, epoch, generation, output):
    geometry = host.result_geometry(image)
    output = Path(output)
    require(not output.exists(), 'Result capture destination already exists.')
    tx('read', hex(geometry['address']), geometry['bytes'], output)
    raw = output.read_bytes()
    info = host.image_manifest(image)
    payload = host.decode_result(raw, epoch, generation, info['manifest']['output_limit'])
    return {'payload': payload, 'record_sha256': hashlib.sha256(raw).hexdigest()}


def backup_regions(image, uploads):
    return host.backup_regions(image, uploads)


def decode_return_report(raw, epoch):
    """Validate normal all-core retirement without application workload claims."""
    cpu = host.module('return_report_layout', ROOT / 'apps/cpu-runtime-qualification/host.py')
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
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
