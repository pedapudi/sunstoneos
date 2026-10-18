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

Trace the actual native boot and application code. A single task in an application
does not imply a single-core image: some device images inherit the eight-core
cached boot barrier. Conversely, do not require multicore qualification for an
independent image without a real dependency.

For DMA, distinguish CPU addresses, device addresses, mapped extents, cache
visibility and lifetime. A buffer reservation alone does not prove translation
or engine confinement. Verify command streams, descriptors, tensors and outputs
against their actual address paths, including internal SRAM or bypass routes.
Exact register readback can reject unsupported state without proving the engine
uses the intended mapping.

Startup must retain and validate session inputs before provider initialization.
The [session contract](../../../docs/board-session-seal.md) binds identities and
geometry; test fixtures that fabricate its authority cannot become launch evidence.
Separate physical prerequisites from assumptions and explain the concrete failure
each prerequisite prevents. Do not demand hostile-task isolation merely to run a
trusted initial workload, or claim whole-board RAM ownership protects MMIO and
recovery code from unconstrained device addressing.

Publication needs actual completion and owned copied output. Cancellation or a
software timeout does not prove hardware stopped. Resource release requires the
device's demonstrated drain or recovery condition. Generation and client identity
must remain bound through result copy and completion acknowledgment.

Check completion for every native client claimed by the application. Successful
USB transfers do not establish another task's completion when its summary fields
remain pending. Match host success to the native producer's required status and
count fields, and preserve partial results when those conditions disagree.

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

Hand off changed behavior, frozen artifacts, checks and their limits, unresolved
physical prerequisites, and the smallest next board procedure. Use existing
evidence tools and component records; do not add a parallel reporting system.
Leave performance measurement until required hardware functionality is established.
