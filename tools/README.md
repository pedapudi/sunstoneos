# USB recovery access

`usb-access.py` grants a user read/write access to the single attached Allwinner
FEL recovery device (`1f3a:efe8`). It discovers the current USB address each time
and refuses to select among multiple matching devices. Python 3 and the host
`getfacl` and `setfacl` commands are required.

## Access across board resets

For autonomous bring-up, install a session rule from this worktree:

```sh
sudo python3 tools/usb-access.py enable-session
python3 tools/usb-access.py check-session
```

The rule grants the selected user access whenever the FEL device reconnects on
its current physical USB port. It matches both the port and USB ID, and checks
the controller path before changing permissions. Keep the board on that port.
FEL has no verified unique serial number; another FEL device plugged into the
same port would also match. Other USB device IDs require separate configuration.

The root-owned helper, ACL backups, and rule reside under `/run`. They survive
board resets and disappear on host reboot. Repository edits cannot change the
installed helper; rerun `enable-session` to install an updated copy. An earlier
`grant` command supplies the existing ACL backup when present.

Undo the session rule and restore the attached device's original ACL with:

```sh
sudo python3 tools/usb-access.py disable-session
```

Disable also works with the board unplugged. Removed device nodes no longer
carry permissions to restore. If another process changed the attached device's
ACL, disable removes the reconnect rule but refuses to overwrite those changes.
Its saved state remains available for inspection and a later restoration attempt.

Before an intentional reconnect, `check-session` verifies the rule's ownership,
port, user, and current device access. Host discovery after a reset must wait
for udev permission handling before issuing the next USB request. A missing
permission is a host precondition failure and must not trigger another reset.

The focused check in `checks/usb-session.py` uses real ACLs on temporary files
and validates the rule with `udevadm verify`. It covers adoption of an existing
grant, reconnect grants, unrelated events, ACL conflicts, and undo with the
board attached or absent. A physical EL1 handoff check then reset the board and
read its result through the recreated USB node without another permission grant.

## Access to one device node

Run these commands from this worktree:

```sh
python3 tools/usb-access.py status
sudo python3 tools/usb-access.py grant
sudo python3 tools/usb-access.py revoke
```

Under sudo, the target account defaults to the caller. Use `--user sunil` to
select it explicitly. The grant saves the original access control list (ACL)
under `/run/sunstoneos-usb-access`, accessible only to root. Revoke restores that
ACL and removes its backup. Repeated grants preserve the original backup.
Restoration refuses a changed device identity or an unexpected ACL change.

An ACL on a USB device node lasts until the node is removed or recreated,
normally on disconnect or reboot. The `grant` command installs no udev rule, and its
backup directory disappears on reboot. Use `enable-session` for firmware
transitions that reconnect the device.

For access previously granted with a manual `setfacl` command, use:

```sh
sudo python3 tools/usb-access.py remove-user
```

This removes the selected user's ACL entry and retains the current ACL mask,
which limits group and named-user permissions. It cannot reconstruct the ACL
from before the manual command. For future grants, use `grant` followed by
`revoke` to restore all original ACL entries, including the mask.

Grant may widen the ACL mask to permit read/write access. Existing group or
named-user entries can consequently gain permissions that the old mask limited.
Revoke restores the original mask. The script requires a fresh device node if
a stale backup matches a reused USB address; unplugging and reconnecting usually
assigns a new address.

## Native runtime access

The [native USB diagnostic runtime](../docs/usb.md) uses the recovery USB ID
with a distinct product string and device revision. The session rule grants
access across both transitions. FEL loaders require the verified ROM revision
`02b3`; they refuse the native runtime revision `0100` before sending requests.
This protocol check runs on the host and does not require reinstalling the
session helper.

Session discovery treats missing sysfs entries and `ENODEV` during an expected
disconnect as temporary device absence. Callers keep their bounded reconnect
deadlines; other I/O errors remain fatal. `checks/usb-disconnect.py` covers this
metadata race without contacting hardware. The host-side fix needs no rule
reinstallation.

`usb-diagnostic.py` loads the bounded RAM runtime, checks its native protocol,
and requests ROM return. `read-returned-result.py` retrieves diagnostics after
a failed session only when host metadata identifies an independently returned
ROM instance. The USB document records prerequisites, commands, and evidence.
