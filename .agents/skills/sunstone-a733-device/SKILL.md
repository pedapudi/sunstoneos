---
name: sunstone-a733-device
description: Integrate A733 peripherals into native SunstoneOS applications and prepare meaningful software evidence and queued physical tests. Use for driver adaptation, power and DMA integration, application launch/capture, and focused fixture maintenance.
---

# Deliver a complete native device path

Start with the assigned row in the [hardware matrix](../../../docs/hardware.md),
its linked component document, and the accepted artifact named in the handoff.
Find missing initialization, ownership, application access, interrupt, completion
or recovery steps. Prefer closing those gaps over adding another isolated API.
Workers have no hardware access; prepare a bounded proposal for the coordinator.

## Reuse the right source and preserve its execution boundary

The target is A733, not a similarly named H618/H616 Orange Pi. Confirm register
maps, clock parents, supply rails, pin functions and routing from the pinned
A733 inputs. A device-tree node alone does not establish fitted-board wiring,
peripheral presence or electrical state. Use the
[wiring guide](../../../docs/board-wiring.md) for board routes and accessories.

Resolve device applicability before treating a family diagram or generated
register map as an implemented processor. The A733 manual explicitly defers
module availability to the device datasheet; its OR100 labels do not add a
second A733 control processor. Preserve contrary family references and the
separate CE ownership requirement in the [remote-writer audit](../../../docs/remote-writers.md#device-applicability-excludes-or100-ce-requires-predecessor-accounting).

Reuse suitable open-source implementations through FreeRTOS adapters. Pin new
references in the existing component manifest or `sources.lock.json`, preserve
licenses, and state whether code runs on the host, CPU, accelerator or other
firmware processor. A source used for instruction comparison is not necessarily
a runtime dependency. Document opaque dependencies; defer their replacement
until functionality and performance tuning are complete.

When adapting upstream initialization, follow the effective implementation,
including build options, weak-symbol overrides and SoC-specific callbacks.
For example, the generic TF-A redistributor power hooks have GIC-600 overrides
in the [implementation-specific driver](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/drivers/arm/gic/v3/gic-x00.c).
Copying only the generic call sequence can omit the hardware power protocol.

Resolve the board build recipe, external-module selection and source-local
preprocessor definitions before treating a driver as the effective reference.
The official Zero 3W current recipe selects BXM, whose local `USE_FPGA`
definition enables GPU core-domain control despite `CONFIG_AW_FPGA` being off.
Checking only that configuration or the separate open-PVR path misses the
additional power sequence. Use the [GPU reference audit](../../../docs/gpu-power.md#audit-of-the-nonreturning-identity-probe)
for the pinned selection chain; preserve its distinction between a default build
recipe and an identified installed image. A source sequence does not establish
that the corresponding instructions executed on a failed board.

Keep firmware command numbers and payload layouts tied to the interface selected
by the native build. A sequencing reference from another DDK can use different
command numbers: its GPU clock-change command is 202, while the selected native
interface uses 204. Compare new command bytes, offsets and shared-memory layouts
with the pinned native header; a model that repeats implementation literals
cannot detect this mismatch.

Trace normal completion through the effective interrupt handler, waiter and
caller before defining output ownership. Retained buffers do not by themselves
justify omitting an upstream reset or stop: G2D pulses mixer reset before waking
even a caller that retains its task. Separate successful completion from timeout
recovery. An ordering difference needs a concrete behavioral explanation;
acknowledgment timing in a single-job polled path need not match an interrupt
service that accepts another job.

## Carry ownership through the native application

Use the existing [clock leases](../../../docs/clock-leases.md),
[power providers](../../../docs/power.md),
[memory reservations](../../../docs/memory-reservations.md) and
[DMA memory contract](../../../docs/dma-memory.md) where the device depends on
them. A local enable bit does not establish a stable parent, settled voltage,
exclusive register access or completed device writes. Shared parents must remain
protected while another consumer uses them.

Trace each mutex-entry callback's error contract before changing ownership
handling. Media-clock and thermal callbacks can fail before acquisition or
return a negative result after acquiring. Their retention adapters must check
actual task/mutex ownership, independently of failed health checks. Preserve
known-held uncertainty, and establish normal task context before querying a
FreeRTOS mutex holder. Unowned denial must not reach an asserting native retain.

Decode clock rates from the selected parent and its documented divider fields.
Distinguish the raw oscillator, PLL reference output and architectural counter;
one declared frequency does not establish the others. A valid reference output
need not use equal multiplier and divider fields when the oscillator differs.
Check the pinned A733 manual in `sources.lock.json` and the effective clock driver
before rejecting an observed configuration or expanding admission. Carry the
derived rates into dependent bus timing and timeout calculations.
Reuse [`a733_clock_reference.h`](../../../include/a733_clock_reference.h) for
the shared raw-oscillator and normalized-reference decoding. Check which source
the consumer actually selects before applying a frequency restriction. Reserved
bits are not source selectors. Decoded register settings establish a configured
rate; they do not measure frequency or establish clock ownership.

Trace the actual native boot and application code. A single task in an application
does not imply a single-core image: some device images inherit the eight-core
cached boot barrier. Conversely, do not require multicore qualification for an
independent image without a real dependency.

Establish task privilege from the selected build and linked stack constructor.
The single-core SRE port with `GUEST=1` starts tasks at EL1t (`SPSR=4`), using
`SP_EL0` as its stack. Selecting that stack does not mean execution at EL0.
The cached SMP port starts tasks at EL0 and uses a different privileged exit
handler. Trace the linked exit and exception vectors before transferring a
privilege finding or changing a device application's return path.

For DMA, distinguish CPU addresses, device addresses, mapped extents, cache
visibility and lifetime. A buffer reservation alone does not prove translation
or engine confinement. Verify command streams, descriptors, tensors and outputs
against their actual address paths, including internal SRAM or bypass routes.
Exact register readback can reject unsupported state without proving the engine
uses the intended mapping.
Use the allocator's returned physical address when constructing device mappings.
If a fixed profile instead uses CPU addresses as physical addresses, explicitly
require equality before transferring ownership to the device.
Check the concrete DMA adapter's admitted memory attributes before choosing a
pool. The shared `dma_memory_accept` policy admits inner-shareable write-back
RAM (`0xff`). A retained non-cacheable pool (`0x44`) needs a reviewed concrete
adapter with exact allocation/role checks, every-page translation validation,
descriptor write permission and ordering appropriate to immutable NC memory.
Keep that exception within its actual consumer; preserve the shared policy.

Cache publication must target the writer's physical address space. EL3 with its
MMU disabled uses Secure addresses, even when `SCR_EL3.NS=1`; maintenance by
virtual address there does not publish dirty Nonsecure EL1 lines. Publish through
the existing EL1 mapping before entering the monitor, or use an explicitly
reviewed Nonsecure maintenance mapping. See the [publication audit](../../../docs/a733-multicore-reference-audit.md#publication-must-cover-the-executable-physical-address-space).
A shared byte array in an instruction model cannot verify security-tagged cache
visibility. Keep monitor writes away from unpublished dirty application lines.

Startup must retain and validate session inputs before provider initialization.
The [session contract](../../../docs/board-session-seal.md) binds identities and
geometry; test fixtures that fabricate its authority cannot become launch evidence.
When adding an application workload, trace its mode through the launcher, session
validator and private boot identity hook before building. The combined Mimus
keyboard workload passed application checks while its inherited identity hook
still rejected every nonzero workload. Its private-hook check is
`checks/mimus-board-identity.py`; it excludes shared CPU startup and models
publication, so it establishes only that hook's admission behavior.
When adapting startup, trace where validated inputs live across exception-level
transitions and BSS clearing. Preserve those inputs until their consumers finish;
matching structure layouts alone do not establish their lifetime.
Separate physical prerequisites from assumptions and explain the concrete failure
each prerequisite prevents. Do not demand hostile-task isolation merely to run a
trusted initial workload, or claim whole-board RAM ownership protects MMIO and
recovery code from unconstrained device addressing.

Publication needs actual completion and owned copied output. Cancellation or a
software timeout does not prove hardware stopped. Resource release requires the
device's demonstrated drain or recovery condition. Generation and client identity
must remain bound through result copy and completion acknowledgment.

For inherited USB hardware, distinguish cold initialization from taking ownership
of the controller used by ROM. CPU startup does not imply peripheral reset.
Use the [USB platform contract](../../../docs/usb-platform.md) and an explicit
handoff that preserves shared clocks and other controllers. A clear DMA start
bit may result from software cancellation; it does not establish completed bus
writes. A USB disconnect delay lets the host observe disconnection but does not
prove DMA completion. Establish completion or source-backed absence of DMA for
the relevant powered lifetime before releasing its memory or resetting resources.

Check completion for every native client claimed by the application. Successful
USB transfers do not establish another task's completion when its summary fields
remain pending. Match host success to the native producer's required status and
count fields, and preserve partial results when those conditions disagree.

Trace each new register access through the complete native access policy.
A controller adapter may allow an offset that the lower MMIO provider rejects.
Check the complete address and peripheral base, then exercise the real provider
in the affected case. A fixture that substitutes its read callback cannot prove
that the native access policy admits the register.

## Validate the changed path without repeating earlier work

Apply [selective validation](../../../docs/validation.md). State the changed
behavior, credible failure and smallest useful check before a batch. Reuse an
unchanged image and successful checks; a cherry-pick or documentation edit does
not invalidate them. Compare relevant source, tool, configuration and compiled
dependency identities when integrating frozen work.

Prevent recurring fixture failures at setup:

- Select the real application feature flags before building. A headset-only
  configuration does not cover transcription startup.
  Restoring the patched TinyUSB MUSB source also requires
  `SUNSTONE_A733_USB=1` on the consuming target. Without that definition, the
  controller translation unit compiles without its `dcd_*` implementations.
  Check definitions when a derived target restores sources it previously removed.
  Derive composition link inputs from the current target or accepted compiled
  closure. An inherited manual source list can omit a dependency added to a
  sibling service, even when the selected case does not execute that service.
- Construct valid sessions and provider configurations with their actual producers.
  Read the actual frozen input's record count and lengths before writing its
  parser; a newer fixture can expect records absent from an older accepted vector.
  Resolve structure sizes, hashes, callback layouts, clock parents, rail ownership
  and deadlines together before running cases. A session-sealing fixture can
  leave provider fields unused; validate its pin and PMIC handoff before reusing
  it for application construction.
  Select the USB reference mode from the compiled admission profile. Ordinary
  cached-SMP board consumers require cold mode 1; ROM-takeover mode 2 is admitted
  only by the explicit `SUNSTONE_SD_SINGLE_CORE` workload 2 branch. A generic
  single-core provider fixture default does not authorize mode 2 for cached SMP.
  Exercise the actual `a733_board_session_valid` before provider construction.
  On a profile mismatch, audit the remaining provider predicates together before
  retrying, preserving the failed setup and completed work.
  Derive modeled pin readbacks from the actual field helper. On A733 banked
  PIO, mux and drive fields use four-bit spacing, while pull fields use two-bit
  spacing. Two adjacent drive-code-1 pins therefore require `0x11`, not `0x5`.
  Inspect all remaining provider readbacks together after a setup mismatch.
  Preserve cross-field bindings when changing fixture identities: the board
  qualification and recovery hashes must identify the same record. Check the
  resulting session with the actual validator before a longer application batch.
  Supervised producers emit an unarmed contract: the SRAM launcher supplies its
  magic and timing fields. A fixture entering after that launcher must consume
  the accepted post-launch contract, or explicitly model arming once and share
  that exact artifact with downstream checks. Preserve the original template.
  Stubbed contract validation does not initialize deadlines used later by boot.
- Retain the accepted timing budget for unchanged lifecycle phases. Account for
  bounded work spread over scheduler ticks before adding a completion phase.
  GPU shutdown, for example, performs one register operation per tick; a shorter
  fixture CLOSE deadline can expire after valid cleanup. Do not relax production
  deadlines to repair a model budget. Preserve the failed case and rerun only
  affected or previously unexecuted cases after correcting setup.
- Place emulator stack, scratch and return-trampoline memory outside the entire
  admitted device pools. An address outside the currently allocated pages may
  still violate the allocator's exclusion of its own pool from output arguments.
  The extended profile reserves `0x48000000..0x79000000`; inherited fixtures with
  stack or scratch near `0x50000000` are unsuitable for allocator execution.
- Resolve native hooks by defining compilation unit and verify the final ELF.
  Local names can collide, and a function may be inlined. Cover the actual return
  width and calling convention rather than a guessed symbol boundary. Filter
  ELF symbols by object/function type when locating retained data. Match MMIO
  access widths to the instructions; an eight-byte fixture read can combine
  two independent four-byte hardware registers.
  A noinline checkpoint marker can lose its argument setup when the callee
  does not use that argument. Verify the linked call sites and bind checkpoint
  classification to actual native state, or keep the marker input live.
  Before modeling task execution, enumerate the selected port's actual yield,
  delay and suspend sites; a yield may be an inline SVC rather than a function
  call. Save CPU, RAM, task and scheduler state on every execution-budget or
  failure stop so unfinished work can resume without repeating initialization.
  Derive debugger addresses from the exact ELF symbols or type information,
  never from manually copied addresses. Verify writes by reading them back.
  Keep the debugger attached until assertion/exit stops preserve terminal RAM.
  Publish fixture readiness once; do not overwrite a client's later completion
  state on subsequent polls. Capture the native result before classifying a
  reporting failure, and do not replay completed device work to repair reporting.
- Snapshot mutable model registers and state by value. Restore each case from
  the preserved checkpoint without sharing dictionaries, lists or buffers with
  a running case. Check pristine case-entry state before injecting a fault.
  Before accepting a fault case, require the intended injection count, expected
  submission count and matching native terminal classification. Retained buffers
  alone cannot distinguish a fault from successful retained operation.
- Use the configured tool paths and Python environment from accepted evidence.
  Before the first configure, enumerate every required cached third-party
  source/header path and effective feature flag from the accepted CMakeCache.
  Verify those paths exist and preserve the generator, compiler and linker
  invocations; copying compiler flags alone does not reproduce the configuration.
  A direct OS configure also needs `SUNSTONE_A733_OS=ON` and
  `SUNSTONE_MIMUS_BOARD_APPLICATION=ON`; options such as
  `SUNSTONE_A733_OS_NPU=ON` do not enable the top-level target. Prefer an existing integration-gate variant
  when it matches the requested profile. The cached `cfg.sh` supplies dependency
  paths and reads the checkout's mimus source record, but does not select the OS.
  Resolving a linker symlink can change invocation behavior; system Python may
  lack the native tooling's packages. Keep builds and downloads in the cache.
  For a small target with a shared-layout change, prefer rebuilding its
  dependent objects through the accepted build graph or compile commands when
  that costs less than proving partial reuse. Mixing old and new debug-type
  definitions caused avoidable GPU fixture setup repairs. Reserve manual object
  reuse for material savings, and bind type lookups to the defining current unit.

If setup fails, preserve completed cases, fix the cause and resume only affected
or unexecuted work. Model hardware effects explicitly; do not present injected
GPU outputs, simulated NPU results or emulated interrupts as physical operation.
Host capture must match the frozen native producer's mode and result layout.
Derive report magic, field widths and status meanings from the frozen native
writer. A fixture assembled from the parser's own constants cannot establish
that the native writer and host parser agree.
For a new supervised caller, check its actual `run_session` entry policy as well
as its capture callback. Direct callback fixtures missed unsupported result keys
for H.264, DSI and UART. Reuse `checks/h264-supervisor-entry.py --caller <path> --case actual`
to stop at readiness before queue or device access; do not replay unchanged cases.

Hand off changed behavior, frozen artifacts, checks and their limits, unresolved
physical prerequisites, and the smallest next board procedure. Use existing
evidence tools and component records; do not add a parallel reporting system.
Leave performance measurement until required hardware functionality is established.

## Early boot0 storage reads and parameter slots

Before DRAM training, use the pinned boot0 single-sector PIO interface. Larger
reads enter a DMA path whose descriptors are in untrained DRAM. Keep the first
word of each unused DRAM parameter slot zero: the selector treats a nonzero word
as a populated timing set. Use the source audit and bounded-read contract in
[the DRAM restore design](../../../docs/designs/dram-training-restore.md#card-boots-need-a-host-built-boot0-variant)
before changing either early boot hook.
