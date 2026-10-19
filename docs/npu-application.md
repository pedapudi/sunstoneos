# Native compiled convolution application

The native application combines a retained boot monitor, a permanent NPU memory
reservation, the PMIC provider, NPU startup and the trusted compiled-model client.
The supervised build supports either the cached eight-core startup or an
independent image running on the boot CPU. Build-time selection chooses one of the four reviewed models in
[the registration catalogue](npu-convolution.md). The client copies the selected
input into private buffers, waits for matched completion, copies and compares
the output, and successfully reaps the job before submitting a second input.
It verifies both results before publishing through USB.

The watchdog profile adopts an inherited PLL rate that can supply 492 MHz and
requires an existing 800 mV DCDC2 supply. The supervised profile connects the
PLL controller before local startup. Its explicit live-DRAM profile can enable
a disabled PLL while adopting a quiescent NPU domain already ON. Neither profile
programs PMIC voltage. No preserved board measurement
establishes the cold PLL or DCDC2 state. Runtime model switching, concurrent models and other graph/operator classes
remain unsupported.
The fixed model and command streams come from pinned external NPU compiler
artifacts. DDR initialization also retains its existing pinned boot firmware.
Replacing those dependencies is deferred until hardware functionality and
performance work are complete.

## Build and admission

Build from the repository root or from the standalone `apps/npu-convolution`
CMake project with the pinned FreeRTOS and TinyUSB source directories.
Supply `A733_NPU_HANDOFF_SOURCE`,
`A733_NPU_MODEL`, `A733_NPU_INPUT` and `A733_NPU_REFERENCE`. The artifact generator
checks the selected model/input/reference tuple in `tools/npu-models.json`; it does
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

The cached multicore target applies private FreeRTOS privilege and vector
patches. Its bootstrap, PMIC provider and NPU worker tasks execute at EL1t;
the client remains at EL0 and USB uses the authenticated SVC entry. The separate
single-core SRE target, including the returning image, starts all tasks at EL1t
with `SP_EL0` as the stack selector. Selecting that stack does not imply EL0
execution. Both profiles require trusted application code and immutable platform
metadata; their mappings do not isolate every private object from arbitrary
application writes.

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
compact observer launcher bundle. It verifies the embedded selected artifacts,
copies launch inputs into a new bundle, and records their identities. A bundle
prepared with `--refusal` is rejected before device discovery.

The live command reuses the existing board claim, host lock, protected FEL
transfer, quarantine and recovery mechanisms. It requires NPU-specific software
and platform evidence in addition to the existing DDR and independent-recovery
records. Only the board coordinator may schedule physical execution.

USB command `0x33` returns a version-2, 1,024-byte frame containing the boot identity, host
challenge, build identity, controller and admission identities, selected artifact
digests, the complete final copied output, both job identities, the first output digest,
final output digest and completion times. Status distinguishes
pending, matching output, mismatching output and fault. The host validates and
saves a complete frame before recording a terminal result. It reports success
only when a matching output is durably captured and a fresh ROM response follows.
That ROM response alone does not establish watchdog causation.

## Focused validation

`checks/npu-application.py` links the actual native image with the explicit
refusal fixture in `checks/npu-application.c`. It preserves generated assembly,
source dependencies, selected artifacts, tool identities, ELF and map before
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
This retained multicore profile has no FEL return. The separate finite returning
profile described below uses a different shutdown and host-restoration contract.
The privileged Device mapping still permits SRAM access, so private-image
ownership does not isolate SRAM from arbitrary privileged code. External users require independent
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

## Selected-model application evidence

The focused variable-model batch is recorded under
`~/.cache/sunstoneos/evidence/npu-models-20261018/native-final/`.
It selects the 12×12 input and four-output-channel model, copies 432 input bytes
and 400 output bytes, and passes two distinct jobs with matching REAP replies.
It also preserves a first-job mismatch when the second matches and refuses
success after a wrong ticket in the second REAP reply. The native serializer and
durable host capture pass with the complete 1,024-byte frame. The host retains
strict parsing for frozen version-1 frames and reads up to 1,040 transport bytes,
including the 16-byte USB response header.

The preserved compact launcher initially failed the source-compatibility check
because its AArch32 entry retained an SCR.NS check absent from current source.
The existing observer preparation tool produced one matching launcher from the
preserved reviewed observer ELF/manifest and current launcher sources. The NPU
image was reused; frozen observer and live-board artifacts were not modified.
No physical test occurred. The instruction check models service completion and
output and cannot establish physical inference or effective confinement.

