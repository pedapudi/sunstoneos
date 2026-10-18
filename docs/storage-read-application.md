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
a 400 kHz card clock. Startup verifies the reset-held controller baseline,
then performs the existing local controller reset and card initialization. The service adds no post-fault reset, recovery pulses or retry. It exposes
no filesystem, raw command, write, erase, trim or update authority.

## Admission binds the entire lifetime

The 880-byte version-two session is hashed and privately sealed in EL3 before
provider construction. Its 592-byte board prefix selects workload 2: shared PMIC
and USB0 services. The storage suffix binds the MMC owner and authority, five
evidence hashes, exact inherited and desired pins, voltage mask, bounded
identification/read durations and the native application's sector range.
The complete session and immutable image geometry must match the tested image.

Physical admission requires current-instance evidence for these conditions:

- Exclusive SD0/card ownership, disabled and drained IRQ193, no DMA, command
  queue, encryption, remote client or other clock/reset writer. The initial
  authority selects either established cold state or one bounded CMD0 attempt.
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

## First initialization can establish the card protocol state

A previously unexecuted read-only SD lifetime may select
`initialization_mode:"cmd0-once"` in its initial authority record. This policy
requires actual native SD 3.3 V signaling, no prior failed native SD lifetime,
exclusive ownership, and authorization for one bounded CMD0 initialization.
It requires the existing controller reset baseline checks; it does not assert
that the card is already cold. Both cold-state fields must be absent or false.
The default policy still requires both cold controller and cold card authority.
Maintenance and write admission are unchanged.

The [native platform](../drivers/storage/a733_mmc_platform.c) checks that SDC0's
module clock is gated and its bus/reset bits are clear, and refuses enabled,
pending or active IRQ193 state. It checks the exact inherited PF pin configuration
and actual card presence. It enables the local bus while holding reset for at
least ten microseconds, then verifies reset release. An inherited enabled
controller or incompatible pin route remains a retained refusal; this policy
adds no takeover sequence.

The service also accepts the exact inherited PF mux word `0x0f3f3f33` for
PF0–6. The pinned A733 manual's `PF_CFG0` table and real-silicon kernel pinctrl
table decode PF0/1/3/5 function 3 as JTAG-MS/DO/DI/CK; PF2/4/6 are disabled.
This pattern requires `cmd0-once` initial authority with both
`rom_jtag_handoff_authorized:true` and `jtag_debug_clients_excluded:true`.
The pattern identifies the route; it cannot establish that debug clients are
absent. Other alternate-function patterns remain refused.

The shared banked pin manager compares every inherited mux, pull and drive
field before writes, reserves all seven pins, disables every mux, updates
pulls/drives and then enables the desired SD route with readback. The controller
remains gated and held in reset through pin handoff. An inherited-field mismatch
performs no pin writes; an uncertain write faults the pin manager and retains
ownership. The service does not restore JTAG or release pins after uncertainty.
Actual pulls, drives, supplies and signaling still need board-specific authority.

The [native controller](../drivers/storage/sunxi_mmc.c) refuses an active command,
busy controller/card state or enabled DMA before its software reset. It waits
for reset bits to clear, configures one-bit legacy PIO and checks clock/control
readback. It then sends CMD0 once with zero argument and the initialization
sequence flag. Command completion alone cannot prove card reset because CMD0
has no response. The driver requires CMD8's voltage/check-pattern echo, bounded
CMD55/ACMD41 readiness, CID/RCA/CSD and selection, then CMD13 ready-for-data in
transfer state before exposing reads. Failure retains ownership without another
reset or CMD0 attempt.

The SD Association's [Physical Layer Simplified Specification version 7.10,
sections 4.2.1–4.2.2](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver7.10.pdf)
defines CMD0 as the SD-mode software reset except for cards in Inactive state.
It requires CMD8 before ACMD41 after reset. An Inactive card may therefore fail
this attempt and require subsequent external recovery. This policy does not
admit SPI mode, uncertain 1.8 V signaling, outstanding media writes or a failed
native session. Actual signaling authority remains necessary; the host GPIO
voltage setting alone cannot establish the card's signaling mode.

