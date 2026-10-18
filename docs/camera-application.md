# Native OV5640 application

The dedicated `sunstone-a733-camera` image connects the existing native OV5640
binding, shared providers and USB0 device transport. It captures one 640 × 480
NV16 frame: 307,200 luminance bytes followed by 307,200 interleaved chroma bytes.
Physical camera operation remains unqualified, and module availability is
unconfirmed.

The image reuses the maintained cached boot, EL3 exception routing and native USB provider
construction. It starts three privileged FreeRTOS tasks for the PMIC provider,
USB0 and camera ownership. It does not start storage workers or source power.
The immutable session page binds the camera record, shared provider settings,
media configuration, memory evidence and recovery evidence to the exact image,
epoch, generation and lease. The protected launch contract hashes the complete
session. Creating hashes or setting proof fields does not establish authority.

Primary preparation constructs the camera and reserves both its device-owned
backing and a separate CPU-owned output allocation before starting the scheduler.
At that point, media and rail headers describe future providers without claiming
live leases. The camera task obtains actual rail and PCK leases, initializes the
media controller, and advances the native camera. The PMIC task independently
services supply observations. Only the camera task uses the cached singleton allocator after startup;
USB reads published output without touching allocator metadata.

The camera task copies both completed planes through the native copy interface.
It publishes output only after both copies succeed and its deadline remains live.
A failed or cancelled capture leaves every resource reserved. Partial output is
unavailable. Cancellation stops further camera advancement; it does not issue a
bus STOP, assert reset, release buffers or claim that transactions drained.
The fixed admitted recovery window bounds the image lifetime. This image neither
arms nor reloads the watchdog and requires genuine launch/recovery admission.
Intentional watchdog expiry remains deferred.

USB requests contain three little-endian 32-bit values: operation, byte offset
and requested byte count. Operation zero queries state; operation one reads at
most 4,096 bytes; operation two cancels. State zero means startup, one means
capture pending, two means complete output and three means retained failure.
Responses include the epoch, generation and complete session SHA256 before
payload bytes. The USB task handles one request at a time and retains its response
until the transport reports completion. The host capture command requires an
explicit USB bus/address, epoch, generation and session digest. It stops on any
transport failure and never retries a refused capture automatically.

Build the image through `apps/camera/CMakeLists.txt` with the existing AArch64
Clang toolchain and pinned FreeRTOS/TinyUSB paths. The build fragment is
`cmake/a733-camera-application.cmake`. Raw NV16 capture uses
`apps/camera/observe.py`; callers must hold the project board lock and satisfy
physical launch admission before opening the explicitly selected device.

Actual launch still requires qualified memory coherence and guard extent,
remote-firmware exclusion, resolved inherited transactions for all ten IOMMU
masters, accessible register domains, module supplies, reset wiring, receiver
timing, analog trim, USB sink ownership and independent bounded recovery.
The [native camera record](camera-native.md) defines these conditions. The
application supplies executable wiring; software checks cannot establish them.

The affected image build and nine application boundary cases pass. The cases
cover immutable admission, complete output publication, a failed second-plane
copy, cancellation, expired deadlines, transient resource contention, USB frame
reads, pending-frame refusal and USB cancellation. Application fixtures model
native controller boundaries or endpoint I/O; the unchanged native camera
composition evidence supplies controller coverage. No board was queried.
Exact sources, dependencies, objects, linked images and check results are retained
under `~/.cache/sunstoneos/evidence/camera-application-20261017/batch1/`.
The native image SHA256 is
`5498d15ed2b683be334da4bd4d051d1e47ca8566c70f48d6af229571b75575fa`.
