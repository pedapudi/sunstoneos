# Native SPI3 command-response transfers through DMA0

The supervised SPI application will send one authenticated command of 1–16 bytes
and receive exactly 4096 bytes under one CS0 assertion. Its existing owner task
will control SPI3 and two DMA0 channels. The existing polling and FIFO interrupt
profiles remain available. No peer opcode or electrical compatibility is assumed.

This design targets the current native gap at `78c36b2b`:
`apps/spi/native.c` constructs only a CPU-buffer WRITE_READ transfer, while
`a733_spi_platform_interrupt_start` explicitly rejects DMA. The SPI core already
implements DMA request gating and terminal wire checks. The DMA controller,
startup adapter, native memory policy, and allocator already provide the required
channel ownership and cache operations. No common DMA, power, or memory change is
proposed.

## Source authority and sequence

The pinned Allwinner Linux revision is
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Its cached
`bsp/drivers/spi-ng/spi-sunxi.c` SHA-256 is
`ea68e8672fe8e0c963605faf8892022c12bca25597f91953051da8ebfbfd2d47`;
`spi-sunxi.h` is
`ba5ef54e3e97f9306f68d8fce1196adaf9cf499dbd2ddf2b82dc66c28f2aa722`.
Both match `sources.lock.json`.

The reference's `sunxi_spi_config_dma_rx` and `sunxi_spi_config_dma_tx` select
FIFO addresses at controller offsets `0x300` and `0x200`.
`sunxi_spi_xfer_master` starts the required DMA directions before exchange.
Its RX callback checks FIFO emptiness. Linux uses the DMA RX callback instead
of SPI transfer-complete notification for DMA reads. This application will
require both the SPI notification and independently verified DMA completion;
neither event alone publishes output.

A733 manual V1.00 table 13-2 assigns request 26 to SPI3 RX and TX. Sections
18.8.8.15–17 define normal DMA handshake `0xe5` and byte-accessible FIFO ports.
The current SPI core admits one-byte accesses and single-beat bursts, enables
TX request bit 24 and RX request bit 8, and verifies their readback. This
extension uses those existing bounds; it does not expand burst or lane support.

The board route remains PE0 CS0, PE1 clock, PE2 MOSI, and PE3 MISO, function 5.
Board schematic V1.2 sheets 2, 9, and 18 establish the route and BLDO1 1.8 V
supply. The existing read-only PMIC snapshot, four-pin lease, alternate-route
exclusion, peer/translator evidence, and exact command-effect authority remain
mandatory. The fixed wire profile stays mode 0, MSB first, active-low CS,
500 kHz, with minimum 2 microsecond setup and hold intervals.

## One image profile and an authenticated DMA handoff

A build option in `cmake/a733-spi.cmake` selects the DMA application. Its
session version 3 extends the existing 832-byte record with a 128-byte,
pointer-free DMA authority record, for 960 bytes total. Polling version 1 and
FIFO IRQ version 2 retain their existing record and output layouts. The DMA
image accepts only version 3; existing images reject it.

The extension uses the field layout already implemented by the UART DMA
application: fabric words and rates, expected/desired CPU and MCU channel
routing, allowed route changes, cold/adopt selection, owner, handoff/controller
identities, establishment flags, channel masks, startup duration, and an
authority digest. SPI keeps its own private definition and checks. Fabric facts
must equal the common sealed board session. The DMA owner must differ from the
SPI, PMIC, and USB owners. Controller identity is `0x04601000`, owned channels
are `0xffff`, used channels are exactly `3`, and establishment flags are the
existing supervised `0x5f` contract.

The startup duration is at least 250 ms and no longer than the existing bounded
operation duration. Both absolute deadlines derive from the admitted session
start, never from successive polls. Actual platform validation checks source,
rate, and routing combinations before any allocation or provider access.
All-channel ownership is required by the existing startup adapter even though
this application transfers only on channels 0 and 1.

