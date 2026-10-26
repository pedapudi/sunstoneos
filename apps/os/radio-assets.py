"""Fixed OS radio/headset assets; no device access or physical authority defaults."""
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = 'a733-os-radio-assets-v1'
RANGE = (0x43000000, 0x43100000)
# Existing returning radio producer predicates, applied to the same evidence
# records. Complete historical latch authority is conditional on the PM mode.
FACTS = {
    'topology': ('fitted_board_routes_verified', 'enabled_3v3_vbat_1v8_io'),
    'pins': ('other_pm_data_writers_excluded', 'pm2_reset_lpo_stable', 'pg0_pg5_pm0_pm1_exclusive'),
    'startup': ('fabric_parent_gates_stable', 'sdc1_gate_off_reset_asserted',
                'store_ahb_mbus_msi_lite1_ready', 'sd_monitor_autogating_disabled',
                'device_mappings_security_verified', 'sdc1_irq_and_other_owners_excluded'),
    'bootstrap': ('awake_bootstrap', 'no_outstanding_debug_or_stale_confirmations', 'write_40500058_40_authorized'),
    'recovery': ('whole_board_power_removal', 'permanent_retention', 'one_attempt', 'no_reuse'),
}


def u64(data, offset):
    return struct.unpack_from('<Q', data, offset)[0]


def features(manifest, image, elf_type, require):
    elf = elf_type(image)
    defined = {s['name'] for s in elf.symbols if s['shndx'] and s['value'] and s['type'] == 2}
    result = {}
    for key, symbol in [('radio', 'os_radio_start'), ('headset', 'os_headset_start')]:
        value = manifest.get(key, False)
        require(type(value) is bool and value == (symbol in defined), 'Linked ' + key + ' capability differs.')
        result[key] = value
    result['msbc'] = 'bluetooth_msbc_init' in defined
    if result['radio'] or result['headset']:
        require(all(not (s[2] & 2 and s[5] and s[3] < RANGE[1] and RANGE[0] < s[3] + s[5])
                    for s in elf.sections), 'Linked image overlaps radio asset reservation.')
    return result


