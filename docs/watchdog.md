# Watchdog recovery and retained failure evidence

Watchdog-reset tests are allowed and in use. Returning development images run
under the host-armed hardware watchdog WDT0. They record progress in
real-time clock (RTC) words that survive its reset.
The mechanism is implemented in [the returning transport](../tools/a733-returning-host.py),
[the image patches](../apps/os/cpu-return/) and
[the breadcrumb interface](../include/sunstone_crumb.h).
The coordinator uses [tools/dev-run.py](../tools/dev-run.py) and follows
[the hardware access rules](../AGENTS.md#hardware-access).

Physical evidence from 2026-10-31 is under
`~/.cache/sunstoneos/evidence/wdt-fel-return-20261031/` and
`~/.cache/sunstoneos/evidence/dev-runs-1031/`.
The latter's `3-3-cpu-return-191044/run/watchdog-report.json` records a stop,
a shortened reset interval and retained fault words. These results establish
the recorded reset paths; they do not prove escape from every peripheral stall.

## The host arms WDT0 before native execution

The returning image declares its expected watchdog mode and session length
in `os_cpu_return_watchdog` in
[apps/os/cpu-return.c](../apps/os/cpu-return.c). The current mode is `0xb1`:
enabled, with a 16-second interval. The host validates that declaration before
launch and requires a reconnect access rule for the selected port and user.

[tools/a733-crumbs.py](../tools/a733-crumbs.py) supplies the arm sequence.
The host clears the breadcrumb words, then programs WDT0 at `0x02050000`
in this order:

| Register | Address | Write |
| --- | --- | --- |
| Reset output | `0x02050018` | `0x16aa0002` |
| Configuration: system reset | `0x02050010` | `0x16aa0001` |
| Mode: enabled, 16 seconds | `0x02050014` | `0x16aa00b1` |
| Restart | `0x0205000c` | `0x14af` |

The upper key is `0x16aa0000`. The transport reads back the lower words and
requires mode/configuration/output to be `0xb1`, `1`, `2` before execution.
The image's [EL3 admission](../apps/os/cpu-return/boot.patch) checks the same
values and requires both ROM redirect flags inactive. It does not install a
CPU redirect for watchdog recovery.

The per-port lock covers upload, launch and return classification. The shared
USB transition guard covers intentional re-enumeration. The host permits one
launch and waits for a distinct ROM instance with access, for at most the
image's session length plus one watchdog interval plus ten seconds.
A transport failure ends requests to that instance.

## The supervisor restarts WDT0; EL3 shortens it on a stop

The CPU0 supervisor calls `os_cpu_return_supervise()` on its existing
20 ms pass. That function checks the session and deadline before calling
`sunstone_watchdog_restart()`, which writes `0x14af` to WDT0 restart.
A failure, expired session or stalled supervisor ends these restarts.
WDT0 then resets the board within the remaining interval.

The [EL3 stop patch](../apps/os/cpu-return/boot.patch) records `EL3_STOP`
and the stop status/detail. If the mode still equals `0xb1`, it writes
`0x16aa0001` to mode and `0x14af` to restart. Mode `0x01` selects 0.5 seconds.
EL3 then parks with interrupts masked. Other watchdog modes are left unchanged;
a stop without an armed watchdog can still retain the board indefinitely.

## RTC words retain progress and the first failure

[include/sunstone_crumb.h](../include/sunstone_crumb.h) defines the layout;
[tools/a733-crumbs.py](../tools/a733-crumbs.py) reads the definitions when decoding.
The words occupy the RTC general-purpose region at `0x07090000`:

| Offset | Meaning |
| --- | --- |
| `0x100` | Session tag: magic `0x5c0d` and the epoch's low 16 bits |
| `0x104` | Last stage and the value's low 16 bits |
| `0x10c` | Core number and a truncated counter sample |
| `0x110` | First failure tag: core, exception level, vector slot and status byte |
| `0x114` | Exception syndrome, or the deliberate exit's detail |
| `0x118` | Exception return address, low 32 bits |
| `0x11c` | Fault address, low 32 bits |

Offset `0x108` is the FEL request flag and is never used for breadcrumbs.
The boot core clears the record and tags the session on EL3 entry.
EL1 progress calls use SVC `0x5c0d`; EL3 progress writes directly.
The last-stage word is shared across cores, so later progress can replace it.
It is not a complete event trace.

The [EL3 vector patch](../apps/os/cpu-return/vectors.patch) records the first
fault or deliberate nonzero exit while the failure tag is empty. It stores
detail, return address and fault address before publishing the tag. Subsequent
handlers preserve a nonzero failure record. A successful SMC records no fault.
These RTC fields survive a watchdog reset; the DRAM result does not.

## A normal return requires RETURN_ROM and mode 0

The [success terminal](../apps/os/cpu-return/terminal.patch) completes native
retirement, disarms WDT0, reads its mode and records `RETURN_ROM` (`0x0503`)
with that value. It parks if the enable bit remains set. A successful path
returns through the reviewed CPU-local forced-FEL entry.

Before reading DRAM, the host reads WDT0 mode, disarms it, reads mode again
and captures the RTC words on the new ROM instance. Normal return requires:

- The watchdog was disabled before the host's disarm.
- The breadcrumb tag matches this session.
- The last stage is `RETURN_ROM`, with recorded mode 0.

Only then does the application-specific decoder inspect native results and
restore saved RAM. A disabled watchdog without the matching success breadcrumb
is an unresolved transition.

If the enable bit was set on return, the transport records a watchdog reset
and writes `watchdog-report.json`. Mode `0x01` identifies the EL3 shortening.
The run fails with `WatchdogReset`; it does not become a successful image run.
The verified fresh ROM instance is usable without quarantining that instance,
but the record keeps resource release forbidden and marks DRAM lost.

## Reset loses DRAM and preserves PMIC rail settings

After watchdog reset, treat all DRAM as uninitialized. Do not read an old
result, transcript or saved-memory image. Run cold DRAM training before any
subsequent DRAM access. The [DDR runner](../tools/run-ddr.py) separately uses
mode `0x41` for experimental restore, with one full-training fallback after
failure. That DDR protocol does not use the application's RETURN_ROM record.

WDT0 does not reset the PMIC rails. DCDC3 and DCDC5 can remain at 1050 and
1000 mV after failure, including after USB replug in the supplied 2026-10-31
measurement. The retained selectors are recorded in
`dev-runs-1031/3-3-rail-helper-2/rail-helper/kernel.log` under
`~/.cache/sunstoneos/evidence/`.
boot0 does not set those rails, as documented in
[the boot0 inventory](vendor-firmware/boot-rom-and-boot0.md#pmic-rails-over-twi).
CPU-return images require both rails at 800 mV. The supplied 2026-10-31 status
reports that the recovery helper to lower both rails is in progress.
The pinned development helper refuses raised DCDC3.

## Unresolved transport failures quarantine the failed instance

[tools/usb-access.py](../tools/usb-access.py) records failed USB instances in
`~/.cache/sunstoneos/recovery-required.json`. `require_rom()` refuses a recorded
instance before sending a request. A cached ROM descriptor proves no recovery.
A distinct instance must pass identity, ROM and access checks.

The returning transport journals the first failed transaction and sends no
further request to that instance. It retains unresolved transitions through
the existing failure handler. A verified watchdog return follows the separate
classification above. Observation timeouts do not authorize releasing a live
run's lock or resetting repeatedly.

## Launchers arm the watchdog for runtime images

The host-armed `0xb1` returning profile is distinct from the existing
four-second `0x41` launcher contracts. The latter are written by
[the SMP launcher](../platform/a733-smp/launch.c),
[observer launcher](../platform/a733-smp-observe/launch.c),
[storage launcher](../platform/a733-storage-launch/launch.c),
[device launcher](../platform/a733-device-launch/launch.c) and
[stage-2 loader](../boot/a733/stage2/loader.c).
Their validators and [maintenance reload verifier](../platform/a733/maintenance_watchdog.c)
require their own mode, configuration and output values. The returning image's
admission does not change those contracts. The isolated
[expiry diagnostic](watchdog-expiry.md) also has its own save/restore driver.

## Choose the boot-input guard from the reset path

A CPU-local normal return enters forced FEL at ROM address `0x20`; it bypasses
the normal storage-boot input sample. A watchdog reset follows the system reset
path. Admit its recovery destination independently under the board rules.
Neither a normal return nor a successful configuration-only test establishes
recovery from a stalled device transaction.

## Captured board ROM and reset ownership

The [ROM inventory](vendor-firmware/boot-rom-and-boot0.md) records the captured
ROM, redirect flags and storage/FEL dispatch. The
[reset-evidence investigation](watchdog-reset-evidence.md) separates reset
causes from ordinary CPU return. These source and model results supplement
the measured watchdog returns; they do not replace a current target check.

The focused host check uses modeled transport and optional instruction-model
execution; it opens no USB node:

```sh
~/.cache/sunstoneos/venv/bin/python -B checks/returning-watchdog.py \
  --out ~/.cache/sunstoneos/build/docs-current-1031/watchdog-check
```
