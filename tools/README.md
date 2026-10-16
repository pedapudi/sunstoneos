# Temporary USB recovery access

`usb-access.py` grants a user read/write access to the single attached Allwinner
FEL recovery device (`1f3a:efe8`). It discovers the current USB address each time
and refuses to select among multiple matching devices. Python 3 and the host
`getfacl` and `setfacl` commands are required.

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
normally on disconnect or reboot. This script installs no udev rule, and its
backup directory disappears on reboot. A firmware transition that reconnects
the device may require a fresh grant.

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
