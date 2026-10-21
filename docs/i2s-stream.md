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
Single-direction receive and transmit profiles share one fixed DMA channel in
the service instance. A duplex service admits one compatible receive/transmit pair
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
stops the stream with a failed result. START refuses finite playback until every
catalogue period has been committed, or every period up to and including a
committed final lease. After START, PERIOD_GET leases only periods that DMA has
already consumed, so an uncommitted period could never be filled; a missing
commit at a later boundary still stops the stream with a failed result.
The driver neither inserts silence nor silently drops samples.

TX_MEMORY_CONSUMED means that DMA has finished reading the segment and can no
longer access its allocation until the client commits it. It does not prove that
those samples have left the serializer. CANCEL disables the direction and may
truncate queued wire output. Canceling a stream that has not started disables its
DMA request and leaves the CTL global-enable bit (GEN) as it was, so it never
starts BCLK and LRCK. Final-wire completion requires a separate bounded
serializer-drain design.

## FIFO occupancy bounds the handoff

The pinned manual defines a 64-entry receive FIFO and a 128-entry transmit FIFO.
Each entry is a sample word. Receive availability is FSTA bits 6:0; transmit
empty space is bits 23:16. The driver uses receive threshold eight and transmit
empty-space threshold 64. It preserves the other FIFO control fields.

The driver detects a finite-transfer completion through the DMA controller's
read-only finite-state query, which reports queue end, error events and
timeout without acknowledging them. At a completion, the driver disables the
selected DMA request before asking DMA to stop. The receiver or transmitter
continues running. It samples FIFO capacity and computes a handoff deadline from
sample rate and channel count, withholding one sample as margin. FIFO errors or
a missed handoff deadline stop the direction and drain DMA without publishing
that affected period as successful audio. A delayed DMA drain does not release
storage merely because the hardware completion event appeared.

This mechanism requires bounded scheduling, register access and DMA drain on the
real board. For mono receive at 16 kHz with an empty FIFO at the boundary, the
deadline is 63 samples, about 3.9 ms. Without a DMA interrupt, the worker detects queue end at its next
FreeRTOS tick, up to 1 ms late. The native I2S image binds the DMA0 interrupt
with a one-tick fallback ([i2s-application.md](i2s-application.md)), which
removes that detection delay while delivery works and keeps the tick bound when
it does not. A two-core register model cannot establish that these bounds hold
at 16 kHz. Physical timing, interrupt latency and FIFO continuity remain
qualification requirements; the per-period `observed_before_us` and
`observed_after_us` records can measure the bridge on the board. No performance
measurement is included in the model checks.

`checks/i2s-interrupt.py` builds `checks/i2s-interrupt.c` on the stream model
above with the image's interrupt adapter and runs two QEMU cases. With a
200-tick fallback, five finite receive periods each bridge into the next
period after their queue-end interrupt alone, the worker reads no DMA register
between periods, each interrupt masks the SPI once and the worker re-enables it
only after acknowledging the event. With the native one-tick fallback and no
interrupt delivery at all, the same five periods bridge within the polled path's
few ticks. DMA and I2S registers, the GIC SPI and interrupt delivery are
modeled.

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
occur under the shared mutex. Each platform write reads the register back and
compares every written bit except the ISTA write-one-to-clear bits and the
self-clearing FCTL flush bits. The stream driver relies on that comparison and
does not read written registers a second time. A failed readback or deadline reached before
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
read-alias exclusion, stop ordering, cache publication, busy authority, late
publication retention, cancellation of a never-started stream without GEN, and
START refusal for a partly committed playback catalogue. The host fixture
`checks/i2s.c` checks the same driver against the actual DMA controller under
sanitizers, including retained faults; [i2s.md](i2s.md) lists its cases.

The private CMake fragment adds the stream driver to the existing DMA service
and startup targets, because the service links stream dispatch even for images
that register no stream job.

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

The stream driver accepts four to eight periods for a cyclic capture ring,
because the copy window described below is empty with fewer than four.
Capture keeps every raw period reserved for DMA. Each period occupies
separate cache lines. Initial publication cleans and invalidates them; later CPU
access only reads them. The worker copies a completed period into a private
4,096-byte staging buffer. It invalidates the raw range and issues a barrier
before copying, using the existing native INVALIDATE operation without cleaning
DMA-written data. A second barrier after the copy completes its memory loads
before the postcheck reads the package-count, destination and remaining-byte
registers. Without it, the processor could read those registers before the
copy finished, and a DMA overwrite of the period could reach staging
undetected. The CPU only reads the raw range, and the next copy of the same
period invalidates it again, so no cache maintenance follows the copy.
No raw pointer reaches the application. `CAPTURED_COPY` grants the immutable
staging copy until its exact ticket is returned. The application copies staging
into its existing four USB slots before returning it.

For absolute period `k`, extended completed-package count `C` and ring length
`N`, acquisition requires `k+2 <= C <= k+N-2`. After the copy it again requires
`C <= k+N-2`. Each bound keeps one whole period between the copy and the DMA
writer:

- The lower bound waits until period `k+1` has also completed, so destination
  writes of period `k` may finish up to one period after the counter reports
  them.
- The upper bound refuses once period `k+N-2` has completed. The writer may
  then be up to one period ahead of the counter and cursor without entering
  slot `k`.

Stable counter, next-descriptor and destination/remaining-byte readings must
agree with the current period. A transition defers acquisition; reuse or an
ambiguous postcheck discards staging and stops the stream. The stream stops on
overrun once `C > k+N-2` without requesting the copy. A held staging copy
survives raw-ring overrun. A full USB ring stops capture without overwriting any
unread copy. USB slots still require complete transfer delivery and exact ACK.

