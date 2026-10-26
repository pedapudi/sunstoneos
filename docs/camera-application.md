# Native OV5640 capture and supervised observation

The dedicated `sunstone-a733-camera` image connects the existing OV5640 binding,
shared providers and USB0 device transport on CAM1 or CAM2. It retains one readable, build-selected
NV16 frame. Each of its luminance and interleaved-chroma planes contains
width × height bytes; the default VGA frame contains 614,400 bytes in total. No MIPI camera is connected; physical capture waits for the accessory and
remains unqualified.

The default image preserves its qualified watchdog admission requirements.
`SUNSTONE_CAMERA_SUPERVISED=ON` selects a separate session and launch ABI for
supervised RAM-only use. Its terminal path retains every CPU, device, lease and
allocation until manual whole-board power removal. It neither arms a watchdog
nor claims independent automatic recovery. The supervised native initializer
accepts an explicit manual-recovery record; the default initializer continues
to require its original recovery record.

Both modes require actual module reset wiring, qualified supplies and bank
voltage, authenticated analog trim and receiver timing, stable clocks and pin
ownership. All ten IOMMU masters and remote firmware must be excluded, and prior
transactions and page walks must be resolved. Both banks start disabled; both
banks and every master's register domain remain accessible through the final
fault scan. The qualified memory contract must cover coherence, aliases and the
four-kibibyte prefetch guards. Supervision changes none of these conditions.

Primary preparation constructs the camera and reserves its device-owned backing
and a separate CPU-owned output allocation before starting FreeRTOS. Media and
rail headers describe future providers without claiming live leases. One camera
task obtains the actual rail and PCK leases, initializes media and advances the
existing sensor, CSI, route and camera DMA controllers. The PMIC provider and
USB0 run independently. Only the camera owner uses the cached singleton
allocator during preparation.

When the session's camera record requests discarded images, the native binding
captures and rearms over them before the readable image; the application sees
only that image. The camera task copies both completed planes through the native
copy interface.
It publishes output only after both copies succeed and the deadline remains
live. USB reads the immutable output without accessing allocator metadata.
Failed or cancelled capture retains every resource. Partial copies are
unavailable. Cancellation stops further advancement without issuing bus STOP,
asserting reset, releasing memory or claiming that transactions drained. A USB
assertion or invalid interface revokes application admission and the common
counter provider before parking. Subsequent native operations refuse that
provider; an already executing bounded callback may finish.

The supervised session is a value-only immutable record. Uploaded provider
and camera deadlines are zero. The native runtime derives its absolute deadline
from the live sixty-second launch contract while retaining the original session
for response identity. The host's durable capture window is shorter than the
native deadline. A deadline never authorizes reset, resource release or reuse.

`apps/camera/prepare.py` adapts the maintained supervised launcher and validates
the actual ELF, linked build identity, DWARF session layout and complete upload
membership. It freezes the session, entry evidence, image and launcher. Required
entry facts include physical module/reset/supply admission, all-master exclusion
and prior drain, powered register access, guard qualification, PMIC/USB sink
ownership, DDR ownership and manual whole-board power removal. Hashes and flags
identify reviewed facts; creating them does not establish those facts.

The admission record binds exact shared, camera and media configuration hashes.
For `camera_sha256`, the helper zeroes only the repeated `camera.evidence` digest
before hashing that camera record, avoiding a circular admission hash. The
complete session hash still covers the actual digest, and admission requires
that it equal the top-level evidence digest. No reset, drain, guard, voltage,
timing or ownership field is excluded from the configuration hash.

`tools/check-a733-camera-supervised.py` uses the shared `run_session` path,
coordinator claim, exclusive target lock, frozen approval binding, protected SRAM
backup and one-attempt transport policy. It accepts USB identity `1f3a:efe8` only
with product `sunstoneos native camera` and serial `sunstoneos-a733-camera`.
`apps/camera/observe.py` decodes saved responses offline and opens no device.
Physical preparation and capture remain the board coordinator's responsibility.

The wire format retains the existing twelve-byte request. Still responses have
a 72-byte header; finite-stream responses have an 80-byte header. Reads return at most 4,096 bytes. USB0 uses the shared board
service in `apps/board/usb0_service.h` for its descriptors, provider startup
and polling loop, which repeats passes within one tick while the transfer makes
progress. The camera image's build identity hashes that header. When an IN
transfer completes, the transmit callback refills the vendor FIFO before the
TinyUSB vendor class decides whether to send a zero-length packet, so a
response cannot end early on a 512-byte boundary. The collector validates epoch,
generation and the immutable session digest on every response. It records all
fragments and requires the entire selected payload before publishing an NV16
artifact and its dimensions. The final request may be shorter than 4096 bytes. A failed transfer, inconsistent response, deadline or
journal failure leaves success unestablished and requires manual recovery.

The root CMake fragment selects the supervised mode without a separate startup
framework. Build `sunstone-a733-camera` with the pinned FreeRTOS/TinyUSB sources
and existing AArch64 toolchain. The [native camera contract](camera-native.md)
retains the device ownership and completed-frame requirements. Continuous
capture, wider modes, interrupts and general reclamation remain outside this
application and still require implementation and qualification.

## Retained launch session

The camera image now retains the complete authenticated session during primary
EL3 admission. Its local boot hook copies the session, rehashes that copy against
the private admitted contract, validates it and publishes the whole retained
object, including readiness, before cached setup. A failed attempt cannot be
retried; failed publication makes the copy unavailable.

