# Bounded HDMI audio streaming through the existing I2S service

Status: implemented and host-tested; physical streaming is unqualified. The
component record is [hdmi-audio.md](../hdmi-audio.md#bounded-streaming-uses-four-reusable-dma-periods).
This design is for maintainers of the FreeRTOS audio, DMA and HDMI bindings.

The opt-in streaming image (`SUNSTONE_HDMI_AUDIO_STREAM`, which requires the
audio and USB options) plays caller-supplied stereo S16LE PCM at 48 kHz through
the internal I2S3 route. Four reusable periods replace the complete-file DMA
allocation. USB0 supplies successive periods while the HDMI task maintains
video and sink health. One file of up to 576,000 frames (twelve seconds) fits
the 20-second supervised lifetime. Every launch retains its resources afterward.

## Reused mechanisms

[Finite HDMI audio](../hdmi-audio.md) provides EDID admission, I2S3 clock
ownership, HDMI automatic CTS, DMA0 startup, exact non-cacheable memory checks
and retained completion. Its USB option copies results through actual TinyUSB
transfers, including terminal zero-length packets.

The [I2S stream provider](../../drivers/audio/a733_i2s_stream.c) implements
finite refill with two to eight periods. GET grants a fresh ticket; PUT commits
a filled period; END commits a partial final period. DMA completion,
stop/drain and FIFO observations govern reuse. Hardware-cyclic TX repeats
immutable storage and grants no refill lease, so streaming caller audio uses
finite periods. [Native I2S playback](../i2s-application.md) provides the
pattern for partial-upload exclusion, credits and final input (`apps/i2s/runtime.c`).

Source references: the cached hashes are in
[the HDMI source manifest](../hdmi-audio-sources.json). The pinned Linux I2S
driver at `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, lines 585–588, maps HDMI
stereo channels to `0x10`; lines 1516–1544 describe TX request, start and stop
handling. The A733 manual V1.00, section 17.1.8.7, defines 128 TX sample-word
slots and FSTA free-space accounting. Its FIFO-empty observation does not prove
serializer completion.

## Fixed I2S3 profile

`a733_i2s_stream_profile` carries a route selector (`enum a733_i2s_route`):
`A733_I2S_ROUTE_I2S0` (zero) is the external I2S0 route, and
`A733_I2S_ROUTE_HDMI3` admits only non-cyclic stereo S16LE TX at 48 kHz, two
32-bit wire slots, 24.576 MHz module clock and no MCLK, with FIFO request 6.
Unknown selectors and I2S3 duplex are refused. Validation derives the register
base and FIFO request from the two fixed routes; callers supply no MMIO
address or request number. I2S3 clears the external PB mask, uses the HDMI TX
channel mapping, and sets the source-backed FCTL TX-hub bit; the I2S0
configure path rejects inherited hub mode. Period handling, END, ticket
checking and controller stop/drain are shared with I2S0.

HDMI supplies concrete stream callbacks. The output task prepares DMA0, adopts
the AUDIO1 parent and holds the exclusive I2S3 lease. Before OPEN it publishes
backend ownership to the DMA worker, and thereafter only that worker
configures or accesses I2S3; the output task alone operates the HDMI sampler.
The worker's acquire callback validates the handed-over parent and local
tokens, performs the I2S3 module and reset sequence, and returns a fresh
disabled controller. Health validates live authority before register access.
Callbacks use the register mutex, an exact address allowlist, verified writes
and absolute deadlines; W1C status and self-clearing flush bits keep their
readback exceptions. An uncertain lock, deadline or register result retains
actually held ownership and prohibits further device writes. The application
exposes no CLOSE, and its release callback cannot authorize reclamation.

## DMA interrupts and the period boundary

`dma_service_i2s_irq` uses INTID 143 on core 0 at GIC priority `0xd8`. Pre-GIC
inherited-enable, pending and active state is refused. The interrupt is routed
while groups are disabled, the SPI stays masked until service activation, and
it is rearmed only after the worker has acknowledged DMA events. The ISR masks
and notifies and performs no DMA MMIO. The DMA worker runs on core 0 at
priority 5; the output and USB tasks use A55 cores at priority 4; PMIC stays at
priority 3 on core 0. The eight-core boot qualification requirements apply to
this image.

One period has 1,024 frames and lasts 21.333 ms. At 96,000 sample words/s the
FIFO holds at most 1.333 ms. The stream keeps its occupancy-derived handoff
deadline `(available_words - 1) * 1000000 / 96000` microseconds, at most 1,322
microseconds. IRQ delivery removes tick-detection latency; the one-tick
fallback can detect a missed IRQ but cannot promise uninterrupted output.

In `a733_i2s_stream_poll`, gated by I2S3 finite TX, a controller in STOPPING
during a nonfinal period handoff repeats its poll without sleeping, for at most
64 polls and 200 microseconds, also bounded by the original occupancy deadline.
The budget is captured once. DMA completion predicates and refresh deadlines
are unchanged, and the shared DMA worker has no polling policy. Usually the
first poll observes drain and arms the next period in the same call.

If the budget cannot finish the handoff, the stream enters the failed-stop path.
With live authority it disables requests and transmission and continues bounded
DMA drain through normal service polls. The affected period is not published as
successful, a drain timeout retains the active allocation, and no samples are
silently dropped. No software model establishes the physical drain latency or
interrupt bound.

The output task keeps its one-tick HDMI health service. Refill uses
nonblocking GET/PUT replies in that task, with at most one pending request, and
advances ready metadata stages in a bounded burst that stops at a pending reply
or missing input. Queue completion is checked before memory access; elapsed time
never substitutes for a ticket.

## USB input authenticated per period

The streaming session is version 3: the 816-byte USB session plus a 32-byte
period-manifest digest, 848 bytes. Its PCM digest binds the full caller file,
and the frame limit is 576,000 only for this linked geometry. The finite offline
and copied-observation profiles keep their contracts.

Preparation writes a manifest into the immutable upload window at `0x41f10000`.
A 64-byte header binds version, format, frame count and full-file SHA-256;
successive 32-byte hashes bind each ordered 4,096-byte period, including the
exact short final period. The largest manifest is 18,080 bytes. Boot verifies
its size, header and session digest once. USB cannot address the window. Each
uploaded period must match its indexed hash before the output task commits it,
which binds input identity without retaining the complete PCM file on the board.

The USB task is the sole producer of four 4,096-byte staging slots. It reads
TinyUSB fragments directly into an unpublished slot. Release publication
exposes only a complete, in-order slot; the output task acquires it, verifies
its period hash, copies it into its exact GET lease, finishes all access before
queuing PUT or END, and returns the staging slot. The worker alone transfers
the DMA period between CPU and device ownership. USB never receives a DMA
pointer or ticket.

The bounded exchange uses a versioned 48-byte request header: magic, version,
request ID, header length, epoch, generation, operation, payload length and
sequence. Operations are STATUS, WRITE, END and CANCEL. WRITE carries one next
period with its exact declared length; END is valid only after every period
upload is complete. One exchange runs at a time, with an absolute one-second
budget clipped to the admitted lifetime, and at most twice the period count
plus 64 requests are admitted. A response uses the 512-byte copied slot and
completion/ZLP accounting and adds credits, accepted, committed and consumed
frame counts; no response slot is reused on a partial or late USB completion.

START requires four committed periods, or the complete shorter file with END.
The caller keeps uploading while playback runs; four DMA periods queue 85.333
ms of samples, and after a period is consumed the other three give about 64 ms
to refill it. Exhausted staging credits cause bounded backpressure; missing
committed data at a DMA boundary fails the stream without silence or replay.

The host entry freezes the original file and manifest, validates the linked
streaming geometry, uses only the selected USB device and session identities,
supplies periods according to credits, then checks copied completion counts and
hashes. It stops after the first transport failure and rejects modeled physical
authority. The supervisor entry accepts the bundle before any device access.

## Completion and failure

Before START, the caller requires enough time before `app.stop_at` for the
declared PCM duration plus a 100-ms processing margin, so startup or upload
delay may refuse playback. Twelve seconds is a file-size ceiling, not a claim
that every host sustains the transport rate.

A healthy EOF requires every declared byte committed and consumed, service
COMPLETE after actual controller drain, and an empty I2S FIFO. The output task
then mutes the sampler and uses the retained HDMI stop. The result reports
source consumption and FIFO emptiness; `wire_drained` stays zero, and no
serializer or sink completion is inferred from a delay, FIFO state, DMA event or
USB acknowledgment.

CANCEL closes producer admission, preserves a partial unpublished slot, and
requests the potentially truncating cancellation. The result reports CANCELED
only after the service's actual stopped and drained result, never reports file
completion and never releases the catalogue. The streaming result has its own
version and a CANCELED state, and the finite decoder rejects it. A checksum
mismatch or ordinary underrun follows controlled stop while authority is valid.

Hotplug loss, expired authority, uncertain MMIO, a missing or late service reply
or a failed DMA drain is a terminal retained failure. The pending request,
client-held periods, all allocations and clock and rail ownership stay frozen;
no reset, retry, reconnect or later upload makes it a clean stream. Manual
whole-board power removal is the recovery requirement.

## Memory, CPU and qualified figures

The NC adapter registers each exact period page and each 64-byte descriptor
slot, accepting only their immutable geometry and roles. Allocator ownership
retains the parent allocation, and service tickets govern temporary CPU writes.
NC stores use device-publication ordering without invalidation or a cacheable
alias. Page translations and protected ranges are validated at admission, with
exact range checks at callback entry. There is no shared DMA-memory policy or
page-table change.

| Figure | Value | Record |
| --- | --- | --- |
| NC DMA memory | 20,480 B (one descriptor page, four period pages); largest finite file 196,608 B | [hdmi-audio-streaming.jsonl](../evidence/hdmi-audio-streaming.jsonl) `hdmi-audio-streaming-budgets` |
| Write-back staging; other linked BSS; text growth | 16,384 B; 960 B; 9,620 B | same |
| Manifest | at most 18,080 B of the 192,000-byte upload window | same |
| Payload copies | two RAM copies, 384,000 B/s; each byte hashed once, 192,000 B/s | same (estimates) |
| Software run | 50,177 frames, 50 periods, 200,708 B copied and hashed once | `hdmi-audio-streaming-software` |

No task or stack is added. The SHA cost estimate (15–25 cycles per byte,
0.58–0.97% of a 494-MHz core) and the short-handoff bound (under 0.94% of one
core if every boundary exhausts it) are estimates, not measurements.

## Files and checks

The implementation owns `apps/hdmi-audio/*`, the audio branches of
`apps/hdmi/{main,output,session,boot.patch}`, a fixed HDMI audio IRQ table and
`cmake/a733-hdmi-application.cmake`. The shared changes are limited to
`include/a733_i2s_stream.h` and `drivers/audio/a733_i2s_stream.c` for the
fixed route and short handoff loop, and `drivers/dma/dma_service_i2s_stream.c`,
which refuses I2S3 duplex pairing. `drivers/audio/dma_service_i2s_irq.c` is
reused as is.

The checks cover the stream fixture (I2S3 configuration, ring wraps, short END,
starvation, late IRQ, bounded delayed drain, timeout retention, FIFO
accounting, one I2S0 compatibility case), the linked streaming image with its
actual DMA, output and USB tasks over more than 48,000 frames, checkpointed
failure cases (corrupt period, partial upload, cancellation, unknown drain, late
response), boot IRQ routing and inherited-state refusal, native USB full and
short packet completion, and host preparation, identity, sequence and credit
refusal with the frozen image and manifest.

## Limitations and open work

Physical USB throughput, IRQ latency, DMA visibility, FIFO continuity, audio
transmission, serializer drain, sink rendering and recovery are unqualified; a
board run needs a reviewed watchdog return path, since every launch still
requires supervised whole-board power removal.
