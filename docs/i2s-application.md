# Native I2S capture and recorded playback

The dedicated `sunstone-a733-i2s` image connects the existing I2S0 stream service
to actual DMA0 startup, native clock and pin providers, linker-owned memory and
USB0 observation. One launch selects receive, transmit, or simultaneous receive and transmit at
16 kHz with one 16-bit PCM channel and two 32-bit wire slots. No microphone or
playback device is physically qualified. Runtime direction 6 accepts caller PCM
through USB and refills finite DMA leases. Final serial-wire drain remains
unqualified; cancellation can truncate queued output.

Primary preparation reserves one descriptor page and four disjoint period pages
from the existing singleton memory pool. Each period carries 4,096 payload bytes.
The native DMA policy admits only these exact pages, registered descriptor slots
and the selected I2S0 FIFO direction. Duplex mode reserves a separate descriptor
page and four period pages for each direction, using DMA channels 0 and 1. The
native policy admits each bank only for its direction's exact memory role and
FIFO route. The existing RAM-copy constructor keeps its original API
and admission requirements.

The protected session binds shared provider settings, DMA prior-drain and remote
exclusion evidence, audio pin configuration, bank voltage and clock evidence,
memory coherence, recorded PCM identity and supervised recovery to the linked
image, epoch, generation and lease. Runtime deadlines derive from the live
launch contract. The separate supervised DMA entry uses manual power removal;
it does not claim watchdog qualification. Actual producer quiescence, inherited
DMA drain and masked interrupts remain mandatory prerequisites.

The client prepares the audio parent before opening the stream. It then hands
backend ownership to the DMA worker, which alone advances the I2S controller and
DMA channel. The PMIC provider and USB0 tasks run independently. Both clock and
pin operations use the existing common resource managers and register mutex.

The original receive mode copies four successful periods into an immutable
application result. Optional transcription mode copies a longer finite utterance.
Playback uses the first 8,192 samples of the existing recorded development WAV,
whose SHA256 is `ef445d0243b19b5a38d7d916911eecf1593570ceb2ab7cf4c2bab7bb5be400b4`.
The build verifies the complete source and format before embedding that excerpt.
It does not alter the mimus input or model. A playback result means DMA consumed
the four periods; it does not establish that the samples left the serializer.

After four copied periods, the client requests CANCEL and requires the service's
terminal CANCELED result with its buffer hold intact before publishing output.
CANCEL may truncate queued wire data. All pages, parent leases, the service
session and any final client-held period remain reserved. Failures, cancellation,
missing replies and supervisor deadlines never authorize allocator release,
period reuse, reset or an assertion that DMA drained. An uncertain session stays
retained until manual power removal.

The USB interface provides bounded status, copied PCM reads and cancellation.
Each response binds the epoch, generation and immutable admitted-session digest.
The observer in `apps/i2s/observe.py` selects an explicit bus and address, requires
those identities, stops on any transport failure and saves raw signed little-
endian 16-bit PCM with its digest. Callers must hold the project board lock and
satisfy actual launch admission before opening the selected device.

Build through `apps/i2s/CMakeLists.txt` with the existing AArch64 toolchain,
pinned FreeRTOS/TinyUSB sources and `SUNSTONE_I2S_RECORDED_WAV` pointing to the
pinned recording. The CMake fragment is `cmake/a733-i2s-application.cmake`.
The [stream contract](i2s-stream.md) retains the FIFO scheduling, cache ownership,
period ticket and teardown requirements. Software checks cannot qualify external
electrical wiring, clock stability, FIFO continuity, microphone sampling or wire
playback.

The affected native image build and twelve new boundary cases pass. The cases
cover immutable supervised admission, USB copied output and refusal, capture and
playback clients, cancellation, deadline retention, failed drain, actual audio
parent/native-adapter preparation and native RX/TX pool construction. Controller
and FIFO timing remain modeled or unexecuted; no check establishes physical
operation. Prior component evidence remains scoped to its recorded source and
ABI. The new image compiles its changed providers and consumers together.

Exact inputs, generated PCM, objects, executables, dependency files and results
are preserved under
`~/.cache/sunstoneos/evidence/i2s-application-20261017/batch1/`.
The original capture/playback image SHA256 is
`fcb6c30270177f3bba15163c7d4cad4cce29659c9a589a592463e55fa958d841`.

USB0 uses the supervised access-rule identity `1f3a:efe8`. The observer requires
the exact I2S product and serial before configuring that explicitly selected
device. A USB assertion or invalid interface revokes application admission and
the shared counter provider before parking. Subsequent DMA device/cache callbacks
and startup/lease operations refuse the faulted provider; active reservations
remain held. No failure callback performs MMIO, drain, reset or release.

