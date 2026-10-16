# GPU free lists retain backing memory through growth and recovery

The native [free-list manager](../drivers/gpu/pvr_free_list.c) constructs GPU page
lists, handles growth and reconstruction requests, and submits their firmware
responses. A free list supplies physical pages to the GPU's parameter memory
allocator. Host checks cover the manager with the real event dispatcher and
command queue. Physical GPU operation remains unqualified.

This document assumes familiarity with memory ownership and firmware queues.
Platform allocation, protected GPU mappings, render-target recovery, and lifecycle
worker integration still need concrete board bindings. The manager exposes those
operations through [bounded callbacks](../include/pvr_free_list.h).

## Initial construction publishes owned pages before exposing the list

Each configured list has a firmware ID, an application owner, a platform resource
token, a GPU table address, and a firmware record address. The configuration also
specifies initial, maximum, and growth page counts plus a reserve percentage.
The platform supplies private metadata for every possible backing page before
initialization. IDs, tokens, mappings, and metadata remain pinned for one firmware
epoch, which identifies a single device initialization.

Page counts are multiples of four. Table addresses align to sixteen bytes;
firmware records align to eight bytes. Configured GPU table ranges and firmware
record ranges must be disjoint. The caller supplies the minimum page count
from verified device features. The pinned driver selects 25, 40, or 50 pages;
alignment may increase the smallest valid configured count.

Initialization validates configuration and metadata without callbacks or writes to
supplied metadata. Polling then clears the table, acquires pages, clears each
4 KiB page, publishes that page, inserts physical page numbers, and publishes the
64-byte firmware record. The GPU cannot use a list until initial polling completes.

Every allocation grants a distinct lease, writable CPU mapping, and aligned
physical page below the manager's 36-bit physical-address limit. The platform must
exclude firmware, tables, manager state, and all other live allocations from that
backing. The manager independently checks overlap with its state, queue, metadata,
and previously admitted pages. A lease records memory ownership; it remains live
after the firmware accepts a response.

The GPU table holds little-endian 32-bit physical page numbers, obtained by
shifting physical addresses right by twelve bits. Each allocation block occupies
entries immediately before the preceding block. Reconstruction preserves this
ordering. Table capacity equals the configured maximum count, including space
for future growth.

The reserve count is the smaller of the growth count and the total page count
multiplied by the reserve percentage, rounded down to a multiple of four.
Firmware can use those reserved pages while the host services a growth request.
The initial record contains the usable count, reserve count, stack index, GPU
addresses, limits, and firmware ID. With a 100 percent threshold and a growth
count at least as large as the initial count, all initial pages can be reserved.
The pinned source permits this configuration: its usable count is zero and its
unsigned stack index is `UINT32_MAX`. The manager preserves that behavior.

Layout and accounting follow the pinned board driver's
[free-list implementation](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_free_list.c)
and [firmware ABI](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_rogue_fwif.h).
Their license notice remains in [LICENSE.pvr](../drivers/gpu/LICENSE.pvr).

## Event admission copies a complete action before returning

Use `pvr_free_lists_event` for the event dispatcher's grow and reconstruction
handlers. Admission validates every resolved resource against the manager's
immutable configuration. It copies the decoded request, resolved list indexes,
and deadline into an eight-entry queue. The callback performs no allocation,
shared-memory operation, render-target recovery, or firmware submission.

Returning success means the manager owns the pending action. Completion requires
subsequent polling. A full action queue or mismatched resource returns failure;
the control worker must retain the event and fail its service. The manager stops
when the command queue enters failure or retirement.

One privileged worker serializes the manager, event dispatcher, control worker,
and command queue. Callbacks cannot reenter them. Poll the manager after control
polling so both components reuse the command queue's recorded return statuses.
Use the same monotonic microsecond clock throughout. Polling rejects time older
than either the manager or command queue has observed. Pending actions have fixed
deadlines measured from the manager's last observed clock value at admission.

