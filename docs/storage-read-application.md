# Native read-only microSD service

`sunstone-a733-storage-read.elf` identifies an inserted SD card on SD0 and
serves bounded reads from two fixed native client tasks. One MMC task owns
all controller and provider calls. A native application task reads its sealed
configured range twice, verifies the copied digest and acknowledges each
result. The USB task accepts host-selected ranges through the same native API.
The application runs under a supervised RAM-only lifetime with manual whole-board
power-removal recovery. It does not arm a watchdog or authorize media writes.

The service uses the existing MMC platform and controller, common clocks, PF
pins, rails, PMIC, register mutex and memory services. SD0 uses PF0–6, one-bit
legacy mode and active-low detection on PF6. Its 800 kHz module clock produces
a 400 kHz card clock. The existing cold setup includes its local controller
reset. The service adds no post-fault reset, recovery pulses or retry. It exposes
no filesystem, raw command, write, erase, trim or update authority.

## Admission binds the entire lifetime

The 880-byte version-two session is hashed and privately sealed in EL3 before
provider construction. Its 592-byte board prefix selects workload 2: shared PMIC
and USB0 services. The storage suffix binds the MMC owner and authority, five
evidence hashes, exact inherited and desired pins, voltage mask, bounded
identification/read durations and the native application's sector range.
The complete session and immutable image geometry must match the tested image.

Physical admission requires current-instance evidence for these conditions:

- Exclusive cold SD0/card protocol state, disabled and drained IRQ193, no DMA,
  command queue, encryption, remote client or other clock/reset writer.
- A private PIO buffer with identity-mapped, cacheable, inner-shareable RAM,
  no aliases and permanent reservation.
- Qualified SD slot wiring, PF routing, pulls, drives, 3.3 V domain and detection.
- Qualified DCDC1/DC1SW2 3.3 V and CLDO5 1.8 V supply topology and settling.
- No card substitution during the admitted lifetime, one supervised attempt,
  full power-removal recovery and retention after uncertainty.

The PMIC service reads programmed supplies through its retained rail token.
These observations do not prove wiring, measured voltage or settling. The model
uses explicitly nonphysical records; it cannot authorize board execution.

The owner checks current provider reservations and the actual detect input
while idle and before reads. Detected removal permanently revokes both clients.
The existing card API copies cached CID/CSD/capacity after those checks; it does
not reread identity. An unobserved removal and replacement between observations
cannot establish that the original card remains inserted. Hotplug recovery and
card substitution remain unsupported modes.

## Two copied client slots share one controller buffer

Image startup binds the actual USB task and native application task to two
fixed slots. The public API is `apps/storage-read/storage_read_service.h`.
Each client may retain one copied request or unread result. A request contains
a first sector, one to eight 512-byte blocks and a monotonically increasing
client request ID. The service assigns a distinct increasing ticket. Slot
ownership also binds the admitted epoch and generation.

Submission checks the complete range using subtraction from decoded capacity.
It checks sector or byte command addressing and rejects overflow before any
provider read call. Descriptors and output spans must exclude private service,
provider and immutable session storage. Client pointers are never retained.
Only the MMC owner advances platform/controller state. It selects the oldest
queued ticket and reads through one fixed 4096-byte PIO buffer.

A complete read requires controller integrity, idle/FIFO checks, any automatic
CMD12 completion and final CMD13 card status. Only successful actual `take`
returns the core to READY and releases its borrowed buffer. A separate owner
step copies the verified bytes and digest into the selected client's result
slot before another request can reuse PIO memory. The other client's completed
result remains immutable while the controller serves a different range.

A client acknowledges its exact successful ticket only after consuming its
copy. The native application verifies the copied digest and acknowledges it.
USB acknowledges only after TX callbacks account for every queued result byte.
Only the slot owner can acknowledge or target its ticket for cancellation.
No dynamic registration or general storage framework is involved.

## Deadlines and uncertainty retain ownership

Each request deadline begins at submission and includes queue time, controller
work and unread completed output. Expiry of any queued, active or completed
request permanently revokes the entire service lifetime. Cancellation of a
matching queued or active/completed ticket also revokes both clients. This
conservative policy preserves another client's active borrower when one queued
request is canceled. A clean acknowledgement permits another request; a fault,
removal, cancellation or deadline never permits post-fault buffer reuse.

The owner retries only explicit BUSY metadata serialization or manager
bookkeeping. It does not retry card operations. Platform retention makes no
controller reset, release or recovery call. An uncertain MMIO access can retain
the common register mutex and stop USB as well as storage. Owned RAM retains
diagnostics, but an error transfer is not guaranteed. Deadlines cannot recover
a stalled MMIO instruction. Every supply must be removed before resource reuse.

## USB records preserve request and transfer identity

Host commands form a bounded byte stream. Byte zero requests a copied status
or result. Byte one irrevocably cancels the entire session. A read command is
32 little-endian bytes: opcode 2, version 2, size 32, block count, request ID,
first sector and zero reserved field. Opcode 3 uses the same geometry with a
ticket at offset 8 and the other argument fields zero to cancel that ticket.
The adapter assembles fragmented commands without borrowing endpoint buffers.
The host permits one command/response transaction at a time.

The 288-byte version-two header contains status, epoch, generation, sector
digest, cached card identity/capacity, controller facts, build ID, ticket, first
sector, request ID, block count and client slot. It also reports the native
application's result and completed read count. Status values are pending 0,
success 1, retained lifetime 2, idle client 3 and rejected request 4. Only success
appends the requested sector bytes, up to 4096. The observer checks the exact
request/client/ticket/range and digest; it makes no filesystem-validity claim.

USB uses its own copied staging buffer. Submission of bytes does not prove
transfer completion. A partially queued frame keeps its buffer and byte counters
until actual completion callbacks account for accepted bytes. Revocation abandons
unsent bytes. Disconnect or suspend permanently revokes the lifetime and retains
staging state; remount cannot revive it. Already accepted bytes cannot be
retracted. A canceling host must reject that lifetime and discard partial frames.

## Build and validation

Enable `SUNSTONE_A733_STORAGE_READ` and build `sunstone-a733-storage-read` with
the pinned board dependencies. The target inherits supervised boot and shared
providers and includes the actual MMC platform/native/core. Use the accepted
Make generator and toolchain. Freeze the final image and complete compiler
closure before executing focused checks.

`apps/storage-read/prepare.py` seals a new session, range, epoch and generation
from the actual tested image and reviewed authority. The host adapter,
`tools/check-a733-storage-read-supervised.py`, captures one to four requested
ranges; its default plan reads sectors zero and one. `--read FIRST:BLOCKS` selects
each range. It preserves fragments and complete records before further I/O and
rejects stale or mismatched results. Queue, board identity, lock and manual
recovery requirements remain in the existing supervised tooling.

The affected native image and eleven new composition cases pass. Host checks
also pass for repeated range capture, fragmented durable records, stale-result
refusal and the version-two producer/package. The compiled closure contains
166 objects and 589 inputs. Evidence is under
`~/.cache/sunstoneos/evidence/storage-read-service-20261018/`.
The checks execute actual linked application and MMC code; scheduling, shared
provider returns, card/MMIO and USB completion are modeled. Physical card reads,
faster SD modes, wider buses,
eMMC, UFS, filesystem operations, updates and watchdog recovery remain unqualified.
The coordinator alone may prepare and execute a reviewed physical request after
recovery of the quarantined board instance and establishment of current authority.
