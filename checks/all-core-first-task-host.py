#!/usr/bin/env python3
"""Check first-task reports and actual host return/retention behavior without USB."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


shared = module('primary_check', ROOT/'checks/primary-publication-host.py')
host = module('first_task_host', ROOT/'tools/all-core-first-task-host.py')


def transform(model_report, task_workload=False):
    def report(data, dram, case):
        raw = bytearray(model_report)
        raw[32:48] = data[32:48]
        epoch = struct.unpack_from('<Q', dram, 0x1004000+24)[0]
        struct.pack_into('<Q', raw, 280, epoch)
        for core in range(8):
            for offset in (0xa00+core*512+8, 0x2000+core*256+8, 0x2800+core*256+8,
                           0x3000+core*64+16, 0x3200+core*64, 0x3a00+core*256+24):
                struct.pack_into('<Q', raw, offset, epoch)
            if core:
                for offset in (512+core*256, 512+core*256+128, 512+core*256+136):
                    struct.pack_into('<Q', raw, offset, epoch)
        if task_workload:
            for core in range(8): struct.pack_into('<Q', raw, 0x3600+core*8, epoch)
        if case == 'invalid-report-restored':
            struct.pack_into('<Q', raw, 0x3200+5*64+40, 0)
        return bytes(raw)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'bundle', 'model', 'survey', 'fel', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--case', action='append')
    parser.add_argument('--task-switch', action='store_true')
    parser.add_argument('--task-workload', action='store_true')
    a = parser.parse_args()
    a.task_switch = a.task_switch or a.task_workload
    host = module('task_profile_host', ROOT/'tools'/(
        'all-core-task-workload-host.py' if a.task_workload else 'all-core-task-switch-host.py' if a.task_switch else 'all-core-first-task-host.py'))
    a.output.mkdir(parents=True, exist_ok=bool(a.case))
    manifest = json.loads((a.build/'manifest.json').read_text())
    host.load(a.build, a.bundle/'image.elf', manifest)
    instruction = json.loads(a.model.read_text())
    model_report = a.model.with_suffix('.bin')
    assert host.digest(model_report) == instruction['report_sha256']
    raw = model_report.read_bytes()
    assert host.decode(raw, raw[32:48], manifest)['first_task_entry_completed']
    instruction_cases = ('wrong-rounds',) if a.task_workload else ('wrong-task-argument',) if a.task_switch else ('wrong-task-argument','restore-el3')
    evidence = {'success': a.model, **{case: a.model.parent/(case+'.json') for case in instruction_cases}}
    proof = {'schema': host.SOFTWARE_SCHEMA if a.task_switch else 'a733-all-core-first-task-software-v1', 'physical_access': False,
        'manifest_sha256': host.digest(a.build/'manifest.json'),
        'host_sources': {n: host.digest(ROOT/n) for n in host.HOST_SOURCES},
        'instruction_evidence': {case: {'path': str(path.resolve()), 'sha256': host.digest(path)}
                                 for case, path in evidence.items()},
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
            assert shared.run_case(a, case, fixture, transform(raw, a.task_workload))
        elif case == 'changed-input-refused':
            for field in ('reservations', 'first_task')+(('task_switch',) if a.task_switch else ())+(('task_workload','patches') if a.task_workload else ()):
                changed = json.loads((a.build/'manifest.json').read_text())
                if field == 'reservations': changed[field][-1][2] -= 8
                elif field == 'first_task': changed[field]['writable_interval'][1] -= 4096
                elif field == 'task_workload': changed[field]['rounds_per_core'] = 31
                elif field == 'patches': changed[field][-1]['replacement'] ^= 4
                else: changed[field]['worker_body'] = not changed[field]['worker_body']
                try: host.load(a.build, a.bundle/'image.elf', changed)
                except ValueError: pass
                else: raise AssertionError('Changed owned-memory scope accepted')
        elif case == 'changed-instruction-evidence-refused':
            for changed_field, changed_value in (('native_sha256', '0'*64), ('case', 'unexecuted'),
                                                  ('off', []), ('clean_complete', []),
                                                  ('workload_results', []), ('interrupt_counts', [0]*8)):
                changed = json.loads(a.model.read_text()); changed[changed_field] = changed_value
                bad_path = a.output/'changed-instruction.json'
                bad_path.write_text(json.dumps(changed)+'\n')
                bad_path.with_suffix('.bin').write_bytes(raw)
                bad_proof = json.loads(json.dumps(proof))
                bad_proof['instruction_evidence']['success'] = {
                    'path': str(bad_path.resolve()), 'sha256': host.digest(bad_path)}
                try: host.validate_software(bad_proof, proof['manifest_sha256'])
                except ValueError: pass
                else: raise AssertionError('Changed instruction evidence accepted: '+changed_field)
        else:
            changes = {
                'incomplete-primary-refused': ((0xa00, 2), (0x2000, 2), (0x2800, 2), (0x3028, 9279)),
                'incomplete-participant-refused': ((512+6*256+216, 0), (0x3000+6*64, 2),
                    (0x3000+6*64+40, 9279), (0x2800+6*256, 2)),
                'wrong-task-context-refused': tuple((0x3200+core*64+offset,
                    (core ^ 1) if a.task_switch and offset == 48 else 1 if offset in (32, 48) else 0)
                    for core in range(8) for offset in (0, 8, 16, 24, 32, 40, 48, 56)),
                'wrong-saved-frame-refused': tuple((0x3a00+core*256+offset, 99)
                    for core in range(8) for offset in (0, 8, 16, 24, 32, 40, 48, 56, 248)),
                'wrong-memory-result-refused': ((0x3400, 1), (0x39f8, 1), (0x4200+32, 257), (0x4200+72, 0)),
                'wrong-workload-result-refused': tuple((0x4280+core*128+offset, value)
                    for core in range(8) for offset, value in ((0,31),(8,0),(16,0),(24,1)))+
                    ((0x42a0,31),(0x4320,1)),
                'wrong-arrival-refused': tuple((0x3600+core*8,0) for core in range(8)),
                'timer-priority-restore-refused': tuple((0x20a8+core*256,8) for core in range(8)),
            }[case]
            for offset, value in changes:
                bad = bytearray(raw); struct.pack_into('<Q', bad, offset, value)
                try: host.decode(bad, raw[32:48], manifest)
                except ValueError: pass
                else: raise AssertionError(f'Invalid report accepted at {offset:#x}')
        accepted['cases'][case] = {'pass': True, 'check_sources': {n: host.digest(ROOT/n) for n in (
            'checks/all-core-first-task-host.py', 'checks/primary-publication-host.py')}}
        accepted_path.write_text(json.dumps(accepted, indent=2)+'\n')
        print('PASS', case, flush=True)
    assert set(accepted['cases']) == set(host.CASES)
    proof.update(case_evidence=accepted['cases'],
        limits='Actual host runner and decoder; modeled FEL, USB, queue and reports. No physical access.')
    (a.output/'software.json').write_text(json.dumps(proof, indent=2)+'\n')


if __name__ == '__main__':
    main()
