#!/usr/bin/env python3
"""Exercise actual CPU-return preparation bindings and capture with modeled FEL.

The bundle contains labeled software authority. The copied terminal report
comes from the linked instruction model; the output record is produced by
executing the linked native completion function with a fixture transcript.
No physical identity, queue, USB node or FEL executable is opened.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
from harness import Image, refused, save

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


host = module('checked_os_cpu_return_host', 'apps/os/cpu-return-host.py')
prior = module('checked_os_cpu_return_native', 'checks/cpu-runtime-qualification.py')
rules = module('checked_os_cpu_return_review', 'apps/os/cpu-return-review.py')


def native_output(bundle, p):
    path = bundle / 'board/image.elf'
    image = prior.fault.Image(path)
    model = prior.Model(image)
    layout = Image(path, {'board_application', 'a733_board_session'})
    board = layout.address('board')
    session = board + layout.layouts['board_application']['session']
    fields = layout.layouts['a733_board_session']
    model.uc.mem_write(session, (bundle / 'board/session.bin').read_bytes()[:592])
    model.put64(session + fields['deadline_us'], 60000000)
    text = b'MODEL ONLY recorded transcript; no physical recognition claim\n'
    model.uc.mem_write(0x41ef0000, text)
    model.call(6, 'os_cpu_return_begin', (0x41ef0000, len(text)))
    raw = bytes(model.uc.mem_read(p['qualification']['output_address'], host.OUTPUT_BYTES))
    assert host.decode_output(raw, p['epoch'], p['generation']) == text.decode()
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--native-report', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--case', action='append', help='Run only selected capture cases')
    for name in ('helper-image', 'helper-binary', 'helper-manifest'):
        parser.add_argument('--' + name, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    prepared = host.validate_bundle(args.bundle)
    report = args.native_report.read_bytes()
    host.decode_report(report, prepared['epoch'])
    output = native_output(args.bundle, prepared)
    (args.out / 'native-output.bin').write_bytes(output)
    rows = []
    manifest = json.loads((args.bundle / 'board/image-input-manifest.json').read_text())
    refused(lambda: host.os_image.image_manifest_check(manifest, (args.bundle / 'board/image.elf').read_bytes()),
            'dedicated host adapter')
    rows.append({'name': 'ordinary-host-refuses-return-profile', 'passed': True})
    entry = json.loads((args.bundle / 'board/admission.json').read_text())
    template = (args.bundle / 'board/session-template.bin').read_bytes()
    evidence = {name: (args.bundle / 'board' / value['name']).read_bytes()
                for name, value in prepared['entry_evidence'].items()}
    def validate(changed):
        return rules.validate(changed, lambda ref: evidence[ref['path']], prepared['image_sha256'],
                              host.hashlib.sha256(template).hexdigest(), host.require)
    validate(entry)
    for name, change, reason in [
            ('missing-normal-review', lambda value: value.pop('normal_return_review'), 'Exact CPU normal-return'),
            ('missing-failure-retention', lambda value: value.pop('no_resource_reuse_before_power_removal_on_failure'),
             'failure retains'),
            ('unconditional-retention', lambda value: value.update(whole_board_power_removal=True),
             'explicit failure-only')]:
        changed = copy.deepcopy(entry)
        change(changed)
        refused(lambda: validate(changed), reason)
        rows.append({'name': name, 'passed': True})
    refused(lambda: host.os_image.validate_admission(entry, prepared['image_sha256'],
            host.hashlib.sha256(template).hexdigest(), template, False, evidence, True,
            inference=manifest['inference']), 'Explicit complete-power-removal/retention plan')
    rows.append({'name': 'default-admission-keeps-unconditional-retention', 'passed': True})
    variants = ('success', 'no-new-ROM', 'bad-report', 'truncated-output', 'wrong-output-session',
                'snapshot-incomplete', 'asset-transfer-failure', 'saved-RAM-changed', 'restore-mismatch')
    if args.case:
        assert set(args.case) <= set(variants)
        variants = args.case
    for variant in variants:
        with tempfile.TemporaryDirectory(prefix='os-return-fel-model-') as temporary:
            root = Path(temporary)
            p = copy.deepcopy(prepared)
            record = {'complete': False, 'launch_attempted': False, 'transport_failed': False,
                      'execution_profile': host.PROFILE, 'root_owned_claim': 'MODEL ONLY - no live claim'}
            state = {'launched': False, 'reads': 0, 'writes_after_launch': 0, 'calls': 0}
            memory = {}
            def tx(*arguments, pass_fds=()):
                state['calls'] += 1
                result = []
                index = 0
                while index < len(arguments):
                    operation = arguments[index]
                    index += 1
                    if operation == 'readl':
                        address = int(arguments[index], 0)
                        index += 1
                        result.append(hex({0x20: 0xeb00008f, 0x24: 0xe3a00911, 0x28: 0xe3a0193d,
                                           0x3c: 0xe3a0605c, 0x40: 0xea00004d}.get(address, 0)))
                    elif operation == 'version':
                        result.append('soc=00001903(A733)')
                    elif operation == 'exe':
                        assert arguments[index] == '0x52000' and not state['launched']
                        index += 1
                        state['launched'] = True
                    elif operation == 'write':
                        address = int(arguments[index], 0)
                        data = Path(arguments[index + 1]).read_bytes()
                        index += 2
                        if variant == 'asset-transfer-failure' and address == 0x44000000:
                            raise ValueError('modeled asset transfer failure')
                        memory[address, len(data)] = data
                        if state['launched']:
                            state['writes_after_launch'] += 1
                    elif operation == 'read':
                        address, size, path = int(arguments[index], 0), int(arguments[index + 1]), Path(arguments[index + 2])
                        index += 3
                        state['reads'] += 1
                        if state['launched'] and address == p['qualification']['report_address']:
                            data = bytearray(report)
                            if variant == 'bad-report':
                                struct.pack_into('<Q', data, 48, 24)
                        elif state['launched'] and address == p['qualification']['output_address']:
                            data = bytearray(output)
                            if variant == 'truncated-output':
                                data = data[:-1]
                            if variant == 'wrong-output-session':
                                struct.pack_into('<Q', data, 24, p['epoch'] + 1)
                        else:
                            data = memory.get((address, size), b'\xa5' * size)
                        if variant == 'snapshot-incomplete' and state['reads'] == 1:
                            data = data[:-1]
                        if variant == 'restore-mismatch' and state['writes_after_launch']:
                            data = bytes([data[0] ^ 1]) + data[1:]
                        path.write_bytes(data)
                    else:
                        raise AssertionError(operation)
                return ' '.join(result)
            def wait():
                assert state['launched']
                if variant == 'no-new-ROM':
                    raise ValueError('modeled missing ROM return')
                if variant == 'saved-RAM-changed':
                    path = root / record['dram_backups'][0]['file']
                    raw = path.read_bytes()
                    path.write_bytes(bytes([raw[0] ^ 1]) + raw[1:])
            reasons = {'no-new-ROM': 'modeled missing ROM', 'bad-report': 'No successful CPU return report',
                       'truncated-output': 'Incomplete copied CPU return output',
                       'wrong-output-session': 'Recorded output belongs',
                       'snapshot-incomplete': 'Incomplete CPU return RAM backup',
                       'asset-transfer-failure': 'modeled asset transfer failure',
                       'saved-RAM-changed': 'Saved CPU return RAM changed',
                       'restore-mismatch': 'DRAM restoration readback differs'}
            action = lambda: host.run_once(tx, wait, lambda: None, args.bundle, p, root, record, lambda: None)
            if variant == 'success':
                action()
                assert record['complete'] and record['dram_restored'] and record['rom_usable']
                assert record['cpu_rail_restoration_pending'] and not record['ownership_release_permitted']
                assert state['writes_after_launch'] == len(record['dram_backups'])
                assert (root / 'transcript.txt').read_bytes() == output[48:48 + struct.unpack_from('<Q', output, 40)[0]]
                if args.helper_image:
                    assert args.helper_binary and args.helper_manifest
                    helper_manifest = json.loads(args.helper_manifest.read_text())
                    host.rail_followup_check(record, args.helper_image, args.helper_binary, helper_manifest)
                    rows.append({'name': 'exact-entry800-rail-helper-admitted', 'passed': True})
                    changed = copy.deepcopy(record)
                    changed['cpu_rail_restoration_pending'] = False
                    refused(lambda: host.rail_followup_check(changed, args.helper_image, args.helper_binary,
                                                            helper_manifest), 'retained successful CPU return claim')
                    changed_manifest = dict(helper_manifest, execution_variant='legacy')
                    refused(lambda: host.rail_followup_check(record, args.helper_image, args.helper_binary,
                                                            changed_manifest), 'artifact or variant')
                    rows.extend({'name': name, 'passed': True} for name in
                                ('missing-pending-rail-claim-refused', 'legacy-helper-variant-refused'))
                    save(args.out / 'MODEL-return-receipt.json', {'physical_access': False, 'model_record': record})
            else:
                refused(action, reasons[variant])
                assert not record['complete'] and record['dirty']
                if variant != 'restore-mismatch':
                    assert state['writes_after_launch'] == 0
            rows.append({'name': variant, 'passed': True, 'transactions': state['calls']})
    save(args.out / 'results.json', {'physical_access': False, 'cases': rows,
                                   'image_sha256': prepared['image_sha256']})
    print('PASS CPU return host:', len(rows), 'cases')


if __name__ == '__main__':
    main()