Four focused USB correction checks pass: descriptor identity, assertion cutoff,
invalid-interface cutoff and observer identity/refusal. The correction reuses
64 unchanged objects, compiles the application failure handler, USB object and
build identity, and relinks the image. Those artifacts and comparison records
are under the existing evidence directory's `usb-fix/` subdirectory. The original
image and twelve successful cases remain preserved without replay.

## Finite capture and native transcription

`SUNSTONE_I2S_TRANSCRIBE=ON` adds a native mimus task to `sunstone-a733-i2s`.
Configure this mode through the repository root with the pinned mimus source,
FreeRTOS, TinyUSB, SMP newlib and C++ toolchain inputs. Receive and playback
images built without that option retain the original session and behavior.
No microphone is connected; capture, inference and electrical operation have
no physical qualification in this mode.

The 640-byte transcription session uses version 2 and selects between 16,000 and 80,000
samples: one to five seconds of mono signed 16-bit little-endian PCM at 16 kHz.
The four existing DMA pages remain the ring. The client copies ordered completed
periods into a static 160,000-byte buffer and returns each consumed period before
requesting the next. It copies only the selected prefix of the final period.
The underlying provider checks DMA completion and FIFO error status before
publishing each period. A missing or repeated ticket, discontinuous frame index,
failed period or failed stop refuses the utterance.

After the final selected sample, the client requests the existing CANCEL operation
as a planned finite stop. It requires the terminal CANCELED result with retained
buffers. User cancellation sets a separate stopping flag and cannot produce a
successful utterance. The driver disables requests, stops DMA and disables the
controller before publishing that terminal result. All DMA pages and leases remain
reserved; no allocator release, REAP, CLOSE or reset follows transcription.
Failures in the discarded tail cannot establish additional valid samples.

A separate inference task runs at priority 3 while the PMIC, USB and client tasks
remain at priority 4 and the DMA worker at priority 5. The inference task never
polls or changes the audio provider. It acquires the immutable completed capture,
builds an ordinary finite WAV header and calls the shared recorded transcription
function in `apps/mimus/recorded.cc`. The existing model loader, recognition,
punctuation and formatting run without a replacement model or resampler.
The model and temporary objects are destroyed before successful publication.

The unchanged prepared base model occupies 7,501,968 bytes at `0x44000000`;
the overlay occupies 6,560,592 bytes at `0x44800000`. Admission fixes both lengths
and SHA-256 values, and inference verifies the actual bytes before loading.
The session authenticates those fields. Offline preparation freezes the exact
model files and requires exclusive DDR ownership of `0x44000000..0x45000000`
and the ASR arena at `0x46000000..0x47000000`, including exclusion of remote writers
and cache aliases. Original tensor bodies remain unchanged. The base SHA-256 is
`f73ccd024645387d2679eea97ae11d0c33f5e4be81b39a9430d1f5d285e8089d`;
the overlay SHA-256 is
`d877604c759b688e46f7f9b4346c7d87496faf6ee64cbb8f1f9837652aade6b2`.

The image uses the existing SMP newlib locks and task reentrancy, C++ startup
constructors and a four-MiB runtime heap. Its private aligned scratch buffer has
97,280 bytes. The finite WAV, captured PCM, model slots, scratch and transcript
remain owned throughout inference and retained afterward. The higher-priority
client propagates cancellation and deadline failure through the recorded engine's
existing stop argument. Model loading remains finite and cannot be interrupted
mid-call by that argument. The sixty-second supervised lifetime denies late
publication; its adequacy for useful physical recognition remains unqualified.

Only a successful return while the capture remains live publishes the bounded
transcript. Buffer overflow, user cancellation, DMA/provider failure or expiry
refuses it. USB operation 3 copies transcript chunks of at most 4,096 bytes.
Its distinct response binds the epoch, generation, immutable session digest,
selected sample count, captured PCM SHA-256 and complete transcript SHA-256.
The observer decodes saved transcript frames with `--operation 3` and derives
the required sample count from the authenticated session. The original PCM decoder accepts
`--pcm-bytes` for the longer finite capture. Pending and failed responses expose
no partial transcript or completion metadata. The existing supervised host tool
selects the duration from the validated frozen profile, durably copies all PCM,
then copies the transcript through bounded fragmented reads. It checks the full
text hash and requires the transcript to identify the durably copied PCM before
recording success. The target lock, queue claim and first-failure behavior remain
those of the existing supervised transport.

