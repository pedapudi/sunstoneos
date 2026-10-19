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

## Carry ownership through the native application

Use the existing [clock leases](../../../docs/clock-leases.md),
[power providers](../../../docs/power.md),
[memory reservations](../../../docs/memory-reservations.md) and
[DMA memory contract](../../../docs/dma-memory.md) where the device depends on
them. A local enable bit does not establish a stable parent, settled voltage,
exclusive register access or completed device writes. Shared parents must remain
protected while another consumer uses them.

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
- Construct valid sessions and provider configurations with their actual producers.
  Resolve structure sizes, hashes, callback layouts, clock parents, rail ownership
  and deadlines together before running cases.
- Resolve native hooks by defining compilation unit and verify the final ELF.
  Local names can collide, and a function may be inlined. Cover the actual return
  width and calling convention rather than a guessed symbol boundary.
- Use the configured tool paths and Python environment from accepted evidence.
  Resolving a linker symlink can change invocation behavior; system Python may
  lack the native tooling's packages. Keep builds and downloads in the cache.

If setup fails, preserve completed cases, fix the cause and resume only affected
or unexecuted work. Model hardware effects explicitly; do not present injected
GPU outputs, simulated NPU results or emulated interrupts as physical operation.
Host capture must match the frozen native producer's mode and result layout.
For a new supervised caller, check its actual `run_session` entry policy as well
as its capture callback. Direct callback fixtures missed unsupported result keys
for H.264 and DSI. Reuse `checks/h264-supervisor-entry.py --caller <path> --case actual`
to stop at readiness before queue or device access; do not replay unchanged cases.

Hand off changed behavior, frozen artifacts, checks and their limits, unresolved
physical prerequisites, and the smallest next board procedure. Use existing
evidence tools and component records; do not add a parallel reporting system.
Leave performance measurement until required hardware functionality is established.
