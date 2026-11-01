#!/usr/bin/env python3
"""Check shared USB budget paths and environment compatibility without devices."""
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import usb_guard


class CompatibilityTest(unittest.TestCase):
  def test_shared_paths(self):
    self.assertEqual(usb_guard.STATE, "/tmp/mimus-usb-churn.json")
    self.assertEqual(usb_guard.STATE_LOCK, "/tmp/mimus-usb-churn.lock")

  def test_settings(self):
    for suffix, value, replacement, index in (
        ("SPACING_S", "21.5", "32.5", 0),
        ("BOARD_CAP", "17", "23", 1),
        ("BOARD_WINDOW_S", "450.5", "850.5", 2)):
      old, new = "MIMUS_USB_" + suffix, "SUNSTONE_USB_" + suffix
      for environment, expected in (({old: value}, value),
                                    ({new: replacement}, replacement),
                                    ({old: value, new: replacement}, replacement)):
        with self.subTest(environment=environment), mock.patch.dict(os.environ, environment, clear=True):
          self.assertEqual(usb_guard.settings()[index], float(expected))
    with mock.patch.dict(os.environ, {}, clear=True):
      self.assertEqual(usb_guard.settings(), (15.0, 12, 600.0))

  def test_health_settings(self):
    failed = "xhci_hcd 0000:80:14.0: HC died"
    for environment, warns in (({}, False),
                              ({"MIMUS_USB_HEALTH": "warn"}, True),
                              ({"SUNSTONE_USB_HEALTH": "warn"}, True),
                              ({"MIMUS_USB_HEALTH": "warn", "SUNSTONE_USB_HEALTH": "refuse"}, False),
                              ({"MIMUS_USB_HEALTH": "refuse", "SUNSTONE_USB_HEALTH": "warn"}, True)):
      with self.subTest(environment=environment), mock.patch.dict(os.environ, environment, clear=True), \
           mock.patch.object(usb_guard, "_health_checked", 0), \
           mock.patch.object(usb_guard.time, "monotonic", return_value=100):
        messages = []
        if warns:
          usb_guard.check_health(log=messages.append, read_log=lambda: failed)
          self.assertTrue(any("continuing" in m for m in messages))
        else:
          with self.assertRaises(SystemExit):
            usb_guard.check_health(log=messages.append, read_log=lambda: failed)


if __name__ == '__main__':
  unittest.main()
