#!/usr/bin/env python3
"""The packaged ROM-word scratch protection succeeds or stops without restoration."""
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
p = argparse.ArgumentParser(description=__doc__); p.add_argument('--bundle', type=Path, required=True); a = p.parse_args()
spec = importlib.util.spec_from_file_location('protected_transport', a.bundle / 'host/tools/returning-kernel.py')
runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
results = []
for case in ('normal', 'readl-transport-failure', 'restore-mismatch'):
    with tempfile.TemporaryDirectory() as tmp:
        evidence = Path(tmp); initial = bytes(range(128)); scratch = bytearray(initial); calls = []; record = {}
        def transaction(*args):
            calls.append(args[0])
            if args[0] == 'read':
                assert args[1:3] == ('0x71000', 128); Path(args[3]).write_bytes(scratch)
            elif args[0] == 'readl':
                assert len(args) == 10
                scratch[:] = bytes(128)
                if case == 'readl-transport-failure':
                    record['transport_failed'] = True
                    raise RuntimeError('transport failed')
                return 'eb00008f e3a00911 e3a0193d e3a0605c ea00004d'
            elif args[0] == 'write':
                assert args[1] == '0x71000' and args[3:6] == ('read', '0x71000', 128)
                scratch[:] = Path(args[2]).read_bytes()
                Path(args[6]).write_bytes(bytes(128) if case == 'restore-mismatch' else scratch)
            else: raise AssertionError(args)
        try:
            words = runner.permission_rom_words(transaction, evidence, record, lambda: None)
        except (RuntimeError, ValueError):
            assert case != 'normal' and record['scratch_dirty'] and not record['scratch_restored']
            if case == 'readl-transport-failure': assert calls == ['read', 'readl']
        else:
            assert case == 'normal' and scratch == initial and record['scratch_restored'] and not record['scratch_dirty']
            assert calls == ['read', 'readl', 'write'] and len(words.split()) == 5
        if record['scratch_dirty']:
            retained = []
            class Helper:
                def stop_after_failure(self, rec, save, protect, error, hold):
                    retained.append('stop'); protect(); hold()
                def hold_ownership(self): retained.append('hold')
            runner.retain_failure(record, lambda: None, lambda: retained.append('quarantine'), RuntimeError('scratch uncertain'), Helper())
            assert 'stop' in retained and 'quarantine' in retained and 'hold' in retained
        results.append({'case': case, 'requests': calls, 'scratch_restored': record['scratch_restored']})
(a.bundle / 'scratch-check.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'cases': results}, indent=2) + '\n')
print('3 packaged scratch cases passed')