## First physical inference prerequisites

The current frozen image under `npu-models-20261018/native-final/` remains a
software-validation artifact. Its modeled responses and nonzero evidence fields
provide no physical startup authority. A coordinator must prepare a separately
reviewed session before using the supervised launcher.

The ordinary RAM path has a concrete source contract. The pinned VIPLite
[`vip_drv_hardware.c`](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/npu/aw_nna_vip/vip2/vip_drv_hardware.c)
function `vipdrv_hw_pdmode_setup_mmu` programs directory register `+0x3b4`
with `(physical_root >> 12) << 4 | 1` and control register `+0x388` with `0x21`.
The native controller now requires the complete control word to equal `0x21`
before triggering, accepting completion or closing. Additional bits cause a
retained fault. This conservative supported-state check does not identify the
meaning of unspecified bits or establish effective translation.

The source's
[`vip_drv_mmu.c`](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/npu/aw_nna_vip/vip2/memory/vip_drv_mmu.c)
encodes a leaf entry from physical bits 12–31, physical bits 32–39 in entry
bits 4–11, write permission in bit 2, exception enable in bit 1 and presence
in bit 0. The native builder uses that encoding with 32-bit device addresses
and 40-bit physical addresses. Unmapped entries contain `2`. A source encoding
and a modeled table walk establish software intent, not physical permissions.

The native reservation maps only these device ranges. Each end is exclusive;
physical addresses come from the retained parent allocation, never from these
device addresses.

| Purpose | Device range | Device permission |
| --- | --- | --- |
| Descriptor | `0xe0000000..0xe0001000` | Read |
| Coefficients | `0xe0002000..0xe0003000` | Read |
| Model commands | `0xe0004000..0xe0084000` | Read |
| Input | `0xe0085000..0xe0086000` | Read |
| Output | `0xe0087000..0xe0088000` | Read/write |
| Bootstrap commands | `0xe0089000..0xe0109000` | Read |

The internal SRAM route is separate. Source `mmu_map_vip_sram` reserves virtual
space beginning at `0x400000` without filling ordinary RAM leaves. The native
bootstrap programs that base through state register `0x0e4e`, and the trusted
convolution initializes the AXI SRAM base/end states `0x0e4f` and `0x0e50` to zero.
The complete 512 KiB SRAM and CPU alias `0x74000..0xf4000` must remain exclusive
and powered. None of these source observations proves that every engine address
passes through ordinary RAM translation. The reference driver's build option
`vpmdHARDWARE_BYPASS_MODE` suppresses engine triggering; its name supplies no
proof about address-translation bypass.

A first inference can run trusted application code and a fixed reviewed model.
Protection against hostile tasks, arbitrary models and concurrent clients is
subsequent OS isolation qualification. Exclusive ownership of all disposable
board RAM can remove conflicts with unrelated RAM allocations, but it cannot
protect executing code, tables, the recovery path or peripheral registers from
an unconstrained engine address. The [address-path review](npu-address-path.md)
now identifies the existing
page-directory MMU as the intended external confinement method for the exact
trusted model, with complete ownership of internal SRAM. It traces the selected
revision-9.2 simulator's descriptor, coefficient, input and output paths and
excludes its observed flat mode. Physical behavior remains under test; the
coordinator must bind this reviewed method and actual board prerequisites to
the immutable session. No admission flag or host-parser rule changes here.

The smallest useful coordinator observations precede command submission:

- Establish the target identity and its independent DDR/recovery record. Confirm
  that the image, session, remote CPUs and every DMA writer satisfy the existing
  exclusive-memory and cache-adoption contract.
- Capture the admitted oscillator, PLL reference, PLL_NPU, selected shared-parent
  gates, fabric selectors and independent DRAM/PHY sources. Mode 3 requires the
  disabled-PLL state and an already-ON, quiescent NPU domain; unsupported partial
  states remain refusals.
- Read DCDC2 through the existing PMIC service and confirm enabled, settled
  800 mV under retained PMIC and pin ownership. The application supplies no
  voltage change. Confirm local reset/gate and shared SRAM ownership prerequisites.
