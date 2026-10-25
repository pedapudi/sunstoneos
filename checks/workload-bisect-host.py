#!/usr/bin/env python3
"""Check return-deadline workload reports and the actual runner without USB.

The actual runner and decoder execute against a simulated FEL instance whose
reports come from the instruction cases of the same build. A completed
workload restores DRAM and closes normally. A deadline return reads the
owned writable interval, writes nothing after the launch, restores nothing
and keeps the instance for power removal.
"""
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
first = module('first_task_check', ROOT/'checks/all-core-first-task-host.py')
host = module('workload_bisect_host', ROOT/'tools/workload-bisect-host.py')


def deadline_transform(model_report, damage=None):
    def report(data, dram, case):
        raw = bytearray(model_report)
        raw[32:48] = data[32:48]
        struct.pack_into('<Q', raw, 280, struct.unpack_from('<Q', dram, 0x1004000+24)[0])
        if damage:
            # A capture re-entered from a fault in the cleaning helper: stage 1.
            struct.pack_into('<Q', raw, host.RECORD_OFFSET+24, 1)
        return bytes(raw)
    return report


def check_deadline_return(state, record, dram, originals):
    # Quarantined and finished: the runner exits instead of holding the lock;
    # the queue claim stays held because the run failed.
    assert state['quarantined'] and not state['retained'] and not record['complete']
    assert record['dram_restored'] is False and record['rom_usable'] is True
    assert record['deadline_return']['reason'] in host.REASONS
    assert record['failure'].startswith('CPU0 returned through the deadline unit'), record['failure']
    commands = state['commands']
    launch = commands.index(['exe', '0x52000'])
    after = commands[launch+1:]
    assert all(c[0] in ('version', 'read') for c in after), after
    name, address, size = host.SNAPSHOT
    assert any(c[:3] == ['read', hex(address), str(size)] for c in after)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'bundle', 'models', 'survey', 'fel', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--case', action='append')
    a = parser.parse_args()
    a.output.mkdir(parents=True, exist_ok=bool(a.case))
    manifest = json.loads((a.build/'manifest.json').read_text())
    host.load(a.build, a.bundle/'image.elf', manifest)
    quiet = manifest['workload_bisect']['quiet_background']
    evidence = {case: a.models/(case+'.json') for case in host.INSTRUCTION_CASES[quiet]}
    success = evidence['success'].with_suffix('.bin').read_bytes()
    assert host.decode(success, success[32:48], manifest)['task_workload_completed']
    stalled = (a.models/'stalled-core.bin').read_bytes() if not quiet else None
    proof = {'schema': host.SOFTWARE_SCHEMA, 'physical_access': False, 'quiet_background': quiet,
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
    # The quiet build's deadline path is the same unit; its records come from
    # the deadline build's stalled cases, which the deadline build's check runs.
    if quiet:
        stalled = (a.models.parent/'deadline-check'/'stalled-core.bin').read_bytes()
    for case in a.case or host.CASES:
        assert case in host.CASES
        if case in ('complete-return', 'transport-failure-retained', 'invalid-report-restored'):
            assert shared.run_case(a, case, fixture, first.transform(success, True))
        elif case == 'deadline-return-retained':
            assert shared.run_case(a, case, fixture, deadline_transform(stalled), final=check_deadline_return)
            record = json.loads((a.output/case/'runtime/session.json').read_text())
            assert record['deadline_return']['stage'] == 3 and not record['deadline_return']['inconsistent']
        elif case == 'damaged-deadline-retained':
            assert shared.run_case(a, case, fixture, deadline_transform(stalled, damage=True), final=check_deadline_return)
            record = json.loads((a.output/case/'runtime/session.json').read_text())
            assert record['deadline_return']['inconsistent'] == 'Deadline record is inconsistent.'
        elif case == 'deadline-quarantine-failure-held':
            # A quarantine record that cannot be written leaves the outcome
            # unsettled: the runner keeps holding the lock, as before the change.
            def held_after_failed_quarantine(state, record, dram, originals):
                assert state['quarantined'] and state['retained'] and not record['complete']
                assert record['dram_restored'] is False and record['deadline_return']['reason'] in host.REASONS
            assert shared.run_case(a, case, fixture, deadline_transform(stalled),
                                   final=held_after_failed_quarantine, quarantine_fails=True)
        elif case == 'deadline-record-checked':
            record = host.deadline_record(stalled, stalled[32:48])
            assert record and record['reason'] == 2 and not record['inconsistent']
            try:
                host.decode(stalled, stalled[32:48], manifest)
            except host.DeadlineReturn as returned:
                assert returned.record == record
            else:
                raise AssertionError('Deadline report decoded as a completed workload')
            offset = host.RECORD_OFFSET
            for field, value in ((8, 2), (16, 9), (24, 1), (24, 7), (40, 0), (200, 9279), (496, 0), (192, 1)):
                bad = bytearray(stalled); struct.pack_into('<Q', bad, offset+field, value)
                assert host.deadline_record(bytes(bad), stalled[32:48])['inconsistent'], field
                # Damaged or not, a report with the magic number is never restored over.
                try: host.decode(bytes(bad), stalled[32:48], manifest)
                except host.DeadlineReturn: pass
                else: raise AssertionError(f'Damaged deadline record decoded at {field}')
            assert host.deadline_record(stalled, bytes(16))['inconsistent'], 'Foreign nonce accepted'
            bad = bytearray(stalled); struct.pack_into('<QQ', bad, offset+16, 1, 3)
            struct.pack_into('<Q', bad, offset+152, 30)
            assert host.deadline_record(bytes(bad), stalled[32:48])['inconsistent'], 'Interrupt other than PPI 29 accepted'
        elif case == 'changed-input-refused':
            for field in ('deadline', 'quiet', 'patches', 'sources'):
                changed = json.loads((a.build/'manifest.json').read_text())
                if field == 'deadline': changed['workload_bisect']['deadline_ticks_after_startup'] += 24000
                elif field == 'quiet': changed['workload_bisect']['quiet_background'] = not quiet
                elif field == 'patches': changed['patches'][-1]['replacement'] ^= 4
                else: changed['sources'].pop('boot/a733/workload-deadline-probe.S')
                try: host.load(a.build, a.bundle/'image.elf', changed)
                except ValueError: pass
                else: raise AssertionError('Changed return-deadline input accepted: '+field)
        accepted['cases'][case] = {'pass': True, 'check_sources': {n: host.digest(ROOT/n) for n in (
            'checks/workload-bisect-host.py', 'checks/all-core-first-task-host.py',
            'checks/primary-publication-host.py')}}
        accepted_path.write_text(json.dumps(accepted, indent=2)+'\n')
        print('PASS', case, flush=True)
    assert set(accepted['cases']) == set(host.CASES)
    proof.update(case_evidence=accepted['cases'],
        limits='Actual host runner and decoder; modeled FEL, USB, queue and reports. No physical access.')
    (a.output/'software.json').write_text(json.dumps(proof, indent=2)+'\n')


if __name__ == '__main__':
    main()
