#!/usr/bin/env python3
"""Manage reversible access to explicitly selected Allwinner FEL USB ports."""

import argparse
from contextlib import contextmanager
import errno
import fcntl
import json
import os
from pathlib import Path
import pwd
import re
import stat
import subprocess
import sys


STATE_DIR = Path("/run/sunstoneos-usb-access")
RULE = Path("/run/udev/rules.d/90-sunstoneos-fel.rules")
SYS_USB = Path("/sys/bus/usb/devices")
DEV_USB = Path("/dev/bus/usb")
RECOVERY_REQUIRED = Path.home() / ".cache/sunstoneos/recovery-required.json"


def board_lock_path(port):
    """One A733 lock per physical USB port, independent of enumeration number."""
    if not re.fullmatch(r"[0-9]+-[0-9]+(?:\.[0-9]+)*", port):
        raise ValueError("Invalid USB physical port.")
    return Path(f"/tmp/sunstoneos-a733-{port}.lock")


@contextmanager
def board_lock(node=None):
    """Select through host metadata, lock that port, and reject a changed target."""
    node = discover() if node is None else node
    before = identity(node)
    port = target(node)
    if (port / "idVendor").read_text().strip() != "1f3a":
        raise ValueError("Target is not an Allwinner device.")
    with board_lock_path(port.name).open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if identity(node) != before or target(node).name != port.name:
            raise ValueError("USB target changed while acquiring its board lock.")
        yield node


def command(*args, input=None):
    return subprocess.run(args, input=input, text=True, check=True,
                          capture_output=True).stdout


def selected_port(port=None):
    """An explicit argument takes precedence over process-local selection."""
    return os.environ.get("SUNSTONE_USB_PORT") if port is None else port


def discover(port=None):
    port = selected_port(port)
    if port is not None:
        board_lock_path(port)  # Validate before resolving any sysfs path.
    matches = []
    for device in ([SYS_USB / port] if port is not None else SYS_USB.iterdir()):
        try:
            ids = tuple((device / key).read_text().strip()
                        for key in ("idVendor", "idProduct"))
            if ids == ("1f3a", "efe8"):
                bus = int((device / "busnum").read_text())
                number = int((device / "devnum").read_text())
                matches.append(DEV_USB / f"{bus:03}/{number:03}")
        except FileNotFoundError:
            continue
    if len(matches) != 1:
        raise ValueError(f"Expected one FEL device{' on port ' + port if port else '; select --port when multiple boards are attached'}; found {len(matches)}.")
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


def target(node):
    for device in SYS_USB.iterdir():
        try:
            if (int((device / "busnum").read_text()) == int(node.parent.name)
                    and int((device / "devnum").read_text()) == int(node.name)):
                return device
        except FileNotFoundError:
            continue
    raise ValueError("Device disappeared from sysfs.")


def write_atomic(path, content, mode=0o600):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(content)
    temporary.chmod(mode)
    temporary.replace(path)


def session_rule(port, uid):
    if not re.fullmatch(r"[0-9]+-[0-9]+(?:\.[0-9]+)*", port):
        raise ValueError("Invalid USB physical port.")
    return (f"# Session access for UID {int(uid)}\n"
            f'ACTION=="add|change", SUBSYSTEM=="usb", ENV{{DEVTYPE}}=="usb_device", '
            f'KERNEL=="{port}", ATTR{{idVendor}}=="1f3a", ATTR{{idProduct}}=="efe8", '
            f'RUN+="/usr/bin/python3 {STATE_DIR}/helper.py session-event"\n')


def session_backup(node):
    old = STATE_DIR / f"{node.parent.name}-{node.name}.json"
    if old.exists() and json.loads(old.read_text())["identity"] == identity(node):
        return old
    # Device numbers can be reused; inode identity distinguishes each instance.
    return STATE_DIR / f"node-{node.parent.name}-{node.name}-{identity(node)[1]}.json"


def session_node(config):
    # sysfs attributes can return ENODEV while an expected USB disconnect is
    # removing the directory. Treat only absence errors as an absent instance;
    # callers retain their existing deadline and other errors remain fatal.
    try:
        return _session_node(config)
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ENODEV):
            return None
        raise