The pinned A733 manual's section 11.3.8.6 defines `SEND_INIT_SEQ` at command bit
15. The pinned bootloader's `drivers/mmc/sunxi_mmc.c` sets that flag for CMD0.
These are the existing [MMC source references](mmc.md), not runtime dependencies.
The first attempt preserves the medium: it issues initialization/identification
and read commands, with no media write, erase, trim or regulator change. A power
cycle is a recovery action after a retained failure, rather than a requirement
solely to assert cold card state before the first eligible attempt.

## An independent boot-CPU image retains the same service

`sunstone-a733-storage-read-single-core.elf` runs the same MMC owner, native
application client, USB client and shared providers on the boot CPU. Its
standalone CMake entry is `apps/storage-read-single-core`. It uses the physically
established AArch64 SRE FreeRTOS port, EL1 vectors, timer and MMU construction.
The application-specific EL3 entry configures only the boot CPU's GIC interface;
it launches no secondary CPU. Before GIC and provider setup, admission checks
all seven secondary CPUs against the existing OFF status/control/mode baseline.
EL1 preparation consumes its retained result. External authority must exclude every
other CPU power/reset writer for the complete admitted lifetime.

EL3 validates the existing contract and seals the complete session before
provider construction. The copied contract and both session snapshots occupy a
separate section that survives EL1 BSS clearing. Runtime preparation checks the
actual boot CPU, privilege, enabled caches/MMU, admitted startup deadline and
SD0 translation. Device mappings are outer-shareable; the private PIO buffer
remains cacheable and inner-shareable. The diagnostic log shares the correctly
aligned non-cacheable result block.

The secure timer expires at the existing admitted session deadline. Expiry,
exceptions and fatal exits halt with all controller, provider, buffer and lock
ownership retained. They perform no reset or resource release. Recovery requires
whole-board power removal, including all attached power sources. This startup
policy needs coordinator review and physical qualification on the selected
board before execution.

The image has its own immutable startup marker and manifest profile,
`storage-read-single-core-supervised-v2`. The common preparer verifies the actual
ELF entry, read-only marker and service geometry. The existing host still requires
both native reads and every requested USB range. This image supports independent
peripheral qualification; all-core FreeRTOS support remains required.

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

Host success requires both USB range completion and the native application's
successful completion of two reads. After the USB ranges finish, bounded status
queries wait for that native result within the existing capture deadline.
A native failure stops further requests; an incomplete count cannot establish
success. This host check reuses the preserved image and launcher.

The exact ROM pin handoff passes focused actual-provider composition checks for
identification/read, inherited mismatch, active module clock, uncertain pin write
and unsupported nearby mux. Five host authority cases and the actual native
session/configuration parser also pass. MMIO, card and time effects remain
modeled. The subsequent adaptive-voltage image recompiles only SD platform and its
unique build identity; it reuses all 164 other pin-handoff objects, including
SD admission and native startup. Its ELF hash is
`f9a8d5da96c08fb5d8111c3fd9cc81389a15ca9080255b21184c15d076e9cc95`.
Evidence is under `~/.cache/sunstoneos/evidence/second-board-20261018-3-3/sd-rom-pins/`.
This image retains the earlier storage startup profile; newer CPU/startup
corrections are not included. Physical entry still requires the coordinator's
admission of that exact profile.

PF adaptive mode is accepted only with sensed 3.3 V, selected 3.3 V and the
original mode unchanged throughout live checks. Four focused actual-provider
cases pass for an adaptive-mode read and separate mode, status and selector
loss during a pending read. Each loss retains ownership and the buffer before
further card commands. This adds no voltage writes or switching commands.
Qualified stable supplies, actual signaling and excluded voltage writers remain
required. Adaptive evidence is under
`~/.cache/sunstoneos/evidence/second-board-20261018-3-3/sd-adaptive-voltage/`.
