# USB recovery access

The [physical recovery instructions](../docs/board-wiring.md) identify the
BOOT/GND pads and the `check-fel-input.py` diagnostic. That diagnostic samples
the hardware input without resetting the board. USB permissions and physical
selection of the ROM recovery path are separate requirements.
The [protected watchdog procedure](../docs/watchdog-expiry.md) uses
`build-watchdog-expiry.sh` and `check-watchdog-expiry.py` for separate
enable/disarm and expiry requests. Both require coordinator review and a
confirmed stable BOOT/GND connection.

`usb-access.py` grants a user read/write access to the single attached Allwinner
FEL recovery device (`1f3a:efe8`). It discovers the current USB address each time
and refuses to select among multiple matching devices. Python 3 and the host
`getfacl` and `setfacl` commands are required.

`usb_guard.py` limits how often board tools re-enumerate USB devices and refuses
board work after the host's xHCI controller has died. Board tools import it from
this directory, and bundles that run a frozen host runner carry a copy. The file
provides the shared board re-enumeration budget and controller-health checks.
Board tools import it from this directory. Every consumer uses the
`/tmp/mimus-usb-churn.json` state under `/tmp/mimus-usb-churn.lock`.
These historical paths keep the budget shared with existing consumers.
`SUNSTONE_USB_SPACING_S`, `SUNSTONE_USB_BOARD_CAP` and
`SUNSTONE_USB_BOARD_WINDOW_S` select the shared budget; `SUNSTONE_USB_HEALTH`
selects the existing warning override. Each setting falls back to the matching
`MIMUS_USB_*` name when its `SUNSTONE_USB_*` name is unset. The shared paths
and fallbacks keep existing worktrees in the same safety budget.

## Access across board resets

For autonomous bring-up, install a session rule from this worktree:

```sh
sudo python3 tools/usb-access.py enable-session
python3 tools/usb-access.py check-session
```

With multiple A733 boards attached, select the physical port explicitly. Enable
access for the second board without replacing the first board's session:

```sh
sudo python3 tools/usb-access.py enable-session --port 3-3
python3 tools/usb-access.py check-session --port 3-3
```

Undo only that board's session with:

```sh
sudo python3 tools/usb-access.py disable-session --port 3-3
```

Existing board runners can select a port through a process-local environment
variable, for example `SUNSTONE_USB_PORT=3-1 python3 tools/run-ddr.py ...`.
An explicit `--port` overrides the environment selector. Without a selector,
discovery refuses multiple matching devices. Device identities, recovery
records, queues and locks remain separate for each port. Never choose the first
matching USB device.

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

Select the port when multiple sessions exist. Disable also works with the board unplugged. Removed device nodes no longer
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

After a failed return to ROM, the loader records the old device identity in
`~/.cache/sunstoneos/recovery-required.json`. Loaders refuse that instance even
when its cached ROM descriptor remains visible. Independent board recovery must
produce a distinct instance that passes the ROM checks. `check-session` reports
permissions only; it does not establish that the device is executing ROM code.
The [watchdog recovery work](../docs/watchdog.md) records the current limitation.

`usb-diagnostic.py` loads the bounded RAM runtime, checks its native protocol,
and requests ROM return. `read-returned-result.py` retrieves diagnostics after
a failed session only when host metadata identifies an independently returned
ROM instance. The USB document records prerequisites, commands, and evidence.

## NPU model inspection

`inspect-nbg.py MODEL` emits JSON metadata for the inspected NBG `0x1001e`
format. It checks container structure and selected references without executing
commands. [NPU format and evidence](../docs/npu.md) describe its limits and
the pinned compiled fixture. Successful inspection does not establish that a
model is safe to submit or compatible with the physical device.

`npu-conv-fixture.py` generates a small ONNX convolution and an exact arithmetic
reference. `npu-conv-pipeline.sh` runs its compiler and simulator workflow inside
the pinned host image. [Compiler setup and evidence](../docs/npu-compiler.md)
record image identities, execution conditions, results, and the emitted format's
current native-loader limitation.

## Build, package, submit and wait

`configure.py SRC BUILD [cmake options]` runs CMake with the aarch64 toolchain
file and every pinned dependency path (`checks/image_dependencies.py` derives
them from `sources.lock.json`); it refuses with the fetch command when one is
missing, and a `-D` option of yours replaces the pinned one.

`session.py build IMAGE --out DIR` writes the typed board session of an image to
`DIR/template/session.bin`, from the image's own symbols and manifest and the
board facts in `session-facts.json`. `session.py check DIR [--admit]` decodes it
and runs the image's EL3 admission. `board.py submit IMAGE --template
DIR/template ... --wait` queues the run, waits, and prints the outcome, stage,
first failure and decoded summary. [The runner document](../docs/board-runner.md)
gives the four commands in order. Do not write packaging or configure scripts
under the build cache.

`clean-generated.py` deletes generated files under the build cache, the trash
directory and session scratch directories, only with `--apply`, and refuses
evidence, sources, host tools, queue databases and any build directory that an
evidence record or queue entry names.
