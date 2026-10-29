"""First-functional CPU clock authority for the exact returning profile.

Historical mechanism qualification is separate from current electrical state.
The linked guard establishes current predicates before the first request.
No record in this schema asserts that integrated OS B1 has passed.
"""
import hashlib
import json
import struct

SCHEMA = 'a733-os-cpu-return-clock-under-test-v1'
COMPATIBILITY_SCHEMA = 'a733-os-cpu-return-clock-compatibility-under-test-v1'
UNDER_TEST_SCHEMAS = (SCHEMA, COMPATIBILITY_SCHEMA)
MARKER = (0x31544e4552505543, 1, 4, 8)
CONDITIONS = {
    'historical-steps-1-through-6-same-physical-board',
    'sole-native-clock-owner-and-external-writers-excluded',
    'current-session-read-only-enabled-rail-inspections',
    'live-bin4-26mhz-boot-clocks-and-pattern-guard',
    'calibrated-five-channel-fresh-normal-thermal-guard',
    'integrated-recognition-dvfs-thermal-and-return-under-test',
    'retained-claim-through-verified-boot-clocks-and-rail-restoration'}

COMPATIBILITY_CONDITIONS = (CONDITIONS - {'historical-steps-1-through-6-same-physical-board'}) | {
    'historical-mechanism-compatible-with-current-target',
    'first-device-outcomes-under-test-and-failure-custody-reviewed'}
APPLICABILITY = {'soc-and-cpu-implementation-revision', 'board-topology-and-regulator-wiring',
                 'pinned-clock-pmic-thermal-sequences'}
RESIDUALS = {'target-regulator-transients', 'target-runtime-dvfs',
             'target-recognition-thermal-irq-integration', 'target-return-and-rail-restoration'}


def compatibility(a, historical, record, profile, evidence, require):
    require(historical.get('schema') == 'a733-os-cpu-return-historical-mechanism-v1' and
            historical.get('reviewed') is True and
            isinstance(historical.get('identity'), list) and len(historical['identity']) == 5 and
            all(type(v) is int for v in historical['identity']) and
            all(isinstance(historical.get(k), str) and historical[k].strip() for k in ('port', 'date')) and
            historical.get('sid_word') == a.get('historical_sid_word') and historical.get('bin') == 4 and
            historical.get('steps') == [1, 2, 3, 4, 5, 6] and historical.get('evidence') and
            not ({'passed', 'same_physical_board', 'board_mapping_basis'} & historical.keys()),
            'Historical mechanism evidence must retain its actual source identity and scope.')
    c = json.loads(evidence(a.get('mechanism_compatibility')))
    require(c.get('schema') == 'a733-os-cpu-return-mechanism-compatibility-v1' and
            c.get('reviewed') is True and c.get('status') == 'under-test' and
            c.get('execution_profile') == 'os-cpu-return-v1' and
            c.get('identity') == record['identity'] and c.get('port') == record['port'] and
            all(c.get(k) == a[k] for k in ('image_sha256', 'build_id', 'session_template_without_approval_sha256')) and
            all(c.get(k) == profile[k] for k in ('epoch', 'generation', 'lease')) and
            c.get('historical_mechanism') == a['historical_qualification'] and
            c.get('source_qualification_reused_as') == 'mechanism_evidence' and
            c.get('target_device_qualification') == 'under-test' and
            not ({'passed', 'same_physical_board', 'board_mapping_basis'} & c.keys()),
            'Mechanism compatibility review names another target/session or claims target qualification.')
    require(set(c.get('applicability', {})) == APPLICABILITY and
            c.get('residuals') == {k: 'under-test' for k in RESIDUALS} and c.get('sources') and
            isinstance(c.get('review_basis'), str) and c['review_basis'].strip(),
            'Mechanism applicability, source pins or first-device residuals absent.')
    for item in [*c['applicability'].values(), *c['sources']]:
        evidence(item)


