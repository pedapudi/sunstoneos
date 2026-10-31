#!/usr/bin/env python3
"""Exercise returned thermal capture against the exact native writer in an ELF.

Time, board identity and policy values are modeled. The linked record emitter,
window publication, object geometry and host decoder execute their actual code.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from harness import Image, refused
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_ARM64_REG_X1

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('return_thermal', ROOT / 'apps/os/cpu-return-thermal.py')
thermal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(thermal)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
layout = thermal.geometry(a.image)
image = Image(a.image, {'os_thermal_service', 'a733_thermal_policy', 'board_application', 'a733_board_session'})
m = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
m.mem_map(0x41000000, 0x1200000)
for address, data in image.parts:
    if 0x41000000 <= address and address + len(data) <= 0x42200000:
        m.mem_write(address, data)
m.mem_map(0x60000000, 0x10000)
time_us = 100000
stop = 0x6000fff0
counter = image.address('a733_smp_counter')
generation = image.address('mimus_board_generation')


def hook(cpu, address, size, unused):
    if address == stop:
        cpu.emu_stop()
    elif address in (counter, generation):
        cpu.reg_write(UC_ARM64_REG_X0, time_us * 24 if address == counter else 7)
        cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))


m.hook_add(UC_HOOK_CODE, hook)


def call(name, x0=0, x1=0):
    m.reg_write(UC_ARM64_REG_SP, 0x6000f000)
    m.reg_write(UC_ARM64_REG_LR, stop)
    m.reg_write(UC_ARM64_REG_X0, x0)
    m.reg_write(UC_ARM64_REG_X1, x1)
    m.emu_start(image.address(name), stop, count=100000)
    assert m.reg_read(UC_ARM64_REG_PC) == stop, name


def put(address, fmt, *values):
    m.mem_write(address, struct.pack(fmt, *values))


board = image.address('board') + image.layouts['board_application']['session']
for key, value in (('epoch', 11), ('generation', 12)):
    put(board + image.layouts['a733_board_session'][key], '<Q', value)
policy = image.address('thermal', 'return-thermal.c') + image.layouts['os_thermal_service']['policy']
fields = image.layouts['a733_thermal_policy']
put(policy + fields['seen'], '<B', 31)
put(policy + fields['cpu_critical_millicelsius'], '<I', 70000)
for i in range(5):
    put(policy + fields['millicelsius'] + i * 4, '<i', 40000 + i * 1000)
    put(policy + fields['seen_us'] + i * 8, '<Q', time_us)
call('record_reading', time_us, 0)
call('os_thermal_record_begin')
for n in range(1, 4):
    time_us += 100000
    put(policy + fields['sequence'], '<I', n)
    for i in range(5):
        put(policy + fields['seen_us'] + i * 8, '<Q', time_us)
    call('record_reading', time_us, 0)
call('os_thermal_record_end', 1)
copied = {name: bytes(m.mem_read(address, size)) for name, address, size in thermal.captures(layout)}
result = thermal.decode(layout, copied, 11, 12)
assert result['classification'] == 'complete' and result['rows'] == 4
assert result['recognition_generation'] == 7 and result['end_us'] - result['begin_us'] == 300000
assert result['readings'][0]['millicelsius'] == [40000, 41000, 42000, 43000, 44000]
cases = ['actual-native-emitter-and-published-window']
# The producer continues recording, while the first recognition stays immutable.
time_us += 100000
call('record_reading', time_us, 0)
call('os_thermal_record_begin')
call('os_thermal_record_end', 0)
later = {name: bytes(m.mem_read(address, size)) for name, address, size in thermal.captures(layout)}
assert thermal.decode(layout, later, 11, 12) == result
cases.append('later-samples-and-recognition-preserve-first-window')
for field, value, message in [('ready', 0, 'not published'), ('first', 603, 'bounds differ')]:
    bad = dict(copied)
    raw = bytearray(bad['thermal-window'])
    struct.pack_into('<I', raw, layout['window']['members'][field], value)
    bad['thermal-window'] = bytes(raw)
    refused(lambda: thermal.decode(layout, bad, 11, 12), message)
    cases.append('reject-' + field)
refused(lambda: thermal.decode(layout, copied, 99, 12), 'session differs')
refused(lambda: thermal.decode(layout, {**copied, 'thermal-readings': b''}, 11, 12), 'Short thermal')
cases += ['reject-session-mismatch', 'reject-short-capture']
for name, raw in copied.items():
    (a.output / (name + '.bin')).write_bytes(raw)
(a.output / 'result.json').write_text(json.dumps(dict(status='PASS', cases=cases, geometry=layout,
    image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(), report=result,
    limits='Actual native writer with modeled time, identity and policy; no physical thermal qualification.'), indent=2) + '\n')
print('PASS returned thermal window: actual native emitter, immutable publication and malformed capture refusal')