def _session_node(config):
    device = SYS_USB / config["port"]
    if not device.exists():
        return None
    if str(device.resolve()) != config["sysfs"]:
        raise ValueError("USB controller path changed.")
    ids = [(device / key).read_text().strip() for key in ("idVendor", "idProduct")]
    if ids != ["1f3a", "efe8"]:
        return None
    bus = int((device / "busnum").read_text())
    number = int((device / "devnum").read_text())
    node = DEV_USB / f"{bus:03}/{number:03}"
    identity(node)
    return node


def sessions():
    path = STATE_DIR / "session.json"
    if not path.exists():
        return {}
    saved = json.loads(path.read_text())
    configs = saved["sessions"] if "sessions" in saved else {saved["port"]: saved}
    for port, config in configs.items():
        board_lock_path(port)
        if config["port"] != port:
            raise ValueError("Session key does not match its USB port.")
    return configs


def session_rules(configs):
    return "".join(session_rule(port, config["uid"])
                   for port, config in sorted(configs.items()) if config["enabled"])


def verify_rules(configs):
    expected = session_rules(configs)
    if (RULE.read_text() if RULE.exists() else "") != expected:
        raise ValueError("Session rules changed; refusing to overwrite them.")


def save_sessions(configs):
    write_atomic(STATE_DIR / "session.json", json.dumps({"sessions": configs}) + "\n")


def install_rules(configs):
    rule = session_rules(configs)
    if rule:
        RULE.parent.mkdir(parents=True, exist_ok=True)
        write_atomic(RULE, rule, 0o644)
    else:
        RULE.unlink(missing_ok=True)
    command("udevadm", "control", "--reload-rules")


def enable_session(node, uid):
    device = target(node)
    config = dict(uid=uid, port=device.name, sysfs=str(device.resolve()), enabled=True)
    configs = sessions()
    verify_rules(configs)
    previous = configs.get(device.name)
    if previous and any(previous[key] != config[key] for key in ("uid", "sysfs")):
        raise ValueError("Another user or controller owns this port's session; disable it first.")
    configs[device.name] = config
    write_atomic(STATE_DIR / "helper.py", Path(__file__).read_text())
    save_sessions(configs)
    install_rules(configs)
    modify("grant", node, uid, session_backup(node))
    print(f"Session access enabled on port {device.name}; survives FEL reconnects until host reboot.")


def disable_session(port=None):
    port = selected_port(port)
    configs = sessions()
    verify_rules(configs)
    if port is not None:
        board_lock_path(port)
    elif len(configs) == 1:
        port = next(iter(configs))
    elif configs:
        raise ValueError("Multiple sessions exist; select --port to disable one.")
    if port not in configs:
        print("No session access rule is installed for this port.")
        return
    config = configs[port]
    config["enabled"] = False
    save_sessions(configs)
    install_rules(configs)
    node = session_node(config)
    if node is not None:
        backup = session_backup(node)
        if backup.exists():
            modify("revoke", node, config["uid"], backup)
    del configs[port]
    if configs:
        save_sessions(configs)
    else:
        (STATE_DIR / "helper.py").unlink(missing_ok=True)
        (STATE_DIR / "session.json").unlink()
    print(f"Session rule removed for port {port}; saved ACL restored if its device is attached.")


def session_event():
    configs = sessions()
    name = os.environ.get("DEVNAME")
    if not configs:
        return
    if not name:
        raise ValueError("USB event has no device node.")
    node = Path(name)
    port = target(node).name
    config = configs.get(port)
    if config is None or not config["enabled"]:
        return
    if node != session_node(config):
        raise ValueError("USB event does not identify the configured FEL device.")
    modify("grant", node, config["uid"], session_backup(node))


