# Bounded HDMI audio streaming through the existing I2S service

Status: implemented with focused software evidence in
[the component record](../hdmi-audio.md); physical streaming remains unqualified.
The source audit uses SunstoneOS `78c36b2b`. This design is for maintainers of
its FreeRTOS audio, DMA and HDMI bindings.

The opt-in streaming image will play caller-supplied stereo S16LE PCM at
48 kHz through the existing internal I2S3 route. Four reusable periods replace
the complete-file DMA allocation. USB0 supplies successive periods while the
existing HDMI task maintains video and sink health. The initial useful scope is
one file of up to 576,000 frames, or twelve seconds, within the existing
20-second supervised lifetime. Every launch retains its resources afterward.

## Existing implementations provide the ownership and transport mechanisms

[Finite HDMI audio](../hdmi-audio.md) already implements EDID admission,
I2S3 clock ownership, HDMI automatic CTS, DMA0 startup, exact non-cacheable
memory checks and retained completion. Its USB option already copies results
through actual TinyUSB transfers, including terminal zero-length packets.
Those functions are not new work.

The [I2S stream provider](../../drivers/audio/a733_i2s_stream.c) already
implements finite refill with two to eight periods. GET grants a fresh ticket;
PUT commits a filled period; END commits a partial final period. DMA completion,
stop/drain and FIFO observations govern reuse. Hardware-cyclic TX repeats
immutable storage and grants no refill lease, so it is unsuitable for changing
caller audio. The streaming path will use finite periods.

[Native I2S playback](../i2s-application.md) already accepts runtime USB input.
Its fixed producer/consumer slots in `apps/i2s/runtime.c` provide the pattern
for partial-upload exclusion, credits and final input. That implementation is
tied to the I2S application and mono 16-kHz frames. HDMI needs a small concrete
caller around the same service operations.

The actual missing bindings are:

- The stream provider fixes register addresses, FIFO request 3 and external
  PB pins to I2S0. HDMI needs internal I2S3, request 6 and TX hub mode.
- HDMI registers one immutable FIFO transfer. Its NC adapter accepts only a
  full descriptor page and one complete PCM extent, rather than period leases.
- The HDMI boot image admits quiet DMA interrupts but installs no DMA route.
- HDMI USB accepts observation requests only; it has no PCM producer.
- A pending finite DMA drain currently yields to a one-tick service wait.
  The stereo 48-kHz FIFO cannot reliably cover arbitrary additional tick waits.

The cached source hashes in [the HDMI source manifest](../hdmi-audio-sources.json)
were checked during this audit. The pinned Linux I2S driver at
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, lines 585–588, maps HDMI stereo
channels to `0x10`; lines 1516–1544 describe TX request/start/stop handling.
The A733 manual V1.00, section 17.1.8.7, defines 128 TX sample-word slots and
FSTA free-space accounting. Its FIFO-empty observation does not prove serializer
completion. The existing manual and source pins remain the references.

## A fixed I2S3 profile reuses finite period handling

Append a route selector to `a733_i2s_stream_profile`: zero retains I2S0;
I2S3 admits only non-cyclic stereo S16LE TX at 48 kHz, two 32-bit wire slots,
24.576 MHz module clock and no MCLK. Reject unknown selectors and I2S3 duplex.
Validation derives the register base and FIFO request from these two fixed
routes; callers cannot supply arbitrary MMIO or request numbers.

For I2S3, clear the external PB mask, use the existing HDMI TX channel mapping,
and set the source-backed FCTL TX-hub bit. The ordinary I2S0 configure path
continues to reject inherited hub mode. Period handling, END, ticket checking
and controller stop/drain remain shared.

HDMI supplies concrete stream callbacks. The output task prepares DMA0, adopts
the existing AUDIO1 parent and holds the exclusive I2S3 lease. Before OPEN it
publishes backend ownership to the DMA worker. Thereafter only that worker
configures or accesses I2S3; the output task alone operates the HDMI sampler.
There is no concurrent access through the old finite `producer()` path.

The worker's acquire callback validates the handed-over parent/local tokens,
performs the current I2S3 module/reset sequence, and returns a fresh disabled
controller. Health validates live authority before register access. Callbacks
use the existing register mutex, exact address allowlist, verified writes and
absolute deadlines. W1C status and self-clearing flush bits keep their existing
readback exceptions. An uncertain lock, deadline or register result retains
actually held ownership and prohibits further device writes. CLOSE is not
exposed by this application; its release callback cannot authorize reclamation.

## DMA interrupts and a short drain loop cover the period boundary

Reuse `dma_service_i2s_irq` and the I2S application's fixed-route boot pattern:
INTID 143, core 0, GIC priority `0xd8`. Preserve the pre-GIC inherited-enable,
pending and active refusal. Route while interrupt groups are disabled, keep
the SPI masked until service activation, and rearm only after the worker has
acknowledged DMA events. The ISR masks and notifies; it performs no DMA MMIO.

