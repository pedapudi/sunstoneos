#!/usr/bin/env python3
"""Check metadata disappearance without opening or resetting a USB device."""
import errno
import importlib.util
import json
from pathlib import Path
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "access", Path(__file__).resolve().parents[1] / "tools/usb-access.py")
access = importlib.util.module_from_spec(spec)
spec.loader.exec_module(access)

with tempfile.TemporaryDirectory(prefix="sunstoneos-disconnect-") as directory:
    root = Path(directory)
    device = root / "3-1"
    device.mkdir()
    config = {"port": "3-1", "sysfs": str(device.resolve())}
    with patch.object(access, "SYS_USB", root):
        for number in (errno.ENOENT, errno.ENODEV, errno.EACCES, errno.EIO):
            with patch.object(Path, "read_text", side_effect=OSError(number, "injected")):
                if number in (errno.ENOENT, errno.ENODEV):
                    assert access.session_node(config) is None
                else:
                    try:
                        access.session_node(config)
                    except OSError as error:
                        assert error.errno == number
                    else:
                        raise AssertionError("Unrelated I/O error was suppressed")
        for key, value in {"idVendor": "1f3a", "idProduct": "efe8",
                           "busnum": "3", "devnum": "60"}.items():
            (device / key).write_text(value)
        with patch.object(access, "identity", return_value=[1, 2, 3, 0, 0]):
            assert access.session_node(config) == access.DEV_USB / "003/060"
            (device / "idProduct").write_text("0000")
            assert access.session_node(config) is None
    # Cached ROM descriptors remain visible after a failed CPU handoff. A stale
    # instance must be refused without reading descriptors or opening USB.
    with patch.object(access, "RECOVERY_REQUIRED", root / "blocked.json"), \
         patch.object(access, "identity", return_value=[1, 2, 3, 0, 0]), \
         patch.object(access, "target", return_value=device) as target:
        access.quarantine_instance(root / "node", "fixture")
        try:
            access.require_rom(root / "node")
        except ValueError as error:
            assert "failed to recover" in str(error)
        else:
            raise AssertionError("Stale ROM instance accepted")
        target.assert_not_called()
        (device / "bcdDevice").write_text("02b3")
        with patch.object(access, "identity", return_value=[1, 4, 5, 0, 0]):
            access.require_rom(root / "node")
        assert json.loads(access.RECOVERY_REQUIRED.read_text())["reason"] == "fixture"
print("PASS: disconnect absence, new ROM instance, unrelated device, and fatal metadata errors.")
