#!/usr/bin/env python3
"""Manage temporary access to the single attached Allwinner FEL USB device."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import pwd
import stat
import subprocess
import sys


STATE_DIR = Path("/run/sunstoneos-usb-access")


def command(*args, input=None):
    return subprocess.run(args, input=input, text=True, check=True,
                          capture_output=True).stdout


def discover():
    matches = []
    for device in Path("/sys/bus/usb/devices").iterdir():
        try:
            ids = tuple((device / key).read_text().strip()
                        for key in ("idVendor", "idProduct"))
            if ids == ("1f3a", "efe8"):
                bus = int((device / "busnum").read_text())
                number = int((device / "devnum").read_text())
                matches.append(Path(f"/dev/bus/usb/{bus:03}/{number:03}"))
        except FileNotFoundError:
            continue
    if len(matches) != 1:
        raise ValueError(f"Expected one FEL device; found {len(matches)}.")
    return matches[0]


def identity(node):
    info = node.lstat()
    if not stat.S_ISCHR(info.st_mode):
        raise ValueError(f"Not a USB character device: {node}")
    return [info.st_dev, info.st_ino, info.st_rdev, info.st_uid, info.st_gid]


def acl(node):
    output = command("getfacl", "--omit-header", "--numeric", "--no-effective",
                     "--", str(node))
    return canonical_acl(output)


def canonical_acl(value):
    return "\n".join(sorted(line for line in value.splitlines() if line)) + "\n"


def check_saved(saved, node, uid, current):
    if saved["identity"] != identity(node) or saved["uid"] != uid:
        raise ValueError("Backup belongs to another device instance or user.")
    if current not in (saved["installed"], saved["original"]):
        raise ValueError("ACL changed after grant; refusing to overwrite it.")


def modify(action, node, uid, backup):
    current = acl(node)
    if backup.exists():
        saved = json.loads(backup.read_text())
        check_saved(saved, node, uid, current)
        if action == "grant":
            if current == saved["original"]:
                command("setfacl", "--no-mask", "--set-file=-", "--", str(node),
                        input=saved["installed"])
            print(f"Access already granted to UID {uid} on {node}.")
            return
        command("setfacl", "--no-mask", "--set-file=-", "--", str(node),
                input=saved["original"])
        if acl(node) != saved["original"]:
            raise ValueError("ACL restoration did not match the backup.")
        backup.unlink()
        print(f"Restored the original ACL on {node}.")
        return
    if action == "revoke":
        raise ValueError("No saved ACL. Use remove-user for a manual grant.")
    if any(line.startswith(f"user:{uid}:") for line in current.splitlines()):
        raise ValueError("User already has an ACL entry. Remove the manual grant "
                         "before using grant with backup.")
    # Compute and save both ACLs before mutation, so an interrupted grant is
    # recoverable. Preserve the existing ACL mask when it already permits rw.
    entries = dict(line.rsplit(":", 1) for line in current.splitlines() if line)
    mask = entries.get("mask:", entries["group:"])
    effective = "rw" + ("x" if "x" in mask else "-")
    installed_entries = [line for line in current.splitlines()
                         if line and not line.startswith("mask:")]
    installed_entries.extend([f"user:{uid}:rw-", f"mask::{effective}"])
    desired = canonical_acl("\n".join(installed_entries))
    saved = {"identity": identity(node), "uid": uid,
             "original": current, "installed": desired}
    temporary = backup.with_suffix(".tmp")
    temporary.write_text(json.dumps(saved) + "\n")
    temporary.replace(backup)
    command("setfacl", "--no-mask", "--set-file=-", "--", str(node), input=desired)
    if acl(node) != desired:
        raise ValueError("Granted ACL did not match the saved permissions.")
    print(f"Granted UID {uid} read/write access to {node}; original ACL saved.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("status", "grant", "revoke", "remove-user"))
    parser.add_argument("--user", default=os.environ.get("SUDO_USER") or pwd.getpwuid(os.getuid()).pw_name,
                        help="account receiving access (default: sudo caller)")
    args = parser.parse_args()
    uid = pwd.getpwnam(args.user).pw_uid
    node = discover()
    identity(node)
    if args.action == "status":
        print(f"FEL device: {node}\n{acl(node)}", end="")
        print(f"Current process has read/write access: {os.access(node, os.R_OK | os.W_OK)}")
        return
    if os.geteuid() != 0 or uid == 0:
        raise ValueError("Run with sudo and select a non-root user with --user if needed.")
    os.umask(0o077)
    STATE_DIR.mkdir(mode=0o700, exist_ok=True)
    info = STATE_DIR.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError(f"Unsafe state directory: {STATE_DIR}")
    with (STATE_DIR / "lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        backup = STATE_DIR / f"{node.parent.name}-{node.name}.json"
        if args.action == "remove-user":
            if backup.exists():
                raise ValueError("A backup exists; use revoke to restore it.")
            command("setfacl", "--no-mask", "--remove", f"user:{uid}", "--", str(node))
            print(f"Removed UID {uid}'s ACL entry from {node}; retained the ACL mask.")
        else:
            modify(args.action, node, uid, backup)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        detail = error.stderr.strip() if isinstance(error, subprocess.CalledProcessError) else str(error)
        sys.exit(f"usb-access: {detail}")