Host preparation binds the new DMA authority evidence to the exact image and
target, verifies its routing and memory facts, and freezes it with the session.
The initial peer record must authorize the exact command and 4096-byte response.
A larger response is not inferred from authority for the former 32-byte limit.
Preparation and frozen verification distinguish the actual linked DMA image
profile from the FIFO image; a mutable manifest label alone is insufficient.
The private ARM32 launcher, EL3 session seal, IRQ routing hook, and AArch64
constructor all use the selected session size and version.

## Four private pages and one owner

The application acquires one 16 KiB extent from the existing cached device pool.
CPU and physical bases must agree, every page must have the admitted write-back,
inner-shareable attributes, and the entire extent remains DEVICE-owned for the
supervised lifetime. The allocator and shared memory map need no extension.

| Offset | Length | DMA role | Use |
| --- | --- | --- | --- |
| `0x0000` | 4096 | descriptors | One RX descriptor for channel 0 |
| `0x1000` | 4096 | descriptors | One TX descriptor for channel 1 |
| `0x2000` | 4096 | device writes | Exactly 4096 received bytes |
| `0x3000` | 4096 | device reads | Authenticated command; remaining bytes zero |

Each descriptor allocation has one 64-byte descriptor and private padding.
Each transfer has one segment and one payload mapping. Descriptor addresses
must remain below the existing `0xfffff800` boundary, and data addresses below
`2^35`. The native policy admits only these four whole pages and the two exact
FIFO endpoints: RX `0x02543300` and TX `0x02543200`, request 26, width 1, burst 1.
Driver state, stacks, sessions, page tables, and USB staging remain outside them.

The existing SPI task is already privileged. It will bind the existing DMA
startup adapter and directly serialize the two-channel DMA controller. No DMA
service worker, queue, extra task, or generic bus service is needed. The DMA
adapter checks the immutable allocation, owner task, live startup reservation,
channel/address allowlist, and common revocation state. DMA0 INTID 143 stays
masked throughout, although the channel event bits remain enabled for polling.
Read/write/cache/barrier/ticks callbacks cannot report errors to the controller;
any callback failure enters a nonreturning retained-failure path and closes
output before further DMA access. COMPLETE and a released transfer pointer
therefore cannot override a callback failure. Complete policy and
translation checks occur at construction/submission, not per payload byte.

## Completion requires wire idle, gated requests, and both drained channels

1. Validate and seal the complete session before provider construction. Reserve
   BLDO1 and verify its existing 1.8 V snapshot. Prepare SPI3 and DMA0 through
   their current clock, reset, pin, mapping, and routing providers.
2. Claim RX channel 0 and TX channel 1. Submit RX first, then TX, while both SPI
   DMA requests are confirmed disabled. Require both channels ACTIVE before
   SPI submission. The controller publishes source/descriptors and prepares
   the destination cache lines through its existing callbacks.
3. Submit DMA WRITE_READ with null CPU pointers and exact byte counts. SPI
   asserts CS, enables the requests, observes setup time, and starts exchange.
   Extend only the SPI platform's interrupt-start admission to permit a valid
   active DMA transfer; preserve the FIFO preload/count checks for CPU transfers.
4. Arm the existing CPU0 SPI3 interrupt route. ISR work remains mask and notify.
   A real notification is mandatory. The owner uses a bounded fallback wait to
   observe DMA errors and the absolute deadline without processing FIFO bytes.
   `a733_dma_finite_state` observes each channel without acknowledging events
   or stopping it while SPI requests remain enabled.
5. After the ISR route is masked and inactive, the existing interrupt poll path
   disables controller interrupts and checks transfer-complete, errors, XCH,
   empty FIFO/internal buffers, and CS hold. The SPI core gates both requests
   and verifies them off before deasserting CS and reporting COMPLETE.
6. Only after confirmed request shutdown, poll both DMA channels through their
   existing stop/drain path. Each must return COMPLETE, restore its saved
   descriptor, pass quiet checks, and release its transfer pointer. The native
   adapter invalidates RX storage before that result. Require both ownership
   predicates false before any CPU RX access. Reap SPI with a fresh remaining
   operation deadline, copy the response, hash it, and publish while still live.

