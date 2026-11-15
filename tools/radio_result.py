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


ASSOC_BYTES = 48
SCAN_COUNTS_BYTES = 16   # struct a733_radio_scan_counts, present when head flags bit 10 is set
RSN_BYTES = 16           # struct a733_radio_rsn, present when head flags bit 11 is set
FAULT_BYTES = 16         # struct aic8800_fault_site, present when head flags bit 12 is set; the last section
# AIC8800_SITE file codes (include/aic8800_security.h).
SITE_FILES = {0: 'other', 1: 'aic8800_supplicant.c', 2: 'aic8800_supplicant_os.c', 3: 'aic8800_sae.c',
              4: 'aic8800_sdio_transport.c', 5: 'aic8800_security.c'}
# enum aic8800_security_stop_reason (include/aic8800_security.h).
STOP_REASONS = ['external', 'deadline', 'completion', 'clock', 'link', 'generation', 'confirmation']
TRANSPORT_ERRORS = ['none', 'host', 'timeout', 'setup', 'rx-bounds', 'unsupported-irq', 'message', 'firmware',
                    'runtime', 'wifi', 'data']
# Record versions this decoder reads (A733_RADIO_RESULT_VERSION). Admission refuses an image that writes another.
VERSIONS = (2, 3)
RSN_CHECKS = ['passed', 'no-element', 'too-long', 'malformed', 'group-not-ccmp', 'pairwise-no-ccmp', 'mfp-required',
              'no-psk', 'other']
# Suite types under OUI 00-0F-AC (IEEE 802.11-2020 Table 9-149 and Table 9-151).
CIPHERS = {1: 'WEP-40', 2: 'TKIP', 4: 'CCMP-128', 5: 'WEP-104', 6: 'BIP-CMAC-128', 7: 'group-not-allowed',
           8: 'GCMP-128', 9: 'GCMP-256', 10: 'CCMP-256', 11: 'BIP-GMAC-128', 12: 'BIP-GMAC-256', 13: 'BIP-CMAC-256'}
AKMS = {1: '802.1X', 2: 'PSK', 3: 'FT-802.1X', 4: 'FT-PSK', 5: '802.1X-SHA256', 6: 'PSK-SHA256', 8: 'SAE',
        9: 'FT-SAE', 11: 'suite-B', 12: 'suite-B-192', 18: 'OWE', 24: 'SAE-EXT-KEY', 25: 'FT-SAE-EXT-KEY'}


def suites(mask, names):
    return [names.get(n, n) for n in range(32) if mask >> n & 1]
ASSOC_FIELDS = ('stage', 'link', 'status', 'supplicant', 'rsn', 'eapol_rx', 'eapol_tx', 'flags', 'candidates',
                'frequency_rssi', 'ms_connect', 'ms_authorized')


def flags_of(data):
    return struct.unpack_from('<I', data, 36)[0]


def record_length(head):
    """Total bytes of the record, from its first HEAD_BYTES bytes (read the rest, then call decode)."""
    if len(head) < HEAD_BYTES or head[:8] != MAGIC:
        raise ValueError('not a radio result record')
    version, total, head_bytes, ap_bytes = struct.unpack_from('<4I', head, 8)
    if version not in VERSIONS or head_bytes != HEAD_BYTES or ap_bytes != AP_BYTES or total < HEAD_BYTES:
        raise ValueError('unsupported radio result version')
    return total