- Bind the focused address-path review to the exact model, command allocations,
  table image and native MMU readback. Establish complete internal SRAM ownership
  and the admitted zero external-SRAM configuration. The first attempt tests
  physical behavior; a successful output alone cannot qualify fault isolation.

After those prerequisites are established, freeze the selected model, native
ELF, raw session and admission records with the existing preparation tool.
Queue one supervised RAM-only attempt using the documented launcher command.
Capture both matched outputs through USB before recovery. Retain all resources
on success, timeout or failure until confirmed full board power removal; stop
communication at the first transport failure. This proposal establishes no
physical observation and does not authorize bypassing the image's admission.

Focused software evidence for the exact MMU-control check resides under
`~/.cache/sunstoneos/evidence/npu-first-physical-20261018/`. The sanitizer check
flips each of the 32 control bits at submission, active completion and close.
Every deviation refuses progress and retains the controller without later
callbacks. The changed controller also compiles for AArch64. Existing model,
cache-maintenance, service and numerical-reference evidence remains applicable
to its original inputs; no physical command or performance measurement occurred.


## Supervised convolution on the boot CPU

The target `a733-npu-convolution-single-core` runs the reviewed trusted convolution
without starting secondary CPUs. Build it from the repository root, or configure
`apps/npu-convolution-single-core` with the existing toolchain, FreeRTOS, TinyUSB
and fixed model/input/reference arguments. The all-core target remains available;
all-core hardware support remains required independently.

The boot monitor copies and authenticates the session at EL3 before providers
initialize. It requires every secondary power state, control and mode to be OFF.
A retained session and bounded runtime contract survive EL1 BSS clearing. The
monitor contract, saved state and translation tables remain inaccessible through
the actual EL1 page tables. Device registers use outer-shareable Device memory;
application state and the compact 2 MiB pool use inner-shareable write-back memory.
The cache-disabled EL3 copy completes with a system barrier before EL1 enables
caching. Existing NPU publication and output-cache maintenance remain in use.

The single-core SRE FreeRTOS port runs the trusted client, worker, PMIC, bootstrap
and USB tasks at EL1. Task creation can restore IRQ delivery before scheduling;
the application masks exceptions again before checking preparation authority.
The monitor disables all SPIs, including NPU interrupt 97. This workload polls
NPU completion. Its USB service calls directly from its trusted task. USB startup
uses the existing ROM-device takeover provider with the NPU application's common
clock registry and register mutex. It retains the reference clock and follows
the provider's PIO checks, detach interval, device reset and PHY setup.

The host binds the execution profile `npu-single-core-supervised-v1` to immutable
ELF geometry, the boot entry and the absence of secondary-startup and EL0 SVC
entry points. Single-core sessions use version 2; the all-core supervised session
uses version 1. The same preparation and capture tools support both profiles.
The single-core admission additionally requires primary cache-transition and
secondary-OFF evidence, the ROM USB PIO predecessor, disabled/inactive/clear USB
interrupts 188–190, and settled USB supplies with their writers excluded.
These conditions supplement existing NPU, SRAM, DMA, PMIC, PLL and recovery authority.

The frozen image is `~/.cache/sunstoneos/evidence/npu-single-core-20261018/image.elf`.
Its SHA-256 is
`150fb6097ae55f6be7541ee992fb86b68be8930ed9e369f0347a3c77c4979517`.
The adjacent manifest, compiler dependency closure and snapshots preserve 177
compiled inputs and 70 linked objects. Eleven focused startup cases execute
native admission, retained copying, EL1 clearing, descriptor walks, static task
construction and the USB takeover provider. Seven host cases cover immutable
profile refusals, package validation, copied two-job capture and USB device-version
matching. Six preprocessing comparisons preserve the accepted SD and GPU branches.
System registers, GIC, counter, USB hardware state and task execution state remain
modeled; the private artifact hash helper uses host SHA-256 over the actual bytes.
Existing model/reference and translated-address-path evidence is reused. No NPU
inference, scheduler qualification or physical access occurs in these checks.

