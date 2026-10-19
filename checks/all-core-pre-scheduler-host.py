#!/usr/bin/env python3
"""Exercise all-core result validation and the actual returning host runner."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


shared = module('primary_check', ROOT/'checks/primary-publication-host.py')
host = module('all_core_host', ROOT/'tools/all-core-pre-scheduler-host.py')


def transform(model_report):
    def report(data, dram, case):
        # Keep the instruction model's actual register observations consistent.
        # Only the runner nonce and uploaded contract epoch belong to this fixture.
        raw = bytearray(model_report)
        raw[32:48] = data[32:48]
        epoch = struct.unpack_from('<Q', dram, 0x1004000+24)[0]
        struct.pack_into('<Q', raw, 280, epoch)
        for core in range(8):
            for offset in (0xa00+core*512+8, 0x2000+core*256+8,
                           0x2800+core*256+8, 0x3000+core*64+16):
                struct.pack_into('<Q', raw, offset, epoch)
            if core:
                for offset in (512+core*256, 512+core*256+128, 512+core*256+136):
                    struct.pack_into('<Q', raw, offset, epoch)
        raw[0x3200:] = host.expected_memory(epoch)
        if case == 'invalid-report-restored':
            struct.pack_into('<Q', raw, 0x3000+40, 191)
        return bytes(raw)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'bundle', 'model', 'survey', 'fel', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--case', action='append')
    a = parser.parse_args()
    a.output.mkdir(parents=True, exist_ok=bool(a.case))
    manifest = json.loads((a.build/'manifest.json').read_text())
    host.load(a.build, a.bundle/'image.elf', manifest)
    instruction = json.loads(a.model.read_text())
    model_report = a.model.parent/'report-success.bin'
    assert host.digest(model_report) == next(c['report_sha256'] for c in instruction['cases'] if c['case'] == 'success')
    smc_return = manifest['hooks']['el1_smc_return']
    raw = model_report.read_bytes()
    assert host.decode(raw, raw[32:48], smc_return)['production_pre_scheduler_completed']
    proof = {'schema': 'a733-all-core-pre-scheduler-software-v1', 'physical_access': False,
        'manifest_sha256': host.digest(a.build/'manifest.json'),
        'host_sources': {n: host.digest(ROOT/n) for n in host.HOST_SOURCES},
        'instruction_evidence': {'path': str(a.model.resolve()), 'sha256': host.digest(a.model)},
        'host_cases': dict.fromkeys(host.CASES, True)}
    fixture = a.output/'MODEL-never-deploy.json'
    fixture.write_text(json.dumps(proof, indent=2)+'\n')
    host.validate_software(proof, proof['manifest_sha256'])
    accepted_path = a.output/'accepted-cases.json'
    accepted = json.loads(accepted_path.read_text()) if accepted_path.exists() else {
        'manifest_sha256': proof['manifest_sha256'], 'host_sources': proof['host_sources'], 'cases': {}}
    assert accepted['manifest_sha256'] == proof['manifest_sha256'] and accepted['host_sources'] == proof['host_sources']
    for case in a.case or host.CASES:
        assert case in host.CASES
        if case in host.CASES[:3]:
            assert shared.run_case(a, case, fixture, transform(raw))
        elif case == 'changed-input-refused':
            changed = json.loads((a.build/'manifest.json').read_text())
            changed['reservations'][-1][2] -= 8
            try:
                host.load(a.build, a.bundle/'image.elf', changed)
            except ValueError:
                pass
            else:
                raise AssertionError('Changed report backup reservation accepted')
        else:
            changes = {
                'incomplete-primary-refused': ((0xa00, 2), (0x2000, 2), (0x2800, 2),
                    (0x140, 0), (0x150, 0), (0x3000, 2), (0x3028, 191)),
                'incomplete-participant-refused': ((512+6*256+216, 0), (0x3000+6*64, 2),
                    (0x3000+6*64+16, 0), (0x3000+6*64+40, 191), (0x2800+6*256, 2)),
                'wrong-memory-result-refused': ((0x3200, 0), (0x3208, 0),
                    (0x3200+7*64+8, 0), (0x4200+32, 257), (0x4200+72, 0), (0x4200+1024, 1)),
            }[case]
            for offset, value in changes:
                bad = bytearray(raw)
                struct.pack_into('<Q', bad, offset, value)
                try:
                    host.decode(bad, raw[32:48], smc_return)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f'Invalid report accepted at {offset:#x}')
        accepted['cases'][case] = {'pass': True, 'check_sources': {n: host.digest(ROOT/n) for n in (
            'checks/all-core-pre-scheduler-host.py', 'checks/primary-publication-host.py')}}
        accepted_path.write_text(json.dumps(accepted, indent=2)+'\n')
        print('PASS', case, flush=True)
    assert set(accepted['cases']) == set(host.CASES)
    proof.update(case_evidence=accepted['cases'],
        limits='Actual host runner and decoder; modeled FEL, USB, queue and reports. No physical access.')
    (a.output/'software.json').write_text(json.dumps(proof, indent=2)+'\n')


if __name__ == '__main__':
    main()
