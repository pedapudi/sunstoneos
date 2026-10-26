#!/usr/bin/env python3
"""Linked combined performance/NPU admission and USB diagnostics.

Reuses the OS NPU instruction model. Native admission and USB dispatch run
from the ELF; RTOS and USB transport effects are modeled. No physical clock
change or operation is established by these cases.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('perf_native_npu', ROOT / 'checks/os-npu.py')
npu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(npu)


def resident(a, manifest):
    """Actual resident admission/USB dispatch; thermal publication is modeled."""
    assert manifest['lifetime'] == 'resident' and manifest['inference']['workers'] == 8
    image = npu.Image(a.build / 'sunstone-a733-os.elf', layout_names=npu.NAMES)
    raw = bytearray(a.resident_session.read_bytes())
    assert len(raw) == 1176 and raw[48:80].hex() == manifest['build_id']
    # The card template has generation but no epoch until monitor entry.
    assert struct.unpack_from('<Q', raw, 24)[0] == 0
    generation = struct.unpack_from('<Q', raw, 32)[0]
    assert generation and struct.unpack_from('<2Q', raw, len(raw)-64) == (0, generation)
    epoch = 17
    struct.pack_into('<Q', raw, 24, epoch)
    struct.pack_into('<Q', raw, len(raw)-64, epoch)
    npu.prep.session_check(raw, False)
    npu.prep.manifest_cpu_clock(manifest, raw)
    m = npu.Machine(image, raw)
    assert m.call('os_session_valid', m.session) == 1
    cases = ['actual resident card grant admitted']
    m.now = 70000000
    m.u.mem_write(m.board, bytes(raw[:592]))
    publication = image.address('report', unit='apps/os/perf.c')
    sequence = image.address('report_sequence', unit='apps/os/perf.c')
    mailbox = image.address('mailbox', unit='apps/os/perf.c')
    # Only publication readiness is supplied by the model; no owner work runs.
    m.u.mem_write(publication, bytes(192));m.put(sequence, 2, 4)
    def exchange(command, request_epoch):
        stopped, reply = m.exchange(struct.pack('<BBHIQ', 8, command, 0, 0x11b if command == 2 else 0, request_epoch))
        assert not stopped and len(reply) == 192
        assert struct.unpack_from('<2Q', reply, 16) == (epoch, generation)
        return reply
    reply = exchange(0, 0)
    assert struct.unpack_from('<I', reply, 32)[0] == 0 and not m.get(mailbox, 8)
    cases.append('resident report remains available beyond finite workload')
    reply = exchange(2, epoch-1)
    assert struct.unpack_from('<I', reply, 32)[0] == 3 and not m.get(mailbox, 8)
    cases.append('stale epoch refuses before mailbox publication')
    reply = exchange(2, epoch)
    answer, number = struct.unpack_from('<2I', reply, 32)
    queued = m.get(mailbox, 8)
    assert answer == 1 and number and queued == number << 32 | 2 << 12 | 0x11b
    cases.append('fresh epoch publishes exactly one command after workload expiry')
    reply = exchange(2, epoch)
    assert struct.unpack_from('<I', reply, 32)[0] == 2 and m.get(mailbox, 8) == queued
    cases.append('occupied mailbox refuses a second command')
    m.put(mailbox, 0, 8)
    m.put(publication+40, number, 4);m.put(publication+44, number, 4);m.put(sequence, 4, 4)
    reply = exchange(0, 0)
    assert struct.unpack_from('<2I', reply, 40) == (number, number)
    cases.append('USB returns the modeled matching completion publication')
    (a.output / 'MODEL-session.bin').write_bytes(raw)
    result = {'cases': cases, 'physical_operation': False, 'thermal_publication_modeled': True,
              'workers': manifest['inference']['workers'],
              'image_sha256': hashlib.sha256((a.build / 'sunstone-a733-os.elf').read_bytes()).hexdigest(),
              'session_sha256': hashlib.sha256(a.resident_session.read_bytes()).hexdigest(),
              'inputs': {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in
                         ('checks/perf-native.py', 'checks/os-npu.py', 'apps/os/admission.c',
                          'apps/os/prepare.py', 'apps/mimus-board/usb0.c', 'apps/os/perf.c')}}
    (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS resident performance native admission and opcode8:', len(cases), 'cases')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    sessions = p.add_mutually_exclusive_group(required=True)
    sessions.add_argument('--cpu-session', type=Path)
    sessions.add_argument('--resident-session', type=Path, help='Bound card template; only resident admission/epoch/mailbox cases')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((a.build / 'os-image/link-check.json').read_text())
    assert manifest['perf_modes'] and manifest['npu'] and manifest['cpu_clock']
    assert manifest['perf_operation'] == 8 and manifest['thermal_report']
    if a.resident_session:
        resident(a, manifest)
        return
    image = npu.Image(a.build / 'sunstone-a733-os.elf', layout_names=npu.NAMES)
    cpu = a.cpu_session.read_bytes()
    assert len(cpu) == 904
    raw = bytearray(cpu[:808] + bytes(272) + cpu[808:])
    raw[48:80] = bytes.fromhex(manifest['build_id'])
    struct.pack_into('<Q', raw, 608, len(raw))
    struct.pack_into('<3I', raw, 488, 0x03000002, 0x83000000, 0x84000000)
    struct.pack_into('<3I', raw, 500, 200000000, 600000000, 400000000)
    struct.pack_into('<6Q4I2Q', raw, 808, npu.prep.NPU_MAGIC, 1, 272, 31, 41, 51, 4,
                     manifest['npu_profile']['client_model'], 2, 0, 0x47400000, 0x800000)
    raw[888:920] = bytes.fromhex(manifest['npu_profile']['catalogue'])
    raw[920:1080] = bytes([1]) * 160
    struct.pack_into('<I', raw, 804, 70000)
    m = npu.Machine(image, raw)
    cases = []

    def accept(value):
        npu.prep.session_check(value, False)
        npu.prep.manifest_cpu_clock(manifest, value)
        m.u.mem_write(m.session, bytes(value))
        assert m.call('os_session_valid', m.session) == 1

    accept(raw)
    cases.append('combined read-only grant, native admission, thermal trip70000')
    epoch, generation = struct.unpack_from('<2Q', raw, 24)
    granted = bytearray(raw)
    struct.pack_into('<2Q4I', granted, 1112, epoch, generation, 3, 1794, 2002, 64)
    granted[1144:1176] = bytes([9]) * 32
    accept(granted)
    cases.append('combined both-cluster grant, native admission')
    for name, offset, fmt, value in (
            ('CPU-NPU owner collision', 1104, '<Q', 31),
            ('single-cluster perf grant', 1128, '<I', 1),
            ('critical trip above vendor limit', 804, '<I', 110001)):
        changed = bytearray(granted)
        struct.pack_into(fmt, changed, offset, value)
        m.u.mem_write(m.session, bytes(changed))
        assert m.call('os_session_valid', m.session) == 0, name
        try:
            npu.prep.session_check(changed, False)
            npu.prep.manifest_cpu_clock(manifest, changed)
        except ValueError:
            pass
        else:
            raise AssertionError(name)
        cases.append(name)
    accept(raw)
    stopped, reply = m.exchange(struct.pack('<BBHIQ', 8, 0, 0, 0, 0))
    assert not stopped and len(reply) == 192
    assert struct.unpack_from('<I', reply)[0] == 0x4d465250
    assert struct.unpack_from('<I', reply, 32)[0] == 2
    cases.append('native USB opcode8 early busy report')
    stopped, reply = m.exchange(struct.pack('<BBHIQ', 7, 0, 64, 0, 0))
    assert not stopped and len(reply) == 64
    assert struct.unpack_from('<I', reply)[0] == 0x424d494d
    assert struct.unpack_from('<I', reply, 28)[0] == 7
    cases.append('native USB opcode7 thermal diagnostic preserved')
    (a.output / 'MODEL-session.bin').write_bytes(raw)
    result = {'cases': cases, 'physical_operation': False,
              'image_sha256': hashlib.sha256((a.build / 'sunstone-a733-os.elf').read_bytes()).hexdigest(),
              'inputs': {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in
                         ('checks/perf-native.py', 'checks/os-npu.py', 'apps/os/admission.c',
                          'apps/os/prepare.py', 'apps/mimus-board/usb0.c', 'apps/os/perf.c')}}
    (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS combined performance/NPU native admission and op7/op8: %d cases' % len(cases))


if __name__ == '__main__':
    main()
