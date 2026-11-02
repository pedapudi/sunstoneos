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
              pmic_parked=464, capture_done=472, elapsed=480, crc=2528, sector=5344, sd_status=274896, status_result=274960, au_sectors=274968,
              au_sectors_uhs=274976, status_us=274984, switch_status=274992, uhs=275056)
REPORT_BYTES = 275184


def pack_input(sectors, uhs_mode=0, reads=1, blocks=8):
    if not 1 <= len(sectors) <= MAX_CAPTURE or sectors != sorted(set(sectors)):
        raise SystemExit('give 1 to 512 distinct sectors')
    raw = struct.pack('<Q4I', INPUT_MAGIC, reads, blocks, 0, len(sectors)) + struct.pack(f'<{MAX_CAPTURE}I', *(sectors + [0] * (MAX_CAPTURE - len(sectors))))
    raw += struct.pack('<2Q', 0, 0) + struct.pack('<QII48s32s', 0, 0, uhs_mode, b'', bytes(32))
    assert len(raw) == INPUT_BYTES
    return raw


def stop_fields_identification(w):
    """The stop word of a lifetime that failed before the card was identified (probe reason 3): fault line 10 bits, why 2, platform stage 5, the core's
    error 4 (sunxi_mmc_error: 1 io, 2 time, 3 timeout, 4 admission, 5 variant, 6 controller, 7 card, 8 unsupported, 9 signal), its protocol step 6 (the
    P_* enum of drivers/storage/sunxi_mmc.c), and the CMD19 sample delay reached divided by two, 5."""
    return dict(fault_line=w >> 22 & 1023, why=w >> 20 & 3, platform_stage=w >> 15 & 31, core_error=w >> 11 & 15, core_protocol=w >> 5 & 63,
                tune_delay_half=w & 31)


def stop_fields(w):
    """The stop word of a failed lifetime (esr of the fault record): fault line 10 bits, why 2, platform stage 5, S18A 1, support bits 5,
    card at 1.8 V 1, fold of the read CRCs 8."""
    return dict(fault_line=w >> 22 & 1023, why=w >> 20 & 3, platform_stage=w >> 15 & 31, s18a=w >> 14 & 1, group1_support_bits=hex(w >> 9 & 31),
                card_at_1v8=w >> 8 & 1, crc_fold=hex(w & 255))


UHS_OUTCOMES = ('none', 'sdr104', 'host', 'declined', 'no_sdr104', 'no_window', 'limited_to_sdr25')


def bank_level(pow_val):
    return {2: 3300, 3: 1800}.get((pow_val >> 10) & 3)


SPEED_MODES = ('SDR12', 'SDR25', 'SDR50', 'SDR104', 'DDR50')


def capabilities(ocr, cid_hex, switch):
    """What the 3.3 V observation shows. S18A is OCR bit 24 of the ACMD41 response that carried S18R (no CMD11 was sent). The bus speed
    support bits are the CMD6 response's group 1 field (bytes 12 and 13, big-endian). At 3.3 V signalling the card reports only functions 0 and 1 (specification Table 4-11 text), so SDR50, SDR104 and DDR50 appear only in a response read at 1.8 V. The CID is manufacturer id (8 bits), OEM id
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
               switch_status=raw[OFFSET['switch_status']:OFFSET['switch_status'] + 64].hex(), uhs=list(q('uhs', 16)))
    out.update(capabilities(out['ocr'], out['cid'], bytes.fromhex(out['switch_status'])))
    u = out['uhs']
    out['uhs_report'] = dict(mode=u[0], card_signals_1v8=bool(u[1]), outcome=UHS_OUTCOMES[u[2]] if u[2] < len(UHS_OUTCOMES) else u[2],
                             reentered=bool(u[3]), bank_at_ready=dict(pow_val=hex(u[4]), pf_bank_mv=bank_level(u[4]), selector=hex(u[5])),
                             bank_after_close=dict(pow_val=hex(u[6]), pf_bank_mv=bank_level(u[6]), selector=hex(u[7])),
                             withstand_mode=dict(at_ready=hex(u[8]), after_close=hex(u[9])),
                             platform_fault=dict(line=u[10] & 0xffff, why=(u[10] >> 16) & 0xff, stage=(u[10] >> 24) & 0xff, fault=(u[10] >> 32) & 0xff),
                             stop_word=hex(u[11]), stop_word_fields=stop_fields(u[11]),
                             tuning=dict(map=f'{u[12]:016x}', passing_delays=[d for d in range(64) if u[12] >> d & 1], chosen_delay=u[13] & 255,
                                         window_start=u[13] >> 8 & 255, window_width=u[13] >> 16 & 255, outcome_code=u[13] >> 24 & 255),
                             card_clock_hz=u[14], module_clock_hz=u[15])
    n = out['done']
    el = struct.unpack_from(f'<{n}I', raw, OFFSET['elapsed']); crc = struct.unpack_from(f'<{n}I', raw, OFFSET['crc'])
    win, nbytes = struct.unpack_from('<2Q', raw, 64)
    out['sustained'] = dict(bytes=nbytes, seconds=round(win / 24e6, 4), MBps=round(nbytes / (win / 24e6) / 1e6, 2) if win else 0)
    out['reads'] = dict(done=n, crc=[hex(c) for c in crc[:32]], per_request_us=dict(first=el[0], median=sorted(el)[n // 2], min=min(el), max=max(el)) if n else None,
                        per_request_MBps_median=round(131072 / sorted(el)[n // 2], 2) if n else 0)
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
    i.add_argument('--uhs-mode', type=int, default=0, choices=(0, 1, 2, 3), help='0 high speed with S18A observed, 1 UHS-I lifetime 1, 2 UHS-I lifetime 2, 3 re-entry then SDR104 with tuning')
    i.add_argument('--reads', type=int, default=1)
    i.add_argument('--blocks', type=int, default=8)
    d = sub.add_parser('decode')
    d.add_argument('--report', type=Path, required=True)
    d.add_argument('--ranges', nargs='+', required=True)
    d.add_argument('--backup', type=Path, required=True, help='the backup file to write')
    k = sub.add_parser('stop', help='decode the fault record of a lifetime that stopped (site byte and esr word)')
    k.add_argument('--site', type=lambda x: int(x, 0), required=True, help='the fault word low byte: the probe reason')
    k.add_argument('--esr', type=lambda x: int(x, 0), required=True)
    a = p.parse_args()
    if a.cmd == 'stop':
        print(json.dumps(dict(reason=a.site, **(stop_fields_identification(a.esr) if a.site == 3 else stop_fields(a.esr))), indent=1)); return
    sectors = sorted({s for r in a.ranges for s in range(int(r.split(':')[0]), int(r.split(':')[0]) + int(r.split(':')[1]))})
    if a.cmd == 'input':
        with a.output.open('xb') as f:
            f.write(pack_input(sectors, a.uhs_mode, a.reads, a.blocks))
        print(a.output, len(sectors), 'sectors')
    else:
        out, records = decode(a.report.read_bytes(), sectors)
        with a.backup.open('xb') as f:
            f.write(records)
        out.update(backup=str(a.backup), backup_sha256=hashlib.sha256(records).hexdigest(), backup_sectors=len(sectors))
        print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
