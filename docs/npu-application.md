# Native fixed convolution application

The native application combines the cached eight-core boot image, a permanent
NPU memory reservation, the PMIC provider, NPU startup and the fixed convolution
client. It copies 192 input bytes into private buffers, waits for a completed
job, copies 72 output bytes, compares them with the pinned reference, and reaps
the job before publishing a result through USB.

The watchdog profile adopts an inherited PLL rate that can supply 492 MHz and
requires an existing 800 mV DCDC2 supply. The supervised profile connects the
PLL controller before local startup. Its explicit live-DRAM profile can enable
a disabled PLL while adopting a quiescent NPU domain already ON. Neither profile
programs PMIC voltage. No preserved board measurement
establishes the cold PLL or DCDC2 state. Broader model support remains separate.
The fixed model and command streams come from pinned external NPU compiler
artifacts. DDR initialization also retains its existing pinned boot firmware.
Replacing those dependencies is deferred until hardware functionality and
performance work are complete.

## Build and admission

Build from the repository root or from the standalone `apps/npu-convolution`
CMake project with the pinned FreeRTOS and TinyUSB source directories.
Supply `A733_NPU_HANDOFF_SOURCE`,
`A733_NPU_MODEL`, `A733_NPU_INPUT` and `A733_NPU_REFERENCE`. The artifact generator
checks the fixed hashes recorded in `apps/npu-convolution/artifacts.py`; it does
not invoke a model compiler. The existing compiler artifacts remain in the
external cache. The target is `a733-npu-convolution`.
The root build enables this target only when a handoff source is supplied.
Application-specific source properties stay within its CMake subdirectory.

The handoff source defines the immutable `a733_npu_application_boot` record.
Its session identifiers must match the compact observer boot contract. The
record binds NPU and command admission, PMIC bus and rail authority, pin
ownership and USB ownership. Owners are nonzero and distinct. Input deadlines
are zero; initialization derives bounded deadlines from the authenticated boot
counter deadline. No default qualified record is provided.

A physical launch of the watchdog profile needs established cold-cache and alias exclusion,
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
The selected recovery contract remains responsible for recovery. Successful output
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

The root configuration also builds and links the native application with the
explicit refusal fixture. Its 101 unchanged repository dependencies match the
preserved runtime inputs. The remaining header change is conditional on a
newlib option that this target does not enable. Application operation renaming
remains local to the NPU executable; the root component target is unaffected.
Central artifacts and dependency comparisons are preserved under
`~/.cache/sunstoneos/evidence/npu-application-20261017/central/`.
No runtime cases were repeated, and this fixture image is not deployable.

## Supervised startup and retained recovery

Select `A733_NPU_SUPERVISED=ON` and build
`a733-npu-convolution-supervised`. The root selection is mutually exclusive with
the default handoff-source application. Supply the same three pinned artifacts,
FreeRTOS and TinyUSB sources. The image contains no admitted session and cannot
be launched by the watchdog-profile host tool.

The host supplies one immutable session at `0x41f00000`. Its complete digest,
build identity, epoch, generation, lease and supervisor bind the version 3 boot
contract to this image. The contract uses magic `0x415337334e504331`, a one-second
startup deadline and a twenty-second session window. EL3 authenticates it and
publishes a private digest receipt before cache adoption. The primary EL1
copies and rechecks the session after the eight-core preparation barrier;
it never reads the protected monitor contract. The exported
`a733_npu_session_layout` gives the exact native session and provider offsets.

NPU flags `0xf7` preserve quiescence, complete shared-SRAM ownership, IRQ masking,
remote exclusion, I2C authority and memory confinement. Their new supervision
bit replaces the watchdog-recovery bit. USB flags `0x17` likewise preserve
USB ownership, mappings and remote exclusion while selecting supervision.
The original `0x7f` and `0x0f` watchdog contracts remain unchanged. These records
must identify independently established facts, including command confinement,
pin ownership, actual PLL consumers and independent DRAM/PHY sources. Nonzero
digests, reservation identities and observed idle bits do not establish them.

The bootstrap first samples DCDC2 through the real PMIC task and rail ledger.
An owned startup transition publishes even a partially acquired rail into its
platform resource record. An unsuitable supply stops startup before PLL writes.
The same bootstrap then polls the existing PLL controller through task-bound
native register operations, the common counter and register mutex. Acquisition
checks the selected oscillator/reference and PERI0 routes before any PLL write,
reserves the actual shared parents and exclusive PLL/local NPU scopes, and
retains the authenticated whole-platform reservation. The local startup later
atomically converts PLL ownership to shared and adopts the replacement token;
no release/reacquire gap exists. Existing rail settling, PCK, local reset,
SRAM, backend, memory and command checks still precede service activation.

