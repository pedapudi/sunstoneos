#!/usr/bin/env python3
"""Execute linked crypto producer refusal and post-copy expiry erasure without hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from harness import Image
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0 as X0, UC_ARM64_REG_X1 as X1
from unicorn.arm64_const import UC_ARM64_REG_SP as SP, UC_ARM64_REG_LR as LR, UC_ARM64_REG_PC as PC
from harness import hook_addresses

p = argparse.ArgumentParser(description=__doc__)
for name in ('image', 'session', 'request', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
image = Image(a.image, ('crypto_application', 'crypto_run', 'crypto_request', 'a733_board_session'))
symbols = {name: image.address(name) for name in ('crypto', 'board', 'crypto_accept', 'board_live', 'a733_smp_counter')}
app = image.layouts['crypto_application']
run = image.layouts['crypto_run']
request_layout = image.layouts['crypto_request']
board = image.layouts['a733_board_session']
base = symbols['crypto']
request_at = base + app['run'] + run['request']
raw = a.request.read_bytes()
assert len(raw) == 2824
session = bytearray(a.session.read_bytes())
assert len(session) == 824
struct.pack_into('<Q', session, 600, 2)
session[760:792] = hashlib.sha256(raw).digest()
results = {}
for variant in ('refused-copy', 'expired-after-copy', 'accepted-copy'):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0x40000000, 0x4000000)
    cpu.mem_map(0x50000000, 0x1000000)
    for address, data in image.parts:
        cpu.mem_write(address, data)
    cpu.mem_write(base, bytes(session))
    cpu.mem_write(symbols['board'], bytes(session[:592]))
    cpu.mem_write(symbols['board'] + board['deadline_us'], struct.pack('<Q', 10**12))
    incoming = bytearray(raw)
    if variant == 'refused-copy':
        incoming[-1] ^= 1
    cpu.mem_write(0x50001000, bytes(incoming))
    calls = []
    finished = []
    def returned(value):
        cpu.reg_write(X0, value)
        cpu.reg_write(PC, cpu.reg_read(LR))
    def hook(machine, address, size, unused):
        if address == 0x50fff000:
            finished.append(True)
            machine.emu_stop()
        elif address == symbols['a733_smp_counter']:
            returned(24)
        elif address == symbols['board_live']:
            calls.append(True)
            returned(int(variant != 'expired-after-copy' or len(calls) == 1))
    hook_addresses(cpu, [0x50fff000, symbols['board_live'], symbols['a733_smp_counter']], hook)
    cpu.reg_write(X0, 0x50001000)
    cpu.reg_write(X1, len(raw))
    cpu.reg_write(SP, 0x50ffeff0)
    cpu.reg_write(LR, 0x50fff000)
    cpu.emu_start(symbols['crypto_accept'], 0, count=3000000)
    assert finished
    accepted = variant == 'accepted-copy'
    assert cpu.reg_read(X0) == accepted
    assert int.from_bytes(cpu.mem_read(base + app['input_ready'], 4), 'little') == accepted
    for field, count in (('key', 16), ('secret', 128), ('input', 2048), ('iv', 64), ('aad', 512)):
        actual = bytes(cpu.mem_read(request_at + request_layout[field], count))
        expected = raw[request_layout[field]:request_layout[field] + count] if accepted else bytes(count)
        assert actual == expected, (variant, field)
    # Metadata stays available for diagnosis; the incoming producer bytes are
    # outside owner storage and are not rewritten by the consumer's scrub.
    assert bytes(cpu.mem_read(0x50001000, len(incoming))) == bytes(incoming)
    assert len(calls) == (1 if variant == 'refused-copy' else 2)
    results[variant] = {'passed': True, 'liveness_calls': len(calls)}
(a.output / 'results.json').write_text(json.dumps({
    'result': 'PASS', 'physical_access': False, 'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
    'request_sha256': hashlib.sha256(raw).hexdigest(), 'cases': results}, indent=2) + '\n')
print('PASS linked crypto producer erasure on copy refusal/expiry and unchanged success publication')
