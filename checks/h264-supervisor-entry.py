#!/usr/bin/env python3
"""Check H.264 capture policy through the real supervisor, stopping before access."""
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
    args = parser.parse_args()
    source = ROOT / 'tools/check-a733-h264-supervised.py'
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
            ('actual-H264-caller', policy['capture_seconds'], policy['result_key'], True),
            ('overlong-H264-capture', 60, policy['result_key'], False),
            ('unknown-result-key', policy['capture_seconds'], 'unknown_result', False)):
        reached.clear()
        try:
            host.supervisor.run_session(settings, unused, {}, ready, forbidden, forbidden,
                product=host.PRODUCT, serial=host.SERIAL, bcd_device=0x0100,
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
        'fel_sha256': host.supervisor.digest(args.fel), 'caller_policy': policy,
        'cases': cases, 'scope': 'Actual capture-policy and FEL-identity checks. '
        'Readiness callback stops before queue checks, evidence-directory creation '
        'or USB access; no transport, native execution or physical qualification.'}, indent=2) + '\n')
    print('PASS actual H264 supervisor entry and capture-bound refusals')


if __name__ == '__main__':
    main()
