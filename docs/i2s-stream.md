# I2S streaming uses bounded DMA storage

The stream driver supports receive, transmit and simultaneous operation on I2S0,
with two to eight equal periods per direction. Finite mode keeps I2S active while
software drains one DMA transfer and arms the next; FIFO storage covers that
interval. Repeated native modes use fixed hardware-linked rings and private
capture staging. Physical continuity and final serial wire drain remain
unqualified. Cyclic capture also requires the device ordering contract described
below before physical admission.

The native profile uses the existing 24.576 MHz audio parent and 16 kHz framing
with two 32-bit wire slots. The format can select one or two channels and 16-bit
or 32-bit containers. A 24-bit microphone uses the documented raw 32-bit capture
format: alignment, sign and unused bits follow the microphone contract. No
sample conversion occurs here. No microphone or playback device has been
physically qualified.

## One worker owns the controller and period catalogue

`dma_service_init` copies the stream profile, segments and mappings into private
storage. A stream job contains one mapping per period and a separate descriptor
allocation with one 64-byte slot per period. All allocations remain reserved for
the service lifetime. The native memory policy must admit their exact extents
and FIFO routes; access to the complete shared allocator pool is insufficient.

Stream OPEN reserves the entire catalogue against every overlapping job,
including DMA readers. Playback periods are writable by the application between
GET and PUT, so ordinary read-only DMA sharing cannot apply to them.
Single-direction profiles use the same service instance and fixed DMA channel
as finite capture. A duplex service admits one compatible receive/transmit pair
on distinct DMA channels, with disjoint descriptor and payload allocations.
Both profiles use the same format, callbacks and platform binding. The worker
dispatches each channel exclusively through the stream core.
It never polls the DMA controller independently of the I2S producer.

Applications receive copied replies from the service queue. In finite mode,
PERIOD_GET grants one period and a fresh generation ticket. PERIOD_PUT consumes
that ticket once:
it returns capture storage or commits immutable playback data. Applications
finish all memory access before queuing PUT. The service refuses stale or
repeated tickets. Reopening a cleanly closed session preserves the generation
sequence. Task abandonment retains its periods and reservations indefinitely.

Finite capture publishes a period only after DMA completion, verified drain, cache
acquisition, live resource checks and the final deadline check. A client-held
period cannot be overwritten when the ring wraps. Missing free capture storage
stops the stream with a failed result. Finite playback requires two committed periods
before start; missing committed storage at a later boundary stops it as well.
The driver neither inserts silence nor silently drops samples.

TX_MEMORY_CONSUMED means that DMA has finished reading the segment and can no
longer access its allocation until the client commits it. It does not prove that
those samples have left the serializer. CANCEL disables the direction and may
truncate queued wire output. Final-wire completion requires a separate bounded
serializer-drain design.

## FIFO occupancy bounds the handoff

The pinned manual defines a 64-entry receive FIFO and a 128-entry transmit FIFO.
Each entry is a sample word. Receive availability is FSTA bits 6:0; transmit
empty space is bits 23:16. The driver uses receive threshold eight and transmit
empty-space threshold 64. It preserves the other FIFO control fields.

At a finite-transfer completion, the driver disables the selected DMA request
and verifies the register before asking DMA to stop. The receiver or transmitter
continues running. It samples FIFO capacity and computes a handoff deadline from
sample rate and channel count, withholding one sample as margin. FIFO errors or
a missed handoff deadline stop the direction and drain DMA without publishing
that affected period as successful audio. A delayed DMA drain does not release
storage merely because the hardware completion event appeared.

This mechanism requires bounded scheduling, register access and DMA drain on the
real board. A two-core register model cannot establish that those bounds hold
at 16 kHz. Physical timing and FIFO continuity remain qualification requirements.
No performance measurement is included in the model checks.

Period records carry a sequential frame offset, host observation times and the
raw direction counter. RXCNT counts samples entering the receive FIFO. TXCNT
counts writes into the transmit FIFO. The observation interval brackets the
completion handling; it is not a wire timestamp or a microphone sampling-time
calibration. Counters may include samples outside the completed period and wrap
at 32 bits.

## Platform ownership remains live through teardown

