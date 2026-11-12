# Retained DMA pages for USB class data

The xHCI core supplies non-cacheable DMA pages to native USB serial and camera
consumers. Application data can remain in these pages after transfer completion.
The application releases a lease when it finishes reading the data. This path is
implemented and checked against a modeled controller; physical DMA visibility,
service timing and sustained camera capture remain unqualified.

The OS generic class profile selects `SUNSTONE_USB_HOST_GENERIC=1` consistently
for the core and its header consumers. Its native constructor takes NC backing
from the unified ledger before concurrent device operation. It completes the
read-only EL0 payload alias before publishing the typed parent pool
([allocation and conversion](designs/ledger-nc.md)).
The lease is sized from the device count in
[a733_usb_memory.h](../include/a733_usb_memory.h). A record takes
`4 KiB * (2 + 2 * endpoints)`: two context pages, and one ring page and one
copy-buffer page per endpoint, EP0 included (40 KiB at the default four
endpoints). The controller pages (16 KiB) and eight scratch pages come first,
then the records that fit before the 4 MiB shared data pool, then the rest
after it. `A733_USB_MEMORY_PARENT_FOR(n)` gives the lease for `n` records and
`A733_USB_MEMORY_ARENA_FOR(n)` the backing allocation: the lease plus
the 3 MiB kept for retained JPEG buffers, in whole 2 MiB blocks.
The manifest's `usb.devices` sets 1 to 127 records, defaulting to 24; controller
initialization also checks its actual MaxSlots. Shared OS and TinyUSB objects
use the same generated `SUNSTONE_USB_HOST_DEVICES` value.
`SUNSTONE_USB_HOST_ENDPOINTS` sets 2 to 31 endpoint contexts per device,
defaulting to four. At 24 devices the parent is `0x4fe000` bytes and the ledger
backing is 8 MiB. At 127 devices the parent is `0x904000` bytes (5 records before
the shared pool, 122 after it) and the backing is 14 MiB. Placement is a ledger
result below 4 GiB; OS images reserve no fixed linked USB arena.
CPU-only HID parser state uses cached ledger memory sized by the same count.
`checks/a733-xhci-dma.py --devices 127` checks controller geometry;
`checks/usb-ledger-memory.py` and `checks/usb-ledger-alias.py` check the OS
allocation, mapping and refusal paths. Every page retains its admitted address
for the lease's lifetime. The parent stays
DEVICE-owned during operation and is never returned to the allocator.

## Bulk and interrupt transfers retain completed pages

The owning task calls `a733_xhci_dma_acquire` with a device and endpoint. A
successful call returns a page pointer, its 4,096-byte capacity and a lease
token. The caller submits the same pointer with `a733_xhci_transfer`. Submission
uses the page's admitted physical address without copying. One transfer may be
outstanding per endpoint. The normal transfer notice supplies the actual byte
count and returns that page to application ownership. The endpoint can then
submit another acquired page while earlier completed pages remain leased.

`a733_xhci_dma_release` refuses a page while hardware owns it. Release also
refuses a token belonging to another endpoint, an already released token and a
page referenced by an outstanding OUT transfer. Release locates the unique
token before checking its recorded owner, so a later device can reuse the USB
address without obstructing release of the disabled device's pages. Tokens come
from a process-wide
monotonic counter that survives controller initialization. Counter exhaustion
retains the session before another token can be issued.

An OUT endpoint may submit a completed IN page from the same device, including
a byte offset within that page. This supports serial echo after removing a
protocol status header without copying the payload. The core keeps a reference
until the OUT completion or proven slot disable. The original IN lease remains
the release authority. A caller must limit the submitted range to valid data;
the core checks that the range lies inside the retained page.

Legacy callers can continue submitting external buffers. Their transfers use
the endpoint's private bounce page and preserve the existing copy behavior.
Full-speed bulk endpoints admit packet sizes of 8, 16, 32 or 64 bytes;
high-speed and SuperSpeed bulk retain their 512- and 1,024-byte packet sizes.
Single-transaction interrupt endpoints support IN and OUT directions.

