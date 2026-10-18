# Native I2S capture and recorded playback

The dedicated `sunstone-a733-i2s` image connects the existing I2S0 stream service
to actual DMA0 startup, native clock and pin providers, linker-owned memory and
USB0 observation. One launch selects receive or transmit at 16 kHz with one
16-bit PCM channel and two 32-bit wire slots. No microphone or playback device
is physically qualified. Full-duplex operation, cyclic DMA and final-wire drain
remain required capabilities outside this profile.

Primary preparation reserves one descriptor page and four disjoint period pages
from the existing singleton memory pool. Each period carries 4,096 payload bytes.
The native DMA policy admits only these exact pages, registered descriptor slots
and the selected I2S0 FIFO direction. Other allocations and the opposite FIFO
receive no authority. The existing RAM-copy constructor keeps its original API
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
