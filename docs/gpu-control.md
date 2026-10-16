# GPU firmware compatibility, health, and event service

The native firmware control component is **implemented and host-tested**. It connects
completed startup to queue activation, a bounded health command, interrupt
acknowledgement, and limited event batches. It has not run on the GPU. Physical
interrupt delivery, firmware publication, event-specific resource handlers, and
shutdown remain outstanding.

This document assumes familiarity with interrupt handlers and shared firmware
queues. The [API](../include/pvr_gpu_control.h) requires completed
[startup](gpu-start.md) and a prepared [command transport](gpu-queues.md)
that share the same firmware allocations. A privileged FreeRTOS worker must serialize
all access. Application commands remain blocked until the controller reaches READY.

## Published compatibility data precedes queue activation

The firmware writes a 64-byte compatibility record at byte 40 of its OS
initialization allocation. Byte 56 of that record marks its contents valid.
The worker first acquires that byte. After it becomes one, it acquires the complete
record and checks the flag again. Firmware must publish the record before setting
the flag and keep it immutable for the boot instance.

Both hardware and firmware revision records must use layout version 3 and encode
BVNC 36.56.104.183. BVNC identifies the GPU branch, version, scalable-unit count,
and configuration. The worker preserves all 64 bytes for diagnosis. Processor
version, software version/build, build options, and supported OS count are recorded
but are not acceptance conditions; their expected physical values are not yet
established. Padding is ignored.

This check supplements the firmware-image validation and the platform's direct
hardware identity check. It does not establish firmware authenticity or replace
image hashes. The pinned source defines the publication fields; actual publication
by the pinned firmware remains a physical bring-up requirement.

After compatibility passes, the worker activates the prepared command transport.
It submits one zero-filled health command and retains its completion token.
Readiness requires command-executed status without cleanup-busy or poll-failure
bits, followed by completion of the observed event backlog. A collected health
result remains recorded across subsequent bounded event batches.

The caller supplies one absolute deadline covering compatibility publication and
health completion. Expiry and clock regression stop the controller. A clock check
before readiness prevents a late result from passing the deadline. The initial
health command runs once; the controller does not schedule recurring health probes.

## Interrupt handling drains a bounded amount of work

For the MIPS firmware processor, interrupt status is register `0x08a8`, bit zero.
Writing one to `0x08b0` acknowledges the event. A worker poll reads status and,
when pending, acknowledges before receiving firmware events. It then consumes at
most the requested budget, from one to thirty-one events, scans command returns,
and rechecks interrupt status.

The poll requests more service when the budget is exhausted or an interrupt remains
pending. It also receives queued events when the interrupt bit is clear. This covers
work that arrived during acknowledgement and avoids depending on an interrupt for
every queue entry. Pending command deadlines still require scheduled polling.

The platform's interrupt top half should mask and notify the worker. GIC setup,
interrupt priority, re-enabling, and physical delivery are not implemented here.
Each worker invocation has a fixed bound of two status reads, one acknowledgement,
the event budget, and one command-return scan. Existing queue operations impose
their own bounds on shared-memory accesses. Callbacks must be bounded and must not
call the worker or command queue recursively.
During activation, a poll may also perform one bounded queue activation,
health submission, or result collection.

Known event types cover buffer backing/unbacking, freelist growth/reconstruction,
context resets, diagnostic dumps, statistics, and clock changes. The worker passes
a copied 88-byte event to a registered handler. That handler must validate the
payload and apply or retain the requested action before reporting success.
The controller does not yet implement those resource-specific actions.

Restart requests and firmware page faults latch failure without dispatching a
handler. Unknown commands, absent handlers, and failed handlers also latch failure.
No event is silently discarded as successfully handled. The last copied event and
the queue's retained payload remain available for diagnosis; a queue-read failure
may leave only a partial payload in the queue record.

## A service fault stops subsequent queue access

The [worker implementation](../drivers/gpu/pvr_gpu_control.c) checks queue state
before any clock or device callback. A queue already failed by another serialized
producer causes immediate controller failure. A controller failure calls
`pvr_ccb_abort()`, which marks the transport failed without device access.
Neither component performs further queue or interrupt I/O afterward.

The queue abort operation preserves already recorded command results for collection.
It does not stop firmware, free backing memory, or authorize resource reuse.
All device-visible allocations and diagnostics remain owned until a separately
verified shutdown or recovery establishes that GPU access has stopped.

READY establishes layout/revision agreement and one executed health command.
It does not establish graphics/compute execution, application command admission,
GPU job completion, or physical qualification. Those remain separate requirements.

## Focused checks cover transitions and interrupt races

One [ASan/UBSan integration check](../checks/pvr-gpu-control.c) exercises the native
worker with the command transport. It covers compatibility publication, mismatched
layouts/revisions, health success and error status, startup deadlines, bounded
batches, and work arriving during acknowledgement. It verifies handling without
an interrupt and continued service after the initial boot deadline.

The check passes 78 retained-failure cases, including failure at each callback
in the successful activation path. It verifies that both components stop I/O,
that unknown/restart/page-fault events are retained, and that an already recorded
command result can still be collected after an unrelated service fault.
The startup state and firmware responses are simulated; no firmware image executes.

The [reference comparison](../checks/pvr-gpu-control-reference.py) compiles the
original compatibility structures, layout assertions, revision initializer, and
MIPS interrupt functions. The complete 64-byte synthetic compatibility fixture
and observed interrupt operations match. Processor/build fields in that fixture
are synthetic and do not establish physical firmware identity. The health command
matches the saved queue-publication reference, whose hash is checked before reuse.
Source hashes are verified against the pinned board-source manifest.

Clang 21 compiled the worker and changed queue component for AArch64 with strict
alignment and freestanding options. The worker contains 1,868 bytes of code/read-only
data, and the queue contains 3,372 bytes. Neither has writable static data.
The worker's individual frames are 64 bytes for initialization, 144 for polling,
and 32 for each clock/status helper. Total stack accounting must also include
nested queue operations and platform callbacks.

Commands, input snapshots, hashes, fixture bytes, interrupt traces, comparison,
and objects are under `~/.cache/sunstoneos/evidence/gpu-control-20261016/`.
Existing queue internals are unchanged apart from the added abort API; their
publication, cache, and malformed-input evidence was reused. No broader OS,
mimus, accelerator, or performance checks were repeated. No device access occurred.
