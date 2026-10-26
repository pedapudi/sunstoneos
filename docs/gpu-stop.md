# Bounded GPU shutdown and resource retention

Native GPU shutdown is **implemented and host-tested**. It sends the reference
forced-idle and power-off requests, waits for their firmware acknowledgements,
checks hardware idle state as the vendor `RGXStop` does, and requires platform isolation.
The sequence has not run on the GPU. Its callbacks, physical shutdown, and
safe buffer reuse remain unqualified.

This document assumes familiarity with device reset and shared command queues.
The [API](../include/pvr_gpu_stop.h) starts from a READY
[firmware control component](gpu-control.md) with an active transport. The owner
must close application admission, hold power and every device-visible allocation,
and serialize control/queue operations through one shutdown worker. Event handlers
remain available until the firmware handshakes finish.

## Separate firmware acknowledgements precede the hardware idle checks

The [implementation](../drivers/gpu/pvr_gpu_stop.c) follows these steps:

1. Clear and publish the shared power-sync word. Submit a forced-idle power command.
2. Service bounded event/return batches and acquire the power-sync word until
   firmware acknowledges with a nonzero value.
3. Clear and publish the word again. Submit the forced power-off command and wait
   for its separate acknowledgement.
4. Retire the control component and command transport, preventing further queue
   or interrupt access through those components.
5. Acknowledge a pending firmware interrupt, check the hardware idle
   conditions and clear the thread-0 associations, in the order of the vendor
   DDK's `RGXStop` for a MIPS core.
6. Complete platform power-off through one admission callback and optional
   bounded polling. Report STOPPED only after completion before the deadline.

Both power commands are 88 bytes with unused bytes cleared. They select power
command type 107, forced-idle request type 2 or power-off request type 1, and
request value one. The shared power-sync word has its own firmware mapping in
[initialization resources](gpu-fw-state.md).

A queue return flag is separate from the power acknowledgement. The controller
records a completed return when available and rejects its error bits. It does
not require that return flag before accepting a power acknowledgement, matching
the reference handshake. Unresolved return tokens remain recorded and cannot be
reported as successful merely because shutdown completed.

One absolute deadline covers both handshakes, hardware polls and isolation.
During a firmware wait, one poll services at most four events and one
command-return scan, then re-reads power-sync for up to 50 µs
(`A733_GPU_SPIN_US`), counted from the first read, before returning. A full
command queue also receives bounded service before another submission attempt.
The owner must continue collecting unrelated command results so their slots can
be reused during shutdown.

## Idle checks and platform isolation have distinct roles

The register sequence is the vendor DDK's `RGXStop` for a MIPS core
(`rgxstartstop.c:992–1241`, ODM DDK `2ac08e8c`;
[firmware stop design](designs/gpu-firmware-stop.md)). It reads the MIPS
wrapper interrupt status (`0x8a8`) and, when the event bit is set, clears it
(`0x8b0`), as `RGXAckIrqMIPS` does (`rgxinit.c:420–462`). It then waits for
Sidekick and the system-level cache to report idle, clears the thread-0
interrupt and background DM associations, and checks the memory-management
interfaces, outstanding reads, system-level cache status, Sidekick and the
firmware wrapper. The firmware feature bits select whether the external BIF
read-status check is required. The pinned A733 profile uses fourteen register
operations. Upstream Linux also clears the thread-1 associations, which the
vendor clears only for a META core, and asserts the soft-reset mask; the vendor
asserts no reset, and the platform OFF removes the power.

Each hardware-stage poll performs one register step. An idle check re-reads its
register for up to 50 µs; a value that stays mismatched leaves the controller at
that step for the next poll, until the deadline. A re-read whose clock shows no
advance for 1,024 consecutive samples (`A733_GPU_SPIN_STALLED_READS`) also
returns, so a stopped clock cannot hold the worker inside one poll. Writes complete before
their callbacks return.

The native backend independently checks firmware forced-idle/OFF state after
each acknowledgement, then completes A733 platform power-off after these local
checks. [The healthy shutdown contract](gpu-isolation.md) follows the pinned
normal power sequence. A failed device cannot use this path as DMA recovery.
The optional asynchronous completion callback yields while pending under the
original deadline; admission occurs once and only result one completes it.

The controller itself frees no memory. The owner separately handles interrupted
jobs, result visibility, diagnostics, and resource release. Successful shutdown
does not establish successful completion of cancelled work or CPU visibility of
GPU output buffers.

