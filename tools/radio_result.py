#!/usr/bin/env python3
"""Decode the radio service's result record (include/a733_radio_result.h)."""
import struct
import sys

MAGIC = b'733RADR1'
SIZE = 928
STAGES = ['none', 'rails-observed', 'guard-passed', 'powered', 'enumerated', 'firmware', 'identified',
          'scanned', 'associated']
RAILS = {0: 'dcdc1', 7: 'dcdc8', 19: 'bldo5', 20: 'cldo1'}


def decode(data):
    """Return the record as plain data, or raise ValueError when it is not a version 1 record."""
    if len(data) != SIZE or data[:8] != MAGIC:
        raise ValueError('not a radio result record')
    version, size = struct.unpack_from('<II', data, 8)
    if version != 1 or size != SIZE:
        raise ValueError('unsupported radio result version')
    stage, failed, failure_stage, flags = struct.unpack_from('<4I', data, 16)
    if stage >= len(STAGES) or failure_stage > len(STAGES):
        raise ValueError('stage out of range')
    result = {
        'stage': STAGES[stage], 'stage_code': stage, 'failed': bool(failed),
        'failed_at': STAGES[failure_stage] if failed and failure_stage < len(STAGES) else None,
        'guard': {'enabled': bool(flags & 1), 'observed': bool(flags & 2), 'bldo5_powered_on': bool(flags & 4)},
    }
    mod_g, val_g, mod_m, val_m = struct.unpack_from('<4I', data, 32)
    power = {0: 'invalid', 1: 'invalid', 2: '3.3V', 3: '1.8V'}
    result['pad_power'] = {'gpio_mod_sel': mod_g, 'gpio_val': val_g, 'sgpio_mod_sel': mod_m, 'sgpio_val': val_m,
                           'pg': power[(val_g >> 12) & 3], 'pm': power[(val_m >> 2) & 3]}
    rail_count, platform_stage, ocr, selected = struct.unpack_from('<4I', data, 80)
    rails = []
    for i in range(min(rail_count, 4)):
        rail, enable, selector, rflags, uv = struct.unpack_from('<4BI', data, 48 + 8 * i)
        rails.append({'rail': RAILS.get(rail, rail), 'enable_byte': enable, 'selector': selector,
                      'enabled': bool(rflags & 1), 'decoded': bool(rflags & 2), 'programmed_uv': uv})
    result['rails'] = rails
    result['platform_stage'] = platform_stage
    rca, functions, cccr, sdio, sd, caps, wide, hz = struct.unpack_from('<8I', data, 96)
    result['card'] = {'ocr': ocr, 'selected_ocr': selected, 'rca': rca, 'function_count': functions,
                      'cccr_revision': cccr, 'sdio_revision': sdio, 'sd_revision': sd, 'capabilities': caps,
                      'wide': bool(wide), 'card_hz': hz, 'functions': []}
    for i in range(8):
        man, dev, cls, present = struct.unpack_from('<2H2B', data, 128 + 8 * i)
        if present:
            result['card']['functions'].append({'function': i, 'manufacturer': man, 'device': dev, 'class': cls})
    chip, sub, fw_flags = struct.unpack_from('<3I', data, 192)
    result['identity'] = {'chip_word': chip, 'subrevision_word': sub}
    ap_count, reported = struct.unpack_from('<2I', data, 208)
    aps = []
    for i in range(min(ap_count, 16)):
        o = 224 + 44 * i
        bssid = data[o:o + 6].hex(':')
        n, rssi = data[o + 6], struct.unpack_from('<b', data, o + 7)[0]
        freq, channel, security = struct.unpack_from('<H2B', data, o + 8)
        ssid = data[o + 12:o + 12 + min(n, 32)].decode('utf-8', 'replace')
        aps.append({'bssid': bssid, 'ssid': ssid, 'frequency_mhz': freq, 'channel': channel, 'rssi_dbm': rssi})
    result['scan'] = {'listed': ap_count, 'reported': reported, 'aps': sorted(aps, key=lambda a: -a['rssi_dbm'])}
    return result


if __name__ == '__main__':
    import json
    print(json.dumps(decode(bytes.fromhex(sys.stdin.read().strip()) if len(sys.argv) < 2 else open(sys.argv[1], 'rb').read()),
                     indent=2))