The stream platform binding uses the actual common audio lease adapter and pin
manager. It authenticates exact token owner, generation and scopes for the
reference parent, stable audio output and local I2S controller. It checks the
pin lease against the manager before admitting operations. Opening and closing
perform the corresponding checks for resources already acquired or still held.
A duplex open acquires PB5, PB6, PB7 and PB8 together. The second direction
joins that binding before either direction starts. Direction updates preserve
the peer's CTL enable and ICTL request bits. Closing either direction releases
its drained DMA channel while the shared binding stays held until the other
direction closes. The closed stream object cannot reopen during that interval.
Uncertain failure marks both streams for recovery and prohibits subsequent
device access, including an idle peer. A busy observer permits no device access
and preserves the original deadline.
Expired or uncertain authority retains the session.

The platform uses PB5 for BCLK and PB6 for LRCK. Capture uses PB8 function 3,
`i2s0_din0`; playback uses PB7 function 3, `i2s0_dout0`. PB4 function 3 optionally
supplies MCLK. The shared pin manager excludes conflicting alternate functions
and GPIO users. Bank voltage does not change. External device voltage, routing,
clock requirements and codec initialization remain separate prerequisites.

Every lease query precedes register-mutex entry. Register access and readback
occur under the shared mutex. A failed readback or deadline reached before
unlock preserves the actually held mutex and all tokens. A deadline first
observed after a successful unlock retains tokens and terminal failure; it does
not claim that the mutex is still held. Terminal recovery makes no further
device calls. Stop disables new FIFO requests, stops the direction, and proves
DMA drain before any allocation can be returned or reused.

The parent, audio lease adapter, pin manager and local binding have platform
lifetime and one serialized owner. Their register callbacks must use the same
shared mutex and monotonic clock history. Cooperative privileged software must
preserve this binding; these checks do not establish isolation from hostile
privileged tasks or exclude remote firmware by themselves. Native DMA routing,
stable clocks, actual memory reservations and independent reset recovery remain
mandatory platform prerequisites.

## Source basis and focused evidence

Register definitions come from the pinned A733 manual, section 17.1.8, and
`snd_sunxi_i2s.c`/`.h` in the audio reference cache. PB7 and PB8 signal names come
from the pinned board pin-controller source. The manual's initialization prose
says to write zero to flush a FIFO, but its register table defines write-one,
self-clearing flush bits. The driver follows that register table and the source
implementation.

`checks/i2s-stream.c` composes the real FreeRTOS service, DMA controller, native
memory/cache adapter, audio lease adapter, pin manager and shared register mutex.
Its device registers and sample data are modeled. The selected cases cover
multi-period capture and playback, client-held period ownership, stale tickets,
read-alias exclusion, stop ordering, cache publication, busy authority and late
publication retention. Existing finite-capture and DMA cases supply unchanged
lower-layer evidence; they are not replayed by this target.

The private CMake fragment adds the new service dependencies to the existing
DMA service and startup targets. Those targets require recompilation because
private service storage has grown; their established scenarios do not require
another execution solely for that layout change.

The duplex compositions in `checks/i2s-stream.c` use the same real providers and
modeled FIFO/DMA registers. They account for five completed periods in each
direction, ring reuse, incompatible pair refusal, catalogue overlap refusal,
independent DRQ control, delayed cancellation drain, both close orders and
failure retention with an idle or active peer. Results and exact inputs are
preserved under `~/.cache/sunstoneos/evidence/i2s-duplex-20261018/`. These checks
establish software behavior; physical audio timing and operation remain
unqualified.

## Hardware-linked repeated streaming

Native session directions 3, 4 and 5 use one fixed four-descriptor ring per
direction. The final descriptor links to the first descriptor. Capture and
playback use separate channels, catalogues and memory. Finite directions retain
their completed-period API. The existing paired provider keeps shared clocks and
pins until both directions have stopped and relinquished their client leases.

Playback commits every source period before starting DMA. The raw source stays
immutable until cancellation and the existing controller quiescence protocol
complete. `TX_REPEAT_PROGRESS` reports cumulative completed memory transfers,
possibly several periods at once. It grants no CPU lease and needs no PUT.
This plays the embedded recording repeatedly; it does not support live refills.