The preserved `package-final/bundle` contains conspicuous software-only admission
fixtures and must not be deployed. A coordinator must produce a fresh bundle from
actual board evidence using `apps/npu-convolution/prepare-supervised.py`. Bind the
session to the frozen build identity, exact PLL/provider snapshots and selected
board instance. Establish exclusive compact DDR, cache/alias exclusion, secondary
OFF state, internal SRAM ownership, supported NPU MMU mode, stable 800 mV DCDC2,
independent DRAM/PHY sources, USB supplies and the ROM PIO lifetime before launch.
Use the existing `tools/check-a733-npu-supervised.py` capture command for one
supervised RAM-only attempt. The native window is 20 seconds and host capture
is bounded to 19 seconds. Success requires two distinct matched jobs and both
copied outputs. Stop communication after the first transport failure. Keep every
resource until confirmed removal of all board power sources, including after
success. Physical translation, provider state and NPU inference remain unresolved.


## Application clock admission and physical candidate

The current NPU image also validates the normalized reference in the
application's private acquisition callback. Its shared NPU/PLL drivers already
contained the normalized-reference correction; the application callback retained
an older DCXO24/equal-factor restriction. It now uses the same reference decoder
and PERI0 control policy as the canonical NPU provider. PERI0 bit 24 is permitted;
input-divider bit 1 remains a refusal.

The clock correction passed six focused acquisition cases, including normalized
reference acceptance and refusal of unsupported inputs. A later case accepts the
observed 200/600/400 MHz fabric profile. Model, numerical-reference and unchanged
startup evidence remain valid for their recorded scope.

The first physical attempt used the trusted convolution, observed clock/power
configuration, and reviewed MMU/SRAM ownership. It produced no native USB frames;
the sole refusal-report read timed out. The user removed power, and a new ROM
identity response confirmed recovery. Evidence is under
`npu-single-core-20261018/physical-candidate/runtime/` and
`second-board-recovery-122-20261018/`. The failure location remains unknown.

## Failure reports use independently validated USB transport

The single-core application now permits bounded failure reporting after an NPU
client failure while the USB transport remains valid. Counter, registry, retained
platform locks, task ownership, session evidence, deadlines and USB provider
checks still stop access when their conditions fail. The earlier full application
liveness check stopped USB polling before the prepared failure frame could be sent.
This source defect does not establish the cause of the physical failure.

Ten focused cases execute the changed native paths, including terminal reporting
before and after USB readiness and refusal after shared-resource or transport
failure. Independent review accepted the implementation and frozen compiler
closure. The image is `npu-usb-terminal-20261018/image.elf`, SHA-256
`22306882147c06f5e0fbbfaf652692d4a5c9c487ebe64f949f41a6bd99dd243b`.
All 123 compiler inputs from repository source match integration; the frozen
closure also records generated and external inputs. Unchanged startup, model,
clock and host tests were reused. Physical reporting and inference remain untested
with this image. A new launch must bind its actual image and board lifetime.

## Returning supply observation and inference integration

The [finite supply observer](../apps/npu-returning/README.md) runs production
PMIC startup and two DCDC2 snapshots using the accepted returning FreeRTOS
objects. It requires enabled, stable 800 mV configuration for a supported
result and completes request cleanup before return. Its six linked checks
pass under `npu-supply-returning-20261018`; physical DCDC2 observation remains
outstanding. It performs no NPU access, DMA, PLL or domain programming.

The convolution wrapper now supports terminal CLOSE and bootstrap-owned local
shutdown. Two real FreeRTOS emulator cases cover successful CLOSE and a retained
MMU-mismatch fault. A sanitizer fixture checks local stop order, preserved
shared state, every failed write, deadline and authority-related register
refusals. Ten instruction cases check the native adapter's allowed local writes
and rejected SRAM, shared-bit, live-divider, domain and PLL changes. These tests
model NPU hardware and establish neither numerical inference nor physical return.
Evidence resides under `npu-close-returning-20261018`.

The finite client/device adapter is implemented behind
`SUNSTONE_NPU_FINITE_RETURN`. It preserves the existing session validator and
service path, waits for terminal CLOSE and bootstrap-owned local shutdown,
then parks the PMIC outside active requests and borrows and suspends the worker
and bootstrap. It validates provider slots, mutexes and NPU interrupt 97 before
reporting copied output. A final quiescence check and task critical section
precede return. Six linked client cases cover matched outputs, numerical mismatch, CLOSE
failure, an uncompleted stop, failed task quiescence and lost final quiescence.
Their service, bootstrap and quiescence replies are modeled. Nineteen native
instruction cases execute the actual worker/bootstrap suspension, PMIC checkpoint,
slot/borrow/mutex checks and interrupt observations, with scheduler and hardware
boundaries modeled. The [NPU-local returning image](../apps/npu-finite/README.md) now composes that
adapter with retained session authentication, owned cache publication, bounded
interrupt cleanup and the accepted forced-FEL reset tail. Its manifest uses
`npu-finite-return-v1`. New linked instruction checks cover the added boundaries;
actual EL3 sealing, EL1 BSS clearing and mapped memory are checked separately.
The selected SRE task constructor produces EL1t with PSTATE 4.