def require_session(node, uid=None):
    uid = os.getuid() if uid is None else uid
    if not RULE.exists():
        raise ValueError("Reconnect access is not installed; run enable-session with sudo.")
    info = RULE.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0
            or stat.S_IMODE(info.st_mode) & 0o022):
        raise ValueError("Session rule is not protected from non-root writes.")
    # The root-owned state directory and ACL backups remain private. Ordinary
    # runners validate the public, protected udev binding without reading them.
    if session_rule(target(node).name, uid) not in RULE.read_text():
        raise ValueError("Session rule does not cover this user and physical USB port.")
    if not os.access(node, os.R_OK | os.W_OK):
        raise ValueError("Current FEL node is not accessible.")


def quarantine_instance(node, reason, observed_identity=None):
    """Refuse further FEL requests to an instance that failed to recover."""
    observed = observed_identity or identity(node)
    port = "instance"
    try:
        if identity(node) != observed:
            raise ValueError("USB instance changed before quarantine.")
        candidate = target(node).name
        board_lock_path(candidate)
        if identity(node) != observed:
            raise ValueError("USB instance changed during quarantine.")
        port = candidate
    except (OSError, ValueError):
        pass  # A disappeared or reused node still needs its original quarantine.
    info = {"identity": observed, "reason": reason}
    RECOVERY_REQUIRED.parent.mkdir(parents=True, exist_ok=True)
    # Include identity even on a known port: a later failure cannot replace an
    # earlier instance's protection, including when USB node numbers are reused.
    key = port + "-" + "-".join(str(int(part)) for part in observed)
    path = RECOVERY_REQUIRED.with_name(RECOVERY_REQUIRED.stem + "-" + key + ".json")
    write_atomic(path, json.dumps(info) + "\n")


def require_rom(node):
    # Retain the old single-record file and all independently failed ports.
    paths = [RECOVERY_REQUIRED] if RECOVERY_REQUIRED.exists() else []
    paths += sorted(RECOVERY_REQUIRED.parent.glob(RECOVERY_REQUIRED.stem + "-*.json"))
    observed = identity(node)
    for path in paths:
        blocked = json.loads(path.read_text())
        if observed == blocked["identity"]:
            raise ValueError("This USB instance failed to recover; its cached ROM descriptor "
                             "does not prove FEL is running. " + blocked["reason"])
    device = target(node)
    if (device / "bcdDevice").read_text().strip() != "02b3":
        raise ValueError("USB device is not the verified A733 ROM service; do not send FEL requests.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("status", "grant", "revoke", "remove-user",
                                              "enable-session", "disable-session", "session-event", "check-session"))
    parser.add_argument("--user", default=os.environ.get("SUDO_USER") or pwd.getpwuid(os.getuid()).pw_name,
                        help="account receiving access (default: sudo caller)")
    parser.add_argument("--port", help="physical USB port, for example 3-3; overrides SUNSTONE_USB_PORT; otherwise selection must be unambiguous")
    args = parser.parse_args()
    uid = pwd.getpwnam(args.user).pw_uid
    if args.action == "check-session":
        require_session(discover(args.port), uid)
        print("Reconnect rule and current USB node permissions are present.")
        return
    if args.action == "status":
        node = discover(args.port)
        identity(node)
        print(f"FEL device: {node}\n{acl(node)}", end="")
        print(f"Current process has read/write access: {os.access(node, os.R_OK | os.W_OK)}")
        print(f"Reconnect rule installed: {RULE.exists()}")
        return
    if os.geteuid() != 0 or (uid == 0 and args.action not in ("session-event", "disable-session")):
        raise ValueError("Run with sudo and select a non-root user with --user if needed.")
    os.umask(0o077)
    STATE_DIR.mkdir(mode=0o700, exist_ok=True)
    info = STATE_DIR.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError(f"Unsafe state directory: {STATE_DIR}")
    with (STATE_DIR / "lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.action == "session-event":
            session_event()
            return
        if args.action == "disable-session":
            disable_session(args.port)
            return
        node = discover(args.port)
        identity(node)
        if args.action == "enable-session":
            enable_session(node, uid)
            return
        if target(node).name in sessions():
            raise ValueError("Session access is installed; use disable-session before manual ACL changes.")
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