Capture keeps all four raw periods reserved for DMA. Each period occupies
separate cache lines. Initial publication cleans and invalidates them; later CPU
access only reads them. The worker copies a completed period into a private
4,096-byte staging buffer. It invalidates the raw range before and after copying,
using the existing native INVALIDATE operation without cleaning DMA-written data.
No raw pointer reaches the application. `CAPTURED_COPY` grants the immutable
staging copy until its exact ticket is returned. The application copies staging
into its existing four USB slots before returning it.

For absolute period `k`, extended completed-package count `C` and ring length
`N`, acquisition requires `k+1 <= C < k+N`. After acquisition it again requires
`C < k+N`. Stable counter, next-descriptor and destination/remaining-byte readings
must agree with the current period. A transition defers acquisition; reuse or an
ambiguous postcheck discards staging and stops the stream. A held staging copy
survives raw-ring overrun. A full USB ring stops capture without overwriting any
unread copy. USB slots still require complete transfer delivery and exact ACK.

The worker extends the 32-bit package counter with checked arithmetic. Progress
observations must remain less than one second apart. The configured sample rate
bounds plausible cumulative progress, allowing 128 FIFO frames. This detects
counter jumps and rejects histories that could hide a wrap. Missing progress,
FIFO errors, unexpected queue end, reuse and consumer backlog stop or retain the
stream through the existing cancellation protocol. Stop samples final DMA
progress before relinquishing resources. A rejected initial counter enters
RECOVERY and never contributes frames. Canceling before arm preserves a held
`TX_FILL` lease and reports no stale progress.

### Documented completion and inferred ordering

The pinned A733 User Manual V1.00, section 13.1.4.2, defines descriptor links.
Section 13.1.4.3 defines package interrupts after transfer of a complete package
and warns that later interrupts may overwrite earlier ones. Section 13.1.7.16
counts completed packages. The counter, rather than interrupt multiplicity,
therefore supplies progress.

The pinned Linux `sunxi-dma.c` at commit
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, lines 1087–1198, closes cyclic
chains. Lines 758–770 invoke callbacks while the channel remains active.
Lines 498–542 extend the package counter and sample a stable descriptor/counter
pair. Matching `lli->p_lli_next` establishes that `LLI_ADDR` names the next
descriptor. The ALSA consumer uses coherent allocation in `snd_sunxi_pcm.c`
line 259 and period callbacks in `pcm_dmaengine.c` lines 132–174. Its coherent
mapping does not establish visibility for this application's write-back mapping.
The same revision's DMA API permits partial synchronization but requires the
transferred range to be complete before CPU access. ALSA stops on overrun rather
than preserving data through unlimited consumer delays.

No inspected source explicitly orders destination-write completion, package-count
increment and next-descriptor/destination-cursor updates. The checked-copy path
infers that a completed package has finished its destination writes and that
reuse appears in the jointly consistent counter/cursor state before reused writes
can escape observation. These are unqualified device assumptions. Manual section
13.1.7.12 warns that remaining-byte data can precede destination arrival; cursor
consistency is not a completion fence. CPU barriers and invalidation do not close
this device-ordering gap.

Cyclic RX physical admission remains false until that ordering contract is
established. The existing launch evidence requires the additional
`cyclic-rx-completion-and-reuse-ordering-established` condition for directions
3 and 5. No qualifying record exists. Physical cache visibility, continuous
capture/playback, underrun/overrun behavior, cancellation bus quiescence and final
serial wire drain remain unqualified. DMA idle does not establish wire drain.

### Offline evidence

`~/.cache/sunstoneos/evidence/i2s-hardware-cyclic-20261019/` preserves descriptor
checks, the actual native link, actual FreeRTOS/provider composition checks and
producer-derived native client cases. The provider cases cover three ring wraps,
coalesced progress, reuse during each invalidation, cursor transitions, retained
staging, held fill leases, untrusted initial counts, delayed start readback and
ambiguous observation history. Modeled registers, DMA writes and cache effects
do not establish physical device ordering. Earlier finite, duplex and USB
transfer/ACK evidence remains applicable. Root's CPU maintenance review and the
focused device review are preserved with the compiled inputs.
