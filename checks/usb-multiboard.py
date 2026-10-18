#!/usr/bin/env python3
"""Check explicit two-board selection, independent session undo and quarantine offline."""
from contextlib import ExitStack, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

source = Path(__file__).resolve().parents[1] / 'tools/usb-access.py'
spec = importlib.util.spec_from_file_location('usb_access', source)
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)


def refuses(function, *args):
    try:
        function(*args)
    except ValueError:
        return
    raise AssertionError('Expected refusal: ' + function.__name__)


with tempfile.TemporaryDirectory(prefix='sunstoneos-usb-multiboard-') as temporary, ExitStack() as mocks:
    root = Path(temporary)
    a.STATE_DIR = root / 'state'
    a.STATE_DIR.mkdir(mode=0o700)
    a.SYS_USB = root / 'sys'
    a.DEV_USB = root / 'dev'
    a.RULE = root / 'rules/90-sunstoneos-fel.rules'
    a.RECOVERY_REQUIRED = root / 'cache/recovery-required.json'
    a.RECOVERY_REQUIRED.parent.mkdir()
    nodes = {}
    identities = {}
    acls = {}
    operations = []
    original = a.canonical_acl('user::rw-\ngroup::r--\nother::---\n')
    for port, number in [('3-1', 86), ('3-3', 88)]:
        device = a.SYS_USB / port
        device.mkdir(parents=True)
        for key, value in dict(busnum='3', devnum=str(number), idVendor='1f3a', idProduct='efe8', bcdDevice='02b3').items():
            (device / key).write_text(value)
        node = a.DEV_USB / f'003/{number:03}'
        node.parent.mkdir(parents=True, exist_ok=True)
        node.touch()
        nodes[port] = node
        identities[node] = [1, number, number, 0, 0]
        acls[node] = original
    def command(*args, input=None):
        operations.append(args)
        if args[0] == 'getfacl':
            return acls[Path(args[-1])]
        if args[0] == 'setfacl':
            assert args[1:3] == ('--no-mask', '--set-file=-')
            acls[Path(args[-1])] = input
            return ''
        assert args == ('udevadm', 'control', '--reload-rules')
        return ''
    mocks.enter_context(patch.object(a, 'command', command))
    mocks.enter_context(patch.object(a, 'identity', lambda node: identities[node]))
    mocks.enter_context(patch.dict(os.environ, {}, clear=True))
    mocks.enter_context(redirect_stdout(io.StringIO()))
    uid = 1000

    # Existing unselected callers refuse ambiguity; process selection is exact.
    refuses(a.discover)
    assert a.discover('3-1') == nodes['3-1']
    with patch.dict(os.environ, SUNSTONE_USB_PORT='3-3'):
        assert a.discover() == nodes['3-3']
        assert a.discover('3-1') == nodes['3-1']
    for port in ('3-2', '../3-1', ''):
        refuses(a.discover, port)
    with patch.dict(os.environ, SUNSTONE_USB_PORT='3-2'):
        refuses(a.discover)
    device = a.SYS_USB / '3-3'
    (device / 'idProduct').write_text('0000')
    refuses(a.discover, '3-3')
    (device / 'idProduct').write_text('efe8')

    # Adopt the exact prior session format, rule and saved one-node ACL.
    first = nodes['3-1']
    a.modify('grant', first, uid, a.STATE_DIR / '003-086.json')
    legacy = dict(uid=uid, port='3-1', sysfs=str((a.SYS_USB / '3-1').resolve()), enabled=True)
    (a.STATE_DIR / 'session.json').write_text(json.dumps(legacy))
    a.RULE.parent.mkdir()
    first_rule = a.session_rule('3-1', uid)
    a.RULE.write_text(first_rule)
    first_acl = acls[first]
    a.enable_session(nodes['3-3'], uid)
    assert acls[first] == first_acl and a.RULE.read_text().startswith(first_rule)
    assert set(a.sessions()) == {'3-1', '3-3'}
    a.enable_session(nodes['3-3'], uid)
    real_lstat = Path.lstat
    def root_rule_info(path, *args, **kwargs):
        info = real_lstat(path, *args, **kwargs)
        return SimpleNamespace(st_mode=info.st_mode, st_uid=0) if path == a.RULE else info
    with patch.object(Path, 'lstat', root_rule_info), patch.object(a, 'sessions', side_effect=PermissionError('private root state')):
        a.require_session(first, uid)
        a.require_session(nodes['3-3'], uid)
        refuses(a.require_session, first, uid + 1)
    refuses(a.disable_session)
    assert acls[first] == first_acl

    # Reconnect only the second board; its event must not touch the first ACL.
    old = nodes['3-3']
    second = old.with_name('089')
    second.touch()
    identities[second] = [1, 89, 89, 0, 0]
    acls[second] = original
    (device / 'devnum').write_text('89')
    before = len(operations)
    with patch.dict(os.environ, DEVNAME=str(second)):
        a.session_event()
        a.session_event()
    assert all(str(first) not in operation for operation in operations[before:])
    assert f'user:{uid}:rw-' in acls[second] and acls[first] == first_acl
    with patch.dict(os.environ, DEVNAME=str(second.with_name('099'))):
        refuses(a.session_event)

    # A conflicting second ACL blocks only that undo; the first rule survives.
    saved_acl = acls[second]
    acls[second] += 'conflict\n'
    refuses(a.disable_session, '3-3')
    assert a.RULE.read_text() == first_rule and acls[first] == first_acl
    assert (a.STATE_DIR / 'helper.py').exists()
    acls[second] = saved_acl
    with patch.dict(os.environ, SUNSTONE_USB_PORT='3-3'):
        a.disable_session()
    assert acls[second] == original and acls[first] == first_acl
    assert set(a.sessions()) == {'3-1'} and a.RULE.read_text() == first_rule
    # Legacy no-selector undo still works once one session remains.
    a.disable_session()
    assert acls[first] == original and not a.RULE.exists()
    assert not (a.STATE_DIR / 'helper.py').exists()

    # Independent failed ports and the legacy failed-instance record all persist.
    legacy_block = {'identity': [1, 80, 80, 0, 0], 'reason': 'old failed board'}
    a.RECOVERY_REQUIRED.write_text(json.dumps(legacy_block))
    legacy_bytes = a.RECOVERY_REQUIRED.read_bytes()
    a.quarantine_instance(first, 'first failed')
    a.quarantine_instance(second, 'second failed')
    assert a.RECOVERY_REQUIRED.read_bytes() == legacy_bytes
    refuses(a.require_rom, first)
    refuses(a.require_rom, second)
    with patch.object(a, 'identity', return_value=legacy_block['identity']):
        refuses(a.require_rom, first)
    # Disappearance cannot prevent recording or overwrite another board.
    with patch.object(a, 'target', side_effect=ValueError('disappeared')):
        a.quarantine_instance(second, 'missing failed device', [1, 90, 90, 0, 0])
    with patch.object(a, 'identity', return_value=[1, 90, 90, 0, 0]):
        refuses(a.require_rom, second)
    # A late failure for an old node must not replace a different failed port.
    first_failed = identities[first][:]
    second_failed = identities[second][:]
    identities[first] = second_failed
    with patch.object(a, 'target', return_value=a.SYS_USB / '3-3'):
        a.quarantine_instance(first, 'late first failure after node reuse', first_failed)
    with patch.object(a, 'identity', return_value=first_failed):
        refuses(a.require_rom, first)
    with patch.object(a, 'identity', return_value=second_failed):
        refuses(a.require_rom, second)
    identities[first] = [1, 91, 91, 0, 0]
    a.require_rom(first)  # A genuinely different instance is not the failed one.

print('PASS: exact port/environment selection, legacy session adoption, independent '
      'reconnect and undo, ACL conflict isolation, and persistent per-board quarantine; '
      'only temporary files and mocked ACL commands.')