Application startup consumes that retained copy before initializing any shared
USB or camera provider. It preserves the admitted bytes separately from the
runtime deadline adjustments. Mutating the original live session after sealing
cannot replace camera ownership, media settings or evidence identities. The
patch applies to the camera's own boot object; shared boot sources are unchanged.
Existing camera provider, pixel, transport and physical qualification limits
remain in force.

Focused binding checks passed against the corrected native image under
`~/.cache/sunstoneos/evidence/camera-binding-20261018/`. The nominal session came
from the existing production preparation functions using the final ELF's build
ID and exact evidence hashes. The checks execute full contract validation,
shape-valid mutations before sealing, failed publication and later consumption
of the retained copy. They verify that publication covers both session bytes and
the readiness receipt. Cache publication and the first provider boundary are
modeled; no camera provider, sensor or pixel work was repeated. The generated
camera boot hook precedes cached setup and is the target's sole boot object.

## Finite streaming copies one frame and retains the hardware

Configure the dedicated camera application with `SUNSTONE_CAMERA_SUPERVISED=ON`
and `SUNSTONE_CAMERA_STREAM=ON` to select continuous capture. The stream option
requires supervised retention and is included in the build identity. The
ordinary still-image profile remains available without that option.

The privileged owner polls the native camera every tick, consumes any admitted
discard frames, and copies one delivered frame in 4 KiB pieces. It publishes
nothing until the entire copy completes. The owner gives the slot back before
requesting sensor stop. The existing packet-quiescence provider refuses, so the
image keeps all source buffers, mappings, clocks and power leases retained.
Physical sensor, slot timing, coherence, guard and drain qualification still
require an attached module.

The stream response uses version 2 and an 80-byte header. Its first 72 bytes
retain the existing layout. State 4 means that a complete immutable frame is
available while hardware remains retained. The word at offset 72 reports
sensor stop confirmed in bit 0, drain proved in bit 1, and hardware retained
in bit 2. Bit 1 remains clear; offset 76 is zero. State 4 may carry flags 4 or
5, so output availability alone never claims sensor-stop completion. A partial
copy reports state 3 and supplies no output. Cancellation still prevents reads.

The host decoder and collector preserve those distinctions. They save the
complete NV16 output with `camera_result=copied-frame-stream-retained` and
separate sensor-stop, drain and retention fields. The supervised runner does
not classify that retained result as successful lifecycle completion. Version
1 still-image responses keep their existing states and 72-byte header.

The native stream image links successfully. Focused AArch64 instruction-model
checks cover capture polling between all 150 copy pieces, withheld partial
output, retained sensor-stop failure, cancellation, and actual USB response
framing. The host decoder consumes the native encoded response; the collector
saves a complete modeled frame and rejects unsupported drain-status claims.
The existing still-output framing cases pass. Evidence is under
`~/.cache/sunstoneos/evidence/resume-camera-native-1025/application/` and
`host-collection/`. No physical camera execution is claimed.


The authenticated media field `camera_mclk_mask` selects CAM1 (2) or CAM2 (4)
for both still and finite-stream images. Other masks refuse. The host binds
that media configuration and the camera's explicit pin/reset/timing record to
the same reviewed entry facts; changing the connector invalidates those facts.
The boot/session layout and NV16 USB response are unchanged. Host image admission
accepts the actual supervised still or stream initializer retained in executable
ELF sections,
so linker removal of the unused still initializer does not reject a stream image.

## Image-bound sizes and full-frame USB capture

Build with `-DSUNSTONE_CAMERA_SIZE=QSXGA` for 2592 × 1944, or select another
existing size listed in the [native layout](designs/camera-native-modes.md).
The default remains VGA. Both still and supervised finite-stream builds support
the full size range. No session or wire-layout extension is needed.

The immutable ELF profile binds dimensions, formats, operation, payload/slot/table
lengths, and linker pool endpoints. Preparation validates that profile against
the actual executable entry and supported sensor sizes. A present malformed
profile is refused. Preserved images without the profile retain legacy VGA
interpretation. The existing image and memory digests bind the selected image
and its exact compact or external DDR ownership record.

Larger profiles require reviewed DDR evidence for `0x41000000..0x42200000` and
`0x48000000..0x4a000000`. Preparation admits the latter only as a distinct,
non-executable NOLOAD reservation. Uploads contain initialized image segments and
the existing session; the pool is not uploaded. The existing boot and supervised
session deadlines remain unchanged.

Host collection derives the 72/80-byte header from the response version, including
when a header spans multiple reads. It checks that version against the selected
image and uses one absolute deadline through all fragments. At maximum size it
collects 2461 payload responses, ending with 1536 bytes. Saved-frame decoding
accepts `--elf` to obtain dimensions from the validated native image.

`camera-modes-1026/host-final-*` evidence checks actual VGA, maximum-still and
maximum-stream ELF/session/DDR admission and malformed profiles. The actual
maximum-stream boot hook rejects an unsealed session and changed profile fields
before external memory admission. Compiled application/USB checks collect both
maximum payloads through fragmented transport and exercise the final short
response. Hardware transactions and image content in these checks are modeled.

The final maximum-stream image also passes the actual offline `prepare` and
`validate_bundle` path, including SRAM-launcher compilation and frozen evidence.
`prepared-max-stream-result.json` records that the 32 MiB NOLOAD pool is retained
in the reservation list and excluded from upload bytes. Its fixture records do
not establish physical DDR or camera authority.
