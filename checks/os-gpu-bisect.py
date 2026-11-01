#!/usr/bin/env python3
"""Check diagnostic GPU retirement in the linked generic OS runtime.

The native return fixtures execute the OS retirement entry and its cold or
CLOSE/PARK proof with modeled clocks, queues and backend shutdown. This check
compares the emitted version-3 receipt with the compiled diagnostic marker and
rejects changed identity or shutdown conditions. Application job and layer limits
belong to the application's checks. No GPU, firmware or scheduler executes.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
from harness import Image, environment, refused, save

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
image = Image(a.image, ())
marker = image.address('os_gpu_bisect_configuration')
raw = next(data[marker-start:marker-start+12] for start, data in image.parts
           if start <= marker and marker+12 <= start+len(data))
version, stage, limit = struct.unpack('<3I', raw)
assert version == 1 and 1 <= stage <= 4
assert (stage not in (1, 2) or limit == 0) and (stage != 3 or limit > 0)
assert stage != 4 or limit < 255
spec = importlib.util.spec_from_file_location('gpu_return', ROOT/'apps/os/gpu-return.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)

if stage == 1:
    selected = ('cold-no-create', 'cold-ready-no-create', 'cold-refused-create',
                'cold-epoch', 'cold-service-epoch', 'cold-published-epoch',
                'cold-owed', 'cold-create-ticket', 'cold-unconsumed-record')
    script = 'os-gpu-untouched.py'
else:
    selected = ()
    script = 'os-gpu-return.py'
native = a.output/'native'
command = [sys.executable, str(ROOT/'checks'/script), '--image', str(a.image.resolve()),
           '--output', str(native)]
for name in selected:
    command += ['--case', name]
run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                     text=True, env=environment(), timeout=300)
(a.output/'native.log').write_text(run.stdout)
save(a.output/'commands.json', [command])
assert run.returncode == 0, run.stdout
summary = json.loads((native/'results.json').read_text())
assert summary['cases'] and all(row['passed'] for row in summary['cases'])
rows = []


def require(ok, why):
    if not ok:
        raise ValueError(why)


def decode(data, epoch, generation, allow=True):
    return decoder.decode(data, epoch, generation, require, allow_untouched=allow)


def mutate(data, index, value):
    result = bytearray(data)
    struct.pack_into('<Q', result, index*8, value)
    return bytes(result)


reports = sorted(native.glob('*-report.bin'))
assert reports, 'The linked fixture emitted no native receipt'
for report in reports:
    data = report.read_bytes()
    magic, actual_version, size, epoch, generation, jobs, flags, value = struct.unpack('<8Q', data)
    assert (magic, actual_version, size, flags, value) == (
        decoder.MAGIC, 3, 64, 0 if stage == 1 else 63, (stage << 32) | limit)
    actual = decode(data, epoch, generation)
    assert actual == {'jobs': jobs, 'gpu_bisect': {'stage': decoder.BISECT_STAGES[stage], 'limit': limit},
                      'gpu_off': stage != 1, 'gpu_never_powered': stage == 1,
                      'worker_parked': stage != 1, 'qualification_passed': False}
    refused(lambda: decode(data, epoch, generation, False), 'GPU bisect retirement')
    rows.append({'name': report.stem, 'passed': True,
                 'sha256': hashlib.sha256(data).hexdigest(), 'jobs': jobs})

sample = reports[0].read_bytes()
epoch, generation = struct.unpack_from('<2Q', sample, 24)
for name, index, value in (
    ('wrong-magic', 0, 0), ('wrong-size', 2, 63), ('wrong-epoch', 3, epoch+1),
    ('wrong-generation', 4, generation+1), ('maximum-sequence', 5, 0xffffffff),
    ('wrong-stop-conditions', 6, 63 if stage == 1 else 0), ('invalid-stage', 7, 5 << 32)):
    refused(lambda index=index, value=value: decode(mutate(sample, index, value), epoch, generation),
            'GPU bisect retirement')
    rows.append({'name': name, 'passed': True})
if stage == 1:
    refused(lambda: decode(mutate(sample, 5, 1), epoch, generation), 'GPU bisect retirement')
    rows.append({'name': 'cold-receipt-has-job', 'passed': True})
save(a.output/'results.json', {'physical_access': False, 'scope': __doc__,
     'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
     'configuration': {'stage': stage, 'limit': limit}, 'native_cases': summary['cases'],
     'cases': rows, 'application_limit_checks': 'Owned by the external application'})
print('PASS diagnostic GPU runtime retirement:', len(summary['cases']), 'native cases,',
      len(rows), 'receipt cases; stage', stage)
