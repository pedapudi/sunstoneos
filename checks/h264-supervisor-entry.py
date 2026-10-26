#!/usr/bin/env python3
"""Check a capture caller through the real supervisor, stopping before access."""
import argparse
import ast
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--caller', type=Path,
                        default=Path('tools/check-a733-h264-supervised.py'))
    parser.add_argument('--case', action='append', choices=('actual', 'overlong', 'unknown'))
    args = parser.parse_args()
    source = ROOT / args.caller
    spec = importlib.util.spec_from_file_location('h264_supervisor_entry', source)
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    # Obtain the policy arguments from the real caller, not a duplicated profile.
    calls = [node for node in ast.walk(ast.parse(source.read_text()))
             if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
             and isinstance(node.func.value, ast.Name)
             and node.func.value.id == 'supervisor' and node.func.attr == 'run_session']
    assert len(calls) == 1
    policy = {item.arg: ast.literal_eval(item.value) for item in calls[0].keywords
              if item.arg in ('capture_seconds', 'result_key')}
    def identity_constant(node):
        if isinstance(node, ast.Name):
            return getattr(host, node.id)
        if isinstance(node, ast.Attribute):
            return getattr(identity_constant(node.value), node.attr)
        return ast.literal_eval(node)
    identity = {item.arg: identity_constant(item.value) for item in calls[0].keywords
                if item.arg in ('product', 'serial', 'bcd_device')}
    reached = []
    class ReadinessReached(Exception):
        pass
    def ready():
        reached.append(True)
        raise ReadinessReached
    def forbidden(*args, **kwargs):
        raise AssertionError('Entry check reached a device or queue callback')
    unused = args.output.with_suffix('.must-not-exist')
    assert not unused.exists()
    settings = SimpleNamespace(fel=args.fel, evidence=unused)
    cases = []
    for name, seconds, key, accepted in (
            ('actual', policy['capture_seconds'], policy['result_key'], True),
            ('overlong', 180 if policy['result_key'] == 'mimus_result' else 60,
             policy['result_key'], False),
            ('unknown', policy['capture_seconds'], 'unknown_result', False)):
        if args.case and name not in args.case:
            continue
        reached.clear()
        try:
            host.supervisor.run_session(settings, unused, {}, ready, forbidden, forbidden,
                **identity,
                capture_seconds=seconds, result_key=key)
        except ReadinessReached:
            assert accepted and reached == [True]
        except (ValueError, RuntimeError) as error:
            assert not accepted and not reached
            assert str(error) == 'Unsupported supervised capture bounds.'
        else:
            raise AssertionError('Supervisor did not stop at the specified boundary')
        assert not unused.exists()
        cases.append({'case': name, 'accepted_to_readiness': accepted})
    args.output.write_text(json.dumps({'passed': True, 'physical_access': False,
        'fel_sha256': host.supervisor.digest(args.fel), 'caller': str(source), 'caller_policy': policy,
        'cases': cases, 'scope': 'Actual capture-policy and FEL-identity checks. '
        'Readiness callback stops before queue checks, evidence-directory creation '
        'or USB access; no transport, native execution or physical qualification.'}, indent=2) + '\n')
    print('PASS supervisor entry:', source, [case['case'] for case in cases])


if __name__ == '__main__':
    main()