The focused runner is `checks/i2s-mimus.py`. Its capture fixture models copied
DMA replies while executing the actual client. Its recognition fixture uses the
actual pinned model bytes and executes WAV construction, hashing and publication;
it models only the shared recorded engine's returned text. These boundaries do
not establish microphone timing, recognition accuracy or physical operation.

The focused C and C++ admission, client and recognition-wrapper fixtures passed
with AddressSanitizer and UndefinedBehaviorSanitizer. The observer and
fragmented-host cases also passed. The host cases include a seven-second modeled wait
before successful inference completion; they use no real sleeps or recognition
replay. The native transcription image also links the actual recorded engine,
model code and SMP newlib runtime. Its ELF passes the preparation tool's image
and session-layout checks, including a four-MiB heap disjoint from the DMA pool.
All 107 linked application and ASR objects and 494 compiler dependencies are
preserved under `~/.cache/sunstoneos/evidence/i2s-mimus-20261017/central/`.
The image SHA-256 is
`5f95175598c58817a9e129ee8ee6887a92e2e0dd631e5af1ef41715d86209710`.
Focused inputs, model files, executables, logs and saved copied-output fixtures
are under the same evidence directory's `focused/` subdirectory. Failed setup
compiles remain preserved. Native C boot uses the I2S session path; the existing
native mimus timing definition applies only to the C++ engine and adapter.
No captured speech, transcript accuracy, inference deadline or physical I2S
operation is qualified by these software results.

## Retained launch session

The I2S image now seals its complete admitted session during primary EL3 boot.
It copies and rehashes the session against the private contract, validates the
copy and publishes the session together with its readiness receipt before cached
setup. A failed seal cannot be retried. Application startup obtains only that
retained copy before creating any shared provider; its runtime deadline changes
remain separate from the admitted bytes.

The transcription profile's sealed bytes include capture duration and fixed model
identities. Later changes to the live session cannot replace those values, DMA
ownership or evidence. The local boot patch follows the maintained supervised
patch and leaves shared boot sources unchanged. Existing DMA, audio, model and
physical qualification evidence is reused without replay.

The focused correction passed five binding checks against the transcription
image in `~/.cache/sunstoneos/evidence/i2s-binding-20261018/`. Nominal records use
the final build ID, embedded PCM digest, pinned model identities and hashes of
written software-only evidence. The checks refuse shape-valid DMA-owner and
capture-duration changes before sealing, refuse failed publication, and verify
that later live changes cannot reach the first provider. Publication includes
all session bytes and the readiness receipt. Cache publication, the counter and
provider entry are modeled. One native link includes the unchanged recognizer;
no audio, DMA, inference or model result was executed again.

## Simultaneous capture and playback

Session direction 2 selects duplex operation. Its DMA service channel mask and
CPU route mask must include channels 0 and 1. The inherited pin array records
PB5, PB6, PB8 and PB7 in that order. The protected session keeps its existing
552-byte layout; the fourth pin configuration occupies previously padded bytes.
Transcription retains its receive-only admission.

The client commits the recorded PCM to the playback catalogue, starts both
directions and alternates copied period requests. Success requires 16,384
captured bytes and 16,384 bytes actually consumed by transmit DMA, with four
completed periods per direction. It then cancels both directions and observes
each terminal CANCELED result with its buffer hold intact. Output publication
occurs only after those counts and both drain observations. The copied output
contains captured PCM; the transmit completion counts describe the recorded
playback memory consumed by DMA.

Duplex USB responses use version 2 and a 96-byte header. After the existing
72-byte identity header, six little-endian 32-bit values report capture bytes,
playback bytes, capture periods, playback periods, drained-direction mask and a
reserved zero. Successful responses require `(16384, 16384, 4, 4, 3, 0)`;
other states expose zero completion metadata. The host validates these fields
before accepting output. Single-direction responses retain version 1 and the
72-byte header.

The native duplex build, focused client and USB checks, two-catalogue native
constructor, producer-bound packaging and host copy checks pass. The host cases
use frames emitted by the compiled USB producer with the linked image's actual
session layout, build identity and recorded PCM digest. Missing transmit counts,
incomplete drain metadata and truncated headers are refused. Focused Astra
review found no unresolved correctness defects. Exact sources, dependencies,
images and results are preserved under
`~/.cache/sunstoneos/evidence/i2s-duplex-20261018/`. No physical launch is queued;
external audio devices and physical duplex continuity remain unqualified.

## Repeated streaming

