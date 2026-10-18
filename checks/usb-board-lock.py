#!/usr/bin/env python3
"""Check A733 port lock isolation using host-only fixtures and real flock."""
from contextlib import ExitStack
import importlib.util
from pathlib import Path
import tempfile
from unittest.mock import patch

source = Path(__file__).resolve().parents[1] / 'tools/usb-access.py'
spec = importlib.util.spec_from_file_location('board_access', source)
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)

with tempfile.TemporaryDirectory() as temporary, ExitStack() as patches:
    root = Path(temporary)
    for port in ('3-1', '3-2'):
        (root / port).mkdir()
        (root / port / 'idVendor').write_text('1f3a')
    selected = root / '3-1'
    node = Path('/fixture/003/065')
    lock_path = access.board_lock_path
    patches.enter_context(patch.object(access, 'board_lock_path',
                                      lambda port: root / lock_path(port).name))
    patches.enter_context(patch.object(access, 'discover', lambda: node))
    patches.enter_context(patch.object(access, 'target', lambda _: selected))
    patches.enter_context(patch.object(access, 'identity', lambda _: [1]))
    with access.board_lock() as admitted:
        assert admitted == node
        # A new USB enumeration at the same physical port still contends.
        try:
            with access.board_lock(Path('/fixture/003/066')):
                raise AssertionError('Same port admitted twice')
        except BlockingIOError:
            pass
        selected = root / '3-2'
        with access.board_lock(Path('/fixture/003/067')):
            pass
    selected = root / '3-1'
    with access.board_lock():
        pass
    with patch.object(access, 'identity', side_effect=[[1], [2]]):
        try:
            with access.board_lock():
                raise AssertionError('Changed node admitted')
        except ValueError as error:
            assert 'changed' in str(error)
    with access.board_lock():
        pass
    try:
        lock_path('../unrelated')
        raise AssertionError('Invalid port accepted')
    except ValueError:
        pass
print('PASS: same-port exclusion across reconnects, different-port independence, '
      'release, changed-node refusal and invalid-port refusal; no device access.')