def validate(record, raw, flags, cache, board, bluetooth, require, *, frozen=False):
    """Return source path/address pairs; their bytes already belong to cache."""
    active = flags['radio'] or flags['headset']
    if not active:
        require('radio_assets' not in record, 'Radio assets supplied for an image without radio/headset.')
        return []
    require(record.get('radio_ddr') is not None, 'Radio profile/firmware DDR reservation required.')

    def read(ref):
        require(isinstance(ref, dict) and set(ref) == {'path', 'sha256'} and
                isinstance(ref['path'], str) and Path(ref['path']).is_absolute(), 'Invalid radio evidence reference.')
        path = ref['path']
        if path not in cache:
            require(not frozen, 'Radio evidence missing from frozen bundle.')
            cache[path] = Path(path).read_bytes()
        data = cache[path]
        require(hashlib.sha256(data).hexdigest() == ref['sha256'], 'Radio evidence or asset changed.')
        return data

    def digest_ref(ref, expected):
        data = read(ref)
        require(hashlib.sha256(data).digest() == expected, 'Profile authority evidence differs.')
        return data

    doc = json.loads(read(record.get('radio_assets')))
    context = dict(identity=record['identity'], port=record['port'], image_sha256=record['image_sha256'],
                   **dict(zip(('epoch', 'generation', 'lease'), struct.unpack_from('<3Q', raw, 24))))
    require(doc.get('schema') == SCHEMA and doc.get('reviewed') is True and
            all(doc.get(k) == v for k, v in context.items()) and
            isinstance(doc.get('lifetime'), str) and doc['lifetime'], 'Radio assets name another reviewed lifetime.')
    require(bool(doc.get('radio_profile')) == flags['radio'] and
            bool(doc.get('headset_profile')) == flags['headset'], 'Radio/headset profile selection differs.')
    profiles, plan = {}, []
    for key, address in [('radio', RANGE[0]), ('headset', RANGE[0] + (0x8000 if flags['radio'] else 0))]:
        if flags[key]:
            ref = doc[key + '_profile']; profiles[key] = read(ref); plan.append((ref['path'], address))
    radio, headset = profiles.get('radio'), profiles.get('headset')
    owners = tuple(struct.unpack_from('<4Q', raw, 456)) + (u64(raw, 616),)
    audio = len(raw) - 808 in (184, 280)
    if audio:
        owners += (u64(raw, 912),)
    spans = None
    if radio is not None:
        spans = board.profile_spans(radio, raw)
        require(all(u64(radio, off) for off in (16, 24, 32, 40, 48, 56, 64, 1224)), 'Radio profile authority absent.')
        platform = radio[184:336]; pm = raw[432:456]
        require(u64(platform, 32) == u64(raw, 184) and platform[56:80] == raw[488:512] and
                radio[384:408] == raw[488:512], 'Radio topology/fabric differs from session.')
        require(u64(platform, 0) and u64(radio, 336) and u64(platform, 0) != u64(radio, 336) and
                all(v not in owners for v in (u64(platform, 0), u64(radio, 336))), 'Radio owners collide.')
        digest_ref(doc.get('radio_ce'), radio[120:152]); digest_ref(doc.get('radio_entropy'), radio[152:184])
        platform_proof = digest_ref(doc.get('platform'), radio[88:120])
        require(doc.get('chip_id') == list(struct.unpack_from('<4I', radio, 72)) and
                doc.get('sid_authority') == u64(raw, 312), 'Radio secure chip identity differs from reviewed evidence.')
        digest_ref(doc.get('radio_sid'), headset[88:120] if headset is not None else raw[320:352])
    if headset is not None:
        require(len(headset) == 832, 'OS headset requires the repeated-capture profile.')
        bt_spans = bluetooth.profile_check(headset, raw, workloads=(0, 1, 4))
        mode = struct.unpack_from('<I', headset, 624)[0]
        require(mode in ((2, 3) if flags['msbc'] else (2,)), 'Headset mode exceeds linked codec capability.')
        require(all(u64(headset, off) for off in (16, 24, 32, 40, 48, 576, 584, 592, 600, 608, 616)) and
                u64(headset, 592) < u64(headset, 584) and u64(headset, 600) <= 5000000 and
                u64(headset, 608) <= u64(headset, 600), 'Headset authority or deadlines invalid.')
        bt_platform = headset[176:328]
        require(u64(headset, 16) == u64(raw, 184) == u64(bt_platform, 32) and
                bt_platform[56:80] == raw[488:512], 'Headset topology/fabric differs from session.')
        require(u64(headset, 176) and u64(headset, 352) and u64(headset, 176) != u64(headset, 352) and
                all(v not in owners for v in (u64(headset, 176), u64(headset, 352))), 'Headset owners collide.')
        # The normal OS adapter and standalone producer share geometry/model
        # checks. Native-only UART/wake predicates remain fixed here too.
        require(struct.unpack_from('<I', headset, 376)[0] == 3 and
                struct.unpack_from('<7I', headset, 380) == (1, 8, 2, 0, 1, 24000000, 1500000) and
                u64(headset, 328) == u64(headset, 48), 'Headset UART/wake profile differs.')
        for key, offset in [('bluetooth_wiring', 56), ('bluetooth_sid', 88),
                            ('bluetooth_transport', 120), ('bluetooth_scheduling', 672)]:
            digest_ref(doc.get(key), headset[offset:offset + 32])
        if radio is not None:
            require(bt_platform == platform and headset[152:176] == pm and
                    u64(headset, 576) == u64(radio, 1224) and headset[496:508] == radio[496:508] and
                    bt_spans == spans and u64(headset, 352) != u64(radio, 336), 'Shared module profiles disagree.')
        else:
            platform, pm, spans = bt_platform, headset[152:176], bt_spans
            platform_proof = read(doc.get('platform'))

    identity, owner, known, value = struct.unpack('<2Q2I', pm)
    require(identity and owner and known in (0, 63) and not value & ~known, 'Invalid normal PM ownership record.')
    proof = json.loads(platform_proof)
    require(proof.get('schema') == 'a733-radio-sdio-id-v1' and
            all(proof.get(k) == doc[k] for k in ('identity', 'port', 'lifetime')), 'Platform proof names another lifetime.')
    facts = {}
    for group, keys in FACTS.items():
        fact = json.loads(read(proof.get(group)))
        require(fact.get('schema') == 'a733-radio-sdio-id-v1-' + group and fact.get('reviewed') is True and
                all(fact.get(k) == doc[k] for k in ('identity', 'port', 'lifetime')) and
                all(fact.get(k) is True for k in keys), 'Missing reviewed radio ' + group + ' facts.')
        facts[group] = fact
    names = ('dcdc1', 'dcdc8', 'bldo5', 'cldo1')
    rails = struct.unpack_from('<4I', platform, 40)
    require(rails[0] == 0 and all(v in (1, 2, 3) for v in rails[1:]) and
            facts['topology'].get('rails') == dict(zip(('vbat', 'pg', 'pm', 'vddio'), (names[v] for v in rails))) and
            facts['topology'].get('routing_evidence'), 'Fitted rail routing absent or different.')
    pins = facts['pins']
    for key, offset in [('expected', 80), ('desired', 104)]:
        require(pins.get(key) == [list(platform[offset + 3*i:offset + 3*i + 3]) for i in range(8)],
                'Radio pin configurations differ from reviewed facts.')
    require(facts['startup'].get('fabric') == list(struct.unpack_from('<3I', platform, 56)) and
            facts['startup'].get('fabric_hz') == list(struct.unpack_from('<3I', platform, 68)),
            'Radio fabric differs from reviewed startup.')
    if known:
        require(pins.get('complete_pm_latch_authority') is True and pins.get('pm_latch') == value and
                pins.get('latch_origin') in ('verified-writer-history', 'verified-reset-contract') and
                pins.get('latch_evidence'), 'Complete PM latch provenance absent.')

    module = radio if radio is not None else headset
    chip, sub, revision, subrevision, is_h, known_revision = struct.unpack_from('<2I4B', module, 496)
    expected_revision, expected_h = (chip >> 16) & 63, int((chip >> 16) & 192 == 192)
    require((revision, subrevision, is_h, known_revision) ==
            (expected_revision, (sub >> 4) & 255, expected_h, 1) and
            revision in (1, 3, 7, 15) and (is_h or revision in (3, 7)), 'Unsupported or inconsistent radio identity.')
    # Reuse the native role catalogue, whose provenance is docs/radio-firmware.md.
    source = (ROOT / 'drivers/net/aic8800_firmware.c').read_text()
    sizes = [int(x.strip()) for x in re.search(r'sizes\[\] = \{([^}]+)\}', source)[1].split(',')]
    hashes = re.findall(r'"([0-9a-f]{64})"', source)[:5]
    payloads = doc.get('payloads')
    require(isinstance(payloads, list) and len(payloads) == 4, 'Exactly four radio firmware roles required.')
    for i, (item, (address, size)) in enumerate(zip(payloads, spans)):
        require(isinstance(item, dict) and set(item) == {'path', 'sha256', 'address'} and
                type(item['address']) is int and item['address'] == address, 'Firmware role address differs.')
        ref = {k: item[k] for k in ('path', 'sha256')}; data = read(ref); role = 4 if i == 3 and is_h else i
        require(len(data) == size == sizes[role] and ref['sha256'] == hashes[role], 'Firmware role differs from native catalogue.')
        plan.append((ref['path'], address))
    extents = [(address, len(cache[path])) for path, address in plan]
    require(len({path for path, _ in plan}) == len(plan) and all(RANGE[0] <= a < RANGE[1] and 0 < n <= RANGE[1]-a for a,n in extents) and
            all(a+n <= b or b+m <= a for i,(a,n) in enumerate(extents) for b,m in extents[i+1:]), 'Radio upload overlap or reservation escape.')
    return plan
