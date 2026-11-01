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
spec = importlib.util.spec_from_file_location('session_card', ROOT / 'apps/os/card.py')
card = importlib.util.module_from_spec(spec)
spec.loader.exec_module(card)
spec = importlib.util.spec_from_file_location('session_slots', ROOT / 'tools/boot-slots.py')
slots = importlib.util.module_from_spec(spec)
spec.loader.exec_module(slots)


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
    run(CLANG, '-std=c11', '-O2', '-no-pie', '-Wl,-Ttext-segment=0x41000000', '-I', ROOT / 'include', '-I', gen,
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
    def exchange(request, state=1):
        op, flags, count, offset, argument = struct.unpack('<BBHIQ', request)
        assert (op, flags, argument) == (0x80, 0, 0)
        part = result.read_bytes()[offset:offset + count] if state else b''
        return struct.pack('<8I2Q16s', 0x424d494d, 1, state, len(part), len(result.read_bytes()),
                           offset, 0, op, 0, 0, bytes(16)) + part
    assert host.read_result(exchange, image, 0, 0) == payload
    assert host.read_result(lambda request: exchange(request, 0), image, 0, 0) is None
    refused(lambda: host.read_result(lambda request: exchange(request)[:-1], image, 0, 0), 'frame state or size')
    prefix = bytearray(host.PREFIX_BYTES)
    struct.pack_into('<3Q', prefix, 0, 0x4153373342535332, 2, host.PREFIX_BYTES)
    struct.pack_into('<I', prefix, 400, 2)
    # Board/service validation is outside this framing model. The extent bytes
    # are opaque and retain their native record lengths.
    thermal = bytearray(host.SERVICE_BYTES[host.THERMAL])
    struct.pack_into('<3Q', thermal, 0, host.SERVICE_MAGIC[host.THERMAL], 1,
                     host.PREFIX_BYTES + len(thermal))
    services = {host.THERMAL: bytes(thermal)}
    wire = host.session_bytes(prefix, services, manifest, info['sha256'], 0, host.BIND_SID, [data])
    bound = slots.os_session_generation(wire, 37)
    assert struct.unpack_from('<2Q', bound, 24) == (0, 37)
    assert bound[host.PREFIX_BYTES:] == wire[host.PREFIX_BYTES:]
    assert host.session_decode(bound, manifest, info['sha256'])['manifest_sha256'] == info['sha256']
    refused(lambda: slots.os_session_generation(bound, 38), 'unassigned zero-epoch')
    cpu_thermal = bytearray(services[host.THERMAL])
    struct.pack_into('<Q', cpu_thermal, 16, host.PREFIX_BYTES + len(cpu_thermal) + host.SERVICE_BYTES[host.CPU_GRANT])
    cpu_grant = struct.pack('<6Q4I32s', host.SERVICE_MAGIC[host.CPU_GRANT], 1, 96, 0x99, 0, 0,
                            3, 2016, 2002, 32, bytes([1]) * 32)
    cpu_wire = host.session_bytes(prefix, {host.THERMAL: cpu_thermal, host.CPU_GRANT: cpu_grant},
                                  manifest, info['sha256'], 0, host.BIND_SID, [data])
    cpu_bound = slots.os_session_generation(cpu_wire, 38)
    assert host.session_decode(cpu_bound)['extents'][host.CPU_GRANT] == cpu_grant
    decoded = host.session_decode(wire, manifest, info['sha256'])
    assert decoded['extents'][host.THERMAL] == services[host.THERMAL]
    facts, uploads = host.input_facts(manifest, [data])
    assert decoded['extents'][host.INPUTS] == facts
    assert uploads == [{'index': 0, 'address': manifest['regions'][0]['address'], 'bytes': len(data),
                        'sha256': hashlib.sha256(data).hexdigest()}]
    headset = copy.deepcopy(manifest)
    headset['devices_optional'] = 1 | 8
    headset['audio'] = {'rate_hz': 16000, 'channels': 1, 'bits': 16,
                        'min_frames': 16000, 'max_frames': 80000}
    headset_digest = hashlib.sha256(host.manifest_tool.pack(headset)).hexdigest()
    headset_prefix = bytearray(prefix)
    struct.pack_into('<I', headset_prefix, 400, 0)
    headset_wire = host.session_bytes(headset_prefix, services, headset, headset_digest, 1 | 8, inputs=[data])
    assert host.AUDIO not in host.session_decode(headset_wire, headset, headset_digest)['extents']
    bundle = out / 'bundle'
    frozen = prepare.prepare(image, prefix, services, [data], bundle, devices=0, flags=host.BIND_SID)
    assert frozen['uploads'][0]['address'] == manifest['regions'][0]['address']
    assert (bundle / 'session.bin').read_bytes() == wire
    empty_bundle = out / 'empty-bundle'
    prepare.prepare(image, prefix, services, [b''], empty_bundle, devices=0, flags=host.BIND_SID)
    assert (empty_bundle / 'input-0.bin').read_bytes() == b''
    source.write_bytes(b'')
    run(image, source, result)
    assert struct.unpack('<IIQ', host.decode_result(result.read_bytes(), 0, 0, 16)) == (1, 0, 0)
    source.write_bytes(data)
    run(image, source, result)
    (bundle / 'input-0.bin').write_bytes(b'changed')
    refused(lambda: prepare.validate_bundle(bundle), 'input changed')
    (bundle / 'input-0.bin').write_bytes(data)
    prepare.validate_bundle(bundle)
    cases = ['compiled-manifest', 'C-result-producer', 'result-symbol-capacity',
             'framed-result', 'framed-result-not-ready', 'truncated-result-frame', 'typed-session-producer',
             'manifest-upload', 'headset-without-I2S-extent', 'actual-bundle-producer',
             'actual-empty-input-bundle-and-result', 'changed-frozen-input',
             'actual-slot-generation-binder', 'assigned-slot-refused', 'slot-binder-preserves-CPU-service']
    image_raw = image.read_bytes()
    elf = host.image_manifest(image)['elf']
    table = elf.get_section_by_name('.symtab')
    result_index = next(index for index, symbol in enumerate(table.iter_symbols()) if symbol.name == 'ss_result_record')
    result_offset = table['sh_offset'] + result_index * table['sh_entsize']
    for name, offset, form, value, reason in [
            ('absolute-result-symbol', 6, '<H', 0xfff1, 'data object'),
            ('function-result-symbol', 4, '<B', 0x12, 'data object'),
            ('result-outside-section', 8, '<Q', 0x03006000, 'allocated writable data')]:
        altered = bytearray(image_raw)
        struct.pack_into(form, altered, result_offset + offset, value)
        refused(lambda: host.result_geometry(altered), reason)
        cases.append(name)
    card.flat_runtime(image_raw)
    # Mutate an actual load segment into a BSS-only reserved-memory collision.
    load_index = next(index for index, segment in enumerate(elf.iter_segments()) if segment['p_type'] == 'PT_LOAD')
    altered = bytearray(image_raw)
    offset = elf['e_phoff'] + load_index * elf['e_phentsize']
    struct.pack_into('<Q', altered, offset + 16, 0x41eff000)
    struct.pack_into('<2Q', altered, offset + 32, 0, 0x2000)
    refused(lambda: card.flat_runtime(altered), 'reserved memory')
    cases.append('BSS-only-reserved-memory-collision')
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