def decode(data):
    """Return the record as plain data. The record's own length decides how much is read."""
    total = record_length(data)
    if len(data) < total:
        raise ValueError('radio result record is shorter than its header says')
    data = data[:total]
    ap_count = struct.unpack_from('<I', data, 216)[0]
    tail = (ASSOC_BYTES if flags_of(data) & 512 else 0) + (SCAN_COUNTS_BYTES if flags_of(data) & 1024 else 0) + \
        (RSN_BYTES if flags_of(data) & 2048 else 0) + (FAULT_BYTES if flags_of(data) & 4096 else 0)
    if total != HEAD_BYTES + ap_count * AP_BYTES + tail:
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
        'bldo5_already_on': bool(flags & 16),
        # Two shutdown paths: the orderly one reaches down with every step read back (bit 3); a failure runs the
        # failure shutdown instead (bits 5 and 6), so a failed run's orderly shutdown is always false.
        'orderly_shutdown': {'matched': bool(flags & 8)},
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
    fw_progress = struct.unpack_from('<I', data, 228)[0]
    result['identity'] = {'chip_word': chip, 'subrevision_word': sub}
    # Firmware engine progress (zero before it claims the module). The time is the clock at the last confirmation,
    # in milliseconds modulo 65536.
    states = ['new', 'verifying', 'validating', 'preparing', 'request', 'waiting', 'delay', 'start-confirmed', 'fault']
    phases = ['adid', 'patch', 'table', 'wifi-image', 'read-pointers', 'wifi-patch', 'start']
    result['firmware'] = {'state': states[fw_flags & 15] if (fw_flags & 15) < len(states) else fw_flags & 15,
                          'error': (fw_flags >> 4) & 15,
                          'phase': phases[(fw_flags >> 8) & 255] if ((fw_flags >> 8) & 255) < len(phases) else (fw_flags >> 8) & 255,
                          'last_confirmation_ms_mod_65536': fw_flags >> 16,
                          'image_bytes_sent': fw_progress & 0x7ffff, 'table_pairs_done': fw_progress >> 19} if fw_flags else None
    # fail_site: source line << 16 | 0x100 when the session decided the failure (the platform decided none) | stage.
    result['platform_failure'] = {'line': fail_site >> 16, 'stage': fail_site & 0xff,
                                  'source': 'session' if fail_site & 0x100 else 'platform'} if fail_site else None
    if fail_site & 0x100 and (fail_site & 0xff) != 1 and (fail_site >> 9) & 0x7f:
        # A live session fault (drivers/net/a733_radio_session.c): the transport error (0 when the dispatcher or
        # network decided) and the Wi-Fi error.
        transport = ['none', 'host', 'timeout', 'setup', 'rx-bounds', 'unsupported-irq', 'message', 'firmware',
                     'runtime', 'wifi', 'data']
        wifi = ['none', 'time', 'message', 'mac', 'version', 'status', 'result', 'transport']
        code = (fail_site >> 9) & 15
        result['platform_failure']['transport_error'] = transport[code] if code < len(transport) else code
        result['platform_failure']['wifi_error'] = wifi[(fail_site >> 13) & 7]
    elif fail_site & 0x100 and (fail_site >> 9) & 0x7f:
        # A refused transport adoption: bit 0 pointers/state, 1 config, 2 function count, 3 function 2 identity,
        # 4 function 1 manufacturer, 5 function 1 block size, 6 function 1 enable timeout.
        result['platform_failure']['refusal_groups'] = [i for i in range(7) if (fail_site >> 9) >> i & 1]
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
        if n or any(data[o + 12:o + AP_BYTES]):
            # The image records no SSID (a733_radio_report_ap); a record that carries one is refused before any
            # field of it reaches a result file.
            raise ValueError('An access point entry carries SSID bytes; the record must hold none')
        aps.append({'bssid': data[o:o + 6].hex(':'),
                    'frequency_mhz': freq, 'channel': channel, 'rssi_dbm': rssi})
    result['scan'] = {'listed': ap_count, 'reported': reported, 'dropped': dropped,
                      'aps': sorted(aps, key=lambda a: -a['rssi_dbm'])}
    if flags & 1024:
        # Records from images before flag bit 10 have only 'reported': the larger of the sightings and the firmware's
        # count, which cannot tell repeated BSSIDs from results that never became events.
        sightings, firmware, merges, set_aside = struct.unpack_from('<4I', data, HEAD_BYTES + ap_count * AP_BYTES)
        if sightings != 0xffffffff and ap_count != sightings - merges - dropped:
            raise ValueError('radio result scan counts do not match its access point count')
        result['scan'].update(sightings=sightings, firmware_reported=firmware, merges=merges, set_aside=set_aside)
    result['association'] = None
    if flags & 512:
        # Sections in order: scan counts (bit 10), association tail (bit 9), RSN summary (bit 11).
        at = HEAD_BYTES + ap_count * AP_BYTES + (SCAN_COUNTS_BYTES if flags & 1024 else 0)
        values = dict(zip(ASSOC_FIELDS, struct.unpack_from('<12I', data, at)))
        stages = ['none', 'candidate-found', 'entropy-ready', 'connecting', 'security-pending', 'securing', 'authorized']
        links = ['down', 'connecting', 'associated-open', 'security-pending', 'disconnecting', 'unresolved', 'secured']
        rssi = values['frequency_rssi'] >> 16
        result['association'] = {
            'stage': stages[values['stage']] if values['stage'] < len(stages) else values['stage'],
            'link': links[values['link']] if values['link'] < len(links) else values['link'],
            'status_code': values['status'], 'supplicant_state': values['supplicant'], 'rsn_state': values['rsn'],
            'eapol_received': values['eapol_rx'], 'eapol_sent': values['eapol_tx'],
            'entropy_unqualified': bool(values['flags'] & 1), 'ssid_matched': bool(values['flags'] & 2),
            'rsn_psk_ccmp': bool(values['flags'] & 4), 'target_built': bool(values['flags'] & 8),
            'entropy_ready': bool(values['flags'] & 16), 'credential_present': bool(values['flags'] & 32),
            'entropy_failed': bool(values['flags'] & 64),
            'candidates': values['candidates'], 'frequency_mhz': values['frequency_rssi'] & 0xffff,
            'rssi_dbm': rssi - 65536 if rssi >= 32768 else rssi,
            'ms_connect_mod_65536': values['ms_connect'], 'ms_authorized_mod_65536': values['ms_authorized']}
    result['rsn'] = None
    fault_bytes = FAULT_BYTES if flags & 4096 else 0
    if flags & 2048:
        check, group, rsn_flags, parse, version, capabilities, pairwise, akm = struct.unpack_from(
            '<3BbHH2I', data, total - fault_bytes - RSN_BYTES)
        result['rsn'] = {
            'check': RSN_CHECKS[check] if check < len(RSN_CHECKS) else check,
            'element_found': bool(rsn_flags & 1), 'other_oui': bool(rsn_flags & 2), 'malformed': bool(rsn_flags & 4),
            'parse_result': parse, 'version': version,
            'group': 'absent' if group == 0xfe else 'other-oui' if group == 0xff else CIPHERS.get(group, group),
            'pairwise': suites(pairwise, CIPHERS) if rsn_flags & 8 else 'absent (CCMP-128)',
            'akm': suites(akm, AKMS) if rsn_flags & 16 else 'absent (802.1X)',
            'capabilities': capabilities, 'mfp_capable': bool(capabilities & 0x80), 'mfp_required': bool(capabilities & 0x40)}
    result['fault_site'] = decode_fault_site(data[total - FAULT_BYTES:total]) if fault_bytes else None
    return result