Root source review of the architecture/reset sequence and returned-RAM host path
found no remaining concrete blocker. The host wrapper is integrated; its checks
cover admission, copied reports, full DRAM restoration and retained failures.
Review records are `npu-finite-return-20261018/root-return-source-review.json`
and `npu-finite-host-20261018/root-host-source-review.json`. Fresh target-specific
physical admission remains required before execution. The earlier admission
profile retains resources until power removal and cannot be silently relabeled
as normal returning retirement. Failure paths continue to retain potentially
active device memory. Physical inference and ROM return remain unqualified.

The NPU reservation needs
265 contiguous 4 KiB pages: two 512 KiB command regions, four 4 KiB buffers and
20 KiB of tables. The complete 2 MiB parent pool must use identity-mapped,
inner-shareable normal write-back memory; the native adapter checks memory
attributes `0xff`, shareability 3 and 64-byte cache ownership. The NPU register
page at `0x03600000` must use Device memory. The model copy, service records,
requests, task stacks and copied outputs must remain outside that pool.

The existing small returning platform maps `0x40000000..0x48000000` with the
required normal attributes except for its non-cacheable result block at
`0x42000000`. The linker constrains the optional device pool below `0x41f00000`.
The monitor must clean the final linked writable application and pool extent,
retain its own tables/state and preserve the validated session across BSS
clearing. The small supply observer does not establish those full-image
properties. Successful CLOSE followed by local gate/reset stop is the engine
stop prerequisite; a separate unspecified external-drain qualification is not
required by the pinned normal teardown path. Shared resource leases and the
allocation remain owned through the confirmed ROM return.

## Optional finite interrupt completion

The [interrupt profile](../apps/npu-interrupt/README.md) runs the same two trusted
jobs through deferred interrupt completion on CPU0. It preserves the default
polled profile and introduces no new inference interface. Bootstrap and both
jobs must each complete after a service-owned event notification; the client and
host require the resulting count of three and a matched interrupt-stop receipt.
CLOSE retains the pending request and session until the device event source is
disabled, full idle is established and interrupt 97 is disabled, inactive and
pending clear. Local shutdown and provider quiescence follow that successful
CLOSE. The root-owned monitor restores the saved interrupt configuration before
SPI enables on normal return.

The new execution profile is `npu-interrupt-finite-return-v1`, with separate
hashed interrupt admission and host receipt checks. Focused native service,
controller, client, routing and host checks pass under
`~/.cache/sunstoneos/evidence/npu-interrupt-service-20261019/`. Existing polled
finite transport, copying and restoration evidence is reused. Two affected
monitor composition cases accept a matching receipt and refuse pending interrupt
97 before route restoration; unchanged cache and ROM-tail evidence is reused.
Physical interrupt delivery, inference and return remain unqualified.

## Live inputs for the fixed selected model

The [runtime application](../apps/npu-runtime/README.md) accepts same-shape inputs
through the existing retained single-core USB task. Each request uses the original
convolution input publication, matched completion, copied output and REAP path.
The caller can repeat with different bytes, then request healthy CLOSE and local
shutdown while USB remains available for the terminal copied result. Model and
shape stay fixed; an output oracle is optional and checked on the host.

The explicit profile is `npu-runtime-single-core-supervised-v1`, with runtime
and IRQ flags enabled and finite return disabled. Existing monitor, dispatcher,
route and interrupt service sources are unchanged. The original finite returning
profiles keep their two-fixture client and ROM-return behavior. Runtime input
ownership, wire fields and focused evidence are documented with the application.
No physical inference or interrupt qualification is established by these checks.

The separate [fixed FP32 relative-attention shift profile](npu-relative-shift.md)
adds authenticated SH/NT execution, an external working pool and chunked live
input/output in a retained single-core IRQ application.

The [complete published NN graph profile](npu-complete-nn.md) uses the same
retained IRQ owner for a fixed 125-operation weight-bearing network with an
external working pool and bounded copied UINT8 caller data.