Session directions 3, 4 and 5 select repeated capture, playback and duplex,
respectively. They retain the 552-byte version 1 session and use the same
providers, two-channel duplex binding and admitted lifetime as finite modes.
Transcription remains receive-only and finite. Playback repeats the embedded PCM
periods; it does not accept arbitrary live playback data.

Repeated modes use [fixed hardware-linked rings](i2s-stream.md#hardware-linked-repeated-streaming).
Playback sources stay immutable while DMA repeats them. Capture uses the service's
checked private staging copy, then copies its 4,096-byte period into one of four
private USB slots. Physical RX admission remains blocked by the documented
package/write/cursor ordering gap. Each slot contains 2,048 mono frames and
remains immutable until the serialized USB consumer has delivered its complete
contents and accepted an acknowledgment for its exact 64-bit ticket. Slot
publication and reuse use release/acquire ordering between the two tasks.

A full copied ring stops new reuse and starts the existing cancellation protocol;
it never overwrites unread samples. The staging period that detected the full
ring remains client-held without being copied or returned. Transmit underrun or
another service failure also stops the pair. Cancellation observes each configured
direction's terminal result and buffer hold before publishing its drain bit.
Catalogues, leases and uncopied periods remain retained. Previously copied slots
can still be read and acknowledged after orderly cancellation or a drained device
failure, while the admitted session remains live. An uncertain failure retains
resources and cannot claim a terminal drain.

Repeated USB responses use version 3 with a 128-byte header. Requests remain
three little-endian 32-bit words. Operation 4 reads an offset and count from the
current copied period; operation 5 acknowledges its ticket, encoded as low and
high words. Operations 0 and 2 report status and request cancellation. A queued
response or a partial read cannot authorize reuse: the USB transfer callback must
report completion of the entire copied period before operation 5 succeeds.
Early, stale, duplicate and foreign tickets are refused.

The first 72 response bytes retain the existing identity and session digest.
The remaining fields are little-endian:

| Offset | Field | Width |
| --- | --- | --- |
| 72 | Copied-period ticket, or acknowledged ticket | 64 bits |
| 80 | First frame of copied period | 64 bits |
| 88 | Copied-period frame count | 32 bits |
| 92 | Raw capture counter | 32 bits |
| 96 | Observed completed capture frames | 64 bits |
| 104 | Observed completed playback frames | 64 bits |
| 112 | Drained-direction mask | 32 bits |
| 116 | Failure flags: 1 copied-ring overrun, 2 service failure | 32 bits |
| 120 | Reserved zero | 64 bits |

Completion counters report actual observed DMA package totals, including final
progress sampled during cancellation. A playback result can account for several
periods at once. Capture totals can include periods that could not be copied. They do not imply that all completed
DMA periods were copied or delivered. In duplex mode, drain bits 0 and 1 describe
capture and playback; single-direction modes use bit 0. State 2 means orderly
cancellation with all configured directions drained. State 3 reports failure;
the host must also inspect the drain mask before claiming a completed stop.

The supervised host defaults to eight periods, accepts a target of 1 through
256, and requires both direction counts for duplex. It saves each copied period
before acknowledging it, then cancels and drains any copied backlog. Failure
flags remain sticky while that backlog is collected. Completion requires an empty
GET issued after a previous response established terminal state, so a last
publication cannot be missed. Empty USB transfers are
bounded per response. The session deadline still bounds the operation.

The native image links, eight ownership cases and two USB delivery cases pass.
Producer-derived host frames exercise 64-bit tickets, both-direction accounting,
partial and duplicate ACK refusal, canceled and failed backlog, failure before
drain, durable copy before ACK, malformed metadata and bounded empty USB reads.
Inputs and results are preserved in
`~/.cache/sunstoneos/evidence/i2s-repeated-20261018/`. Existing finite and duplex
provider evidence remains applicable. These results establish software ownership
and protocol behavior, not physical audio continuity or wire drain.

## Runtime PCM uses copied input and finite TX leases

Session direction 6 selects runtime playback. It retains the 552-byte version 1
session, one DMA channel and four period pages. Directions 0 through 5 keep their
existing protocols. Direction 4 is immutable cyclic replay; direction 6 refills
CPU-owned periods through the finite stream API. The session's recorded-PCM
digest still identifies the embedded development fixture. It does not identify
the caller's runtime data; the host preserves that data and its SHA256 separately.

The format is signed 16-bit little-endian PCM, one channel at 16 kHz. Each
4,096-byte period contains 2,048 frames. The selected native plan has two memory
bytes per frame, despite its two 32-bit wire slots. A caller supplies complete
periods; the application does not resample, insert silence or create padding.

USB copies input into four private slots. One serialized upload fills the next
unpublished slot. The complete period is published with release ordering; the
audio task uses acquire ordering before reading it. That task alone owns DMA
service requests. It obtains `TX_FILL`, copies input into the corresponding DMA
allocation and returns the exact lease ticket with PUT. After four committed
periods it starts playback. Each subsequent `TX_MEMORY_CONSUMED` grants a new
CPU lease for refill. A completed memory transfer never implies serial output
completion.

Returning a copied input slot after its CPU copy allows another upload. It does
not return the DMA allocation. The DMA lease remains held until its exact PUT.
A pending service reply is collected under its original operation before the
client selects another request, even when new input or cancellation arrives.
A missing input period remains unavailable to DMA. The client retains its lease
and observes service status; the finite stream stops on missing committed storage,
FIFO errors or deadlines. No unread copied input or leased DMA buffer is silently
overwritten.

### Version 4 accepts one sequenced period per upload

All requests begin with three little-endian 32-bit words. Operation 0 requests
status and operation 2 requests cancellation; both remaining words must be zero.
Operation 6 carries the next 64-bit input sequence as low and high words, followed
by exactly 4,096 payload bytes. Sequences start at one and must increase by one.
One request remains in flight until its response transfer completes.

A partial upload stays unpublished and must finish within one second. USB accepts
fragmented headers and payloads. A full copied ring causes the complete upload
to be discarded through a bounded scratch buffer; the response reports BUSY and
does not advance the sequence. The caller may retry that sequence after credit
returns. Stale sequences, malformed extents and expired partial uploads retain
the failed session. Closure racing publication discards the unpublished period
and preserves the terminal status path.

Responses are 128 bytes with magic `0x53493241` and version 4. Bytes 24 through
71 retain epoch, generation and the immutable session digest. The payload count
at byte 16 is zero; byte 20 contains the fixed period size, 4,096. Byte 12 echoes
the operation. Remaining fields are little-endian:

| Offset | Field | Width |
| --- | --- | --- |
| 72 | Last fully accepted input sequence | 64 bits |
| 80 | Frames committed through successful PUT | 64 bits |
| 88 | Frames observed in `TX_MEMORY_CONSUMED` replies | 64 bits |
| 96 | Free copied-input slots, from zero through four | 32 bits |
| 100 | Sticky service-failure flag | 32 bits |
| 104 | Observed terminal DMA-stop flag | 32 bits |
| 108 | Upload result: zero for status, one accepted, two BUSY/discarded | 32 bits |
| 112 | Reserved zero | 128 bits |

The response state at byte 8 is zero before work, one while active, two after
orderly cancellation or three on failure. A terminal state without the DMA-stop
flag does not establish drain. Memory-consumed frames count only collected
service completions. They can be lower than all transfers that occurred before
cancellation. Catalogues, copied input and held leases remain retained until the
admitted recovery procedure; this mode does not release them to another client.

The existing supervised host accepts `--runtime-pcm` and `--runtime-periods`.
The raw file must contain the requested prefix plus four caller-provided guard
periods, with at most 260 periods total. The host uploads successive periods
according to credit and exact acceptance, then cancels once the requested prefix
has memory completions. Guard periods keep refill ahead of that prefix. They may
be truncated by cancellation; the host does not claim the entire file reached
the wire. It preserves input, responses, counts and failure/drain status before
reporting a result. No FINISH or WIRE_DRAINED operation exists.

### Offline runtime input evidence

`~/.cache/sunstoneos/evidence/i2s-runtime-pcm-20261019/` preserves the actual
native link, nine compiled USB/client cases, eight host cases using the compiled
USB metadata producer, and an actual FreeRTOS/provider composition. The provider
sequence consumes eight distinct periods through refills and retains resources
after cancellation. It checks the selected plan's two-byte memory frame size.

The USB/client cases cover partial publication, full-ring refusal, stale input,
a pending STATUS during input arrival, closure during publication, cancellation
with a pending GET, service failure and the partial-upload deadline. Host cases
cover input/session binding, memory-count bounds, failure/drain reporting and
malformed or undurable results. An injected audio-progress interleaving verifies
that the compiled USB producer reads consumed frames before committed frames,
so its counter bounds remain consistent. Register, DMA, USB transport and cache effects
are modeled. These results establish software behavior, not physical audio,
cache visibility, finite FIFO scheduling bounds or serial-wire completion.

The separate source inspection in
`~/.cache/sunstoneos/evidence/i2s-playback-drain-20261019/` records why the pinned
manual and Linux stop path do not establish graceful wire drain. Cyclic RX's
undocumented package/write/cursor ordering gap remains unchanged.