The existing DMA worker runs on core 0 at priority 5 in the streaming image.
The existing output and USB tasks use A55 cores and priority 4; PMIC remains
priority 3 on core 0. These are service tasks, and no A76 admission is needed.
The existing eight-core boot qualification requirements still apply to this image.

One period has 1,024 frames and lasts 21.333 ms. At 96,000 sample words/s,
the FIFO holds at most 1.333 ms. Preserve the stream's occupancy-derived
handoff deadline: `(available_words - 1) * 1000000 / 96000` microseconds.
Its maximum is 1,322 microseconds. IRQ delivery removes normal tick detection
latency; the existing one-tick fallback can detect a missed IRQ but cannot
promise uninterrupted output.

Keep the timing change inside `a733_i2s_stream_poll`, gated by I2S3 finite TX.
When the controller reports STOPPING during a nonfinal period handoff, repeat
its existing poll without sleeping, for at most 64 polls and 200 microseconds,
also bounded by the original occupancy deadline. Capture that budget once.
Do not change DMA completion predicates, refresh deadlines, or add a shared
DMA-worker polling policy. Usually the first poll observes drain and arms the
next period in the same call.

If this short budget cannot finish the handoff, enter the existing failed-stop
path. With live authority, disable requests/transmission and continue bounded
DMA drain through normal service polls. Do not publish the affected period as
successful. A drain timeout retains the active allocation. This stops a missed
real-time deadline explicitly instead of continuing with silently missing samples.
No software model establishes the physical drain latency or interrupt bound.

The output task continues its existing one-tick HDMI health service. Refill uses
nonblocking GET/PUT replies in that task, with at most one pending request.
It advances ready metadata stages in a bounded burst, stopping at a pending
reply or missing input. This reuses the current unqualified-interrupt fallback
rather than adding a polling task. Queue completion is always checked before
memory access; elapsed time cannot substitute for a ticket.

## USB input is authenticated before each period is committed

Add `SUNSTONE_HDMI_AUDIO_STREAM`, requiring the existing audio and USB options.
The finite offline and copied-observation profiles retain their contracts.
The streaming session is version 3: the 816-byte USB session plus a 32-byte
period-manifest digest. Its existing PCM digest binds the full caller file;
the frame limit becomes 576,000 only for this linked streaming geometry.

Preparation writes a manifest into the existing immutable upload window at
`0x41f10000`. A 64-byte header binds version, format, frame count and full-file
SHA-256; successive 32-byte hashes bind each ordered 4,096-byte period, including
the exact short final period. The largest manifest is 18,080 bytes. Boot verifies
its exact size, header and session digest once. The window remains reserved and
immutable; USB cannot address it. Each uploaded period must match its indexed
hash before the output task commits it. This preserves pre-playback input
identity without uploading or retaining the complete PCM file on the board.

The existing USB task becomes the sole producer of four 4,096-byte staging
slots. It reads TinyUSB fragments directly into an unpublished slot, avoiding
an additional fragment buffer. Release publication exposes only a complete,
in-order slot; the output task acquires it, verifies its period hash, copies
it into its exact GET lease and finishes all access before queuing PUT/END.
It then returns the staging slot. The worker alone transfers the DMA period
between CPU and device ownership. USB never receives a DMA pointer or ticket.

Extend the bounded exchange with a versioned 48-byte request header containing
magic, version, request ID, header length, epoch, generation, operation, payload
length and sequence. Operations are STATUS, WRITE, END and CANCEL. WRITE carries
one next period, with its exact declared length; END is valid only after every
period upload is complete. Permit one exchange at a time, with an absolute
one-second budget clipped to the admitted lifetime. A response uses the existing
512-byte copied slot and completion/ZLP accounting, adding credits, accepted,
committed and consumed frame counts. No response slot is reused on a partial or
late USB completion. Limit requests to twice the admitted period count plus 64.

START requires four committed periods, or the complete shorter file with END.
The caller keeps uploading while playback runs; four DMA periods provide
85.333 ms of queued samples. After a period is consumed, the other three give
about 64 ms to refill it. Exhausted staging credits cause bounded backpressure;
missing committed data at a DMA boundary fails the stream without silence or
replay. END on the final partial lease uses the existing provider semantics.

The host entry extends the existing preparation/capture path. It freezes the
original file and manifest, validates the linked streaming geometry, uses only
the selected USB device and session identities, supplies periods according to
credits, then checks copied completion counts and hashes. It stops after the
first transport failure. Preparation still rejects modeled physical authority.
The actual supervisor entry must accept the new bundle before any device access.

## Completion and failure preserve the physical boundary

Before START, the caller requires enough remaining time before `app.stop_at`
for the declared PCM duration plus a 100-ms processing margin. Startup or upload
delay may therefore refuse playback. Twelve seconds is a file-size ceiling,
not a claim that every host can sustain the required transport rate.