The guard costs one period of capture latency and one period of consumer
tolerance. Native session directions 3 and 5 use four periods, which leaves
the single value `C == k+2`: the client must return each staging copy within
one period, 128 ms for 2,048 16-bit mono frames at 16 kHz. An eight-period
ring would leave five values, so the client could fall four periods behind.
The driver and the DMA pool already accept eight periods, but the session
shares its period count with the finite directions and the USB transfer sizes,
so an eight-period receive ring needs a period count of its own there.

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
increment and next-descriptor/destination-cursor updates. Manual section
13.1.7.12 warns that remaining-byte data can precede destination arrival; cursor
consistency is not a completion fence. CPU barriers and invalidation do not close
this device-ordering gap.

The one-period guard replaces exact ordering with a bounded-margin assumption.
A checked copy is correct when both of these lags stay below one period:

- the time from a package-count increment until every destination write of
  that package is visible in memory;
- the distance by which the DMA writer runs ahead of the package counter and
  the destination cursor.

At 16 kHz one period lasts 64 ms to 128 ms, depending on the frame size, while
a bus write completes in microseconds. The margin remains an unqualified device
assumption until a board test measures it.

Cyclic RX physical admission remains false until that measurement exists. The
existing launch evidence requires the
`cyclic-rx-completion-and-reuse-ordering-established` condition for directions
3 and 5, which a record of the measured margin would satisfy. No qualifying
record exists. A board test could measure the margin with the I2S loopback
mode (`I2S/PCM_CTL` bit 3, manual section 17.1.8.1), which connects DOUT to
DIN. Cyclic playback of distinct known periods then makes every captured period
predictable. Reading raw slot `k` without the guard, at the instant `C` first
reaches `k+1` and again just before it reaches `k+N`, would show how far
destination writes trail the counter and how far the writer leads it. Physical
cache visibility, continuous capture/playback, underrun/overrun behavior,
cancellation bus quiescence and final serial wire drain remain unqualified. DMA
idle does not establish wire drain.

### Offline evidence

`~/.cache/sunstoneos/evidence/i2s-hardware-cyclic-20261019/` preserves descriptor
checks, the actual native link, actual FreeRTOS/provider composition checks and
producer-derived native client cases. That evidence predates the copy guard.
The [cyclic provider check](../checks/i2s-cyclic.c), run by
[its runner](../checks/i2s-cyclic.py), covers the guarded window. Its cases
cover three ring wraps,
coalesced progress, reuse during the invalidation and after the copy, cursor transitions, retained
staging, held fill leases, untrusted initial counts, delayed start readback and
ambiguous observation history. Three further cases model the margin
assumption with lags of one period minus one 32-byte burst:

- Destination writes that finish only while the next period is written. The
  guarded copies are complete; the unguarded window would copy a period whose
  data has not all arrived.
- A writer that runs ahead of the counter and cursor. Copies at `C == k+2`
  are complete, and a jump to `C == k+3` stops the stream without a copy.
  The unguarded window would copy slot `k` after the writer had reused it.
- An eight-period capture ring completes three wraps and then tolerates four
  periods of consumer lag.

Modeled registers, DMA writes and cache effects do not establish physical
device ordering or the margin. Earlier finite, duplex and USB
transfer/ACK evidence remains applicable. Root's CPU maintenance review and the
focused device review are preserved with the compiled inputs.

## Finite playback accepts an explicit final lease

`DMA_SERVICE_PERIOD_END` appends operation 8 without changing request layout.
For this operation only, `request.timeout` contains the nonzero valid frame count
of the exact `period_ticket` returned by GET. END accepts only finite TX, validates
multiplication and allocation bounds, and requires a committed queued prefix with
no later queued payload. It changes no state on refusal. Existing operations and
cyclic playback retain their contracts.

The worker builds a private immutable segment prefix before submitting the final
DMA transfer. Its byte count is `valid_frames * plan.frame_bytes`; the authenticated
catalogue allocation remains retained in full. Package completion still follows
the existing DMA ownership-ending protocol. Final period accounting uses that
prefix, and subsequent PUT operations release leases without scheduling refill.

After final DMA ownership ends, the worker leaves TX enabled and polls FIFO state.
The deadline is the time to drain 128 FIFO entries at the configured frame rate
plus the operation budget; at 16 kHz mono the drain takes 8 ms. A FIFO-empty
observation enters `A733_I2S_INPUT_ENDED`, which maps to retained service
COMPLETE. It does not release shared clocks or pins, disable the serializer, or
admit CLOSE/REAP.

The final DMA completion checks the FIFO error bits, and no sample enters the
FIFO after it. A TX underrun (ISTA TXU) observed while polling therefore means
that the serializer emptied the FIFO; it is expected and does not fail the
stream. Only an FSTA reading of 128 free entries reports the end of input. TX
overflow, invalid free counts and expired observations retain failure. Whether
the device latches TXU at that point is inferred from ordinary underrun
behavior and has not been observed on the board. Explicit CANCEL may
truncate output and uses the existing stop protocol; duplex peer control bits
remain preserved.

The A733 User Manual V1.00, section 17.1.8.7, defines FSTA bits 23:16 as free TX
FIFO entries; 128 means the FIFO is empty. Section 17.1.8.11 counts samples written
into that FIFO. Neither establishes final serializer-bit completion. The pinned
Linux `snd_sunxi_i2s.c` at commit
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, lines 1516–1544, flushes the FIFO and
resets TXCNT while stopping. Its delay and FIFO-empty loop do not establish
lossless wire drain. The DMAengine STOP/synchronize operations end DMA ownership,
not serializer activity. The cached source hashes and sections are retained in
the file-playback evidence. Complete physical file playback remains unqualified;
cyclic RX's undocumented device-ordering gap also remains open.