## Retirement closes normal service without discarding diagnostics

The command transport now has a RETIRED state. It refuses new I/O while allowing
collection of already recorded results. An unresolved token returns failure
because retirement permits no further device polling. Retirement alone does not
stop the GPU or authorize buffer reuse.

The control component retires its queue and then stops its own polling. An
external fault can still move both retired components to FAILED. Shutdown uses
that path if an idle check times out, register access fails, or platform isolation
fails after normal firmware service has closed.

A callback failure, clock regression, or deadline expiry latches shutdown failure
and prevents further callbacks. An external control/queue failure is detected
before another clock or device access. All backing and diagnostics remain owned.
The controller retries nothing and replays no startup; the owner may start the
stop without the acknowledgements, described next.

The isolation callback may succeed after the deadline. In that case the controller
records the observed success but remains FAILED and retains resources. It does
not silently extend the deadline or turn a late acknowledgement into acceptance.

## The stop without the firmware's acknowledgements

`pvr_gpu_stop_init_unacknowledged` is upstream's hard reset
(`pvr_power_reset` with `hard_reset`, `pvr_power.c:496–531`, which calls
`pvr_power_fw_disable` without the power requests and then powers the
device down). It is admitted only while the control component and its queue
are latched FAILED, so no command can follow it, and only from a new
controller or from one that failed before its first register step and
before isolation. It starts at the register sequence: the same interrupt
acknowledgement, idle polls and DM-association clears, then the isolation
callback, under its own deadline.

Without the acknowledgements, the idle polls are the only evidence that no
GPU memory access is in flight. When one never matches, the stop fails at
its deadline before isolation, and the GPU must stay powered until a power
removal. The first-job image (`apps/gpu-power-id/fw-start.c`) takes this
path when its control failed or the firmware acknowledged neither request,
in the last 100 ms of the stop's 300 ms; set 27 on port 3-1 showed why it is
needed ([first GPU job](designs/gpu-first-job.md)).

## Focused integration checks cover failure and retirement

One [ASan/UBSan check](../checks/pvr-gpu-stop.c) links shutdown, control, queue,
and firmware decoding. It starts from a simulated READY control state and uses
the pinned firmware's feature record. Both power requests, separate sync-word
clears, the interrupt acknowledgement with and without a pending event, idle
ordering, and isolation are exercised through callbacks. A synthetic feature
variant checks omission of the external BIF read-status operation; it does not
establish another board profile.

The check passes 121 retained-failure cases. These include callback failures,
each distinct idle-register wait, missing acknowledgements, clock regression,
command errors, and late isolation. A held idle register and a missing
acknowledgement are each re-read for at least 50 µs within one poll. With a
stopped clock, both re-reads return without advancing and shutdown completes
once the clock runs. The stop without acknowledgements is refused while the
control is READY and after a register step, completes from NEW and after a
failed handshake without sending a command, and fails before isolation on a
held idle poll. It verifies that terminal states make no
callbacks, that recorded results survive retirement, and that a power acknowledgement
can arrive without a command-return flag.

The focused [control integration check](../checks/pvr-gpu-control.c) also passes
its 78 retained-failure cases with the changed lifecycle APIs. This regression
was selected because queue retirement changes result collection and admission.
Unchanged firmware staging, cache-publication, OS, mimus, and NPU evidence was reused.

The [reference comparison](../checks/pvr-gpu-stop-reference.py) compiles the
original power-request and firmware-stop functions. Both defined command payloads
match. The idle plan matches the upstream function's after four vendor
differences, each checked: the interrupt acknowledgement first, the early
system-level cache poll, no thread-1 association writes, and no soft reset. The
capture shim clears unused command bytes while copying only fields assigned by
the original request functions. Platform isolation
has no reference substitute in this check. Source hashes are verified against
the pinned board-source manifest.

Clang 21 compiled shutdown, control, and queue components for AArch64 with strict
alignment and freestanding options. Shutdown's code and read-only data are
2,876 bytes, with no writable static data. Its individual frames are 1,904
bytes for initialization, 176 for polling, and 32 for the clock helper. Total stack accounting must include nested decoding, control, queue,
and platform callbacks.

Commands, input snapshots, hashes, command bytes, register traces, regression
output, reference comparison, and objects are under
`~/.cache/sunstoneos/evidence/gpu-stop-20261025/` (the upstream sequence's run
is under `gpu-stop-20261016/`). No device access or performance measurement
occurred.