Any timeout, missing IRQ, DMA error, uncertain write, lost authority, or failed
drain closes the output gate and retains all allocations and resource tokens.
Fault handling masks the SPI interrupt and retries only retention metadata.
It does not issue a speculative SPI stop, cancel a DMA channel with requests
possibly enabled, or release resources. The existing SPI fault path may retain
the common register mutex, so later DMA cleanup is not assumed available.
Hardware may remain uncertain until the separately reviewed board and peer
recovery. Successful completion also retains channel claims, clocks, pins,
parent allocation, and the peer lifetime; it adds no reuse authority.

## Copied result and bounded costs

The DMA wire header remains 128 bytes with a distinct version. Its 4192-byte
payload has 32 bytes of ordinary observation fields, 32 bytes of padded exact
command, 32 bytes of DMA completion/drain facts, and 4096 received bytes. The
host checks mode, byte counts, command, actual IRQ notification, both completed
channels, request shutdown, drain, session digest, and payload digest.

USB copies the immutable payload into its existing private response storage.
The host assembles the 4320-byte frame across short reads under one absolute
capture deadline. Pending and failed frames remain header-only. One request
fetches one complete response; fragmentation cannot restart the deadline or
publish a truncated result.

Design budgets, to be measured in the focused compiled checks:

- One 16 KiB pool allocation; no per-transfer allocation or extra task stack.
  Total added pool and static storage stays below 48 KiB, including DMA state,
  the 4192-byte payload, and the 4320-byte USB response. Text growth stays below
  32 KiB relative to the current SPI image.
- One command copy of at most 16 bytes into DMA RAM. One 4096-byte RX copy into
  the payload, then one 4192-byte payload copy into USB staging. No intermediate
  RX buffer and no per-byte CPU FIFO access.
- Five page-sized cache operations on a successful transfer: two descriptor
  cleans, one TX clean, RX clean/invalidate before DMA, and RX invalidate after
  verified drain. No cache operation covers unrelated storage.
- At most one finite-state observation per channel per owner wake. During wire
  activity, SPI IRQ wakes the owner and a maximum 10 ms fallback bounds DMA
  fault observation. Startup, IRQ-route deferral, and drain advance at one tick
  per step. Drain is bounded by 100 ms and the remaining operation deadline.
- At 500 kHz, 16 command bytes plus 4096 response bytes require 65.792 ms of
  serial clocks, excluding gaps and setup/hold. This is arithmetic, not measured
  throughput. Count register accesses, wakeups, cache bytes, and copies in the
  model; report physical timing as unqualified.

## Focused validation and ownership

Build one DMA image with the accepted native toolchain. Use existing SPI linked
fixtures, extended to execute the actual application, SPI platform/core, DMA
controller, startup binding, native mapping/cache callbacks, and copied output.
Model only hardware effects, scheduling, and unavailable peer/physical authority.
Cover success, delayed RX completion after the SPI event, missing IRQ, DMA error,
failed drain, and output revocation. Assert no request enable before both ACTIVE
channels, no DMA stop before request shutdown, and no RX copy before both drains.

Add cheap admission cases for wrong channels, routing, fabric, memory extent,
request count, exact command, truncated authority, and mismatched image profile.
Exercise actual offline prepare/freeze/verification and fragmented host capture.
Check the private launcher/session/IRQ-route boundary on the same image. Reuse
existing FIFO polling/IRQ evidence; run one narrow preserved-profile compatibility
case for the changed interrupt admission instead of repeating the old suites.
Reuse source-table, descriptor encoder, DMA controller, and native cache evidence
where their production inputs remain unchanged.

Owned changes are `apps/spi/`, `cmake/a733-spi.cmake`, the narrow SPI platform
interrupt admission and its header/documentation, existing SPI checks, and this
design. No common DMA, power, memory, storage, radio, board-status, or queue file
is included. Source and compiled artifact closure will use the existing evidence
snapshot tool. No board access is planned; an identified 1.8 V-compatible peer,
physical interrupt/DMA/cache behavior, and recovery remain unqualified.
