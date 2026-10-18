# Native OV5640 capture and supervised observation

The dedicated `sunstone-a733-camera` image connects the existing OV5640 binding,
shared providers and USB0 device transport. It retains one 640 × 480 NV16 frame:
307,200 luminance bytes followed by 307,200 interleaved chroma bytes. No MIPI
camera is connected; physical capture waits for the accessory and remains unqualified.

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

The camera task copies both completed planes through the native copy interface.
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

The wire format retains the existing twelve-byte request and seventy-two-byte
response header. Reads return at most 4,096 bytes. The collector validates epoch,
generation and the immutable session digest on every response. It records all
fragments and requires all 150 chunks before publishing a 614,400-byte NV16
artifact and metadata. A failed transfer, inconsistent response, deadline or
journal failure leaves success unestablished and requires manual recovery.

The root CMake fragment selects the supervised mode without a separate startup
framework. Build `sunstone-a733-camera` with the pinned FreeRTOS/TinyUSB sources
and existing AArch64 toolchain. The [native camera contract](camera-native.md)
retains the device ownership and completed-frame requirements. Continuous
capture, wider modes, interrupts and general reclamation remain outside this
single-frame application and still require implementation and qualification.

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
