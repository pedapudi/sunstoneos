#!/usr/bin/env python3
"""Check that tools/session.py builds the session that the board admitted, from the image alone.

Three images that ran on the board are rebuilt from the images and their input files: a mimus CPU-return image, an NPU queue image and the
storage-write image. Each is found in the board queue (~/.cache/sunstoneos/board-queue/entries) by its SHA-256. The check requires:
  decode      tools/session.py check decodes the built session with apps/os/host.py and finds no disagreement with the image;
  admission   the image's own EL3 admission (tools/host_admission.py) admits the built session and its launch contract (admitted: true);
  identical   the built session equals the session the queue entry ran (the entry's template for the NPU image, which a packaging
              script wrote; the session the runner prepared for the other two) byte for byte, except for the epoch, generation and
              lease that every build draws afresh, and the CPU grant's copies of the epoch and generation;
  mutation    an NPU session with another catalogue digest than the image's is reported by `check --image` and refused by the EL3
              admission (session_decode); a session whose manifest digest differs, and one with a
              wrong input count, are refused;
  refusals    a field that no source determines is an error that names it: a facts file without a thermal field, an NPU image without a
              declaration, a device grant without pinned board facts, an input count that differs from the manifest.
The admission of the NPU image is compiled from this checkout's sources with the image's own configured build (the image was built from
an older tree that lacks the NPU declaration unit of the host pre-flight). Usage: session-build.py --output DIR
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
CACHE = harness.CACHE
ENTRIES = CACHE / 'board-queue/entries'
CHECK_INPUTS = ['tools/session.py', 'tools/session-facts.json', 'apps/os/host.py', 'apps/os/prepare.py', 'tools/host_admission.py',
                'checks/data/npu-session-template/session.bin']
# What each case was built from. build: the CMake build directory that holds an image whose admission needs it.
CASES = [
    {'name': 'mimus CPU-return', 'image': '7e2d7f10de96', 'reference': 'run/bundle/session.bin'},
    {'name': 'NPU queue', 'image': '6cc60b2a4044', 'reference': 'template/session.bin',
     'declaration': {'slots': 1, 'in_flight': 64, 'graphs': 1, 'links': 0},      # the manifest.json the image was configured with
     'build': CACHE / 'build/npu-mimus-1103/bP5g128', 'build_image': 'sunstone-a733-os.elf'},
    {'name': 'storage-write', 'image': 'cb9bb22b5303', 'reference': 'run/bundle/session.bin'},
]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def entry_of(prefix):
    """The most recent queue entry of the image whose SHA-256 starts with prefix that returned, and its directory."""
    found = []
    for directory in sorted(ENTRIES.iterdir()):
        try:
            job = json.loads((directory / 'entry.json').read_text())
        except (OSError, ValueError):
            continue
        if job['image_sha256'].startswith(prefix) and (directory / 'run/result.json').is_file():
            if json.loads((directory / 'run/result.json').read_text()).get('outcome') == 'returned':
                found.append(directory)
    assert found, f'no returned queue entry of image {prefix} under {ENTRIES}'
    return found[-1]


def normalized(session, host, raw):
    """raw with the epoch, generation and lease (and the CPU grant's copies) zeroed, for the comparison of two sessions."""
    raw = bytearray(raw)
    raw[24:48] = bytes(24)
    decoded = host.session_decode(bytes(raw), legacy=True)
    if host.CPU_GRANT in decoded['offsets']:
        at = decoded['offsets'][host.CPU_GRANT] + 32
        raw[at:at + 16] = bytes(16)
    return bytes(raw)


def refusal(function, *words):
    try:
        function()
    except session.SessionError as error:
        message = str(error)
        assert all(word in message for word in words), (words, message)
        return message
    raise AssertionError('no error naming ' + ', '.join(words))


def main():
    global session
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    session = load('session_build_tool', ROOT / 'tools/session.py')
    host = session.records()
    passed = []
    for case in CASES:
        entry = entry_of(case['image'])
        image = entry / 'image.elf'
        assert hashlib.sha256(image.read_bytes()).hexdigest().startswith(case['image'])
        inputs = sorted(entry.glob('input-*.bin'), key=lambda p: int(p.stem.split('-')[1]))
        directory = out / case['name'].replace(' ', '-')
        declaration = None
        if 'declaration' in case:
            declaration = out / (directory.name + '-declaration.json')
            declaration.write_text(json.dumps(case['declaration']))
        record = session.build(image, directory, npu_declaration=declaration, inputs=inputs)
        raw = (directory / 'template/session.bin').read_bytes()
        # decode
        report = session.inspect(raw, image, host)
        assert not report['problems'], (case['name'], report['problems'])
        assert session.find_session(directory) == directory / 'template/session.bin'
        # admission
        build = case.get('build')
        if build:
            verdict = session.admit(build / case['build_image'], raw, build, ROOT)
        else:
            verdict = session.admit(image, raw)
        assert verdict['admitted'] is True, (case['name'], verdict)
        admitted = 'admitted: true'
        # identical
        reference = (entry / case['reference']).read_bytes()
        assert len(raw) == len(reference), (case['name'], len(raw), len(reference))
        assert normalized(session, host, raw) == normalized(session, host, reference), f"{case['name']}: differs from {entry.name}"
        differing = sum(a != b for a, b in zip(raw, reference))
        assert 0 < differing <= 40, (case['name'], differing)       # the fresh epoch, generation and lease, and the grant's copies
        passed.append(f"{case['name']} ({record['profile']}, {len(raw)} bytes, {admitted}, equals {entry.name} but {differing} identity bytes)")
        case['raw'], case['image_path'], case['directory'] = raw, image, directory
    # mutation
    npu = next(c for c in CASES if 'declaration' in c)
    mutated = bytearray(npu['raw'])
    at = bytes(mutated).find(struct.pack('<Q', host.SERVICE_MAGIC[host.NPU])) + 80          # struct os_npu_session.catalogue
    mutated[at] ^= 1
    problems = session.inspect(bytes(mutated), npu['image_path'], host)['problems']
    assert any('npu catalogue' in p for p in problems), problems
    build = npu['build']
    verdict = session.admit(build / npu['build_image'], bytes(mutated), build, ROOT)
    assert verdict['admitted'] is False and verdict['term'] == 'session_decode', verdict
    refused = 'refused by the EL3 admission (%s)' % verdict['refusal'].split(': ')[-1]
    passed.append('wrong catalogue digest ' + refused)
    control = CASES[0]
    wrong = bytearray(control['raw'])
    wrong[592 + 32 + 7] ^= 1              # a byte of the session header's manifest digest
    try:
        session.inspect(bytes(wrong), control['image_path'], host)
        raise AssertionError('a session with another manifest digest decoded')
    except session.SessionError as error:
        assert 'manifest digest' in str(error), str(error)
    verdict = session.admit(control['image_path'], bytes(wrong))
    assert verdict['admitted'] is False and verdict['term'] == 'session_decode', verdict
    passed.append('wrong manifest digest refused by decode and by the EL3 admission')
    # refusals that name the field
    image, facts = CASES[1]['image_path'], json.loads(session.FACTS.read_text())
    declaration = out / 'NPU-queue-declaration.json'
    inputs = sorted(entry_of(CASES[1]['image']).glob('input-*.bin'))
    variant = copy.deepcopy(facts)
    del variant['profiles']['control']['thermal']['apb1']
    (out / 'facts-no-apb1.json').write_text(json.dumps(variant))
    mimus = entry_of(CASES[0]['image'])
    mimus_inputs = sorted(mimus.glob('input-*.bin'))
    refusal(lambda: session.build(mimus / 'image.elf', out / 'r1', inputs=mimus_inputs, fact_file=out / 'facts-no-apb1.json'), 'thermal', 'apb1')
    refusal(lambda: session.build(image, out / 'r2', inputs=inputs), 'npu declaration')
    refusal(lambda: session.build(image, out / 'r3', npu_declaration=declaration, inputs=inputs, devices=1), 'devices')
    refusal(lambda: session.build(image, out / 'r4', npu_declaration=declaration, inputs=inputs[:1]), 'inputs')
    refusal(lambda: session.build(mimus / 'image.elf', out / 'r5', inputs=mimus_inputs, devices=1), 'devices')
    refusal(lambda: session.pack('thermal', {'magic': 1}), 'thermal field')
    passed.append('a field no source gives is an error naming it (thermal apb1, npu declaration, devices, inputs)')
    for line in passed:
        print('PASS ' + line)


if __name__ == '__main__':
    main()
