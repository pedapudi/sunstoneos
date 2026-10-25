#!/usr/bin/env python3
"""Decode UHS-I SD0 records (version 4) and refuse inconsistent ones; summarize throughput.

Synthetic records only: the host observer apps/storage-read/observe.py and the
launcher's throughput plan in tools/check-a733-storage-read-supervised.py run
unchanged. No image, board, USB or card access."""
import argparse, hashlib, importlib.util, json, struct
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__); p.add_argument('--output', type=Path, required=True)
a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path); value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value); return value


observe = module('uhs_observe', ROOT / 'apps/storage-read/observe.py')
launcher = module('uhs_launcher', ROOT / 'tools/check-a733-storage-read-supervised.py')
BUILD = bytes(range(32))


def session(profile):
    raw = bytearray(880); struct.pack_into('<2Q', raw, 24, 11, 12); raw[48:80] = BUILD
    struct.pack_into('<3Q', raw, 592, 0x4153373352444131, 2, 880); struct.pack_into('<I', raw, 860, profile)
    return bytes(raw)


def record(profile, *, version=4, status=1, width=4, timing=3, divisor=0x81, hz=150000000, uhs=None, elapsed=180):
    payload = bytes((i * 7) & 255 for i in range(4096)) if status == 1 else b''
    raw = bytearray(288)
    struct.pack_into('<4I2Q', raw, 0, 0x44524453, version, status, len(payload), 11, 12)
    if payload: raw[32:64] = hashlib.sha256(payload).digest()
    struct.pack_into('<IHBB', raw, 96, 0xc0ff8000, 0x1234, 1, 0); struct.pack_into('<Q', raw, 104, 1 << 25)
    facts = [0] * 19; facts[5] = len(payload); facts[18] = 1
    if version == 4: facts[6] = elapsed if status == 1 else 0; facts[7] = 0 if uhs is None else uhs
    struct.pack_into('<19I', raw, 112, *facts); raw[208:240] = BUILD
    struct.pack_into('<3Q4I', raw, 240, 5, 64, 1, 8 if payload else 0, 0, 0, 0)
    struct.pack_into('<2I', raw, 280, profile | width << 8 | timing << 16 | divisor << 24, hz)
    return bytes(raw) + payload


def word(outcome, signal, caps=3, delay=0, start=0, width=0):
    return outcome | signal << 4 | caps << 5 | delay << 8 | start << 16 | width << 24


accepted, refused = [], []
for name, profile, kwargs in [
        ('SDR104 150 MHz', 3, dict(uhs=word(1, 1, 3, 32, 20, 24))),
        ('SDR104 100 MHz', 3, dict(divisor=2, hz=100000000, uhs=word(1, 1, 1, 9, 2, 14))),
        ('host lacks 1.8 V: 3.3 V high speed', 3, dict(timing=1, divisor=4, hz=50000000, uhs=word(2, 0, 2))),
        ('card declines: 3.3 V high speed', 3, dict(timing=1, divisor=4, hz=50000000, uhs=word(3, 0))),
        ('no SDR104: SDR25 at 1.8 V', 3, dict(timing=1, divisor=4, hz=50000000, uhs=word(4, 1))),
        ('no window: SDR25 at 1.8 V', 3, dict(timing=1, divisor=4, hz=50000000, uhs=word(5, 1, 3, 0, 40, 11))),
        ('profile 2 at version 4', 2, dict(timing=1, divisor=4, hz=50000000, uhs=0)),
        ('profile 2 at version 3 from an older image', 2, dict(version=3, timing=1, divisor=4, hz=50000000))]:
    value = observe.decode(record(profile, **kwargs), session(profile)); accepted.append(name)
    assert value['status'] == 1 and value['card_hz'] == kwargs.get('hz', 150000000)
    if profile == 3:
        assert value['uhs_outcome'] == kwargs['uhs'] & 15 and value['elapsed_us'] == 180
        assert value['module_hz'] == (300000000 if kwargs.get('divisor', 0x81) == 0x81 else 400000000 // kwargs['divisor'])
for name, profile, kwargs in [
        ('SDR104 at the 400 MHz / 4 divisor', 3, dict(divisor=4, hz=50000000, uhs=word(1, 1))),
        ('SDR104 without the bank switch', 3, dict(uhs=word(1, 0))),
        ('bank switched with a declined CMD11', 3, dict(timing=1, divisor=4, hz=50000000, uhs=word(3, 1))),
        ('missing outcome', 3, dict(uhs=word(0, 1))),
        ('300 MHz parent outside UHS-I', 2, dict(timing=1, divisor=0x81, hz=150000000, uhs=0)),
        ('UHS-I state in profile 2', 2, dict(timing=1, divisor=4, hz=50000000, uhs=word(1, 1))),
        ('profile 3 at version 3', 3, dict(version=3, uhs=None)),
        ('rate that differs from the divisor', 3, dict(hz=100000000, uhs=word(1, 1))),
        ('timing on a pending record', 3, dict(status=0, elapsed=99, uhs=word(1, 1))),
        ('sample delay beyond 63', 3, dict(uhs=word(1, 1, 3, 64)))]:
    try:
        raw = record(profile, **kwargs)
        if kwargs.get('status') == 0: raw = bytearray(raw); struct.pack_into('<I', raw, 136, 99); raw = bytes(raw)
        observe.decode(raw, session(profile))
    except ValueError:
        refused.append(name); continue
    raise AssertionError(name)
plan = launcher.throughput_plan(0, 1 << 24)
assert len(plan) == 256 and plan == launcher.throughput_plan(0, 1 << 24)
assert all(f % 8 == 0 and f + b <= 1 << 24 for f, b in plan[:224]) and len({f for f, _ in plan[128:224]}) > 90
reads = [dict(blocks=b, bytes=b * 512, elapsed_us=300 if b == 8 else 125) for _, b in plan]
summary = launcher.throughput_summary(reads)
assert summary['sequential']['mb_per_s'] == round(4096 * 128 / (300 * 128), 3) and summary['per_block_us'] == 25.0
assert summary['per_request_us'] == 100.0
for bad in (reads[:-1], reads[:-1] + [dict(reads[-1], elapsed_us=0)]):
    try: launcher.throughput_summary(bad)
    except ValueError: continue
    raise AssertionError('incomplete throughput reads accepted')
(a.output / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False, accepted=accepted,
    refused=refused, summary_example=summary), indent=2) + '\n')
print('PASS SD0 record version 4: %d accepted, %d refused; throughput plan of 256 reads and summary'
      % (len(accepted), len(refused)))
