#!/usr/bin/env python3
"""Check that tools/session.py builds a session that the image's own EL3 admission admits, and refuses what it cannot determine.

The check uses two images that the gate builds from the tree under test (checks/gate-inputs.json): a CPU-return image without the NPU
service and one with it. It reads no queue entry and no other worktree's build. For each image it requires:
  decode      tools/session.py check decodes the built session with apps/os/host.py and finds no disagreement with the image;
  admission   the image's own EL3 admission (tools/host_admission.py, compiled from the image's source tree) admits the session and its
              launch contract (admitted: true);
and, across them:
  mutation    an NPU session with another catalogue digest than the image's is reported by `check --image` and refused by the EL3 admission
              (session_decode); a session with another manifest digest is refused by decode and by the EL3 admission;
  refusals    a field that no source determines is an error that names it: a facts file without a thermal field, an NPU image without a
              declaration, a device grant without pinned board facts, an input count that differs from the manifest;
  source      the host admission refuses a source tree other than the one the image's build directory was configured from, a tree that no
              longer exists, and an image that does not carry the build ID its build directory recorded.
Usage: session-build.py --control-elf ELF --npu-elf ELF --output DIR
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys

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
    parser.add_argument('--control-elf', type=Path, required=True, help='a CPU-return OS image without the NPU service')
    parser.add_argument('--npu-elf', type=Path, required=True, help='a CPU-return OS image with the NPU service')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    SESSION = session = load('session_build_tool', ROOT / 'tools/session.py')
    admission = load('session_build_admission', ROOT / 'tools/host_admission.py')
    host = session.records()
    passed = []
    built = {}
    for name, image in (('control', args.control_elf), ('npu', args.npu_elf)):
        directory = out / name
        directory.mkdir(exist_ok=True)
        declaration = None
        if name == 'npu':
            symbols, elf = session.read_symbols(image)
            models = struct.unpack('<I', session.symbol_bytes(elf, symbols, 'os_npu_model_count')[:4])[0]
            declaration = out / 'npu-declaration.json'
            declaration.write_text(json.dumps({'slots': 1, 'in_flight': 1, 'graphs': models, 'links': 0}))
        inputs = placeholders(host, image, directory)
        record = session.build(image, directory, npu_declaration=declaration, inputs=inputs)
        raw = (directory / 'template/session.bin').read_bytes()
        assert record['profile'] == name, record['profile']
        report = session.inspect(raw, image, host)
        assert not report['problems'], (name, report['problems'])
        assert session.find_session(directory) == directory / 'template/session.bin'
        verdict = session.admit(image, raw)
        assert verdict['admitted'] is True, (name, verdict)
        built[name] = (image, raw, inputs, declaration)
        passed.append(f"{name} image: {len(raw)} bytes, services {', '.join(record['services'])}, decoded without disagreement, admitted: true")
    # mutation
    image, raw, inputs, declaration = built['npu']
    mutated = bytearray(raw)
    mutated[bytes(mutated).find(struct.pack('<Q', host.SERVICE_MAGIC[host.NPU])) + 80] ^= 1      # os_npu_session.catalogue
    assert any('npu catalogue' in p for p in session.inspect(bytes(mutated), image, host)['problems'])
    verdict = session.admit(image, bytes(mutated))
    assert verdict['admitted'] is False and verdict['term'] == 'session_decode', verdict
    passed.append('a wrong catalogue digest is reported by check --image and refused by the EL3 admission (%s)' % verdict['refusal'].split(': ')[-1])
    image, raw, _, _ = built['control']
    wrong = bytearray(raw)
    wrong[592 + 32 + 7] ^= 1                  # a byte of the session header's manifest digest
    refusal(lambda: session.inspect(bytes(wrong), image, host), 'manifest digest')
    verdict = session.admit(image, bytes(wrong))
    assert verdict['admitted'] is False and verdict['term'] == 'session_decode', verdict
    passed.append('a wrong manifest digest is refused by decode and by the EL3 admission')
    # refusals that name the field
    control, npu = built['control'], built['npu']
    facts = json.loads(session.FACTS.read_text())
    variant = copy.deepcopy(facts)
    del variant['profiles']['control']['thermal']['apb1']
    (out / 'facts-no-apb1.json').write_text(json.dumps(variant))
    refusal(lambda: session.build(control[0], out / 'r1', inputs=control[2], fact_file=out / 'facts-no-apb1.json'), 'thermal', 'apb1')
    refusal(lambda: session.build(npu[0], out / 'r2', inputs=npu[2]), 'npu declaration')
    refusal(lambda: session.build(npu[0], out / 'r3', npu_declaration=npu[3], inputs=npu[2], devices=1), 'devices')
    refusal(lambda: session.build(control[0], out / 'r4', inputs=control[2], devices=1), 'devices')
    refusal(lambda: session.build(control[0], out / 'r5', inputs=control[2] + control[2]), 'inputs')
    refusal(lambda: session.pack('thermal', {'magic': 1}), 'thermal field')
    passed.append('a field no source gives is an error naming it (thermal apb1, npu declaration, devices, inputs)')
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
