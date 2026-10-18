# Native fixed convolution application

The native application combines the cached eight-core boot image, a permanent
NPU memory reservation, the PMIC provider, NPU startup and the fixed convolution
client. It copies 192 input bytes into private buffers, waits for a completed
job, copies 72 output bytes, compares them with the pinned reference, and reaps
the job before publishing a result through USB.

The first profile requires inherited NPU PLL operation at 492 MHz and a stable
800 mV DCDC2 supply. The existing startup implementation checks those values.
Cold PLL provisioning and broader model support remain separate integrations.

## Build and admission

Build `apps/npu-convolution` as a standalone CMake project with the pinned
FreeRTOS and TinyUSB source directories. Supply `A733_NPU_HANDOFF_SOURCE`,
`A733_NPU_MODEL`, `A733_NPU_INPUT` and `A733_NPU_REFERENCE`. The artifact generator
checks the fixed hashes recorded in `apps/npu-convolution/artifacts.py`; it does
not invoke a model compiler. The existing compiler artifacts remain in the
external cache. The target is `a733-npu-convolution`.

The handoff source defines the immutable `a733_npu_application_boot` record.
Its session identifiers must match the compact observer boot contract. The
record binds NPU and command admission, PMIC bus and rail authority, pin
ownership and USB ownership. Owners are nonzero and distinct. Input deadlines
are zero; initialization derives bounded deadlines from the authenticated boot
counter deadline. No default qualified record is provided.

A physical launch needs separately established cold-cache and alias exclusion,
exclusive CPU and memory ownership, coherent mappings, complete NPU SRAM
ownership, effective command confinement, quiescent remote writers, and an
independently qualified watchdog with ROM return. USB0, its PHY, clocks and
complete shared register words must remain owned for the session. Admission
flags identify that evidence; they cannot establish physical facts.

The standalone target applies private FreeRTOS privilege and vector patches.
Only the actual bootstrap, PMIC provider and NPU worker tasks receive EL1t
execution. The client remains at EL0; USB uses the existing authenticated SVC
entry. Current image mappings do not isolate arbitrary EL0 writes to all private
metadata. These guards therefore require trusted application code and immutable
platform metadata.

## Memory and service lifetime

Initialization runs on the primary core before scheduler release, after the
existing eight-core preparation guard. It creates one shared counter, clock
registry, register and I2C mutexes, pin manager and rail ledger. The real PMIC
provider task and startup task use these same objects. USB retains its exclusive
USB0 and USB reference-clock tokens in that registry.

The actual NPU reservation constructor consumes 265 pages, or 1,085,440 bytes,
from the compact 2 MiB write-back pool. Its complete parent allocation stays in
DEVICE state for the image lifetime. The convolution client controls CPU access
to individual buffers through its existing copy and cache protocol. Model data,
command streams, tables, task stacks, requests and results retain platform
lifetime. This profile does not establish capacity for concurrent use of every
board device.

The bootstrap task polls actual PMIC and NPU startup until the deferred worker
can be activated. Every application-bound NPU operation authenticates the real
worker and permanent allocation, then delegates to the existing native driver.
Atomic clock, registry or retained-lock faults latch an application failure.
The result encoder checks those same indicators before publishing status.

A deadline, service failure or uncertain provider state retains the session.
The application does not close, reset, retry or free the NPU after uncertainty.
The independent watchdog remains responsible for recovery. Successful output
publication follows a matched DONE reply, the actual output-copy API, reference
comparison and explicit REAP.

## USB observation

`apps/npu-convolution/observe.py` provides `prepare`, `validate` and `launch`
commands. Preparation consumes the reviewed native ELF manifest and the preserved
compact observer launcher bundle. It verifies the embedded fixed artifacts,
copies launch inputs into a new bundle, and records their identities. A bundle
prepared with `--refusal` is rejected before device discovery.

The live command reuses the existing board claim, host lock, protected FEL
transfer, quarantine and recovery mechanisms. It requires NPU-specific software
and platform evidence in addition to the existing DDR and independent-recovery
records. Only the board coordinator may schedule physical execution.

USB command `0x33` returns a 512-byte frame containing the boot identity, host
challenge, build identity, controller and admission identities, fixed artifact
digests, copied output, output digest and completion times. Status distinguishes
pending, matching output, mismatching output and fault. The host validates and
saves a complete frame before recording a terminal result. It reports success
only when a matching output is durably captured and a fresh ROM response follows.
That ROM response alone does not establish watchdog causation.

## Focused validation

`checks/npu-application.py` links the actual native image with the explicit
refusal fixture in `checks/npu-application.c`. It preserves generated assembly,
source dependencies, fixed artifacts, tool identities, ELF and map before
instruction execution. The prepared physical image retains zero admission flags.

The selected instruction check constructs the real application, shared providers,
FreeRTOS tasks, reserved buffers and convolution service in modeled architectural
state. Separate client cases inject matching reference bytes, mismatching bytes
or a failed service reply at the device-service boundary. The actual client
wrappers copy input and output, enforce matched tickets, reap and publish. A late
registry failure must override a previously completed result and refuse native
NPU access while retaining the parent.

The same check exercises the native frame encoder, durable host capture and
refusal-only launch guard. Architectural translation/cache effects and service
completion are modeled. The unchanged convolution SHA-256 helper delegates to
host hashlib over the actual buffer bytes to avoid replaying its instruction
suite. Its existing constituent evidence is reused. Injecting reference output checks publication and
comparison; it does not execute numerical inference. Existing controller,
assembler, service, PMIC, vector and USB constituent evidence remains applicable
to its original inputs. Physical NPU execution remains unqualified.

The first native image build and link passed with no undefined symbols. The
focused instruction composition passed on that preserved ELF. Preparation used
1,087 modeled AT instructions and performed no device MMIO. Matching and
mismatching cases each executed OPEN, SUBMIT, STATUS and REAP; the failed reply
stopped before REAP and completion publication. Raw inputs, image and case
records are in `~/.cache/sunstoneos/evidence/npu-application-20261017/batch1`.
