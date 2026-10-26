#!/usr/bin/env python3
"""Write or verify the DRAM pattern of the MBUS qualification through FEL.

  mbus-dram-pattern.py write|verify BUS:DEV EVIDENCE_DIR

Two 4 MiB regions, at 0x60000000 and 0xc0000000, above the 64 MiB at
0x40000000 that the DDR start, the launchers, the returning images and their
result blocks use (docs/memory-qualification.md, "Memory layout"). Each
64-bit word holds its own address XOR 0x5a5aa5a5c3c33c3c, so a word moved
to another address also fails. write uploads both regions with one
`sunxi-fel write` each; verify reads them back with `sunxi-fel read` and
compares every byte. The tool runs only the pinned sunxi-fel, records every
command in EVIDENCE_DIR/pattern-<mode>.json, never retries, and exits 1 on
any difference."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

FEL = Path.home() / '.cache/sunstoneos/build/sunxi-a733/sunxi-fel'
FEL_SHA = '4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e'
REGIONS = (0x60000000, 0xc0000000)
BYTES = 4 << 20
KEY = 0x5a5aa5a5c3c33c3c


def pattern(base):
    return b''.join(struct.pack('<Q', (base + 8 * i) ^ KEY) for i in range(BYTES // 8))


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ('write', 'verify'):
        sys.exit(__doc__)
    mode, device, evidence = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    evidence.mkdir(parents=True, exist_ok=True)
    if hashlib.sha256(FEL.read_bytes()).hexdigest() != FEL_SHA:
        sys.exit('sunxi-fel changed')
    record = {'mode': mode, 'device': device, 'regions': [], 'physical_access': True}
    ok = True
    for base in REGIONS:
        expected = pattern(base)
        path = evidence / f'pattern-{base:08x}.bin' if mode == 'write' else evidence / f'read-{base:08x}.bin'
        if mode == 'write':
            path.write_bytes(expected)
            command = [str(FEL), '-d', device, 'write', hex(base), str(path)]
        else:
            command = [str(FEL), '-d', device, 'read', hex(base), str(BYTES), str(path)]
        start = time.time()
        run = subprocess.run(command, capture_output=True, text=True, timeout=60)
        entry = {'base': hex(base), 'command': command, 'exit_code': run.returncode, 'seconds': time.time() - start,
                 'expected_sha256': hashlib.sha256(expected).hexdigest()}
        if run.returncode:
            entry['stderr'] = run.stderr
            ok = False
        elif mode == 'verify':
            data = path.read_bytes()
            entry['read_sha256'] = hashlib.sha256(data).hexdigest()
            differ = [i for i in range(0, min(len(data), BYTES), 8) if data[i:i + 8] != expected[i:i + 8]]
            entry.update(bytes_read=len(data), differing_words=len(differ), first_difference=hex(base + differ[0]) if differ else None)
            ok = ok and len(data) == BYTES and not differ
        record['regions'].append(entry)
        (evidence / f'pattern-{mode}.json').write_text(json.dumps(record, indent=1) + '\n')
        if run.returncode:
            break
    record['passed'] = ok
    (evidence / f'pattern-{mode}.json').write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'mode': mode, 'passed': ok}))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
