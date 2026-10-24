# Native compiled convolution application

The native application combines a retained boot monitor, a permanent NPU memory
reservation, the PMIC provider, NPU startup and the trusted compiled-model client.
The supervised build supports either the cached eight-core startup or an
independent image running on the boot CPU. Build-time selection chooses one of the four reviewed models in
[the registration catalogue](npu-convolution.md). The client copies the selected
input into private buffers, waits for matched completion, copies and compares
the output, and successfully reaps the job before submitting a second input.
It verifies both results before publishing through USB.

Every build is supervised. NPU startup runs the
[startup sequencer](npu-startup.md) on the power-domain owner: it requires
PLL_NPU disabled and domain 4 OFF, enables PLL_NPU by method 2, and requests
domain 4 ON through the owner, which also verifies the 800 mV DCDC2 supply. No
profile programs PMIC voltage. No preserved board measurement establishes the
cold PLL or DCDC2 state. Runtime model switching, concurrent models and other graph/operator classes
remain unsupported.
The fixed model and command streams come from pinned external NPU compiler
artifacts. DDR initialization also retains its existing pinned boot firmware.
Replacing those dependencies is deferred until hardware functionality and
performance work are complete.

## Build and admission

Build from the repository root with `A733_NPU_SUPERVISED=ON`, or from the
standalone `apps/npu-convolution` CMake project, with the pinned FreeRTOS and
TinyUSB source directories. The standalone project refuses to configure
without `A733_NPU_SUPERVISED`. Supply `A733_NPU_MODEL`, `A733_NPU_INPUT` and
`A733_NPU_REFERENCE`. The artifact generator checks the selected
model/input/reference tuple in `tools/npu-models.json`; it does not invoke a
model compiler. The existing compiler artifacts remain in the external cache.
The target is `a733-npu-convolution-supervised`. Application-specific source
properties stay within its CMake subdirectory.

The authenticated session page supplies the `a733_npu_application_boot`
record ([header](../include/a733_npu_application.h)). Its profile holds the
NPU, command-admission, USB, PMIC and pin evidence identities and the NPU,
PMIC, USB and bridge owners. Owners are nonzero and distinct. The record also
holds the fabric record, the PMIC configuration and the pin handoff. The PMIC
startup deadline is zero in the input; initialization derives it from the
authenticated boot counter deadline. The identities name reviewed records;
they cannot establish physical facts.

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
registry, register and I2C mutexes, pin manager, rail ledger, GPU/PCK resource
manager and power-domain owner. The power service task, which runs the PMIC
provider and the owner, and the startup sequencer use these same objects. USB retains its exclusive
USB0 and USB reference-clock tokens in that registry.

The actual NPU reservation constructor consumes 265 pages, or 1,085,440 bytes,
from the compact 2 MiB write-back pool. Its complete parent allocation stays in
DEVICE state for the image lifetime. The convolution client controls CPU access
to individual buffers through its existing copy and cache protocol. Model data,
command streams, tables, task stacks, requests and results retain platform
lifetime. This profile does not establish capacity for concurrent use of every
board device.

The bootstrap task calls the startup step once per tick until the deferred
worker can be activated. Every application-bound NPU operation authenticates the real
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

The application builds only as a supervised image. The supervised instruction
check, `checks/npu-supervised.py`, constructs the image's providers, the power
service and the startup sequencer under Unicorn and runs four cases: the
startup reaching READY, a wrong rail, domain 4 already on, and a PLL_NPU LOCK
timeout; the three refusals retain their tokens. `checks/npu-supervised-host.py`
covers packaging, admission, capture and the launcher's contract publication
in 33 cases with USB, FEL and the queue mocked. Physical NPU execution remains
unqualified.

## Supervised startup and retained recovery

Select `A733_NPU_SUPERVISED=ON` and build
`a733-npu-convolution-supervised` with the three pinned artifacts and the
FreeRTOS and TinyUSB sources. The image contains no admitted session.

The host supplies one immutable session at `0x41f00000`. Its complete digest,
build identity, epoch, generation, lease and supervisor bind the version 3 boot
contract to this image. The contract uses magic `0x415337334e504331`, a one-second
startup deadline and a twenty-second session window. EL3 authenticates it and
publishes a private digest receipt before cache adoption. The primary EL1
copies and rechecks the session after the eight-core preparation barrier;
it never reads the protected monitor contract. The exported
`a733_npu_session_layout` gives the exact native session and provider offsets.

