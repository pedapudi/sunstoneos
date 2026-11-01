#!/usr/bin/env python3
"""Report the selected evidence child before any board access."""
import argparse
from contextlib import contextmanager, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import patch

import harness

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class BeforeBoard(BaseException):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True)
    results = []
    for family in ('smp', 'device-launch', 'storage-launch', 'smp-observe', 'smp-supervised', 'returning'):
        work = args.output / family
        work.mkdir()
        evidence = work / 'evidence'
        evidence.mkdir()
        (evidence / 'earlier.log').write_text('preserve earlier evidence\n')
        bundle = work / 'bundle'
        bundle.mkdir()
        (bundle / 'prepared.json').write_text('{}\n')
        (bundle / 'configuration.json').write_text('{}\n')
        fel = work / 'never-executed-tool'
        fel.write_bytes(b'host fixture')
        @contextmanager
        def board_lock():
            raise BeforeBoard()
            yield  # The sentinel prevents even a simulated device instance.
        access = SimpleNamespace(board_lock=board_lock)
        guard = SimpleNamespace(check_health=lambda: None)
        path = 'tools/a733-returning-host.py' if family == 'returning' else 'tools/check-a733-' + family + '.py'
        host = module('evidence_report_' + family.replace('-', '_'), path)
        base = host if family == 'smp' else getattr(host, 'base', None)
        owner = base if family == 'storage-launch' else host
        original_module = owner.module
        def load(name, path):
            return access if Path(path).name == 'usb-access.py' else original_module(name, path)
        output = io.StringIO()
        try:
            with redirect_stdout(output), patch.dict(sys.modules, {'usb_guard': guard}), \
                    patch.object(owner, 'module', side_effect=load):
                if family == 'returning':
                    base = module('evidence_returning_base', 'tools/check-a733-smp.py')
                    settings = SimpleNamespace(bundle=bundle, evidence=evidence, fel=fel)
                    host.run(settings, base=base, profile='fixture',
                        validate_bundle=lambda _: {'image_sha256': '00' * 32}, admit=lambda *a: None,
                        execute=lambda *a: None, description='host fixture', access=access, guard=guard)
                elif family == 'smp-supervised':
                    host.run_session(SimpleNamespace(evidence=evidence), bundle, {}, lambda: None,
                        lambda *a: None, lambda *a: None, product='fixture', serial='fixture',
                        bcd_device=0x100, capture_seconds=1, result_key='scheduler_result')
                elif family == 'storage-launch':
                    @contextmanager
                    def store(*a): yield object()
                    with patch.object(host.store_api, 'ProtectedDirectory', side_effect=store), \
                            patch.object(host, 'validate_bundle', return_value=({}, {})):
                        host.main(['launch', '--bundle', str(bundle), '--storage', str(work / 'storage'),
                                   '--fel', str(fel), '--evidence', str(evidence), '--confirm-bridge-stable'])
                else:
                    with patch.object(host, 'validate_bundle', return_value={}):
                        host.main(['launch', '--bundle', str(bundle), '--fel', str(fel),
                                   '--evidence', str(evidence), '--confirm-bridge-stable'])
        except BeforeBoard:
            pass
        children = [p for p in evidence.iterdir() if p.is_dir()]
        assert len(children) == 1
        assert (evidence / 'earlier.log').read_text() == 'preserve earlier evidence\n'
        results.append(dict(family=family, passed=str(children[0]) in output.getvalue()))
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    assert all(row['passed'] for row in results), results
    print('PASS: six launchers report their actual evidence directory before board access.')


if __name__ == '__main__':
    main()
