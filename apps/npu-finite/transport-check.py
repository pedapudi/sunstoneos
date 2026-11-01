#!/usr/bin/env python3
"""Check returning-host compatibility and the current board runner's fake transport.

--return-window-only selects the current runner transport group, including its
bounded return and watchdog cases. --case accepts only the current groups below;
historical extracted-transport case names are unsupported. All files are kept
under --evidence, and the runner uses fake FEL/board effects without USB access.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
sys.path.insert(0, str(ROOT / 'tools'))
import harness
from host_evidence import unique_directory

CHECK_INPUTS = ('tools/a733-returning-host.py', 'tools/board.py',
                'checks/board-runner.py', 'tools/host_evidence.py')
CASES = ('profile-routing', 'retained-failure', 'board-runner-transport')
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--return-window-only', action='store_true')
p.add_argument('--case', action='append', choices=CASES)
a = p.parse_args()
if a.return_window_only and a.case and set(a.case) != {'board-runner-transport'}:
    p.error('--return-window-only selects only --case board-runner-transport')
selected = a.case or (['board-runner-transport'] if a.return_window_only else list(CASES))
if len(selected) != len(set(selected)):
    p.error('each --case may be selected only once')
a.evidence = unique_directory(a.evidence)
print('Evidence:', a.evidence, flush=True)

spec = importlib.util.spec_from_file_location('returning_transport_compat', ROOT / 'tools/a733-returning-host.py')
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)
rows = []


def routing():
    calls = []
    arguments = types.SimpleNamespace(bundle=a.evidence / 'unopened-bundle')
    def forbidden(*args, **kwargs):
        raise AssertionError('Compatibility shim executed a removed transport callback')
    fake = types.ModuleType('board')
    def legacy_run(args, kind):
        assert args is arguments
        calls.append(kind)
        return 7
    fake.legacy_run = legacy_run
    profiles = (('npu-finite-return-v1', 'npu'), ('NPU-active-cancel-return-v1', 'npu'),
                ('npu-sdk-return-v1', 'npu'), ('gpu-return-v1', 'gpu-return'),
                ('GPU-compute-return-v1', 'gpu-return'), ('cpu-return-v1', 'cpu-return'),
                ('', 'cpu-return'), ('gpu-npu-profile', 'npu'))
    with patch.dict(sys.modules, {'board': fake}):
        for profile, kind in profiles:
            before = len(calls)
            try:
                compat.run(arguments, profile=profile, validate_bundle=forbidden,
                           admit=forbidden, execute=forbidden, return_timeout=90)
            except SystemExit as error:
                assert error.code == 7
            else:
                raise AssertionError('Compatibility shim did not preserve runner exit status')
            assert calls[before:] == [kind]
        failure = ValueError('modeled runner refusal')
        def refused(args, kind):
            assert args is arguments and kind == 'npu'
            calls.append(kind)
            raise failure
        fake.legacy_run = refused
        try:
            compat.run(arguments, profile='npu-finite-return-v1')
        except ValueError as error:
            assert error is failure
        else:
            raise AssertionError('Compatibility shim swallowed runner refusal')
    return dict(name='profile-routing', passed=True, profiles=list(profiles),
                runner_calls=len(calls), callbacks_executed=0)


def retained_failure():
    results = []
    # This utility remains callable independently of the delegated launch API.
    for name, record, clean, double_hold, stop_raises in (
            ('untouched', {}, False, False, False),
            ('dirty-before-launch', {'dirty': True}, False, False, False),
            ('dram-probe-before-launch', {'dram_probe_dirty': True}, False, False, False),
            ('transport-before-launch', {'transport_failed': True}, False, False, True),
            ('launched', {'launch_attempted': True}, False, True, False),
            ('clean-DDR-refusal', {}, True, False, False)):
        events = []
        error = ValueError('modeled run failure')
        def hold():
            events.append('hold')
        def save():
            events.append('save')
        def quarantine():
            events.append('quarantine')
        def stop(r, journal, deny, why, *, hold):
            assert r is record and journal is save and deny is quarantine and why is error
            events.append('stop')
            if double_hold:
                hold();hold()
            if stop_raises:
                raise OSError('modeled stop failure')
        base = types.SimpleNamespace(hold_ownership=hold,
            clean_dram_refusal=lambda r, why: clean, stop_after_failure=stop)
        if stop_raises:
            harness.refused(lambda: compat.retain_failure(base, record, save, quarantine, error),
                            'modeled stop failure', OSError)
        else:
            compat.retain_failure(base, record, save, quarantine, error)
        assert events.count('stop') == int(name != 'untouched')
        assert events.count('hold') == int(name not in ('untouched', 'clean-DDR-refusal'))
        results.append(dict(name=name, passed=True, events=events))
    return dict(name='retained-failure', passed=True, cases=results)


def runner_transport():
    output = a.evidence / 'board-runner-transport'
    temporary = a.evidence / 'tmp'
    temporary.mkdir(exist_ok=True)
    command = [sys.executable, str(ROOT / 'checks/board-runner.py'),
               '--out', str(output), '--only', 'transport']
    result = subprocess.run(command, capture_output=True, text=True,
                            env={**harness.environment(), 'TMPDIR': str(temporary.resolve())})
    (a.evidence / 'board-runner-transport.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    cases = json.loads((output / 'results.json').read_text())
    assert cases and all(case['passed'] for case in cases)
    return dict(name='board-runner-transport', passed=True, command=command,
                returncode=result.returncode, cases=cases, physical_access=False)


for case in selected:
    rows.append({'profile-routing': routing, 'retained-failure': retained_failure,
                 'board-runner-transport': runner_transport}[case]())
inputs = {str(ROOT / name): hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
          for name in (*CHECK_INPUTS, 'apps/npu-finite/transport-check.py')}
(a.evidence / 'transport-result.json').write_text(json.dumps(dict(passed=True,
    physical_access=False, selected=selected, return_window_only=a.return_window_only,
    cases=rows, source_inputs=inputs,
    scope='Current returning-host shim and existing board-runner fake transport; no retired extracted-transport variants executed.'), indent=2) + '\n')
print('PASS current transport groups:', ', '.join(selected))
