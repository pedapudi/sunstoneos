#!/usr/bin/env python3
"""Distinguish restored DRAM mismatches from uncertain probe failures."""
import argparse
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import harness

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True)
    results = []
    for family in ('smp', 'device-launch', 'storage-launch', 'smp-observe', 'smp-supervised'):
        host = module('dram_refusal_' + family.replace('-', '_'), 'tools/check-a733-' + family + '.py')
        base = host if family == 'smp' else host.base
        for failure in ('comparison', 'restore', 'test-transport', 'restore-transport', 'short-test',
                        'backup-transport', 'backup-short', 'restored-journal'):
            output = args.output / (family + '-' + failure)
            output.mkdir()
            record = {'launch_attempted': False, 'transport_failed': False}
            original = b'\0' * 8
            memory = original
            writes = 0
            events = []
            def transaction(*arguments, pass_fds=()):
                nonlocal memory, writes
                events.append(arguments[0])
                at = 0
                while at < len(arguments):
                    operation = arguments[at]
                    if operation == 'write':
                        memory = Path(arguments[at + 2]).read_bytes()
                        writes += 1
                        at += 3
                        if failure == ('test-transport' if writes == 1 else 'restore-transport'):
                            record['transport_failed'] = True
                            raise OSError('Injected probe transport loss')
                    else:
                        assert operation == 'read'
                        if not writes and failure == 'backup-transport':
                            record['transport_failed'] = True
                            raise OSError('Injected backup transport loss')
                        data = memory
                        if not writes and failure == 'backup-short': data = memory[:-1]
                        if writes == 1 and failure == 'comparison': data = original
                        if writes == 1 and failure == 'short-test': data = memory[:-1]
                        if writes == 2 and failure == 'restore': data = bytes([1]) + memory[1:]
                        Path(arguments[at + 3]).write_bytes(data)
                        at += 4
            def save():
                if failure == 'restored-journal' and writes == 2 and not record.get('dram_probe_dirty'):
                    raise OSError('Injected restoration journal failure')
            try:
                base.verify_dram(transaction, output, record, save)
            except (ValueError, OSError) as error:
                quarantined, held = [], []
                stop = getattr(host, 'stop_after_failure', base.stop_after_failure)
                stop(record, lambda: None, lambda: quarantined.append(True), error,
                     hold=lambda: held.append(True))
                clean = failure == 'comparison'
                passed = (not quarantined and not held and writes == 2 and memory == original
                          if clean else quarantined == [True] and held == [True])
                results.append(dict(family=family, failure=failure, passed=bool(passed),
                                    quarantined=bool(quarantined), held=bool(held)))
            else:
                raise AssertionError('Probe fault was accepted: ' + failure)
    returning = module('dram_returning_transport', 'tools/a733-returning-host.py')
    base = module('dram_returning_base', 'tools/check-a733-smp.py')
    record = {'dirty': False, 'launch_attempted': False, 'transport_failed': False}
    actions = []
    returning.retain_failure(base, record, lambda: None, lambda: actions.append('quarantine'),
                             base.DramNotReady('DRAM comparison refused after restoration'),
                             hold=lambda: actions.append('hold'))
    assert not actions and record['prelaunch_refusal'] == 'dram-not-ready'
    results.append(dict(family='returning', failure='comparison', passed=True))

    cpu = module('dram_cpu_qualification', 'apps/cpu-runtime-qualification/host.py')
    def transport(settings, **callbacks):
        state = {'dirty': False, 'launch_attempted': False, 'transport_failed': False}
        try:
            callbacks['execute'](lambda *a: None, lambda: None, lambda: None, {}, state, lambda: None)
        except cpu.base.DramNotReady as error:
            assert state['dirty'] is False
            actions = []
            returning.retain_failure(cpu.base, state, lambda: None,
                lambda: actions.append('quarantine'), error, hold=lambda: actions.append('hold'))
            assert not actions
        else:
            raise AssertionError('CPU qualification did not refuse the unready DRAM.')
    survey = SimpleNamespace(capture_caller=lambda *a: {}, caller_interval=lambda _: (0x70000, 60))
    with patch.object(cpu.base, 'module', return_value=survey), \
            patch.object(cpu.base, 'verify_dram', side_effect=cpu.base.DramNotReady('Restored comparison mismatch')), \
            patch.object(cpu.returning, 'run', side_effect=transport):
        cpu.run(SimpleNamespace(evidence=args.output, bundle=args.output))
    results.append(dict(family='cpu-returning', failure='comparison', passed=True))
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    assert all(row['passed'] for row in results), [r for r in results if not r['passed']]
    print('PASS: 42 prelaunch cases release only exact restored mismatches and retain uncertain probes.')


if __name__ == '__main__':
    main()
