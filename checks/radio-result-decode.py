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

IDENTITY_RECORD = bytes.fromhex(
    '373333524144523102000000e8000000e80000002c0000000600000000000000000000000f0000005055150180f90f02'
    'aaaa0a000e00000000ff1703a05a320007ffe4010000000013bc0d0340771b0014bc0d0340771b000400000022000000'
    '00ffffa0000030000b390000020000000300000004000000030000001f0000000000000000000000a1c8820000010000'
    'a1c8820007010000a1c88201070100000000000000000000000000000000000000000000000000000000000000000000'
    '0000000000000000208807fb21080000000000000000000000000000000000000000000000000000')


def main():
    assert radio_result.record_length(RECORD[:radio_result.HEAD_BYTES]) == len(RECORD)
    r = radio_result.decode(RECORD)
    assert r['failed'] and r['failed_at'] == 'guard' and r['stage'] == 'rails-observed'
    assert not r['orderly_shutdown']['matched']  # a failed run takes the failure shutdown path, never the orderly one
    assert r['guard'] == {'asked': True, 'observed': True, 'refused': True}
    # BLDO5 was off, so the power-on was needed; no write ran (stage never reached 'powered').
    assert r['bldo5_needs_power_on'] and not r['bldo5_already_on'] and not r['failure_shutdown']['ran']
    assert r['platform_failure']['guard_refusal'] == {'code': 0x431, 'pg_bits': 3, 'unmet': ['pg']}
    assert r['platform_failure']['stage'] == 7 and r['platform_failure']['source'] == 'platform'
    # The board read 1.8 V for PG and PM with BLDO5 off (enable byte 0xb8, selector 13).
    assert r['pad_power']['pg'] == '1.8V' and r['pad_power']['pm'] == '1.8V' and r['pad_power']['gpio_val'] == 0x020ff980
    bldo5 = [x for x in r['rails'] if x['rail'] == 'bldo5'][0]
    assert not bldo5['enabled'] and bldo5['selector'] == 13 and bldo5['enable_byte'] == 0xb8
    assert r['identity'] == {'chip_word': 0, 'subrevision_word': 0} and not r['scan']['aps']
    assert r['firmware'] is None
    # Firmware progress words (offsets 208 and 228): state waiting (5), phase table (2), 1,234 ms, 2,000 pairs, 512 bytes.
    import struct
    raw = bytearray(RECORD)
    struct.pack_into('<I', raw, 208, 5 | 2 << 8 | 1234 << 16)
    struct.pack_into('<I', raw, 228, 512 | 2000 << 19)
    f = radio_result.decode(bytes(raw))['firmware']
    assert f == {'state': 'waiting', 'error': 0, 'phase': 'table', 'last_confirmation_ms_mod_65536': 1234,
                 'image_bytes_sent': 512, 'table_pairs_done': 2000}, f
    # Records already captured decode as they were written: the identity-stage run's record (entry
    # 1793750775070389039-2a278fd8) is stage 'identified', with the module's words, not 'firmware'.
    i = radio_result.decode(IDENTITY_RECORD)
    assert i['stage'] == 'identified' and i['stage_code'] == 6 and not i['failed'] and i['firmware'] is None
    assert i['identity'] == {'chip_word': 0xfb078820, 'subrevision_word': 0x821}
    # The association tail: flags bit 9 of the head, 48 bytes after the access point list, no text field.
    head_flags = struct.unpack_from('<I', RECORD, 36)[0]
    tail = struct.pack('<12I', 6, 6, 0, 2, 9, 4, 4, 0x3f, 2, 2437 | (0x10000 * 0xffd7), 15000, 15900)
    a = bytearray(RECORD)
    struct.pack_into('<I', a, 36, head_flags | 512)
    struct.pack_into('<I', a, 12, len(RECORD) + 48)
    wire = bytes(a) + tail
    assert radio_result.record_length(wire[:radio_result.HEAD_BYTES]) == len(wire)
    assoc = radio_result.decode(wire)['association']
    assert assoc == {'stage': 'authorized', 'link': 'secured', 'status_code': 0, 'supplicant_state': 2, 'rsn_state': 9,
                     'eapol_received': 4, 'eapol_sent': 4, 'entropy_unqualified': True, 'ssid_matched': True,
                     'rsn_psk_ccmp': True, 'target_built': True, 'entropy_ready': True, 'credential_present': True,
                     'entropy_failed': False, 'candidates': 2, 'frequency_mhz': 2437, 'rssi_dbm': -41,
                     'ms_connect_mod_65536': 15000, 'ms_authorized_mod_65536': 15900}, assoc
    assert radio_result.decode(RECORD)['association'] is None
    # An access point entry holds BSSID, RSSI, frequency and channel. An entry with an SSID length or SSID bytes is
    # refused, and the error names no byte of it.
    entry = bytes.fromhex('021122334455') + bytes([0, 0xc3]) + struct.pack('<H2B', 2437, 6, 0) + bytes(32)
    listed = bytearray(RECORD)
    struct.pack_into('<I', listed, 12, len(RECORD) + len(entry))
    struct.pack_into('<I', listed, 216, 1)
    assert radio_result.decode(bytes(listed) + entry)['scan']['aps'] == [
        {'bssid': '02:11:22:33:44:55', 'frequency_mhz': 2437, 'channel': 6, 'rssi_dbm': -61}]
    # The scan counts (flags bit 10) follow the list: 3 sightings, firmware count 12, 2 merges, 4 set aside.
    counted = bytearray(listed)
    struct.pack_into('<I', counted, 12, len(RECORD) + len(entry) + 16)
    struct.pack_into('<I', counted, 36, head_flags | 1024)
    scan = radio_result.decode(bytes(counted) + entry + struct.pack('<4I', 3, 12, 2, 4))['scan']
    assert (scan['sightings'], scan['firmware_reported'], scan['merges'], scan['set_aside']) == (3, 12, 2, 4), scan
    try:
        radio_result.decode(bytes(counted) + entry + struct.pack('<4I', 3, 12, 1, 4))
    except ValueError:
        pass
    else:
        raise SystemExit('scan counts that disagree with the listed count were accepted')
    # Version 3 with every section: scan counts, the association tail, then the RSN summary (flags bit 11). The
    # summary is the stage-1 refusal shape: a WPA/WPA2 mixed element, TKIP group, CCMP and TKIP pairwise, PSK.
    full = bytearray(listed)
    struct.pack_into('<I', full, 8, 3)
    struct.pack_into('<I', full, 12, len(RECORD) + len(entry) + 16 + 48 + 16)
    struct.pack_into('<I', full, 36, head_flags | 1024 | 512 | 2048)
    rsn = struct.pack('<3BbHH2I', 4, 2, 1 | 8 | 16, -1, 1, 0, 1 << 4 | 1 << 2, 1 << 2)
    decoded = radio_result.decode(bytes(full) + entry + struct.pack('<4I', 3, 12, 2, 4) + tail + rsn)
    assert decoded['association']['stage'] == 'authorized' and decoded['scan']['merges'] == 2
    assert decoded['rsn'] == {'check': 'group-not-ccmp', 'element_found': True, 'other_oui': False, 'malformed': False,
                              'parse_result': -1, 'version': 1, 'group': 'TKIP', 'pairwise': ['TKIP', 'CCMP-128'],
                              'akm': ['PSK'], 'capabilities': 0, 'mfp_capable': False, 'mfp_required': False}, decoded['rsn']
    # The fault site section (flags bit 12) follows the RSN summary; it holds codes only. The values are the
    # aic8800-supplicant key-timeout case: a key add whose confirmation never came.
    faulted = bytearray(full)
    struct.pack_into('<I', faulted, 12, len(RECORD) + len(entry) + 16 + 48 + 16 + 16)
    struct.pack_into('<I', faulted, 36, head_flags | 1024 | 512 | 2048 | 4096)
    site = struct.pack('<H4BH4BHH', 0x4056, 10, 0x34, 0x12, 1 << 4, 37, 0, 0, 0x25, 0xb, 0x4031, 1234)
    decoded = radio_result.decode(bytes(faulted) + entry + struct.pack('<4I', 3, 12, 2, 4) + tail + rsn + site)
    assert decoded['rsn']['group'] == 'TKIP', decoded['rsn']
    assert decoded['fault_site'] == {
        'transport_site': {'file': 'aic8800_sdio_transport.c', 'line': 0x56}, 'transport_error': 'data',
        'data_state': 'fault', 'data_error': 'transport', 'security_state': 'waiting', 'security_stopped': True,
        'security_operation': 'add', 'key_slot': 0, 'security_stop_reason': 'deadline', 'expected_cfm': 37, 'cfm_bytes': 0, 'cfm_status': 0,
        'supplicant_state': 'fault', 'queued_actions': 2, 'head_action': 'key', 'head_in_flight': True,
        'port_pending': False, 'handshake_completed': True, 'eapol_deferred': False,
        'stop_site': {'file': 'aic8800_sdio_transport.c', 'line': 0x31}, 'ms_mod_65536': 1234}, decoded['fault_site']
    assert not any(word in key for key in decoded['fault_site'] for word in ('bssid', 'frequency', 'rssi', 'ssid'))
    assert radio_result.decode(bytes(full) + entry + struct.pack('<4I', 3, 12, 2, 4) + tail + rsn)['fault_site'] is None
    struct.pack_into('<I', full, 8, 4)
    try:
        radio_result.decode(bytes(full) + entry + struct.pack('<4I', 3, 12, 2, 4) + tail + rsn)
    except ValueError:
        pass
    else:
        raise SystemExit('a record version the decoder does not know was accepted')
    for named in (entry[:6] + bytes([4]) + entry[7:], entry[:12] + b'case' + entry[16:]):
        try:
            radio_result.decode(bytes(listed) + named)
        except ValueError as error:
            assert 'case' not in str(error)
        else:
            raise SystemExit('an access point entry with SSID bytes was accepted')
    try:
        radio_result.decode(wire[:-4])
    except ValueError:
        pass
    else:
        raise SystemExit('a truncated association tail was accepted')
    print('PASS radio result decode: guard refusal with PG at 1.8 V and BLDO5 off, firmware progress and the association tail')


if __name__ == '__main__':
    main()
