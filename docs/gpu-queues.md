# GPU firmware command transport

The native firmware queue transport is **implemented and host-tested**.
It formats shared queues, submits commands, collects firmware return statuses,
and receives firmware events. It has not communicated with a physical GPU.
The [firmware initialization records](gpu-fw-state.md) now have native
preparation and host evidence. The FreeRTOS service, platform memory operations,
and application GPU job completion remain outstanding.

This document assumes familiarity with ring buffers and device drivers.
The host-command queue is called KCCB in the reference driver. The reverse
queue, through which firmware requests host actions, is called FWCCB.

## Shared regions match the pinned firmware ABI

The interface is [pvr_ccb.h](../include/pvr_ccb.h), implemented in
[pvr_ccb.c](../drivers/gpu/pvr_ccb.c). The protocol follows the pinned board
driver's `pvr_ccb.c`, `pvr_fw.c`, and firmware interface headers, with their
[MIT notice](../drivers/gpu/LICENSE.pvr) retained.

| Region | Bytes | Ownership |
| --- | ---: | --- |
| Host-command control | 144 | Host writes producer index; firmware writes consumer index |
| Host commands | 11,264 | 128 slots of 88 bytes; host publishes commands |
| Return statuses | 512 | Host clears each slot before publication; firmware writes its result |
| Firmware-event control | 144 | Firmware writes producer index; host writes consumer index |
| Firmware events | 2,816 | 32 slots of 88 bytes; firmware publishes events |

Control structures place the write index at byte 0 and the read index at byte
128. The wrap mask and command size follow at bytes 132 and 136. One slot
remains unused to distinguish full and empty queues. Control allocations need
the firmware's 16-byte base alignment; word accesses must be aligned and atomic.
The platform can map these objects through the [MIPS preparation interface](gpu-mips.md).

The transport accepts already admitted, trusted commands. It checks the command
header's magic and type encoding, but does not validate payload semantics,
firmware pointers, resource ownership, or application permissions. Those checks
belong to command producers and the privileged service.

## Publication and result ownership are separate

Submission copies the command and clears its old return word. It publishes
those writes before advancing the shared producer index, publishes that index,
and writes the GPU scheduler register. Completion of that register write is
required before returning a submission token.

Tokens contain an epoch, a monotonically increasing serial number, and a slot.
Each queue initialization requires a distinct nonzero epoch. A slot remains
reserved until the caller collects its recorded return status, even after
firmware advances the consumer index. This prevents a later submission from
overwriting an uncollected result. Duplicate and stale tokens are refused.

The firmware's command-executed bit acknowledges processing of a control
command. It does not prove completion of the GPU workload requested by that
command. Application resources still require separate job-completion evidence
before release. Cleanup-busy and poll-failure status bits are returned intact.

Receiving an event copies its complete payload before acknowledging its slot.
Each call consumes at most one event. Restart and firmware page-fault requests
return the event and stop further queue traffic. Other valid event types are
delivered to the service for dispatch; their handlers remain unimplemented.

## Deadlines and failures retain ownership for recovery

The transport performs no internal waits or retries. Submission returns
immediately when the ring or a return reservation prevents reuse. A passed
submission deadline prevents publication. Polling performs one bounded scan
of pending return slots and stops the queue when a pending deadline expires.
The service must invoke polling on interrupts and before pending deadlines.

Corrupt control fields, impossible consumer movement, unsupported return bits,
and failed memory/barrier/register callbacks also stop the queue. Subsequent
operations perform no device access. Already recorded results remain available
for collection without I/O. A write failure may have taken effect, so failure
never authorizes replay or reuse of shared memory.

The platform must retain every shared allocation until it verifies that the
GPU can no longer access it. Reinitialization of a prepared, active, or failed
queue is refused. A verified device stop permits disposal of the state and
initialization with a new epoch. This component cannot establish that stop.

All calls require serialization through one privileged worker or lock.
Platform callbacks supply bounded shared-memory access, publication barriers,
and the scheduler-register write. They must preserve firmware-owned data during
cache maintenance. CPU/physical mappings, cache coherence, power, interrupts,
and recovery remain platform responsibilities.

## Focused checks cover ordering, wrap, deadlines, and retained failures

The [host check](../checks/pvr-ccb.c) passes address and undefined-behavior
sanitizers. Its model separates CPU and device memory and checks that command
bytes and the cleared return word are visible before producer-index publication.
It checks that event reads complete before consumer-index publication.

The same check covers ring capacity, both queue wraps, delayed result
collection, stale tokens, completion flags, restart/page-fault events, corrupt
metadata, monotonic time, and deadline expiry. It injects failures at twelve
submission callbacks, ten event callbacks, seven polling callbacks, and one
formatting callback. Each failure stops subsequent device access, including
when the failed write changed device-visible memory.

The [reference check](../checks/pvr-ccb-reference.py) verifies ABI sizes and
offsets against the pinned source assertions. It compiles the original queue
initialization and submission functions with host substitutes for kernel locks
and register writes. All five shared regions match byte-for-byte after a health
command. Source hashes are checked against the pinned download manifest.

The host check passed before and after strengthening its publication-order
assertions. Only that affected check was repeated. The native implementation,
reference comparison, and AArch64 compilation were unchanged and their evidence
was reused. That queue revision compiled to 3,320 bytes of code/read-only data
and no writable static data with freestanding and strict-alignment options.

Commands, logs, shared-memory snapshots, source copies, hashes, and compiled
objects are in `~/.cache/sunstoneos/evidence/gpu-ccb-20261016/`. No physical
device access, broader OS regression run, or performance measurement occurred.

The [firmware control worker](gpu-control.md) uses the added `pvr_ccb_abort()`
operation to stop queue I/O after external faults. Abort preserves recorded
results and retains all device backing. Its integration evidence supplements
the unchanged queue publication and completion checks above.
With abort included, the current queue component compiles to 3,372 bytes of
code/read-only data and no writable static data; its object and input identities
are recorded with the control-worker evidence.
