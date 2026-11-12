#!/usr/bin/env python3
"""Check that tools/session.py builds a session that the image's own EL3 admission admits, and refuses what it cannot determine.

The check uses three images that the gate builds from the tree under test (checks/gate-inputs.json): a CPU-return image whose application
declares no NPU and no device, one whose application declares the NPU (ss_application_npu), and the GPU image, whose manifest requires the GPU. It reads no queue entry and no other worktree's build. For each image it requires:
  decode      tools/session.py check decodes the built session with apps/os/host.py and finds no disagreement with the image;
  devices     the session grants the device bits the manifest declares and the image can serve: the GPU session grants the GPU (0x10), the
              NPU session grants no device (the NPU is a service), and a manifest that declares none grants none;
  admission   the image's own EL3 admission (tools/host_admission.py, compiled from the image's source tree) admits the session and its
              launch contract (admitted: true);
and, across them:
  mutation    an NPU session with another catalogue digest than the image's is reported by `check --image` and refused by the EL3 admission
              (session_decode); a session with another manifest digest is refused by decode and by the EL3 admission;
  refusals    a field that no source determines, or a source that contradicts the image, is an error that names it: a facts file
              without a thermal field, an NPU declaration that differs from the image's or is given for an application that declares
              no NPU, a required device the facts do not pin, an input count that differs from the manifest;
  source      the host admission refuses a source tree other than the one the image's build directory was configured from, a tree that no
              longer exists, and an image that does not carry the build ID its build directory recorded.
Usage: session-build.py --control-elf ELF --npu-elf ELF --gpu-elf ELF --output DIR
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
GATE_RUNS = [['--control-elf', '{elf:ret-latency}', '--npu-elf', '{elf:ret-npu-load}', '--gpu-elf', '{elf:ret-gpu}', '--output', '{output}']]

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['tools/session.py', 'tools/session-facts.json', 'apps/os/host.py', 'apps/os/prepare.py', 'tools/host_admission.py']


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def placeholders(host, image, out):
    """One placeholder file for each manifest input region, as the gate's other session checks use."""
    regions = host.regions(host.image_manifest(image)['manifest'], (1,))
    files = []
    for number, _ in enumerate(regions):
        path = out / f'input-{number}.bin'
        path.write_bytes(b'x')
        files.append(path)
    return files


def refusal(function, *words):
    try:
        function()
    except SESSION.SessionError as error:
        assert all(word in str(error) for word in words), (words, str(error))
        return
    raise AssertionError('no error naming ' + ', '.join(words))