Each poll performs one bounded operation: a page allocation, a 4 KiB clear,
a page publication, a table chunk of at most 64 entries, a record operation,
a render-target recovery callback, or a response step. Table and record writes
include publication before the next step. Allocation validation compares against
at most eight lists of 16,384 admitted pages each. Callback implementations must
also bound their work; this component makes no latency or throughput claim.

## Growth distinguishes resource refusal from uncertain failure

A growth request consumes the previous reserve in host accounting. The manager
then acquires the configured number of pages, publishes their table entries,
computes the next reserve, and prepares command 110. That response includes the
firmware record address, added count, new total count, and reserve count.

An allocator return of zero guarantees refusal without effects or an output
lease. During growth, this produces the supported zero-growth response.
Previously acquired and published spare pages remain owned and can satisfy a
later request. Reaching the configured maximum also returns zero growth without
calling the allocator. A refusal during initial construction fails initialization.

An uncertain allocator result, invalid descriptor, failed memory operation, or
failed publication stops the manager and command queue. The manager records any
returned descriptor before validation or another callback. The platform also
retains ownership by epoch, resource token, and allocation slot when uncertainty
prevents a complete descriptor. No success or zero-growth response is submitted
for these failures.

The manager has no release or restart operation. Failure preserves leases,
partial table changes, the pending action, and response diagnostics. Reclamation
requires independently verified device isolation and disposal of all leases and
unresolved allocation attempts. Further polling after failure performs no
callbacks.

## Reconstruction restores backing before issuing its response

A reconstruction request resolves every list before admission. The worker rebuilds
page entries from retained allocations, reads the firmware's current page count,
restores its stack index, and clears allocated-page counters. It then requires
`recover_targets` to complete recovery of every linked render-target record.
The callback must set hardware-recovery state and clear the last-geometry flag,
as required by the pinned source. A platform with no linked targets must establish
that fact before returning success.

Only completed reconstruction of all requested lists permits command 112 with
the original count and IDs. Empty requests and repeated IDs retain their ABI
meaning. A recovery failure preserves backing and prevents the response.
The supplied host check simulates render-target completion; a concrete linked
render-target catalogue and its board memory operations remain required.

Response submission retains its action when the command queue has no free slot.
The worker collects the recorded firmware return on a later poll. A failed
submission, expired deadline, or unsuccessful return stops the manager.
Command acceptance does not establish GPU job completion or permit page release.

## Focused evidence checks ABI agreement and retained failures

The [host runner](../checks/pvr-free-list.py) verifies downloaded source hashes,
compiles unmodified reference functions for reserve calculation, record
initialization, and page insertion, and runs AddressSanitizer and
UndefinedBehaviorSanitizer. Reference headers also define the expected firmware
request and response structures.

The check compares complete initial records, page tables, and response bytes.
It exercises successful growth, a partial allocation refusal, reuse of retained
spares, growth at capacity, reconstruction of two lists, and response submission
after all command return slots were occupied. Fault cases cover
uncertain allocation, publication failure, render-target recovery failure,
allocator reentry, and deadline expiry. Every terminal case verifies that later
polling preserves ownership and performs no callbacks. Separate validation cases
cover overlapping GPU tables, a clock older than the queue clock, and an initial
reserve containing every page.

Run the focused check with the pinned source directory and a private evidence
directory:

```sh
python3 checks/pvr-free-list.py \
  --sources /path/to/pinned/imagination \
  --output /path/to/private/evidence
```

Use `--case validation` to run only the configuration and clock cases.
The runner records compiler identity, input hashes, reference functions, binary
hash, and results. Existing command-queue and event-decoder evidence remains
applicable to their unchanged implementations. Board testing requires concrete
allocation and publication bindings, protected table/record mappings, linked
render-target recovery, worker polling, executable firmware, and qualified
independent reset recovery. No board executable or physical qualification is
provided by this component.
