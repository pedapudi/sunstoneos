#!/usr/bin/env python3
"""Host check of the supervised integrated OS preparation and launcher.

It prepares a bundle for a linked OS image from synthetic reviewed records,
then checks that:

- the image's own admission (checks/os-image-model.c, compiled with the
  image's build identity) accepts the prepared session with the contract the
  SRAM launcher would complete, and the built launcher names the OS request,
  contract and session size;
- preparation refuses a missing or wrong reviewed record, changed evidence,
  a link check of another configuration and out-of-rule session fields;
- tools/check-a733-os-supervised.py freezes and revalidates the bundle, bounds
  the generations, and its read-only claim check accepts only the matching
  claim (through a modeled database connection);
- its --fault-report flag is refused before any launch step for an image
  without a733_fault_previous, and otherwise reads the report once through
  a modeled device before the capture, which a failed read never reaches.

Every physical fact is a labeled synthetic record. No device, USB, queue
database or FEL program is opened; the launcher's transport is never called.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HZ = 24000000


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


tool = module('os_launch_tool', ROOT / 'tools/check-a733-os-supervised.py')
prep = tool.preparation


def refused(action):
    try:
        action()
    except ValueError:
        return True
    raise AssertionError('refusal expected')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'base', 'overlay', 'wav', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--clang', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--lld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/bin/ld.lld-21')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = args.build / 'sunstone-a733-os.elf'
    link_check = args.build / 'os-image/link-check.json'
    manifest = json.loads(link_check.read_text())
    audio = manifest['audio']
    image_hash = hashlib.sha256(image.read_bytes()).hexdigest()
    cases = {}

    # The image's admission, compiled on the host with the image's identity.
    build_id = bytes.fromhex(manifest['build_id'])
    model = out / 'os-image-model'
    command = ['clang', '-std=c11', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-Iinclude', '-Iplatform/a733-smp-cached', '-Iapps/os', '-DSUNSTONE_BOARD_SUPERVISED=1',
               '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DA733_SMP_CONTRACT_MAGIC=0x415337334f534331ULL',
               '-DOS_MODEL_BUILD_ID=' + ','.join(str(b) for b in build_id),
               *(['-DSUNSTONE_A733_OS_AUDIO=1'] if audio else []),
               'checks/os-image-model.c', 'drivers/storage/storage_digest.c', '-o', str(model)]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(model), '--write-session', str(out / 'model-session.bin'), '9'], check=True)
    raw = bytearray((out / 'model-session.bin').read_bytes())
    assert len(raw) == (992 if audio else 808)

    # Synthetic reviewed records, labeled as such, bound to this image.
    facts = out / 'MODEL-NOT-PHYSICAL'
    facts.mkdir()

    def put(name, value):
        data = json.dumps(value, sort_keys=True).encode()
        path = (facts / (name + '.json')).resolve()
        path.write_bytes(data)
        return {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest()}
    target = {'identity': 'OS-SOFTWARE-MODEL', 'port': 'NO-PHYSICAL-PORT', 'image_sha256': image_hash}
    marker = put('MODEL', {'physical_access': False, 'warning': 'Synthetic fixture only. Never physical authority.'})
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    identities = {'epoch': epoch, 'generation': generation, 'lease': lease}
    thermal_facts = {
        'initial': ['exclusive_ths_and_irq71', 'remote_writers_excluded', 'no_inherited_thermal_service',
                    'security_access_authorized', 'gic_route_owned', 'boot_lineage_reviewed'],
        'electrical': ['avcc_adc_digital_supplies_stable', 'sid_shadows_readable', 'device_mappings_verified',
                       'no_physical_aliases'],
        'clocks': ['stable_apb1_ref_osc', 'no_clock_reset_writers',
                   'gpadc_consumers_excluded' if raw[768] else 'stable_enabled_gpadc24m'],
        'supervision': ['whole_board_power_removal', 'permanent_retention', 'no_reuse', 'one_attempt']}
    record = {**target, **identities, 'schema': prep.ENTRY_SCHEMA, 'reservation': 100,
              'whole_board_power_removal': True, 'no_resource_reuse_before_power_removal': True,
              'recovery': {'mode': 'manual-usb-reconnect', 'demonstrated_fel': True,
                           'explicit_reconnect_before_resumption': True, 'preserve_microsd': True,
                           'one_attempt': True, 'evidence': [marker]}}
    for key, offset in zip(thermal_facts, (632, 664, 696, 728)):
        ref = put('thermal-' + key, {**target, 'schema': 'a733-thermal-' + key + '-v1', 'reviewed': True,
                                     **dict.fromkeys(thermal_facts[key], True)})
        raw[offset:offset + 32] = bytes.fromhex(ref['sha256'])
        record[key] = ref
    conditions = ('source-backed-cpu-dsu-transition', 'cold-cache-and-dirty-alias-exclusion', 'dual-pas-dram',
                  'exclusive-cpus-memory-dma', 'remote-writers-excluded', 'usb-clock-power-reset-ownership',
                  'usb-device-mapping', 'physical-counter-continuity')
    record['platform'] = put('platform', {**target, 'schema': 'a733-smp-observer-platform-v1',
                                          'entry_conditions_established': True,
                                          'physical_coherence_result': 'under-test',
                                          'conditions': {k: marker for k in conditions}})
    ddr = {**target, 'schema': 'a733-ddr-reservation-v1', 'exclusive': True, 'published_cache_disabled': True,
           'remote_writers_excluded': True, 'evidence': [marker]}
    record['ddr'] = put('ddr', {**ddr, 'ranges': [[0x41000000, 0x42200000]]})
    record['mimus_ddr'] = put('mimus-ddr', {**ddr, 'ranges': prep.mimus.RANGES})
    if audio:
        dma_sha256 = hashlib.sha256(raw[prep.AUDIO_DMA:prep.AUDIO_DMA + prep.AUDIO_DMA_BYTES]).hexdigest()
        audio_cold, samples = struct.unpack_from('<2I', raw, prep.AUDIO_TAIL)
        audio_facts = {**target, **identities, 'schema': prep.AUDIO_SCHEMA, 'audio_cold': audio_cold,
                       'capture_samples': samples, 'dma_sha256': dma_sha256,
                       'inherited_sha256': hashlib.sha256(raw[976:985]).hexdigest(),
                       'conditions': {k: marker for k in sorted(prep.AUDIO_CONDITIONS)}, 'one_attempt': True,
                       'whole_board_power_removal': True, 'no_resource_reuse_before_power_removal': True}
        record['audio'] = put('audio', audio_facts)
        raw[prep.AUDIO_EVIDENCE:prep.AUDIO_EVIDENCE + 32] = bytes.fromhex(record['audio']['sha256'])
        record['dma_nonsecure_access'] = put('dma-nonsecure', {
            **target, **identities, 'schema': 'a733-dma-nonsecure-access-v1', 'physical_authority': True,
            'modeled': False, 'reviewed': True, 'controller': 0x04601000, 'service_channels': 1,
            'nonsecure_channels': 1, 'dma_evidence': lease, 'dma_handoff': lease, 'configuration_sha256': dma_sha256,
            **dict.fromkeys(prep.security_authority.FACTS, True), 'evidence': [marker]})
    template = out / 'MODEL-session-template.bin'
    template.write_bytes(raw)
    record['session_template_sha256'] = hashlib.sha256(raw).hexdigest()
    admission = out / 'MODEL-admission.json'
    admission.write_text(json.dumps(record, indent=2) + '\n')
    assets = [args.base, args.overlay, args.wav]

    def prepare(name, session=template, entry=admission, check=link_check):
        return prep.prepare(image, check, entry, session, assets, out / name, str(args.clang), str(args.lld))
    bundle = out / 'MODEL-NOT-DEPLOYABLE-bundle'
    value = prepare(bundle.name)
    assert value['audio'] is audio
    inner = prep.validate_board(bundle / 'board')
    session = (bundle / 'board/session.bin').read_bytes()
    assert session[:48] == raw[:48] and session[48:80] == build_id and session[80:] == raw[80:]
    base_text = (bundle / 'board/board-launch-base.c').read_text()
    assert ('CONTRACT->reserved[1]==%du' % len(raw)) in base_text and '0x415337334f534c31' in base_text
    cases['prepared-bundle-and-launcher-for-session-size'] = True

    # The contract as the SRAM launcher completes it, judged by the image's admission.
    contract = bytearray((bundle / 'board/contract.bin').read_bytes()[:224])
    armed = 100 * HZ
    struct.pack_into('<Q', contract, 0, prep.CONTRACT_MAGIC)
    struct.pack_into('<4Q', contract, 80, armed, armed + HZ, armed + 60 * HZ, armed + 60 * HZ)
    (out / 'launched-contract.bin').write_bytes(contract)

    def admit(contract_path, now):
        run = subprocess.run([str(model), '--admit', str(contract_path), str(bundle / 'board/session.bin'), str(now)],
                             capture_output=True, text=True)
        return run.returncode == 0 and run.stdout.strip() == 'admitted'
    assert admit(out / 'launched-contract.bin', armed + HZ // 10)
    assert not admit(out / 'launched-contract.bin', armed + HZ)
    changed = bytearray(contract)
    struct.pack_into('<Q', changed, 184, 808 if audio else 992)
    (out / 'changed-size-contract.bin').write_bytes(changed)
    assert not admit(out / 'changed-size-contract.bin', armed + HZ // 10)
    cases['image-admission-accepts-launched-contract'] = True

    # Preparation refuses each missing or changed reviewed fact before writing.
    def entry_with(change, name):
        value = copy.deepcopy(record)
        change(value)
        path = out / ('MODEL-admission-' + name + '.json')
        path.write_text(json.dumps(value))
        return path

    def session_with(change, name):
        """A changed template under a record naming that template, so that only
        the changed field's own rule can refuse it."""
        value = bytearray(raw)
        change(value)
        path = out / ('MODEL-session-' + name + '.bin')
        path.write_bytes(value)
        entry = entry_with(lambda v: v.update(session_template_sha256=hashlib.sha256(value).hexdigest()), name)
        return {'session': path, 'entry': entry}
    refusals = {
        'board-entry-schema': lambda: prepare('r1', entry=entry_with(lambda v: v.update(schema='a733-board-supervised-entry-v2'), 'schema')),
        'other-session-identities': lambda: prepare('r2', entry=entry_with(lambda v: v.update(lease=v['lease'] + 1), 'lease')),
        'missing-input-reservation': lambda: prepare('r3', entry=entry_with(lambda v: v.pop('mimus_ddr'), 'ddr')),
        'thermal-evidence-hash': lambda: prepare('r4', **session_with(lambda v: v.__setitem__(slice(632, 633), b'\0'), 'thermal')),
        'workload-two': lambda: prepare('r5', **session_with(lambda v: struct.pack_into('<I', v, 400, 2), 'workload')),
    }
    other = dict(manifest, audio=not audio)
    (out / 'other-link-check.json').write_text(json.dumps(other))
    refusals['link-check-of-other-configuration'] = lambda: prepare('r6', check=out / 'other-link-check.json')
    resident = dict(manifest, lifetime='resident')
    (out / 'resident-link-check.json').write_text(json.dumps(resident))
    refusals['resident-link-check'] = lambda: prepare('r7', check=out / 'resident-link-check.json')
    # Preparation does not stage the radio or Bluetooth profile and firmware yet.
    for index, option in enumerate(('radio', 'headset')):
        (out / (option + '-link-check.json')).write_text(json.dumps(dict(manifest, **{option: True})))
        refusals[option + '-link-check'] = (lambda name, path: lambda: prepare(name, check=path))(
            'r%d' % (8 + index), out / (option + '-link-check.json'))
    if audio:
        refusals.update({
            'missing-audio-record': lambda: prepare('a1', entry=entry_with(lambda v: v.pop('audio'), 'audio')),
            'missing-dma-authority': lambda: prepare('a2', entry=entry_with(lambda v: v.pop('dma_nonsecure_access'), 'dma')),
            'audio-evidence-hash': lambda: prepare('a3', **session_with(lambda v: v.__setitem__(slice(832, 833), b'\0'), 'audio')),
            'capture-below-one-second': lambda: prepare('a4', **session_with(lambda v: struct.pack_into('<I', v, 972, 15999), 'samples')),
            'dma-handoff-not-lease': lambda: prepare('a5', **session_with(lambda v: struct.pack_into('<Q', v, 920, lease + 1), 'handoff')),
            'two-dma-channels': lambda: prepare('a6', **session_with(lambda v: struct.pack_into('<H', v, 966, 3), 'channels')),
        })
        partial = json.loads((facts / 'audio.json').read_bytes())
        partial['conditions'].pop('stop-after-first-failure')
        incomplete = put('audio-incomplete', partial)

        def condition_missing():
            # The session names the incomplete record, so only its conditions refuse it.
            changed = session_with(lambda v: v.__setitem__(slice(832, 864), bytes.fromhex(incomplete['sha256'])),
                                   'conditions')
            entry = json.loads(changed['entry'].read_text())
            entry['audio'] = incomplete
            changed['entry'].write_text(json.dumps(entry))
            prepare('a7', **changed)
        refusals['audio-condition-missing'] = condition_missing
    else:
        refusals['audio-authority-without-audio'] = lambda: prepare('n1', entry=entry_with(
            lambda v: v.update(audio=marker), 'unexpected-audio'))
    for name, action in refusals.items():
        assert refused(action), name
        cases['refuses-' + name] = True
    # Every refusal comes before the bundle directory is created.
    assert not any((out / n).exists() for n in ('r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'a1', 'a2', 'a3',
                                                 'a4', 'a5', 'a6', 'a7', 'n1'))

    # The frozen bundle no longer depends on the original evidence files.
    shutil.rmtree(facts)
    prepared = tool.freeze_bundle(bundle, 1)
    assert prepared['audio'] is audio and prepared['_combined_frozen']['board/session.bin'] == session
    tool.entry_check(prepared, target['identity'], target['port'])
    assert refused(lambda: tool.entry_check(prepared, 'OTHER-USB-INSTANCE', target['port']))
    if audio:
        assert refused(lambda: tool.freeze_bundle(bundle, 3))
    (bundle / 'board/session.bin').write_bytes(session[:-1] + bytes([session[-1] ^ 1]))
    assert refused(lambda: tool.freeze_bundle(bundle, 1))
    (bundle / 'board/session.bin').write_bytes(session)
    cases['launcher-freezes-and-revalidates-without-original-evidence'] = True

    # The read-only claim check against a modeled connection; no database exists.
    claim = out / 'claim-model'
    claim.mkdir()

    def ref(path):
        return {'path': str(Path(path).resolve()), 'sha256': tool.digest(path)}
    proof = claim / 'software-model.json'
    proof.write_text(json.dumps({'schema': 'a733-os-supervised-software-v1', 'physical_access': False, 'audio': audio,
        'admission_cases': True, 'launcher_cases': True, 'host_capture': True, 'host_lifecycle': True,
        'image': ref(bundle / 'board/image.elf'), 'launcher': ref(bundle / 'board/launch.bin'),
        'sources': {name: ref(path) for name, path in tool.software_sources().items()},
        'evidence': [ref(Path(__file__))], 'fixture_only': 'Not board admission or queue submission.'}))
    prerequisites = {'supervised-ram-recovery': ref(bundle / 'board/admission.json'), 'os-supervised-software': ref(proof)}
    submission = {'prerequisites': list(prerequisites),
                  'artifacts': [ref(bundle / name) for name in prepared['_combined_files']]}
    row = {'state': 'claimed', 'owner': 'model-owner', 'submission': json.dumps(submission),
           'review': json.dumps({'prerequisites': prerequisites})}

    class Query:
        def __init__(self, value):
            self.value = value

        def fetchone(self):
            return self.value

    class Connection:
        row_factory = None

        def execute(self, sql, parameters=()):
            if 'settings' in sql:
                return Query([''])
            assert parameters == ('model-claim',)
            return Query(row)

        def close(self):
            pass

    def connect(database_uri, **kwargs):
        assert database_uri.endswith('?mode=ro') and kwargs == {'uri': True}
        return Connection()
    original = tool.sqlite3.connect
    tool.sqlite3.connect = connect
    try:
        for case in ('accepted', 'owner', 'missing-session-artifact', 'recovery-record'):
            saved = copy.deepcopy(row)
            if case == 'owner':
                row['owner'] = 'different'
            if case == 'missing-session-artifact':
                value = copy.deepcopy(submission)
                value['artifacts'] = [v for v in value['artifacts'] if not v['path'].endswith('/board/session.bin')]
                row['submission'] = json.dumps(value)
            if case == 'recovery-record':
                value = copy.deepcopy(prerequisites)
                value['supervised-ram-recovery'] = ref(proof)
                row['review'] = json.dumps({'prerequisites': value})
            try:
                tool.claimed(claim / 'never-opened.sqlite', 'model-claim', 'model-owner', bundle, prepared)
                assert case == 'accepted'
            except ValueError:
                assert case != 'accepted'
            row.clear()
            row.update(saved)
            cases['claim-' + case] = True
    finally:
        tool.sqlite3.connect = original
    assert not (claim / 'never-opened.sqlite').exists()

    # --fault-report. The supervised transport is replaced by a function that
    # hands the capture callback a modeled operation 5 device; nothing else of
    # the launcher's transport runs.
    faults = module('os_launch_fault_model', ROOT / 'checks/fault-report.py')
    report = faults.report()
    stripped = out / 'MODEL-image-without-fault-report.elf'
    subprocess.run(['/usr/lib/llvm-21/bin/llvm-objcopy', '--strip-symbol=a733_fault_previous',
                    str(bundle / 'board/image.elf'), str(stripped)], check=True)
    without = dict(prepared, _combined_frozen=dict(prepared['_combined_frozen'],
                                                   **{'board/image.elf': stripped.read_bytes()}))
    tool.require_fault_report(prepared)
    assert refused(lambda: tool.require_fault_report(without))
    calls = []

    class Unused:
        def __enter__(self):
            calls.append('launch-view')
            return out / 'never-launched', {}

        def __exit__(self, *unused):
            return False

    def session(args, view, launch, ready, entry, capture, **unused):
        calls.append('session')
        args.evidence.mkdir()
        record = {}
        capture(device, lambda: calls.append('same'), time.monotonic() + 60, record, lambda: calls.append('save'))
        return record

    def captured(device, same, deadline, prepared_value, evidence, record, save):
        calls.append(('capture', len(device.writes)))
        return record
    saved = (tool.freeze_bundle, tool.combined.launch_view, tool.supervisor.run_session,
             tool.combined.capture_device, tool.supervisor.base.fel_request)
    tool.combined.launch_view = lambda value: Unused()
    tool.supervisor.run_session = session
    tool.combined.capture_device = captured
    tool.supervisor.base.fel_request = lambda *unused: calls.append('fel')

    def launch(name, value, *flags):
        tool.freeze_bundle = lambda path, count: value
        calls.clear()
        return tool.main(['--bundle', str(bundle), '--fel', str(out / 'no-fel'), '--evidence', str(out / name),
                          '--database', str(claim / 'never-opened.sqlite'), '--claim', 'model-claim',
                          '--owner', 'model-owner', '--supervised-manual-reconnect', *flags])
    try:
        device = faults.Device(report, 512)
        assert refused(lambda: launch('fault-refused', without, '--fault-report'))
        assert calls == [] and not (out / 'fault-refused').exists()
        cases['fault-report-refused-for-image-without-report'] = True

        record = launch('fault-read', prepared, '--fault-report')
        assert calls[:2] == ['launch-view', 'session'] and ('capture', 1) in calls and 'fel' not in calls
        assert device.writes == [faults.tool.REQUEST] and len(device.reads) == 3
        evidence = out / 'fault-read'
        assert (evidence / 'fault-report.bin').read_bytes() == report
        decoded = json.loads((evidence / 'fault-report.json').read_text())
        assert decoded == json.loads(json.dumps(faults.decoder.decode_report(report)))
        assert record['fault_report'] == {'path': 'fault-report.bin', 'bytes': len(report),
                                          'sha256': hashlib.sha256(report).hexdigest(),
                                          'decoded': 'fault-report.json'}
        cases['fault-report-read-once-before-capture'] = True

        device = faults.Device(report, 512, stop=True)
        try:
            launch('fault-stopped', prepared, '--fault-report')
            raise AssertionError('a stopped device must end the run')
        except faults.Stopped:
            pass
        assert len(device.writes) == 1 and not any(isinstance(c, tuple) for c in calls)
        assert not (out / 'fault-stopped/fault-report.bin').exists()
        cases['fault-report-error-ends-run-before-capture'] = True

        device = faults.Device(report, 512)
        record = launch('fault-not-requested', prepared)
        assert ('capture', 0) in calls and not device.writes and 'fault_report' not in record
        cases['no-fault-report-request-without-flag'] = True
    finally:
        (tool.freeze_bundle, tool.combined.launch_view, tool.supervisor.run_session,
         tool.combined.capture_device, tool.supervisor.base.fel_request) = saved
    assert not any(name == 'usb' or name.startswith('usb.') for name in sys.modules)
    result = {'passed': True, 'physical_access': False, 'audio': audio, 'image_sha256': image_hash,
              'build_id': manifest['build_id'], 'tool_sha256': tool.digest(Path(tool.__file__)),
              'preparation_sha256': tool.digest(Path(prep.__file__)), 'cases': cases,
              'limits': 'All physical facts synthetic; the transport, FEL program and board are never used.'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS OS supervised preparation and launcher (%s): %d cases' % ('audio' if audio else 'no audio', len(cases)))


if __name__ == '__main__':
    main()
