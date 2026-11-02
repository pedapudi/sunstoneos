#!/usr/bin/env python3
"""The backup probe's wire formats: input.py's offsets equal the compiler's, an input round-trips, and a synthetic report decodes
into a backup file in the format tools/storage_bounds.py hashes."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'apps/sd-backup-probe'))
import input as probe_input


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    names = ['stage', 'reason', 'done', 'planned', 'identified_us', 'elapsed_us', 'cid', 'csd', 'ocr', 'rca', 'sectors', 'scr', 'mode',
             'card_hz', 'baseline_start', 'baseline_end', 'clock_off', 'pmic_parked', 'capture_done', 'sector', 'sd_status',
             'status_result', 'au_sectors', 'au_sectors_uhs', 'status_us', 'switch_status', 'uhs']
    source = ('#include <stddef.h>\n#include <stdio.h>\n#include "probe.h"\nint main(void){\n' +
              ''.join(f'printf("{n} %zu\\n", offsetof(struct sdp_report, {n}));\n' for n in names) +
              'printf("report %zu\\ninput %zu\\n", sizeof(struct sdp_report), sizeof(struct sdp_input));return 0;}\n')
    (a.output / 'o.c').write_text(source)
    r = subprocess.run([str(harness.CLANG), '-I', str(ROOT / 'apps/sd-backup-probe'), str(a.output / 'o.c'), '-o', str(a.output / 'o')], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    got = dict(line.split() for line in subprocess.run([str(a.output / 'o')], capture_output=True, text=True, check=True).stdout.splitlines())
    for n in names:
        assert int(got[n]) == probe_input.OFFSET[n], (n, got[n], probe_input.OFFSET[n])
    assert int(got['report']) == probe_input.REPORT_BYTES and int(got['input']) == probe_input.INPUT_BYTES
    sectors = [0] + list(range(32768, 32770)) + list(range(62656, 62720))
    raw = probe_input.pack_input(sectors)
    magic, reads, blocks, first, count = struct.unpack_from('<Q4I', raw, 0)
    assert (magic, reads, blocks, first, count) == (probe_input.INPUT_MAGIC, 1, 8, 0, len(sectors))
    assert list(struct.unpack_from(f'<{count}I', raw, 24)) == sectors
    report = bytearray(probe_input.REPORT_BYTES)
    o = probe_input.OFFSET
    struct.pack_into('<Q', report, o['magic'], probe_input.REPORT_MAGIC)
    struct.pack_into('<Q', report, o['capture_done'], count)
    struct.pack_into('<3Q', report, o['status_result'], 1, 8192, 8192)
    for i, s in enumerate(sectors):
        report[o['sector'] + 512 * i:o['sector'] + 512 * (i + 1)] = bytes((s + j) & 0xff for j in range(512))
    out, records = probe_input.decode(bytes(report), sectors)
    assert out['au_sectors'] == 8192 and out['captured'] == count and len(records) == 520 * count
    assert records[:8] == struct.pack('<Q', 0) and records[520:528] == struct.pack('<Q', 32768)
    try:
        probe_input.decode(bytes(report), sectors + [70000])
    except SystemExit:
        pass
    else:
        raise AssertionError('a short capture was accepted')
    print(f'sd-backup-probe: {len(names)} offsets equal the compiler\'s; {count}-sector input and report round-trip into a backup file')


if __name__ == '__main__':
    main()
