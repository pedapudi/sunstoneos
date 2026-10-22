# Bounded GPU shutdown and resource retention

Native GPU shutdown is **implemented and host-tested**. It sends the reference
forced-idle and power-off requests, waits for their firmware acknowledgements,
checks hardware idle state, asserts reset, and requires platform isolation.
The sequence has not run on the GPU. Its callbacks, physical shutdown, and
safe buffer reuse remain unqualified.

This document assumes familiarity with device reset and shared command queues.
The [API](../include/pvr_gpu_stop.h) starts from a READY
[firmware control component](gpu-control.md) with an active transport. The owner
must close application admission, hold power and every device-visible allocation,
and serialize control/queue operations through one shutdown worker. Event handlers
remain available until the firmware handshakes finish.

## Separate firmware acknowledgements precede hardware reset

The [implementation](../drivers/gpu/pvr_gpu_stop.c) follows these steps:

1. Clear and publish the shared power-sync word. Submit a forced-idle power command.
2. Service bounded event/return batches and acquire the power-sync word until
   firmware acknowledges with a nonzero value.
3. Clear and publish the word again. Submit the forced power-off command and wait
   for its separate acknowledgement.
4. Retire the control component and command transport, preventing further queue
   or interrupt access through those components.
5. Check the reference hardware idle conditions, clear thread associations,
   assert the selected soft-reset mask, and read back reset to fence the write.
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

One absolute deadline covers both handshakes, hardware polls, reset, and isolation.
During a firmware wait, one poll services at most four events and one
command-return scan, then re-reads power-sync for up to 50 µs
(`PVR_GPU_SPIN_US`), counted from the first read, before returning. A full
command queue also receives bounded service before another submission attempt.
The owner must continue collecting unrelated command results so their slots can
be reused during shutdown.

## Idle checks and platform isolation have distinct roles

The register sequence checks the Sidekick control block, clears four firmware
thread associations, and checks the memory-management interfaces, outstanding
reads, system-level cache status, and firmware-wrapper idle state. The firmware
feature bits select the reset mask and whether the external BIF read-status check
is required. The pinned A733 profile uses fifteen register operations, including
the added reset readback.

Each hardware-stage poll performs one register step. An idle check re-reads its
register for up to 50 µs; a value that stays mismatched leaves the controller at
that step for the next poll, until the deadline. A re-read whose clock shows no
advance for 1,024 consecutive samples (`PVR_GPU_SPIN_STALLED_READS`) also
returns, so a stopped clock cannot hold the worker inside one poll. Writes complete before
their callbacks return. The final reset readback fences the write; it is not
interpreted as proof of interconnect isolation.

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
There is no automatic hard-reset fallback or startup replay. Independent recovery
remains a separate requirement.

The isolation callback may succeed after the deadline. In that case the controller
records the observed success but remains FAILED and retains resources. It does
not silently extend the deadline or turn a late acknowledgement into acceptance.

## Focused integration checks cover failure and retirement

One [ASan/UBSan check](../checks/pvr-gpu-stop.c) links shutdown, control, queue,
and firmware decoding. It starts from a simulated READY control state and uses
the pinned firmware's feature record. Both power requests, separate sync-word
clears, idle/reset ordering, and isolation are exercised through callbacks.
A synthetic feature variant checks alternate reset selection and omission of
the external BIF read-status operation; it does not establish another board profile.

The check passes 120 retained-failure cases. These include callback failures,
each distinct idle-register wait, missing acknowledgements, clock regression,
command errors, and late isolation. A held idle register and a missing
acknowledgement are each re-read for at least 50 µs within one poll. With a
stopped clock, both re-reads return without advancing and shutdown completes
once the clock runs. It verifies that terminal states make no
callbacks, that recorded results survive retirement, and that a power acknowledgement
can arrive without a command-return flag.

The focused [control integration check](../checks/pvr-gpu-control.c) also passes
its 78 retained-failure cases with the changed lifecycle APIs. This regression
was selected because queue retirement changes result collection and admission.
Unchanged firmware staging, cache-publication, OS, mimus, and NPU evidence was reused.

The [reference comparison](../checks/pvr-gpu-stop-reference.py) compiles the
original power-request and firmware-stop functions. Both defined command payloads
and the complete idle/reset plan match. The capture shim clears unused command
bytes while copying only fields assigned by the original request functions.
Native traces also include the additional reset readback. Platform isolation
has no reference substitute in this check. Source hashes are verified against
the pinned board-source manifest.

Clang 21 compiled shutdown, control, and queue components for AArch64 with strict
alignment and freestanding options. Their code/read-only sizes are 2,592, 2,060,
and 3,440 bytes respectively, with no writable static data. Shutdown's individual
frames are 1,968 bytes for initialization, 160 for polling, and 32 for the clock
helper. Total stack accounting must include nested decoding, control, queue,
and platform callbacks.

Commands, input snapshots, hashes, command bytes, register traces, regression
output, reference comparison, and objects are under
`~/.cache/sunstoneos/evidence/gpu-stop-20261016/`. No device access or performance
measurement occurred.
