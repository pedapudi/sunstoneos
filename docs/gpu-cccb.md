# GPU client command transport

The native client command transport is **implemented and host-tested**. It
writes command sequences into a context's shared ring and submits a firmware
notification through the existing kernel command queue. Complete ring and
notification bytes match pinned driver functions. Physical GPU execution,
context construction, job validation, and completion fences remain outstanding.

This document assumes familiarity with device drivers and shared-memory queues.
The [interface](../include/pvr_cccb.h) and
[implementation](../drivers/gpu/pvr_cccb.c) implement the client circular command
buffer, abbreviated CCCB in the source. The separate
[kernel command queue](gpu-queues.md) carries firmware control messages,
including the notification called KICK. A successful KICK return means firmware
accepted the notification; it does not establish GPU job completion.

## Format an unattached ring before admitting commands

Initialization records the kernel queue's epoch, a firmware context address,
callbacks, and an absolute deadline without performing I/O. Polling then writes
the 32-byte control record and publishes it. Its write, read, and dependency
cursors start at zero; its wrap mask is the ring size minus one.

The ring size must be a power of two from 256 through 32,768 bytes. The pinned
driver uses 32,768-byte rings for compute, geometry, fragment, and transfer
contexts. This component supports smaller rings for bounded reservations and
focused checks. It preserves the pinned writer's eight-byte command alignment
and 40-byte headers. The shared-header comment describing 16-byte write
alignment differs from the executable writer, which advances by header size
plus an eight-byte-aligned payload.

The platform must keep the owning firmware context unattached during formatting.
Before the first sequence, it must publish and attach the context with the
correct ring and control addresses. Those operations are not supplied here.
The platform also proves backing ownership, disjoint allocations, firmware
address validity, and uncached mappings or equivalent cache maintenance.

## Reserve space and publish each sequence once

Admission copies up to eight trusted command descriptors. Payload lengths must
be multiples of eight, and payload storage remains immutable and mapped until
notification acceptance or independently verified isolation after failure.
Commands must use the published task or generic command identifiers. Semantic
validation of payloads, embedded addresses, dependencies, and permissions
belongs to the future job producer.

Headers and payloads, plus one padding header, must fit within half the ring.
This source-derived limit permits wraparound without a separate padding-only
notification. Before writing, the transport samples the firmware read cursor
and checks the unchanged wrap mask. Cursor progress cannot exceed previously
published bytes. Insufficient room leaves the operation pending without writes.

The writer preserves room for a padding header at the ring's end. When a command
would cross that boundary, it writes a padding command and resumes at zero.
Each poll writes one header or at most 256 payload bytes. Headers contain command
type, payload length, and external/internal job references. Workload-estimation
fields remain zero. Unused padding payload bytes are not interpreted as commands.

A publication barrier completes all ring writes before KICK submission. The
88-byte notification contains the firmware context address, resulting write
cursor, wrap mask, and up to three cleanup-record addresses. A full kernel queue
leaves submission pending without rewriting the ring. After submission, the
transport collects the reserved return slot and requires status one.

One notification may be outstanding per client ring. The privileged owner
continues normal firmware control, event handling, and kernel return polling
while the client transport waits. The client transport does not duplicate that
service work. Separate rings can progress under the same serialized owner.
Combined geometry/fragment notifications are not implemented.

## Failed operations retain their resources

One monotonic microsecond deadline covers waiting for space, writing,
publication, and notification acceptance. Each poll checks time before and
after its bounded step, including after callbacks that finish late. Time cannot
precede the latest observation from the client or kernel queue.

An I/O failure, corrupt cursor, changed epoch, expired deadline, or unsuccessful
notification return latches failure and aborts the shared kernel queue. A failed
write may already have changed shared memory. The transport retains command
descriptors, borrowed payloads, queue tokens, and all associated ownership.
Subsequent polls perform no callbacks. Independent isolation is required before
discarding failed resources.

Successful notification acceptance also does not authorize releasing GPU data,
contexts, or cleanup records. Job completion, device cache visibility, context
cleanup, and resource reclamation require separate protocols. Integration with
the GPU service and physical qualification remain open.

## Focused checks compare real transport bytes

The [host check](../checks/pvr-cccb.py) compiles extracted, unchanged ring-writing,
space, control-initialization, and notification-fill functions from the pinned
driver. Its sanitizer fixture links the native client transport to the actual
kernel queue implementation. Eighteen successive sequences vary payload sizes,
cross ring boundaries, and compare complete ring and notification bytes.

The same fixture checks empty payloads, client-ring pressure, a full kernel
queue, delayed returns, and the distinction between cursor progress and command
acceptance. Focused faults cover malformed cursors, writes that take effect
before failing, late completion, clock regression, changed epochs, unsuccessful
returns, and terminal refusal. Invalid admission preserves state and performs
no I/O. Guard bytes remain intact.

The first build exposed a host-header macro collision in the reference shim.
After that fixture correction, the sanitizer check passed. Clang 21 also
compiled the component for freestanding AArch64 with strict alignment. The
object has 2,976 bytes of code/read-only data and no writable static data.
The largest direct frame is 96 bytes; called functions add their own frames.

Evidence is under `~/.cache/sunstoneos/evidence/gpu-cccb-20261016/`.
`results.json` records the host inputs and result; `target.json` records the
target command, inputs, object identity, and frame sizes. The reference uses
[pvr_cccb.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_cccb.c)
and associated headers checked against the cached source manifest. Existing
GPU, emulator, board, and mimus checks were not repeated. No performance
measurement or physical request was made.
