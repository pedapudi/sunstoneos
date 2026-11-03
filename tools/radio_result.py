#!/usr/bin/env python3
"""Decode the radio service's result record (include/a733_radio_result.h)."""
import struct
import sys

MAGIC = b'733RADR1'
HEAD_BYTES = 232
AP_BYTES = 44
STAGES = ['none', 'rails-observed', 'guard-passed', 'powered', 'enumerated', 'firmware', 'identified',
          'scanned', 'associated']
RAILS = {0: 'dcdc1', 7: 'dcdc8', 19: 'bldo5', 20: 'cldo1'}


def record_length(head):
    """Total bytes of the record, from its first HEAD_BYTES bytes (read the rest, then call decode)."""
    if len(head) < HEAD_BYTES or head[:8] != MAGIC:
        raise ValueError('not a radio result record')
    version, total, head_bytes, ap_bytes = struct.unpack_from('<4I', head, 8)
    if version != 2 or head_bytes != HEAD_BYTES or ap_bytes != AP_BYTES or total < HEAD_BYTES:
        raise ValueError('unsupported radio result version')
    return total


def decode(data):
    """Return the record as plain data. The record's own length decides how much is read."""
    total = record_length(data)
    if len(data) < total:
        raise ValueError('radio result record is shorter than its header says')
    data = data[:total]
    ap_count = struct.unpack_from('<I', data, 216)[0]
    if total != HEAD_BYTES + ap_count * AP_BYTES:
        raise ValueError('radio result length does not match its access point count')
    stage, failed, failure_stage, flags = struct.unpack_from('<4I', data, 24)
    # Records from images before flag bit 7 carry the same refusal in fail_site: stage 7 (guard) with a 0x4xx code.
    fail_site = struct.unpack_from('<I', data, 212)[0]
    if (fail_site & 0xffff) == 7 and (fail_site >> 24) == 4:
        flags |= 128
    if stage >= len(STAGES) or failure_stage > len(STAGES):
        raise ValueError('stage out of range')
    result = {
        'stage': STAGES[stage], 'stage_code': stage, 'failed': bool(failed),
        'failed_at': ('guard' if flags & 128 else STAGES[failure_stage] if failure_stage < len(STAGES) else None) if failed else None,
        'guard': {'asked': bool(flags & 1), 'observed': bool(flags & 2), 'refused': bool(flags & 128)},
        # The rails showed BLDO5 off, so the power-on writes were needed; they ran only if the stage reached 'powered'.
        'bldo5_needs_power_on': bool(flags & 4),
        'clean_failure': bool(flags & 256),
        'shutdown_readbacks_matched': bool(flags & 8),
        'bldo5_already_on': bool(flags & 16),
        'failure_shutdown': {'ran': bool(flags & 32), 'matched': bool(flags & 64)},
    }
    mod_g, val_g, mod_m, val_m = struct.unpack_from('<4I', data, 40)
    power = {0: 'invalid', 1: 'invalid', 2: '3.3V', 3: '1.8V'}
    result['pad_power'] = {'gpio_mod_sel': mod_g, 'gpio_val': val_g, 'sgpio_mod_sel': mod_m, 'sgpio_val': val_m,
                           'pg': power[(val_g >> 12) & 3], 'pm': power[(val_m >> 2) & 3]}
    rail_count, platform_stage, ocr, selected = struct.unpack_from('<4I', data, 88)
    rails = []
    for i in range(min(rail_count, 4)):
        rail, enable, selector, rflags, uv = struct.unpack_from('<4BI', data, 56 + 8 * i)
        rails.append({'rail': RAILS.get(rail, rail), 'enable_byte': enable, 'selector': selector,
                      'enabled': bool(rflags & 1), 'decoded': bool(rflags & 2), 'programmed_uv': uv})
    result['rails'] = rails
    result['platform_stage'] = platform_stage
    rca, functions, cccr, sdio, sd, caps, wide, hz = struct.unpack_from('<8I', data, 104)
    result['card'] = {'ocr': ocr, 'selected_ocr': selected, 'rca': rca, 'function_count': functions,
                      'cccr_revision': cccr, 'sdio_revision': sdio, 'sd_revision': sd, 'capabilities': caps,
                      'wide': bool(wide), 'card_hz': hz, 'functions': []}
    for i in range(8):
        man, dev, cls, present = struct.unpack_from('<2H2B', data, 136 + 8 * i)
        if present:
            result['card']['functions'].append({'function': i, 'manufacturer': man, 'device': dev, 'class': cls})
    chip, sub, fw_flags, fail_site = struct.unpack_from('<4I', data, 200)
    result['identity'] = {'chip_word': chip, 'subrevision_word': sub}
    # fail_site: source line << 16 | 0x100 when the session decided the failure (the platform decided none) | stage.
    result['platform_failure'] = {'line': fail_site >> 16, 'stage': fail_site & 0xff,
                                  'source': 'session' if fail_site & 0x100 else 'platform'} if fail_site else None
    if flags & 128:
        # A guard refusal: the 'line' is 0x400 | PG bits (GPIO_POW_VAL 13:12) << 4 | the unmet conditions.
        code = fail_site >> 16
        result['platform_failure']['guard_refusal'] = {
            'code': code, 'pg_bits': (code >> 4) & 3,
            'unmet': [name for bit, name in ((1, 'pg'), (2, 'dcdc1'), (4, 'bldo5'), (8, 'cldo1')) if code & bit]}
    reported, dropped = struct.unpack_from('<2I', data, 220)
    aps = []
    for i in range(ap_count):
        o = HEAD_BYTES + AP_BYTES * i
        n = data[o + 6]
        rssi = struct.unpack_from('<b', data, o + 7)[0]
        freq, channel, security = struct.unpack_from('<H2B', data, o + 8)
        if n > 32:
            raise ValueError('SSID length out of range')
        aps.append({'bssid': data[o:o + 6].hex(':'), 'ssid': data[o + 12:o + 12 + n].decode('utf-8', 'replace'),
                    'frequency_mhz': freq, 'channel': channel, 'rssi_dbm': rssi})
    result['scan'] = {'listed': ap_count, 'reported': reported, 'dropped': dropped,
                      'aps': sorted(aps, key=lambda a: -a['rssi_dbm'])}
    return result


if __name__ == '__main__':
    import json
    print(json.dumps(decode(bytes.fromhex(sys.stdin.read().strip()) if len(sys.argv) < 2 else open(sys.argv[1], 'rb').read()),
                     indent=2))
