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
    assert decoded['transport_trace'] is None
    # The transport trace (flags bit 13) follows the fault site: a key add written and completed, then nothing read.
    traced = bytearray(faulted)
    struct.pack_into('<I', traced, 12, len(RECORD) + len(entry) + 16 + 48 + 16 + 16 + 28)
    struct.pack_into('<I', traced, 36, head_flags | 1024 | 512 | 2048 | 4096 | 8192)
    trace = struct.pack('<H2BHH6BH8BHH', 36, 1 | 2 | 8, 4, 1000, 0, 0, 0, 0, 0, 0, 1, 0, 19, 0, 0x01, 0, 255, 0x09,
                        0x01, 2 | 8, 0, 0)
    decoded = radio_result.decode(bytes(traced) + entry + struct.pack('<4I', 3, 12, 2, 4) + tail + rsn + site + trace)
    assert decoded['fault_site']['expected_cfm'] == 37 and decoded['rsn']['group'] == 'TKIP', decoded
    assert decoded['transport_trace'] == {
        'tx_id': 36, 'tx_submitted': True, 'tx_completed': True, 'data_write_after': False, 'irq_wait': True,
        'record_held': False, 'dispatcher_faulted': False, 'credit': 4, 'tx_age_ms': 1000, 'rx_records': 0,
        'rx_by_kind': {'responses': 0, 'data': 0, 'data_confirmations': 0, 'other': 0}, 'rx_aggregates': 0,
        'status_reads': 1, 'rx_last_id': 0, 'phase': 'rx-status', 'last_status': 0, 'queued_events': 0,
        'queued_records': 1, 'queued_record_kinds': ['unclaimed'], 'held_type': None,
        'data_cfm': {'sent': True, 'retry': False, 'software_retry': False, 'acknowledged': True},
        'interrupts': 0, 'wakes': 1, 'host_saw_sdio_latch': False, 'controller_read': True,
        'controller_sdio_latch': False, 'controller_sdio_unmasked': True, 'gic_masked': False,
        'slowest_confirmed_id': None, 'slowest_confirmation_ms': None}, decoded['transport_trace']
    # Authorized: the trace alone, after the RSN summary, naming the slowest confirmation (a key add, 5.9 s).
    alone = bytearray(full)
    struct.pack_into('<I', alone, 12, len(RECORD) + len(entry) + 16 + 48 + 16 + 28)
    struct.pack_into('<I', alone, 36, head_flags | 1024 | 512 | 2048 | 8192)
    slow = trace[:-4] + struct.pack('<HH', 36, 5900)
    decoded = radio_result.decode(bytes(alone) + entry + struct.pack('<4I', 3, 12, 2, 4) + tail + rsn + slow)
    assert decoded['fault_site'] is None and decoded['rsn']['group'] == 'TKIP', decoded
    assert decoded['transport_trace']['slowest_confirmed_id'] == 36 and \
        decoded['transport_trace']['slowest_confirmation_ms'] == 5900, decoded['transport_trace']
    assert not any(word in key for key in decoded['transport_trace'] for word in ('bssid', 'frequency', 'rssi', 'ssid'))
    assert decoded['address'] is None
    # The address stage (flags bit 14): the DHCP and DNS section, last, after the trace. Counts, codes and times only.
    addressed = bytearray(alone)
    struct.pack_into('<I', addressed, 12, len(RECORD) + len(entry) + 16 + 48 + 16 + 28 + 64)
    struct.pack_into('<I', addressed, 36, head_flags | 1024 | 512 | 2048 | 8192 | 16384)
    section = struct.pack('<I8H7IH3B3xIHBB', 1 | 2 | 4 | 8 | 16 | 32 | 128 | 256 | 512 | 1024, 1, 1, 1, 1, 0, 1, 1, 0,
                          250, 262, 263, 271, 300, 318, 86400, 1, 0, 2, 1, 41, 9, 1, 1)
    counts = struct.pack('<4I', 3, 12, 2, 4)
    decoded = radio_result.decode(bytes(addressed) + entry + counts + tail + rsn + slow + section)
    a = decoded['address']
    assert decoded['transport_trace']['slowest_confirmed_id'] == 36 and decoded['rsn']['group'] == 'TKIP', decoded
    assert a['dhcp'] == {'discover': {'sent': 1, 'first_ms': 250}, 'offer': {'received': 1, 'first_ms': 262},
                         'request': {'sent': 1, 'first_ms': 263}, 'ack': {'received': 1, 'naks': 0, 'first_ms': 271},
                         'lease_seconds': 86400, 'address_private': True, 'router': True, 'router_private': True,
                         'offer_ack_address_match': True, 'offer_ack_server_match': True, 'hostname_sent': False,
                         'dns_servers': 2, 'lease_published': True}, a['dhcp']
    assert a['dns'] == {'queries': 1, 'responses': 1, 'query_ms': 300, 'response_ms': 318, 'time_ms': 18, 'id_matched': True,
                        'rcode': 'no-error', 'answers': 1}, a['dns']
    assert a['resolve'] == {'reply': 'ok', 'address_returned': True} and a['dhcp_stopped'] and not a['deadline_passed'], a
    # Placement: the IP task's wakes and the cores it and the radio owner ran on, one list entry per mask bit.
    assert a['ip_task'] == {'wakes': 41, 'empty_wakes': 9, 'cores': [0]} and a['owner_cores'] == [0], a
    placed = radio_result.decode(bytes(addressed) + entry + counts + tail + rsn + slow + section[:-2] + bytes([0x24, 0x81]))
    assert placed['address']['ip_task']['cores'] == [2, 5] and placed['address']['owner_cores'] == [0, 7], placed['address']
    # A refused request: one NAK, no ACK fields, no DNS exchange, the resolve's reply address-lost.
    nak = struct.pack('<I8H7IH3B3xIHBB', 0, 2, 1, 1, 0, 1, 0, 0, 0, 250, 262, 263, 270, 0, 0, 0, 0, 0, 0, 7, 30, 12, 1, 1)
    a = radio_result.decode(bytes(addressed) + entry + counts + tail + rsn + slow + nak)['address']
    assert a['dhcp']['ack'] == {'received': 0, 'naks': 1, 'first_ms': 270} and a['dhcp']['lease_seconds'] is None and \
        a['dns']['rcode'] is None and a['dns']['time_ms'] is None and a['resolve']['reply'] == 'address-lost', a
    # The section holds counts, codes, flags and times: every value is a number, a flag, a code name or None, and no
    # field names an address, MAC or host name.
    def values(value):
        return [w for v in value.values() for w in (values(v) if isinstance(v, dict) else v if isinstance(v, list) else [v])]
    assert all(v is None or isinstance(v, (bool, int)) or v in radio_result.NET_RESULTS + radio_result.DNS_RCODES
               for v in values(a)), a
    def keys(value):
        return [k for k, v in value.items() for k in [k] + (keys(v) if isinstance(v, dict) else [])]
    assert not any(word in key for key in keys(a) for word in ('mac', 'gateway_address', 'server_address', 'host_name', 'ip_address', 'address_value')), keys(a)
    try:
        radio_result.decode(bytes(addressed) + entry + counts + tail + rsn + slow + section[:-4])
    except ValueError:
        pass
    else:
        raise SystemExit('a truncated address section was accepted')
    assert radio_result.decode(bytes(full) + entry + struct.pack('<4I', 3, 12, 2, 4) + tail + rsn)['fault_site'] is None
    struct.pack_into('<I', full, 8, max(radio_result.VERSIONS) + 1)
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
    print('PASS radio result decode: guard refusal with PG at 1.8 V and BLDO5 off, firmware progress, the association tail and the address section')


if __name__ == '__main__':
    main()
