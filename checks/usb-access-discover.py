#!/usr/bin/env python3
"""FEL discovery while a board re-enumerates.

The kernel removes a USB device's sysfs directory in steps. Between reading
idVendor and devnum, an attribute read can fail with ENODEV (device torn down)
or ENOENT (directory gone). discover() must report the board as absent so the
runner's wait loop keeps polling. Any other error must still propagate.
"""
import argparse
import errno
import importlib.util
import pathlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('discover_access', ROOT/'tools/usb-access.py')
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument('--output', type=Path, required=True, help='new directory for the fixture sysfs tree')
scratch = parser.parse_args().output
scratch.mkdir()
if True:
    sys_usb = scratch/'sys'
    device = sys_usb/'3-1'
    device.mkdir(parents=True)
    for name, value in (('idVendor', '1f3a'), ('idProduct', 'efe8'), ('busnum', '3'), ('devnum', '119')):
        (device/name).write_text(value + '\n')
    access.SYS_USB = sys_usb
    access.DEV_USB = scratch/'dev'
    access.board_lock_path = lambda port: None

    assert access.discover('3-1') == access.DEV_USB/'003/119'

    original = pathlib.Path.read_text
    for code in (errno.ENODEV, errno.ENOENT, errno.EIO):
        def failing(self, *args, code=code, **kwargs):
            if self.name == 'devnum':
                raise OSError(code, 'injected')
            return original(self, *args, **kwargs)
        pathlib.Path.read_text = failing
        try:
            access.discover('3-1')
        except ValueError as error:
            assert code in (errno.ENODEV, errno.ENOENT), f'errno {code} was treated as absence'
            assert 'found 0' in str(error)
        except OSError as error:
            assert code == errno.EIO and error.errno == errno.EIO, error
        else:
            raise AssertionError(f'errno {code} produced a device')
        finally:
            pathlib.Path.read_text = original

    assert access.discover('3-1') == access.DEV_USB/'003/119'
print('PASS usb-access-discover: ENODEV and ENOENT during re-enumeration read as absent; other errors propagate')
