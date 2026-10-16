#!/usr/bin/env python3
"""Exercise reconnect grants and undo using real ACLs on temporary files."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "access", Path(__file__).resolve().parents[1] / "tools/usb-access.py")
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)
real_command = a.command


def command(*args, **kwargs):
    if args[:2] == ("udevadm", "control"):
        return ""
    return real_command(*args, **kwargs)


def fixture_identity(node):
    st = node.lstat()
    return [st.st_dev, st.st_ino, st.st_rdev, st.st_uid, st.st_gid]


with tempfile.TemporaryDirectory(prefix="sunstoneos-acl-") as directory:
    root = Path(directory)
    a.STATE_DIR = root / "state"
    a.STATE_DIR.mkdir(mode=0o700)
    a.RULE = root / "rules/90-sunstoneos-fel.rules"
    a.SYS_USB = root / "sys"
    a.DEV_USB = root / "dev"
    device = a.SYS_USB / "3-1"
    device.mkdir(parents=True)
    for key, value in dict(busnum="3", devnum="30", idVendor="1f3a", idProduct="efe8").items():
        (device / key).write_text(value)
    node = a.DEV_USB / "003/030"
    node.parent.mkdir(parents=True)
    node.touch(mode=0o640)
    uid = os.getuid()
    with patch.object(a, "command", command), patch.object(a, "identity", fixture_identity):
        original = a.acl(node)
        # Adopt the prior one-node grant without losing its original ACL.
        a.modify("grant", node, uid, a.STATE_DIR / "003-030.json")
        a.enable_session(node, uid)
        a.enable_session(node, uid)
        subprocess.run(["udevadm", "verify", str(a.RULE)], check=True)
        a.disable_session()
        assert a.acl(node) == original
        assert not a.RULE.exists()
        # A fresh session receives a reconnect on a new device number.
        a.enable_session(node, uid)
        node.unlink()
        node = node.with_name("031")
        node.touch(mode=0o600)
        original = a.acl(node)
        (device / "devnum").write_text("31")
        with patch.dict(os.environ, DEVNAME=str(node)):
            a.session_event()
            a.session_event()
        assert f"user:{uid}:rw-" in a.acl(node)
        before = a.acl(node)
        with patch.dict(os.environ, DEVNAME="/dev/bus/usb/003/099"):
            try:
                a.session_event()
            except ValueError:
                pass
            else:
                raise AssertionError("Unrelated USB event accepted")
        assert a.acl(node) == before
        # Refuse to undo a later permission change; allow recovery once repaired.
        real_command("setfacl", "-m", "other::r--", str(node))
        try:
            a.disable_session()
        except ValueError:
            pass
        else:
            raise AssertionError("Changed ACL overwritten")
        assert not a.RULE.exists()
        saved = json.loads(a.session_backup(node).read_text())
        real_command("setfacl", "--no-mask", "--set-file=-", str(node), input=saved["installed"])
        a.disable_session()
        assert a.acl(node) == original
        a.disable_session()
        # Removing the session while unplugged needs no connected device.
        a.enable_session(node, uid)
        node.unlink()
        for entry in device.iterdir():
            entry.unlink()
        device.rmdir()
        a.disable_session()
        assert not a.RULE.exists()
print("PASS: grant adoption, reconnect, event isolation, ACL conflict refusal, and undo.")
