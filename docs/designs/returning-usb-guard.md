# Bound shared USB transition ownership to native execution

The returning transport holds its physical port lock across admission, RAM backup,
upload, native execution, report capture and restoration. Its shared USB churn
reservation covers only the operation that can replace the ROM instance: the
fixed SRAM launcher at `0x52000` through discovery of the new ROM instance with
host permissions and the original physical port. Report reads and restoration
follow outside that reservation.

The previous transport reserved two re-enumerations around the complete device
callback. That labeled bulk RAM transfers as a transition and delayed another
board's launch throughout backup, upload and restoration. The existing kernel
runner already uploads before its churn reservation and captures results after
it (`tools/check-a733-kernel.py`).

The transport supplies one `launch()` callback. It acquires the unchanged shared
churn reservation, repeats entry admission after any wait, records the native
attempt, executes the launcher once and waits for the permitted successor. A
separate private latch refuses a second call. Direct native execution through the
transaction callback refuses outside this interval. The CPU qualification,
returning recognition, finite GPU, GPU entry checkpoint and finite NPU adapters
use this callback. The memory qualification producer has no runnable shared
transport adapter; its future integration must use this interface.

A failed admission before the launch command unwinds the churn reservation and
retains dirty state under the physical port lock. An uncertain launch or successor
wait invokes the existing failure hold inside the reservation. Report or restore
failure after an observed successor retains the physical port outside it. Every
transfer still verifies instance identity and stops on its first transport failure.
The shared guard's existing event lifetime, spacing, churn cap and host health
policy remain authoritative. In particular, its 300-second stale-event limit
prevents claiming indefinite exclusion of other processes from an open event.

The source basis is the pinned Sunxi tools revision
`b92f4ed3115a520e3ec406697d268239e3f29488`: `fel_lib.c` uses synchronous bulk
transfers and `feldev_done` releases the interface, closes the handle and exits
libusb before a successful CLI invocation returns. `fel.c` calls that cleanup
at normal exit. Ordinary completed RAM commands do not request re-enumeration.
The shared guard's recorded controller failure involved continued writes to a
disconnected device; retaining identity and first-failure checks remains necessary.
This source comparison does not qualify concurrent physical transfers or recovery
from a failed controller.

The change adds no native work, transfer, copy, allocation or recurring polling.
It retains one churn reservation for the same two transitions and adds one entry
recheck immediately before launch. Host fixtures check that backup/upload and
report/restore run outside the reservation, launch and successor validation run
inside it, pre-launch refusal holds only the port, and uncertain return holds
both scopes. Frozen physical packages keep their original transport bytes.