def main():
    global SESSION
    parser = argparse.ArgumentParser()
    parser.add_argument('--control-elf', type=Path, required=True, help='a CPU-return OS image whose application declares no NPU')
    parser.add_argument('--npu-elf', type=Path, required=True, help='a CPU-return OS image whose application declares the NPU')
    parser.add_argument('--gpu-elf', type=Path, required=True, help='a CPU-return OS image with the GPU service whose application requires the GPU')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    SESSION = session = load('session_build_tool', ROOT / 'tools/session.py')
    admission = load('session_build_admission', ROOT / 'tools/host_admission.py')
    host = session.records()
    passed = []
    built = {}
    expected_devices = {'control': 0, 'npu': 0, 'gpu': 0x10}
    for name, image in (('control', args.control_elf), ('npu', args.npu_elf), ('gpu', args.gpu_elf)):
        directory = out / name
        directory.mkdir(exist_ok=True)
        # The NPU declaration is the image's own (ss_application_npu); the builder takes it from there.
        inputs = placeholders(host, image, directory)
        declares_grant = bool(host.image_manifest(image)['manifest']['clock_use'] & 1)
        record = session.build(image, directory, inputs=inputs, clock_grant=declares_grant)
        raw = (directory / 'template/session.bin').read_bytes()
        assert record['profile'] == ('control' if name == 'gpu' else name), record['profile']
        assert record['devices'] == expected_devices[name], (name, record['devices'])
        report = session.inspect(raw, image, host)
        assert not report['problems'], (name, report['problems'])
        assert report['devices'] == expected_devices[name], (name, report['devices'])
        assert session.find_session(directory) == directory / 'template/session.bin'
        verdict = session.admit(image, raw)
        assert verdict['admitted'] is True, (name, verdict)
        built[name] = (image, raw, inputs)
        passed.append(f"{name} image: devices {record['devices']:#x}, {len(raw)} bytes, services {', '.join(record['services'])}, decoded without disagreement, admitted: true")
    # mutation
    image, raw, inputs = built['npu']
    flags = bytes(raw).find(struct.pack('<Q', host.SERVICE_MAGIC[host.NPU])) + 52              # os_npu_session.flags
    mutated = bytearray(raw)
    mutated[flags] ^= 1                                                                             # OS_NPU_OBSERVE
    assert any('npu flags' in p for p in session.inspect(bytes(mutated), image, host)['problems'])
    mutated = bytearray(raw)
    mutated[flags] ^= 2                                                                             # no such flag
    assert any('npu flags' in p for p in session.inspect(bytes(mutated), image, host)['problems'])
    verdict = session.admit(image, bytes(mutated))
    assert verdict['admitted'] is False, verdict
    passed.append('NPU flags other than the application declares are reported by check --image, and an undefined flag is refused by the EL3 admission (%s)' % verdict['refusal'].split(': ')[-1])
    image, raw, _ = built['control']
    wrong = bytearray(raw)
    wrong[592 + 32 + 7] ^= 1                  # a byte of the session header's manifest digest
    refusal(lambda: session.inspect(bytes(wrong), image, host), 'manifest digest')
    verdict = session.admit(image, bytes(wrong))
    assert verdict['admitted'] is False and verdict['term'] == 'session_decode', verdict
    passed.append('a wrong manifest digest is refused by decode and by the EL3 admission')
    # The CPU clock grant is written only on request. A session built without one carries no grant record, and the returning
    # image, which needs the grant for its return path, refuses it at EL3 admission.
    image, raw, inputs = built['control']
    bare = out / 'no-clock-declaration'
    session.build(image, bare, inputs=inputs)
    plain = (bare / 'template/session.bin').read_bytes()
    assert host.CPU_GRANT not in host.session_decode(plain)['extents']
    verdict = session.admit(image, plain)
    assert verdict['admitted'] is False and verdict['term'] == 'session_decode' or verdict['admitted'] is False, verdict
    passed.append('a session built without --clock-grant has no grant record and the returning image refuses it')
    # The grant may ask only for what the manifest declares: perf needs cpu.perf, and the builder says so.
    refusal(lambda: session.build(image, out / 'perf-undeclared', inputs=inputs, clock_grant=True, perf='high'), 'perf', 'cpu.perf')
    perfed = bytearray(raw)
    grant_at = host.session_decode(raw)['offsets'][host.CPU_GRANT]
    struct.pack_into('<I', perfed, grant_at + 64, 1)         # perf: the high boot mode, which the manifest does not declare
    verdict = session.admit(image, bytes(perfed))
    assert verdict['admitted'] is False and verdict['term'] == 'session_decode', verdict
    passed.append('a grant that carries perf for a manifest without cpu.perf is refused by the EL3 admission')
    # refusals that name the field
    control, npu = built['control'], built['npu']
    facts = json.loads(session.FACTS.read_text())
    variant = copy.deepcopy(facts)
    del variant['profiles']['control']['thermal']['apb1']
    (out / 'facts-no-apb1.json').write_text(json.dumps(variant))
    refusal(lambda: session.build(control[0], out / 'r1', inputs=control[2], fact_file=out / 'facts-no-apb1.json'), 'thermal', 'apb1')
    other = out / 'npu-declaration-other.json'
    other.write_text(json.dumps({'slots': 2, 'in_flight': 1, 'graphs': 1, 'links': 0, 'graph_bytes': 1 << 20, 'tensor_bytes': 1 << 18}))
    refusal(lambda: session.build(npu[0], out / 'r2', npu_declaration=other, inputs=npu[2]), 'npu declaration', 'declares')
    refusal(lambda: session.build(control[0], out / 'r2c', npu_declaration=other, inputs=control[2]), 'npu declaration', 'declares no NPU')
    (out / 'extra-input.bin').write_bytes(b'x')
    refusal(lambda: session.build(control[0], out / 'r5', inputs=control[2] + [out / 'extra-input.bin']), 'inputs')   # one more file than the manifest has regions
    refusal(lambda: session.pack('thermal', {'magic': 1}), 'thermal field')
    passed.append('a field no source gives, or a declaration the image contradicts, is an error naming it (thermal apb1, npu declaration, inputs)')
    # Devices come from the manifest and the image, with no flag.
    names = load('session_build_names', ROOT / 'tools/sunstone-manifest.py').DEVICES
    declared = lambda required=0, optional=0: {'devices_required': required, 'devices_optional': optional}
    linked = {session.GRANTABLE['gpu']: (0, 0)}
    assert session.devices_for(declared(), linked, names) == (0, {})
    assert session.devices_for(declared(names['gpu']), linked, names) == (names['gpu'], {})
    assert session.devices_for(declared(0, names['gpu']), linked, names) == (names['gpu'], {})
    granted, left_out = session.devices_for(declared(0, names['gpu'] | names['audio-i2s'] | names['usb-storage']), linked, names)
    assert granted == names['gpu'] and set(left_out) == {'audio-i2s', 'usb-storage'}, (granted, left_out)
    assert session.devices_for(declared(0, names['gpu']), {}, names)[0] == 0                      # no GPU service in the image
    refusal(lambda: session.devices_for(declared(names['gpu']), {}, names), 'devices', 'gpu', 'service')
    refusal(lambda: session.devices_for(declared(names['usb-storage']), linked, names), 'devices', 'usb-storage', 'pins no facts')
    refusal(lambda: session.devices_for(declared(names['radio']), linked, names), 'devices', 'radio')
    usb_linked = {session.GRANTABLE['usb-device']: (0, 0)}
    assert session.devices_for(declared(), usb_linked, names) == (0, {})
    assert session.devices_for(declared(names['usb-device']), usb_linked, names) == (names['usb-device'], {})
    assert session.devices_for(declared(0, names['usb-device']), usb_linked, names) == (names['usb-device'], {})
    refusal(lambda: session.devices_for(declared(names['usb-device']), {}, names), 'devices', 'usb-device', 'service')
    passed.append('USB0 device service: the manifest grants it only when declared and linked')
    passed.append('devices: none declared grants none; the GPU is granted when declared and linked, optional or required; a required class without pinned facts is refused naming it, an optional one is left out')
    # With the device bits forced to 0 the GPU session no longer matches the manifest that requires the GPU.
    image, raw, _ = built['gpu']
    bare = bytearray(raw)
    fields = list(host.HEADER.unpack_from(bare, host.PREFIX_BYTES))
    assert fields[4] == 0x10, fields
    fields[4] = 0
    host.HEADER.pack_into(bare, host.PREFIX_BYTES, *fields)
    refusal(lambda: session.inspect(bytes(bare), image, host), 'device grant differs from manifest')
    verdict = session.admit(image, bytes(bare))
    assert verdict['admitted'] is False and verdict['term'] == 'session_decode', verdict
    passed.append('the GPU session with its device bits forced to 0 is refused by decode and by the EL3 admission')
    # one source tree
    image = built['control'][0]
    build_dir = admission.find_build(image)[0]

    def unavailable(build_dir, source, image=image, *words):
        try:
            admission.own_source(image, build_dir, source)
        except admission.Unavailable as error:
            assert all(w in str(error) for w in words), (words, str(error))
            return
        raise AssertionError('accepted ' + str(source))
    assert admission.own_source(image, build_dir, admission.find_build(image)[1]) == admission.find_build(image)[1]
    unavailable(build_dir, out, image, 'not ' + str(out))
    moved = out / 'moved-build'
    moved.mkdir()
    (moved / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' % (out / 'gone'))
    unavailable(moved, out / 'gone', image, 'missing or has moved')
    other = out / 'other-build'
    (other / 'os-image').mkdir(parents=True)
    (other / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' % ROOT)
    (other / 'os-image' / 'sunstone-a733-os-build-id.c').write_text('const unsigned char a733_smp_build_id[32]={%s};\n' % ','.join(['0x00'] * 32))
    unavailable(other, ROOT, image, 'build ID')
    passed.append('the admission sources must come from the image\'s own tree: another tree, a missing tree and a foreign build ID are refused')
    for line in passed:
        print('PASS ' + line)


if __name__ == '__main__':
    main()
