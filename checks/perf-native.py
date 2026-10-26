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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--cpu-session', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((a.build / 'os-image/link-check.json').read_text())
    assert manifest['perf_modes'] and manifest['npu'] and manifest['cpu_clock']
    assert manifest['perf_operation'] == 8 and manifest['thermal_report']
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
