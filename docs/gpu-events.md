# GPU firmware events resolve owned resources before dispatch

Native decoding and dispatch are implemented for the eight event types delegated
by the GPU control worker. Host checks use the pinned firmware structures and the
existing queue/control implementation. Physical firmware events remain unqualified.
This document assumes familiarity with firmware queues and GPU resource ownership.

The dispatcher does not implement depth/stencil allocation, free-list growth,
reconstruction, or their firmware responses. Those operations require a resource
manager. An absent handler fails the service; recognizing an event does not count
as completing its requested operation.

## Decoding follows the fixed firmware ABI

The [decoder](../drivers/gpu/pvr_gpu_events.c) requires one 88-byte command and
accepts unaligned input. It uses explicit little-endian reads and changes its
output only after successful validation. The first word contains the command
magic and event ID; the next word contains compatibility flags. Payloads begin
at byte eight.

| Event ID | Decoded request or notification | Resources resolved |
| --- | --- | --- |
| 101 | Back a depth/stencil buffer with physical memory | One depth/stencil buffer |
| 102 | Remove a depth/stencil buffer's backing | One depth/stencil buffer |
| 103 | Grow a GPU free list | One free list |
| 104 | Reconstruct free lists after recovery | Every requested free list, in order |
| 105 | Report a context reset, reason, engine, job, and fault information | One context, or every registered context when the all-contexts flag is set |
| 106 | Request a debug dump | Device-wide handler |
| 107 | Adjust a render statistic for an accounting ID | One registered accounting owner |
| 108 | Notify the host of a core clock rate | Device-wide handler |

Reset reasons, engine numbers, reset flags, and statistic selectors must belong
to the pinned ABI. A reconstruction request may contain at most eighteen IDs.
The decoder retains the recovery counter and signed statistic adjustment.
Reset page-catalog and fault addresses are diagnostic values; it never dereferences
them. The handler uses the page-fault flag to determine whether the fault address
is meaningful.

Padding and inactive union members may contain stale data. The decoder ignores
them. It preserves outer compatibility flags because the source defines no valid-bit
mask for that field. Zero resource IDs, an empty reconstruction request, duplicate
reconstruction IDs, and a zero clock notification remain representable. Their
resource-specific effects belong to the handler.

The definitions come from the pinned board driver's
[firmware ABI](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_rogue_fwif.h)
and related common/shared headers. The original license remains in
[LICENSE.pvr](../drivers/gpu/LICENSE.pvr). The runner verifies source hashes
against the existing downloaded-source manifest before compiling reference fixtures.

## One epoch owns an immutable resource catalogue

The [dispatcher interface](../include/pvr_gpu_events.h) copies up to 64 resource
bindings at initialization. Each binding maps a resource kind and firmware ID to
a nonzero platform owner and token. The same numeric ID may exist in different
resource kinds. Duplicate keys within a kind are refused.

The platform verifies each binding and pins its backing object for the firmware
epoch. Tokens are platform identities, not firmware addresses. Catalogue entries
cannot be replaced or reused during that epoch. Initialization performs no resource
operation and preserves the destination on refusal.

Every dispatch must supply the current queue epoch. The dispatcher resolves all
requested resources before calling one handler. An unknown ID late in a
reconstruction request therefore prevents actions on earlier IDs. An all-contexts
reset resolves the registered contexts and ignores the individual context ID.
Accounting IDs map to SunstoneOS application owners; they require no Linux or
POSIX process implementation.

The single privileged worker serializes dispatch. Callback arguments are read-only
and their temporary target-pointer array lasts only for the call. Resource records
remain pinned in the dispatcher, but a later dispatch overwrites the decoded event.
A handler must copy the event and resolved target identities into storage owned
by an implemented resource service before returning a deferred action.
It may return success only after performing or retaining the complete required
action. Logging an unimplemented allocation request does not satisfy that contract.
Callbacks must be bounded and cannot recursively enter the queue, control worker,
or event dispatcher.

The original
[free-list handlers](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_free_list.c)
show the remaining resource work. Growth accounts for consumed reserve pages,
adds backing, and returns page counts. Reconstruction rebuilds backing lists and
updates associated render-target state. Their response commands must be retained
and submitted by the worker after control polling; this dispatcher sends none.

## Failures preserve diagnostics and ownership

Malformed payloads, stale epochs, missing ownership, unsupported handlers, failed
actions, and recursive dispatch latch failure. Subsequent calls preserve the
failure and invoke no handlers. A failed action may already have changed resources;
there is no automatic rollback, retry, or reclamation.

The dispatcher retains valid disjoint input bytes, up to the command size, and
records the supplied length. Invalid input pointers or overlapping storage cannot
be copied. The raw event is authoritative after decode or epoch failure; the
previous decoded event may remain in diagnostic storage. Verified device isolation
is required before discarding the catalogue or initializing another epoch.

A platform control callback calls `pvr_gpu_events_dispatch` with the current queue
epoch and copied 88-byte event, then returns its result unchanged. The existing
control worker converts refusal into a retained control/queue failure. Its queue
consumer has already acknowledged the event slot; retained diagnostic and resource
state must therefore survive that failure.

## Focused host evidence covers decoding and control integration

The source-derived fixture fills the original firmware structures with nonzero
padding and exercises all eight event types. AddressSanitizer and
UndefinedBehaviorSanitizer checks pass for ownership resolution, signed values,
unaligned input, catalogue copying, all-context resets, unsupported handlers,
action side effects, recursive dispatch, and nineteen rejected events.

A separate focused control fixture accepts an owned growth request, then receives
a reconstruction request whose final ID is unknown. No reconstruction callback
runs. The controller and queue retain failure, preserve the raw event, and perform
no later device I/O. This fixture reuses the existing software device model without
running its broader test suite. It does not execute GPU firmware instructions.

Evidence is retained in `~/.cache/sunstoneos/evidence/gpu-events-20261016/`.
The `integrated` directory holds the host logs and final source snapshots.
The AArch64 object occupies 3,112 code bytes with no static data or BSS. Compiler
stack frames are 144 bytes for decoding, 1,920 for initialization, and 592 for
dispatch; callback frames are additional. Physical stack use remains unmeasured.

Reproduce after relevant inputs change:

```sh
python3 checks/pvr-gpu-events.py \
  --sources ~/.cache/sunstoneos/sources/gpu/board-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/imagination \
  --output ~/.cache/sunstoneos/evidence/gpu-events-local
```

No board-test request has been queued. A physical executable still needs qualified
recovery, board power/memory/interrupt bindings, real resource handlers, response
submission, and workloads that produce these events. Host evidence establishes
decoding and dispatch behavior, while those hardware operations remain required.
