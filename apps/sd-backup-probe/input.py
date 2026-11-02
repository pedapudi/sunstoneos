#!/usr/bin/env python3
"""Pack the input of the read-only microSD backup probe, or decode its report into a backup file; no device access.

The probe reads the SD Status register (ACMD13) and the listed sectors, one sector at a time, and returns them. `decode`
writes the sectors as a backup file (records of a u64 little-endian sector and its 512 bytes, in increasing sector order:
the format tools/storage_bounds.py hashes) and prints a JSON summary with the file's SHA-256 and the allocation unit."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

INPUT_MAGIC = 0x3144524450445353
REPORT_MAGIC = 0x3154505250445353
MAX_CAPTURE = 512
INPUT_BYTES = 2184
# Byte offsets in struct sdp_report (apps/sd-backup-probe/probe.h); checks/sd-backup-probe.py compares them with the compiler's.
OFFSET = dict(magic=0, stage=16, reason=24, done=32, planned=40, identified_us=48, elapsed_us=56, cid=80, csd=112, ocr=144,
              rca=152, sectors=160, scr=168, mode=184, card_hz=192, baseline_start=200, baseline_end=328, clock_off=456,
              pmic_parked=464, capture_done=472, sector=3296, sd_status=272848, status_result=272912, au_sectors=272920,
              au_sectors_uhs=272928, status_us=272936, switch_status=272944)
REPORT_BYTES = 273008


def pack_input(sectors):
    if not 1 <= len(sectors) <= MAX_CAPTURE or sectors != sorted(set(sectors)):
        raise SystemExit('give 1 to 512 distinct sectors')
    raw = struct.pack('<Q4I', INPUT_MAGIC, 1, 8, 0, len(sectors)) + struct.pack(f'<{MAX_CAPTURE}I', *(sectors + [0] * (MAX_CAPTURE - len(sectors))))
    raw += struct.pack('<2Q', 0, 0) + struct.pack('<QII48s32s', 0, 0, 0, b'', bytes(32))
    assert len(raw) == INPUT_BYTES
    return raw


SPEED_MODES = ('SDR12', 'SDR25', 'SDR50', 'SDR104', 'DDR50')


def capabilities(ocr, cid_hex, switch):
    """What the 3.3 V observation shows. S18A is OCR bit 24 of the ACMD41 response that carried S18R (no CMD11 was sent). The bus speed
    support bits are the CMD6 response's group 1 field (bytes 12 and 13, big-endian). The CID is manufacturer id (8 bits), OEM id
    (16), product name (40), revision, serial, manufacturing date."""
    cid = bytes.fromhex(cid_hex)
    support = (switch[12] << 8) | switch[13]
    return dict(s18a=bool(ocr & (1 << 24)), ccs=bool(ocr & (1 << 30)),
                bus_speed_support=[m for i, m in enumerate(SPEED_MODES) if support & (1 << i)], bus_speed_support_bits=support,
                selected_function=switch[16] & 15, max_current_support=(switch[0] << 8) | switch[1],
                manufacturer_id=cid[0], oem_id=cid[1:3].decode('ascii', 'replace'), product_name=cid[3:8].decode('ascii', 'replace'),
                product_revision=f'{cid[8] >> 4}.{cid[8] & 15}', serial=cid[9:13].hex(),
                manufacturing_date=f'20{(((cid[13] & 15) << 4) | (cid[14] >> 4)):02d}-{cid[14] & 15:02d}')


def decode(raw, requested):
    if len(raw) < REPORT_BYTES:
        raise SystemExit('report too short')
    q = lambda name, n=1: struct.unpack_from(f'<{n}Q', raw, OFFSET[name])
    if q('magic')[0] != REPORT_MAGIC:
        raise SystemExit('not a read-only SD backup probe report')
    captured = q('capture_done')[0]
    out = dict(stage=q('stage')[0], reason=q('reason')[0], done=q('done')[0], identified_us=q('identified_us')[0],
               cid=''.join(f'{w:08x}' for w in q('cid', 4)), csd=''.join(f'{w:08x}' for w in q('csd', 4)),
               ocr=q('ocr')[0], rca=q('rca')[0], card_sectors=q('sectors')[0], mode=q('mode')[0], card_hz=q('card_hz')[0],
               baseline_changed=[i for i, (a, b) in enumerate(zip(q('baseline_start', 16), q('baseline_end', 16))) if a != b],
               clock_off=q('clock_off')[0], pmic_parked=q('pmic_parked')[0], captured=captured,
               status_result=q('status_result')[0], au_sectors=q('au_sectors')[0], au_sectors_uhs=q('au_sectors_uhs')[0],
               status_us=q('status_us')[0], sd_status=raw[OFFSET['sd_status']:OFFSET['sd_status'] + 64].hex(),
               switch_status=raw[OFFSET['switch_status']:OFFSET['switch_status'] + 64].hex())
    out.update(capabilities(out['ocr'], out['cid'], bytes.fromhex(out['switch_status'])))
    if captured != len(requested):
        raise SystemExit(f'captured {captured} of {len(requested)} sectors; stage {out["stage"]} reason {out["reason"]}')
    records = b''.join(struct.pack('<Q', s) + raw[OFFSET['sector'] + 512 * i:OFFSET['sector'] + 512 * (i + 1)] for i, s in enumerate(requested))
    return out, records


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('input')
    i.add_argument('--output', type=Path, required=True)
    i.add_argument('--ranges', nargs='+', required=True, help='FIRST:COUNT pairs, for example 0:1 32768:2')
    d = sub.add_parser('decode')
    d.add_argument('--report', type=Path, required=True)
    d.add_argument('--ranges', nargs='+', required=True)
    d.add_argument('--backup', type=Path, required=True, help='the backup file to write')
    a = p.parse_args()
    sectors = sorted({s for r in a.ranges for s in range(int(r.split(':')[0]), int(r.split(':')[0]) + int(r.split(':')[1]))})
    if a.cmd == 'input':
        with a.output.open('xb') as f:
            f.write(pack_input(sectors))
        print(a.output, len(sectors), 'sectors')
    else:
        out, records = decode(a.report.read_bytes(), sectors)
        with a.backup.open('xb') as f:
            f.write(records)
        out.update(backup=str(a.backup), backup_sha256=hashlib.sha256(records).hexdigest(), backup_sectors=len(sectors))
        print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
