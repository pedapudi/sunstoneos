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

Capture copies four successful periods into an immutable application result.
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
The current image SHA256 is
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
