#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check compiled split records, work conservation and decoder rejection paths."""
import argparse
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'checks'))
from harness import CLANG, SANITIZE, environment, refused

spec = importlib.util.spec_from_file_location('split_host', Path(__file__).with_name('split-host.py'))
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
env = {**environment(), 'TMPDIR': str(a.output.resolve())}
exe = a.output / 'split-model'
subprocess.run([str(CLANG), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', *SANITIZE,
                '-I' + str(root / 'include'), str(Path(__file__).with_name('split-model.c')),
                str(Path(__file__).with_name('kernel.c')), str(root / 'kernel/scheduler/topology.c'),
                '-o', str(exe)], check=True, env=env)
payload = subprocess.check_output([str(exe)], env=env)
(a.output / 'model.bin').write_bytes(payload)
d = host.decode(payload)
assert len(payload) == host.RESULT_BYTES
assert d['speedup'] > 1 and d['modes']['proportional']['median_finish_skew_us'] < d['modes']['equal']['median_finish_skew_us']
for offset, value, reason in ((0, 0, 'workload'), (32, 0, 'capacity'), (576 + 8, 0, 'readiness'),
                               (576 + 64, 1, 'range'), (576 + 64 + 24, 0, 'timing'),
                               (576 + 64 + 32, 0, 'digest'), (576 + 64 + 40, 1, 'core')):
    bad = bytearray(payload)
    struct.pack_into('<Q' if offset >= 576 or offset == 0 else '<I', bad, offset, value)
    refused(lambda: host.decode(bad), reason)
bad = bytearray(payload)
bad[320:576] = bytes(bad[320:576]).replace(b'transitions=3', b'transitions=5')
refused(lambda: host.decode(bad), 'epoch')
print('C split layout, private result lines, exact work digests, capacity shares, skew and epoch rejection pass; synthetic times only')