def site(value):
    return {'file': SITE_FILES.get(value >> 12, value >> 12), 'line': value & 0xfff} if value else None


def decode_fault_site(raw):
    """The fault site section (include/aic8800_fault_site.h): codes and states only."""
    (transport_site, transport_error, data_word, security, slot, expected, cfm_bytes, cfm_status, supplicant, actions,
     stop_site, ms) = struct.unpack('<H4BH4BHH', raw)
    data_states = ['idle', 'request', 'waiting', 'done', 'fault']
    data_errors = ['none', 'time', 'link', 'transport']
    security_states = ['idle', 'request', 'waiting', 'done', 'fault']
    operations = ['add', 'delete', 'port']
    supplicant_states = ['new', 'handshake', 'authorized', 'retiring', 'retired', 'fault']
    def name(names, value): return names[value] if value < len(names) else value
    return {
        'transport_site': site(transport_site), 'transport_error': name(TRANSPORT_ERRORS, transport_error),
        'data_state': name(data_states, data_word & 15), 'data_error': name(data_errors, data_word >> 4),
        'security_state': name(security_states, security & 15), 'security_stopped': bool(security & 16),
        'security_operation': name(operations, security >> 5), 'key_slot': slot & 7,
        'security_stop_reason': name(STOP_REASONS, slot >> 4) if security & 16 else None, 'expected_cfm': expected,
        'cfm_bytes': cfm_bytes, 'cfm_status': cfm_status,
        'supplicant_state': name(supplicant_states, supplicant & 15), 'queued_actions': supplicant >> 4,
        'head_action': ('key' if actions & 1 else 'eapol') if supplicant >> 4 else None,
        'head_in_flight': bool(actions & 2), 'port_pending': bool(actions & 4),
        'handshake_completed': bool(actions & 8), 'eapol_deferred': bool(actions & 16),
        'stop_site': site(stop_site), 'ms_mod_65536': ms}


if __name__ == '__main__':
    import json
    print(json.dumps(decode(bytes.fromhex(sys.stdin.read().strip()) if len(sys.argv) < 2 else open(sys.argv[1], 'rb').read()),
                     indent=2))
