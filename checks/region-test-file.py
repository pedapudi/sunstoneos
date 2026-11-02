#!/usr/bin/env python3
"""Check the deterministic region test file generator against an independent computation."""
import hashlib
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import region_test_file as g

for size in (1, 31, 32, 33, 4096, 100003, 1 << 20):
    want = b''.join(hashlib.sha256(b'sunstone-region-test-v1' + i.to_bytes(8, 'little')).digest()
                    for i in range((size + 31) // 32))[:size]
    for chunk in (1, 7, 32768):
        assert b''.join(g.stream(size, chunk)) == want, (size, chunk)
# The whole file: its length and digest are the pinned values.
digest, total = hashlib.sha256(), 0
for data in g.stream(g.SIZE):
    digest.update(data)
    total += len(data)
assert total == 536870912 and digest.hexdigest() == g.EXPECTED_SHA256, (total, digest.hexdigest())
print('region test file generator passed')
