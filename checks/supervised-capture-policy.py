#!/usr/bin/env python3
"""Check supervised launch delegation without entering runner admission or USB."""
import importlib.util
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import tempfile
from unittest.mock import patch

if not __debug__:
    raise SystemExit('checks require assertions')
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('capture_policy', ROOT / 'tools/check-a733-smp-supervised.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
sys.path.insert(0, str(ROOT / 'tools'))
import board


def forbidden(*args, **kwargs):
    raise AssertionError('supervised adapter reached a legacy session callback')


root = Path(tempfile.mkdtemp(prefix='sunstone-capture-policy-'))
bundle = root / 'bundle'
args = SimpleNamespace(fel=root / 'model-fel', evidence=root / 'must-not-exist',
                       bundle=bundle)
# The runner owns image admission, watchdog bounds and recovery. Exercise the
# real legacy adapter and stop at its CLI boundary, before any device access.
with patch.dict(os.environ, {'SUNSTONE_USB_PORT': '3-3'}), \
        patch.object(board, 'main', return_value=23) as runner:
    try:
        policy.run_session(args, bundle, {}, forbidden, forbidden, forbidden,
            product='host fixture', serial='host fixture', bcd_device=0x100,
            capture_seconds=8, result_key='application_result', capture_limit_seconds=8)
    except SystemExit as error:
        assert error.code == 23, error.code
    else:
        raise AssertionError('supervised adapter did not propagate the runner exit code')
    runner.assert_called_once_with([
        'run', str(bundle / 'image.elf'), '--kind', 'probe', '--port', '3-3',
        '--fel', str(args.fel), '--evidence', str(args.evidence),
        '--template', str(bundle)])
assert not args.evidence.exists()
print('PASS supervised launch delegates image, port, inputs and exit status; no device access')
