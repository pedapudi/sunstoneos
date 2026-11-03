#!/usr/bin/env python3
"""Decode a radio result record that a board run returned (entry 1793742152141316018-59ac4fd1, port 3-1): the
first-run guard refused with PG reading 1.8 V while BLDO5 was off, before any write. The record is the 232
bytes the descriptor named, unchanged."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import radio_result

RECORD = bytes.fromhex(
    '373333524144523102000000e8000000e80000002c000000010000000100000002000000070000005055150180f90f02'
    'aaaa0a000e00000000ff1703a05a320007ffe4010000000013b80d0240771b0014b80d0340771b000400000007000000'
    '000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
    '000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
    '00000000000000000000000000000000000000000700310400000000000000000000000000000000')


def main():
    assert radio_result.record_length(RECORD[:radio_result.HEAD_BYTES]) == len(RECORD)
    r = radio_result.decode(RECORD)
    assert r['failed'] and r['failed_at'] == 'guard' and r['stage'] == 'rails-observed'
    assert r['guard'] == {'asked': True, 'observed': True, 'refused': True}
    # BLDO5 was off, so the power-on was needed; no write ran (stage never reached 'powered').
    assert r['bldo5_needs_power_on'] and not r['bldo5_already_on'] and not r['failure_shutdown']['ran']
    assert r['platform_failure']['guard_refusal'] == {'code': 0x431, 'pg_bits': 3, 'unmet': ['pg']}
    assert r['platform_failure']['stage'] == 7
    # The board read 1.8 V for PG and PM with BLDO5 off (enable byte 0xb8, selector 13).
    assert r['pad_power']['pg'] == '1.8V' and r['pad_power']['pm'] == '1.8V' and r['pad_power']['gpio_val'] == 0x020ff980
    bldo5 = [x for x in r['rails'] if x['rail'] == 'bldo5'][0]
    assert not bldo5['enabled'] and bldo5['selector'] == 13 and bldo5['enable_byte'] == 0xb8
    assert r['identity'] == {'chip_word': 0, 'subrevision_word': 0} and not r['scan']['aps']
    print('PASS radio result decode: guard refusal with PG at 1.8 V and BLDO5 off')


if __name__ == '__main__':
    main()