## Camera packets share the retained page pool

One isochronous IN stream can keep eight service intervals on its ring. Each
interval uses a free shared page. Completed intervals move out of the ring
queue when `a733_xhci_isoch_acquire` returns their pointer, actual byte count and
token. The caller can retain these pages as fragments of a camera frame while
the controller submits later intervals into other pages.

The shared pool also supplies bulk camera transfers. Its 1,024 pages allow a
consumer to retain more than a 614,400-byte VGA YUYV frame even when packets carry
only 1,024 bytes each. Payload headers and actual packet sizes determine the
number of pages available for complete queued frames. Pool exhaustion applies
backpressure without overwriting leased bytes. Release makes a page available
for a later submission. Dropped isochronous intervals carry no application lease.

The isochronous open call accepts raw USB `wMaxPacketSize`, including its
high-speed transaction count. Full-speed service payloads can contain up to
1,023 bytes. High-speed service payloads can contain three 1,024-byte
transactions. The endpoint context records the packet size, burst count and
complete service payload. Each isochronous TRB describes one service interval
and records the last burst's packet count. The core retains the existing
100 ms bound on unexplained queued-ring inactivity.

## Disconnect stops DMA before page reuse

Disconnect immediately revokes legacy caller destinations and stops new
submissions. Endpoint stop/reset and Disable Slot retain their matched-command
checks and five-second hardware bounds. A successful Disable Slot proves that
the controller can no longer access the device's pages. Pages still queued to
hardware are discarded at that point.

Application leases survive Disable Slot. The disabled device record and its
pages remain reserved until the application releases every lease. Waiting for
these releases has no hardware timeout and does not block another device's
normal traffic. The core clears the record's pages only after its final release.
A lost attachment generation, stale event, failed command or hardware deadline
retains the entire parent. No uncertain controller state permits page reuse.

## Software checks establish ownership behavior

`checks/a733-xhci-dma.py` compiles the actual core with address and undefined
behavior sanitizers. Its model follows submitted command and transfer rings,
updates output contexts and writes the physical destinations named by TRBs.
It checks direct serial echo, shared-pool exhaustion, stale tokens, generation
loss, bulk and isochronous releases after Disable Slot, full-speed bulk,
interrupt OUT, control STALL recovery, all occupied device slots and an owner
interrupt service that leaves completions queued until its explicit drain.

The isochronous case retains 700 short packets totaling 716,800 bytes while
wrapping the transfer and event rings. It checks the high-speed three-transaction
context and TRB fields and verifies that every retained packet stays intact.
The disconnect case holds application data for six seconds after Disable Slot,
then releases it and verifies record reclamation. The existing detach fixture
continues to cover command failures, disconnect races, hierarchy order and
stale events. These checks model register and DMA effects; they establish no
physical throughput or recovery result.

The implementation follows xHCI 1.2 sections 4.8.2.3, 4.8.2.4, 4.11.2.3 and
6.4.1.3, together with the pinned Linux `xhci-ring.c` isochronous packet-count
and endpoint-recovery logic. Their URLs, revisions, hashes and licenses remain
in `sources.lock.json`. Linux is a development reference; FreeRTOS remains the
runtime OS. The core does not link Linux driver code.

The default legacy profile keeps its 128 KiB parent, three device records,
seven endpoint ring/bounce pairs and one copied 512-byte isochronous stream.
Borrowed-page APIs refuse this profile. Run the DMA check with `--profile legacy`
to exercise its copied isochronous path and confirm borrowed-page refusal.

Descriptor lookup selects the active core record when a USB address is reused.
An old record may remain reserved by a completed application lease after
Disable Slot. Its release stays bound to the old token; it cannot shadow the
new attachment's descriptor state. The adapter regression covers either record
order and refuses lookup when every matching record is detaching.