def validate(a, data, record, image_hash, raw, profile, grant, evidence, require, Elf):
    require(a.get('schema') in UNDER_TEST_SCHEMAS and a.get('execution_profile') == 'os-cpu-return-v1' and
            a.get('status') == 'under-test' and a.get('reviewed') is True and 'passed' not in a and
            a.get('identity') == record['identity'] and a.get('port') == record['port'] and
            a.get('image_sha256') == image_hash and
            all(a.get(k) == profile[k] for k in ('epoch', 'generation', 'lease')),
            'CPU return under-test authority names another profile, image, board or session.')
    require(len(raw) == 904 and struct.unpack_from('<3Q', raw, 808) == (0x4153373343505531, 1, 96) and
            struct.unpack_from('<I', raw, 400)[0] == 2,
            'CPU return under-test authority requires the fixed staged session.')
    normalized = bytes(raw[:872]) + bytes(32)
    require(a.get('session_template_without_approval_sha256') == hashlib.sha256(normalized).hexdigest() and
            a.get('build_id') == bytes(raw[48:80]).hex(), 'CPU return normalized template differs.')
    require(grant['clusters'] == 3 and grant['max_mhz'] == (1794, 2002) and
            4 <= grant['changes'] <= 64 and
            a.get('grant') == {'owner': grant['owner'], 'clusters': 3, 'max_mhz': [1794, 2002],
                               'changes': grant['changes']} and
            grant['approval'] == hashlib.sha256(data).digest(), 'CPU return under-test grant differs.')
    final_review = json.loads(evidence(record.get('normal_return_review')))
    require(final_review.get('cpu_clock_approval') == record.get('cpu_clock_approval') and
            final_review.get('session_template_sha256') == hashlib.sha256(raw).hexdigest() and
            a.get('entry_continuity') == {k: record[k] for k in ('ddr', 'platform', 'mimus_ddr')},
            'CPU return final review or entry continuity differs.')
    expected = {'version': 1, 'workers': 8, 'owner_core': 6, 'core_mask': 255,
                'owner_priority': 3, 'a55_priority': 2, 'a76_priority': 3}
    require(record.get('inference') == a.get('inference') == expected and
            a.get('guard_marker') == list(MARKER), 'CPU return inference or guard capability differs.')
    require(a.get('thermal') == {k: record[k] for k in ('initial', 'electrical', 'clocks', 'supervision')} and
            a.get('thermal_config_sha256') == hashlib.sha256(raw[760:808]).hexdigest() and
            struct.unpack_from('<I', raw, 804)[0] == 70000,
            'CPU return under-test thermal configuration differs.')
    require(a.get('one_attempt') is True and a.get('failure_policy') == 'retain-without-retry' and
            all(isinstance(a.get(k), str) and a[k].strip() for k in
                ('supervisor', 'root_owned_claim', 'approval_text', 'approval_date')) and
            set(a.get('conditions', {})) == (COMPATIBILITY_CONDITIONS if a['schema'] == COMPATIBILITY_SCHEMA else CONDITIONS), 'CPU return under-test review obligations absent.')
    for item in a['conditions'].values():
        evidence(item)
    historical = json.loads(evidence(a.get('historical_qualification')))
    if a['schema'] == COMPATIBILITY_SCHEMA:
        require(not ({'same_physical_board', 'board_mapping_basis'} & a.keys()),
                'Compatibility approval cannot assert historical board identity.')
        compatibility(a, historical, record, profile, evidence, require)
    else:
        require(historical.get('schema') == 'a733-os-cpu-return-historical-qualification-v1' and
                historical.get('reviewed') is True and historical.get('identity') == record['identity'] and
                historical.get('port') == record['port'] and historical.get('same_physical_board') is True and
                isinstance(historical.get('board_mapping_basis'), str) and historical['board_mapping_basis'].strip() and
                historical.get('sid_word') == a.get('historical_sid_word') and
                isinstance(a.get('historical_sid_word'), int) and historical.get('steps') == [1, 2, 3, 4, 5, 6] and
                historical.get('evidence'), 'Historical physical qualification or board mapping absent.')
    for item in historical['evidence']:
        evidence(item)
    # This reviewed SID selects the same fixed column that the live native
    # guard requires. Import the authoritative pinned table decoder at caller.
    require(a.get('historical_bin') == 4, 'CPU return historical bin differs.')
    require(a.get('sources'), 'CPU return under-test source provenance absent.')
    for item in a['sources']:
        evidence(item)
    helper = a.get('rail_helper', {})
    manifest = json.loads(evidence(helper.get('manifest')))
    elf_bytes, binary = evidence(helper.get('image')), evidence(helper.get('binary'))
    require(manifest.get('bundle') == 33 and manifest.get('execution_variant') == 'os-cpu-return-dcdc3-800-v1' and
            manifest.get('elf_sha256') == hashlib.sha256(elf_bytes).hexdigest() and
            manifest.get('binary_sha256') == hashlib.sha256(binary).hexdigest(),
            'CPU return under-test rail-helper identity differs.')
    elf = Elf(elf_bytes)
    markers = [s for s in elf.symbols if s['shndx'] != 0 and s['name'] == 'os_cpu_return_rail_profile']
    require(len(markers) == 1 and markers[0]['size'] == 32 and
            struct.unpack('<4Q', elf.read(markers[0]['value'], 32)) == (0x31524c4941525043, 1, 800000, 1000000),
            'CPU return under-test rail-helper marker differs.')
