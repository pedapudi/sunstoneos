#!/usr/bin/env python3
"""Check generic host records against compiled manifest and C result producers."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
from harness import CLANG, environment, refused

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('session_host', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
spec = importlib.util.spec_from_file_location('session_prepare', ROOT / 'apps/os/prepare.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    gen = out / 'generated'
    def run(*argv):
        done = subprocess.run(list(map(str, argv)), capture_output=True, text=True, env=environment())
        with (out / 'commands.log').open('a') as log:
            log.write(repr(list(map(str, argv))) + '\n' + done.stdout + done.stderr)
        assert done.returncode == 0, done.stderr
    run(sys.executable, ROOT / 'tools/sunstone-manifest.py', 'emit', '--manifest',
        ROOT / 'apps/hello-compute/manifest.json', '--output', gen)
    # This existing C call model uses the public header to produce the result.
    image = out / 'hello-compute'
    layout = out / 'result-layout.c'
    layout.write_text('#include "sunstone_session.h"\n'
                      'unsigned char ss_result_record[sizeof(struct ss_result) + 16];\n')
    run(CLANG, '-std=c11', '-O2', '-I', ROOT / 'include', '-I', gen,
        ROOT / 'checks/sunstone-app-abi.c', ROOT / 'apps/hello-compute/main.c',
        gen / 'hello_compute-manifest.c', layout, '-o', image)
    info = host.image_manifest(image)
    manifest = info['manifest']
    assert info['section'] == (gen / 'manifest.bin').read_bytes()
    data = b'Wikipedia'
    source = out / 'input.bin'
    source.write_bytes(data)
    result = out / 'result.bin'
    run(image, source, result)
    payload = host.decode_result(result.read_bytes(), 0, 0, manifest['output_limit'])
    assert struct.unpack('<IIQ', payload) == (0x11e60398, 0, len(data))
    assert host.result_geometry(image)['bytes'] == len(result.read_bytes())
    prefix = bytearray(host.PREFIX_BYTES)
    struct.pack_into('<I', prefix, 400, 2)
    # Board/service validation is outside this framing model. The extent bytes
    # are opaque and retain their native record lengths.
    thermal = bytearray(host.SERVICE_BYTES[host.THERMAL])
    struct.pack_into('<3Q', thermal, 0, host.SERVICE_MAGIC[host.THERMAL], 1,
                     host.PREFIX_BYTES + len(thermal))
    services = {host.THERMAL: bytes(thermal)}
    wire = host.session_bytes(prefix, services, manifest, info['sha256'], 0, host.BIND_SID, [data])
    decoded = host.session_decode(wire, manifest, info['sha256'])
    assert decoded['extents'][host.THERMAL] == services[host.THERMAL]
    facts, uploads = host.input_facts(manifest, [data])
    assert decoded['extents'][host.INPUTS] == facts
    assert uploads == [{'index': 0, 'address': manifest['regions'][0]['address'], 'bytes': len(data),
                        'sha256': hashlib.sha256(data).hexdigest()}]
    bundle = out / 'bundle'
    frozen = prepare.prepare(image, prefix, services, [data], bundle, devices=0, flags=host.BIND_SID)
    assert frozen['uploads'][0]['address'] == manifest['regions'][0]['address']
    assert (bundle / 'session.bin').read_bytes() == wire
    (bundle / 'input-0.bin').write_bytes(b'changed')
    refused(lambda: prepare.validate_bundle(bundle), 'input changed')
    (bundle / 'input-0.bin').write_bytes(data)
    prepare.validate_bundle(bundle)
    cases = ['compiled-manifest', 'C-result-producer', 'result-symbol-capacity', 'typed-session-producer',
             'manifest-upload', 'actual-bundle-producer', 'changed-frozen-input']
    def bad(name, raw, reason):
        refused(lambda: host.session_decode(raw, manifest, info['sha256']), reason)
        cases.append(name)
    bad('truncated-header', wire[:655], 'session size')
    altered = bytearray(wire)
    altered[624] ^= 1
    bad('manifest-digest', altered, 'manifest digest')
    altered = bytearray(wire)
    struct.pack_into('<I', altered, 616, 1)
    bad('unlisted-device', altered, 'device grant')
    altered = bytearray(wire)
    struct.pack_into('<I', altered, 620, 0)
    bad('unbound-storage-free', altered, 'SID binding')
    altered = bytearray(wire)
    struct.pack_into('<I', altered, 656 + 8 + 216, host.THERMAL)
    bad('duplicate-extent', altered, 'Duplicate')
    altered = bytearray(wire)
    struct.pack_into('<Q', altered, decoded['offsets'][host.THERMAL] + 16, 999)
    bad('service-record-size', altered, 'service record header')
    altered = bytearray(wire)
    struct.pack_into('<Q', altered, decoded['offsets'][host.INPUTS], manifest['regions'][0]['bytes'] + 1)
    bad('oversized-input-fact', altered, 'Input length')
    # An unknown extension is allowed, but its alignment and uniqueness still apply.
    extended = host.session_bytes(prefix, {**services, 17: b'x'}, manifest, info['sha256'], 0, host.BIND_SID, [data])
    altered = bytearray(extended)
    altered[host.session_decode(extended)['offsets'][17] + 1] = 1
    bad('nonzero-padding', altered, 'padding')
    pinned = copy.deepcopy(manifest)
    pinned['regions'][0].update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    host.input_facts(pinned, [data])
    refused(lambda: host.input_facts(pinned, [b'wrongdata']), 'pinned manifest')
    cases.append('pinned-input-mismatch')
    for name, offset, value, reason in [('result-status', 40, 0, 'incomplete'),
                                         ('result-overflow', 48, 17, 'capacity'),
                                         ('result-session', 24, 99, 'another session')]:
        altered = bytearray(result.read_bytes())
        struct.pack_into('<Q', altered, offset, value)
        refused(lambda: host.decode_result(altered, 0, 0, manifest['output_limit']), reason)
        cases.append(name)
    # The actual linked manifest supplies backup regions; the input-only image
    # contributes no private spans, and uploaded adjacent ranges are merged.
    assert host.backup_regions(image, [{'address': 0x41000000, 'bytes': 0x1000},
                                       {'address': 0x41001000, 'bytes': 0x1000}]) == [
                                           {'address': 0x41000000, 'bytes': 0x2000}]
    (out / 'session.bin').write_bytes(wire)
    summary = {'passed': True, 'physical_access': False, 'cases': cases,
               'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
               'manifest_sha256': info['sha256'],
               'scope': 'Real compiled application and C result producer; board and services are modeled opaque records.'}
    (out / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('PASS', len(cases), 'generic host record cases')


if __name__ == '__main__':
    main()
