# Bound shared USB transition ownership to native execution

**Status: open.** `tools/usb_guard.py` implements the shared churn reservation
and is used by the DDR restore, the watchdog expiry checks, the native USB
diagnostic and the DMA copy observer. The queue runner `tools/board.py` owns
returning launches and does not reserve the shared churn budget; the scoping
below is the requirement for any launch path that does.

A returning transport holds its physical port lock across admission, RAM
backup, upload, native execution, report capture and restoration. The shared
USB churn reservation (`usb_guard.reenumeration`) covers only the operation
that can replace the ROM instance: the fixed SRAM launcher at `0x52000`
through discovery of the new ROM instance with host permissions and the
original physical port. Backup, upload, report reads and restoration run
outside that reservation, so bulk RAM transfers are not counted as transitions
and do not delay another board's launch.

## Launch interval

The transport supplies one `launch()` callback. It acquires the shared churn
reservation, repeats entry admission after any wait, records the native
attempt, executes the launcher once and waits for the permitted successor. A
separate private latch refuses a second call. Direct native execution through
the transaction callback refuses outside this interval.

- A failed admission before the launch command unwinds the churn reservation
  and retains dirty state under the physical port lock.
- An uncertain launch or successor wait invokes the failure hold inside the
  reservation.
- A report or restore failure after an observed successor retains the physical
  port outside the reservation.
- Every transfer verifies instance identity and stops on its first transport
  failure.

The guard's event lifetime, spacing, churn cap and host health policy stay
authoritative. Its 300-second stale-event limit (`MAX_EVENT_S`) means an open
event does not claim indefinite exclusion of other processes.

## Source basis

The pinned Sunxi tools revision `b92f4ed3115a520e3ec406697d268239e3f29488`
uses synchronous bulk transfers in `fel_lib.c`; `feldev_done` releases the
interface, closes the handle and exits libusb before a successful CLI
invocation returns, and `fel.c` calls that cleanup at normal exit. Ordinary
completed RAM commands do not request re-enumeration. The guard's recorded
controller failure involved continued writes to a disconnected device, so the
identity and first-failure checks stay. This source comparison does not
qualify concurrent physical transfers or recovery from a failed controller.

## Cost

The scoping adds no native work, transfer, copy, allocation or recurring
polling. It keeps one churn reservation for the same two transitions and adds
one entry recheck immediately before launch.

## Validation and open work

Host fixtures are to check that backup and upload and report and restore run
outside the reservation, launch and successor validation run inside it,
pre-launch refusal holds only the port, and uncertain return holds both
scopes. Frozen physical packages keep their original transport bytes.

1. Apply the interval to the queue runner's launch path or record why the
   queue's port serialization replaces it; `tools/board.py` replaces the
   guard in its DDR path with a no-op.
2. Add the host fixtures above for the chosen launch path.