The session's evidence digests bind reviewed records of admission, clock,
power and supervision conditions. `prepare-supervised.py` requires these
conditions in them:

- clock: independent DRAM and PHY sources, exclusive PLL_NPU consumers and
  writers, no autonomous parent-selector writers, and supervised acceptance of
  the cross-PLL disturbance risk of a method-2 enable;
- power: DCDC2 stable at 800 mV, and authenticated PMIC pin inheritance;
- supervision: manual whole-board power removal, all resources retained until
  that removal, and a stop after the first failure.

These records must identify independently established facts. Nonzero digests,
reservation identities and observed idle bits do not establish them.

The power service verifies DCDC2 through the PMIC provider and publishes the
result in the power-authority word. The bootstrap task then calls the
[startup sequencer](npu-startup.md). Its register pass checks the reference,
fabric and PERI0 routes, every PLL_NPU consumer, PLL_NPU disabled and domain 4
OFF before any write. It then enables PLL_NPU by method 2, opens the local
clocks and resets, and requests domain 4 ON through the owner. Startup admits
no other PLL or domain state. The backend, memory and command checks precede
service activation.

The session record is 456 bytes: version 3 for the single-core profiles and
version 2 for the all-core profile. Its layout word list
`a733_npu_session_layout` has six entries. The USB protocol is unchanged.

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

The manual's analogue-disturbance warning still applies, and software
timeouts require responsive CPU/interconnect hardware. No physical safety or
successful inference is inferred from the Linux runtime source or modeled tests.

Supervised admission leaves watchdog and reset redirects disabled. Every task,
clock, rail, SRAM and memory reservation remains owned after success, timeout or
failure, until the external supervisor confirms full board power removal.
USB disappearance or cable reconnection alone does not prove power removal or
release ownership. No automatic reset, cold provisioning or voltage writer is
introduced.

`checks/npu-supervised.py` links the supervised image and executes its
constructor, power service, startup sequencer and refusal cases with modeled
architectural state and device responses. It does not run inference or
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

The supervised host adapter passes 33 packaging/capture cases and five additional
checks that bind queue approval to the initially validated bytes. Baseline cases
were preserved when that binding changed. The host batch built its SRAM launcher
once and reused the native ELF. Evidence is under
`npu-supervised-host-20261017/`; the final software record is
`binding1/software.json`. No host fixture establishes numerical inference.

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
  gates, fabric selectors and independent DRAM/PHY sources. Startup requires
  PLL_NPU disabled and domain 4 OFF; any other state is refused.
- Read DCDC2 through the power service and confirm enabled, settled 800 mV
  under retained PMIC and pin ownership. The application supplies no
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
entry points. Single-core sessions use version 3; the all-core supervised session
uses version 2. The same preparation and capture tools support both profiles.
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
session to the frozen build identity, the fabric and provider records and the
selected board instance. Establish exclusive compact DDR, cache/alias exclusion, secondary
OFF state, internal SRAM ownership, supported NPU MMU mode, stable 800 mV DCDC2,
independent DRAM/PHY sources, USB supplies and the ROM PIO lifetime before launch.
Use the existing `tools/check-a733-npu-supervised.py` capture command for one
supervised RAM-only attempt. The native window is 20 seconds and host capture
is bounded to 19 seconds. Success requires two distinct matched jobs and both
copied outputs. Stop communication after the first transport failure. Keep every
resource until confirmed removal of all board power sources, including after
success. Physical translation, provider state and NPU inference remain unresolved.


## Application clock admission and physical candidate

The startup sequencer's register pass validates the normalized reference with
the shared decoder and applies the PERI0 control policy of the canonical NPU
provider. PERI0 bit 24 is permitted. `checks/npu-application-clock-reference.py`
runs six reference-clock cases through the linked image's sequencer: 26 MHz
and 24 MHz oscillators normalized to the 24 MHz reference are admitted; wrong
factors, an unstable or disabled reference and a PERI0 input-divider bit are
refused before any write.

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
