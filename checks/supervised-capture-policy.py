#!/usr/bin/env python3
"""Check declared application capture limits before any device or queue access."""
import hashlib
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
from unittest.mock import patch

if not __debug__:
    raise SystemExit('checks require assertions')
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('capture_policy', ROOT / 'tools/check-a733-smp-supervised.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)

class Ready(Exception):
    pass

def ready():
    raise Ready

def forbidden(*args, **kwargs):
    raise AssertionError('capture policy reached a device or queue callback')

cases = 0
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    executable = root / 'model-fel'
    executable.write_bytes(b'host-only executable identity fixture')
    args = SimpleNamespace(fel=executable, evidence=root / 'must-not-exist')
    with patch.object(policy.base, 'FEL_HASH', hashlib.sha256(executable.read_bytes()).hexdigest()):
        for key, seconds, limit, accepted in [
                ('application_result', 3, 3, True),
                ('application_result', 3.1, 3, False),
                ('application_result', 200, 200, True),
                ('application_board_result', 8, 8, True),
                ('application_board_result', 9, 8, False),
                ('application_result', 1, None, False),
                ('application_result', 1, True, False),
                ('application_result', 1, float('inf'), False),
                ('application_result', 1, float('nan'), False),
                ('application_result', 1, 0, False),
                ('board_result', 59, None, True),
                ('board_result', 60, None, False),
                ('board_result', 60, 200, False)]:
            try:
                policy.run_session(args, root, {}, ready, forbidden, forbidden,
                    product='host fixture', serial='host fixture', bcd_device=0x100,
                    capture_seconds=seconds, result_key=key, capture_limit_seconds=limit)
            except Ready:
                assert accepted, (key, seconds, limit)
            except ValueError as error:
                assert not accepted and str(error) == 'Unsupported supervised capture bounds.'
            else:
                raise AssertionError('capture policy passed the readiness boundary')
            assert not args.evidence.exists()
            cases += 1
print(f'PASS declared capture policy: {cases} cases; no device or queue access')