Session PLL mode 2 retains enabled-PLL retuning and the original domain-OFF
startup. Mode 3 selects [live-DRAM enable](npu-pll.md), with disabled PLL, output
and lock detector, clear LOCK, independent live DDRPLL/PHY sources and a quiescent
domain already ON. Other partial PLL states are refused. The 608-byte session
layout and USB protocol are unchanged; the exact PLL bytes bind the selected mode.

Mode 3 requires both PCK command and status to report ON. Local startup releases
the existing core/AXI/AHB resets and enables the module using the retained
resources. It preserves SRAM reset bit 19 and the SRAM mapping, skips all PCK
timing and power writes, and never requests OFF. The pinned PCK provider likewise
avoids a power transition when a domain is already ON. The default OFF startup
path remains unchanged.

Before startup, the authenticated session must establish abandonment of both
shared SRAM aliases: CPU `0x74000..0xf3fff` and the NPU's internal SRAM window.
No CPU, DMA engine, remote firmware or retained FEL continuation may use them.
The native ELF places all loadable code, data, stacks, vectors, tables and results
in `0x41000000..0x42200000`; the existing all-core preparation barrier precedes
construction. All native tasks and callbacks retain those image allocations.
The supervised image never returns through FEL. Its privileged Device mapping
still permits SRAM access, so this is exclusive private-image ownership, not
isolation against arbitrary privileged code. External users require independent
exclusion; pointer placement and ON status cannot establish that exclusion.

The host requires explicit live-profile snapshot, SRAM-abandonment and supervised
cross-PLL disturbance records. They supplement existing clock, rail and recovery
evidence. The manual's analogue-disturbance warning still applies, and software
timeouts require responsive CPU/interconnect hardware. No physical safety or
successful inference is inferred from the Linux runtime source or modeled tests.

Supervised admission leaves watchdog and reset redirects disabled. Every task,
clock, rail, SRAM and memory reservation remains owned after success, timeout or
failure, until the external supervisor confirms full board power removal.
USB disappearance or cable reconnection alone does not prove power removal or
release ownership. No automatic reset, cold provisioning or voltage writer is
introduced.

The focused supervised check executes the actual linked constructor, PMIC
service and rail consumer, bootstrap, enabled-retune controller and startup
adoption. Its architectural state, device responses and admission evidence are
modeled. The changed cases cover authenticated-session refusal, real rail
publication before PLL adoption, unsuitable supply refusal and unstable-parent
retention. Existing controller, copied-client, numerical-reference and USB
evidence remain in their original scope; this check does not run inference or
establish physical readiness.

The supervised host tools prepare an immutable bundle and capture the existing
NPU result frame. Preparation requires a raw session and a separate admission
record; it supplies no board authority by default:

```sh
python apps/npu-convolution/prepare-supervised.py \
  --image ELF --manifest image-manifest.json --session RAW --admission JSON \
  --output BUNDLE --clang CLANG --lld LLD --objcopy OBJCOPY
python tools/check-a733-npu-supervised.py \
  --bundle BUNDLE --fel FEL --evidence OUT --database DB --claim ID --owner OWNER \
  --supervised-manual-reconnect
```

The queue requires `supervised-ram-recovery` and `npu-supervised-software`
evidence. Despite the launch option's name, reconnecting a cable does not
release resources; recovery requires confirmed full board power removal.
The preparation module's `validate_bundle` function provides offline bundle
validation. It currently has no separate validation command.

The preserved supervised evidence is
`~/.cache/sunstoneos/evidence/npu-supervised-20261017/`. The first root-configured
native link passed. The focused execution passed after two fixture setup
corrections, using that same ELF: a Python buffer conversion and the modeled
interrupt mask before scheduler entry. Native source did not change. The
successful case performed seven PLL writes and adopted the replacement clock
token; wrong voltage and an unstable PERI0 parent performed no PLL writes.
Both failures retained their acquired rail. The check stops at token adoption;
that evidence does not cover full local startup, worker activation or inference.

The supervised host adapter passes 33 packaging/capture cases and five additional
checks that bind queue approval to the initially validated bytes. Baseline cases
were preserved when that binding changed. The host batch built its SRAM launcher
once and reused the native ELF. Evidence is under
`npu-supervised-host-20261017/`; the final software record is
`binding1/software.json`. No host fixture establishes numerical inference.

The live-DRAM mode has separate evidence under
`~/.cache/sunstoneos/evidence/npu-live-20261017/batch1/`. The first root-configured
link and focused execution pass with ELF SHA-256
`16e8952bbf11ebe3cbd84dd94efa4c8b104148dcbc3cb1513c7a546e5d597df9`.
All 468 build and fixture inputs were archived before execution. The successful
case reaches actual service activation through the constructor, PMIC sampling,
PLL enable, token transfer and local ON-domain startup. The worker does not
execute commands. OFF-domain and lock-timeout cases retain resources; host
checks refuse missing SRAM-abandonment or disturbance evidence and unknown
modes. Hardware responses, architecture and external admission are modeled.
No previous scheduler, controller, numerical or USB workload was rerun, and no
physical operation was performed.