A healthy EOF requires every declared byte committed and consumed, service
COMPLETE after actual controller drain, and an empty I2S FIFO. The output task
then mutes the sampler and uses the existing retained HDMI stop. Report source
consumption and FIFO emptiness; `wire_drained` stays zero. No serializer or sink
completion is inferred from a delay, FIFO state, DMA event or USB acknowledgment.

An explicit CANCEL closes producer admission, preserves a partial unpublished
slot, and requests the existing potentially truncating cancellation. It reports
CANCELED only after the service's actual stopped/drained result. It never reports
file completion or releases the catalogue. Give the streaming result its own
version and explicit CANCELED state; the old finite decoder must reject that
version. A checksum mismatch or ordinary underrun follows controlled stop
while authority remains valid.

Hotplug loss, expired authority, uncertain MMIO, missing/late service reply or
failed DMA drain is terminal retained failure. Freeze the pending request,
client-held periods, all allocations and clock/rail ownership. No reset, retry,
reconnect or later upload can turn this into a clean stream. Manual whole-board
power removal remains the recovery requirement.

## Memory and CPU costs are bounded by the period size

| Resource or work | Proposed budget and comparison |
| --- | --- |
| NC DMA memory | One 4,096-byte descriptor page plus four period pages: 20,480 bytes; current maximum finite allocation is 196,608 bytes |
| WB input staging | Four 4,096-byte slots: 16,384 bytes; needed to decouple fragmented USB uploads from service-client leases |
| Immutable manifest | At most 18,080 bytes in the existing 192,000-byte upload window |
| Tasks and stacks | No added task or stack; reuse PMIC, output, USB and DMA worker |
| Control metadata | Budget at most 2 KiB beyond staging and existing service storage; report actual struct and linked-size deltas |
| Period rate | 46.875 full periods/s; one queue-end interrupt per period |
| Payload copies | TinyUSB RX FIFO to staging, then staging to NC period: two RAM copies, 384,000 bytes/s; USB PIO ingress is separate existing device work |
| Digests | Manifest once at boot; each payload byte hashed once before PUT, 192,000 bytes/s; no full-file rehash per request |
| SHA estimate | At 15–25 cycles/byte, 2.88–4.8 million cycles/s, about 0.58–0.97% of a 494-MHz core; this is not a measurement |
| Normal service messages | One GET and one PUT per full period, with END replacing the final PUT; BUSY and health fallback overhead measured separately |
| Short handoff polling | At most 200 microseconds per period, under 0.94% of one core if every boundary exhausts it; exhaustion stops playback |

The NC adapter will register each exact period page and each 64-byte descriptor
slot, accepting only their immutable geometry and roles. Allocator ownership
retains the parent allocation; service tickets govern temporary CPU writes.
NC stores need the existing device-publication ordering, without invalidation
or a cacheable alias. Validate page translations and the full protected ranges
at admission, retain exact range checks at callback entry, and measure any
remaining provider validation work rather than concealing it in these budgets.
There is no shared DMA-memory policy or CPU page-table change.

## Files and focused acceptance

Implementation owns `apps/hdmi-audio/*`, the audio branches of
`apps/hdmi/{main,output,session,boot.patch}`, a fixed HDMI audio IRQ table,
`cmake/a733-hdmi-application.cmake`, and component documentation/checks.
The shared source changes are limited to `include/a733_i2s_stream.h` and
`drivers/audio/a733_i2s_stream.c` for the fixed route and short handoff loop;
`drivers/dma/dma_service_i2s_stream.c` must reject I2S3 duplex pairing explicitly.
Reuse `drivers/audio/dma_service_i2s_irq.c` unchanged. No edit to
`drivers/dma/dma_service.c`, the DMA controller, common memory or power code is
proposed. Agree any additional shared change with the coordinator before editing.

Select these checks after independent design review:

- Extend the existing stream fixture for I2S3 configuration, several ring wraps,
  short END, starvation, late IRQ, bounded delayed drain, timeout retention and
  source-derived FIFO accounting. One affected I2S0 case guards route compatibility.
- Build one actual streaming image. Execute its constructor and real DMA/output/
  USB tasks with more than 48,000 caller frames, exercising refill and a partial
  final period. Count copies, hashes, queue traffic, bridge polls and stack use.
- From preserved checkpoints, exercise a corrupt period, partial USB upload,
  cancellation, unknown drain and late response without replaying startup.
- Check actual boot IRQ routing and inherited-state refusal; check native USB
  full/short packet completion against the real producer.
- Run focused host preparation, identity/sequence/credit refusal and actual
  supervisor-entry cases with the frozen image and generated manifest.

Reuse the finite HDMI and copied-USB evidence named in the component documents,
plus existing I2S ticket and controller-drain cases where their inputs are
unchanged. Models establish software ownership and bounded decisions. Physical
USB throughput, IRQ latency, DMA visibility, FIFO continuity, audio transmission,
serializer drain, sink rendering and recovery remain unqualified.
