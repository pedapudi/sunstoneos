---
name: sunstone-a733-multicore
description: Diagnose or change A733 secondary CPU startup, GIC-600 initialization, memory coherence and FreeRTOS SMP in SunstoneOS. This work is reserved for the primary agent; use the documented physical checkpoints and pinned native-image contracts.
---

# Advance multicore startup from the last observed boundary

The user reserves multicore diagnosis, implementation, review and board tests
for the primary agent. Do not delegate this work. Apply the
[board operations skill](../sunstone-a733-board/SKILL.md) for physical execution.
Use [current startup evidence](../../../docs/a733-smp-supervised.md) to establish
what has run; a successful instruction model is not a physical checkpoint.

## Follow the actual native control flow

The A733 has six Cortex-A55 and two Cortex-A76 cores. Its supported topology uses
MPIDR affinity level 1 for the core index and affinity level 0 equal to zero.
Do not substitute the common Aff0 core-index convention or derive SGI routing
from it. Use the [topology and power controller](../../../docs/cpu-start.md) and
[entry protocol](../../../docs/cpu-entry.md).

Read only the layer implicated by the observation:

| Boundary | Code and contract |
| --- | --- |
| Entry address and CPU power request | `platform/a733/cpu_start.c`, `docs/cpu-start.md` |
| ARMED, ARRIVED, RELEASE and RELEASED | `platform/a733/cpu_entry.S`, `docs/cpu-entry.md` |
| Identity, revision controls and publication | `platform/a733/cpu_coherence.c`, `docs/cpu-coherence.md` |
| Local interrupt initialization | `platform/a733/cpu_continue.c`, `docs/cpu-continue.md` |
| Cached EL3/EL1 transition and barriers | `platform/a733-smp-cached/continue.c`, `continue.S` and `docs/a733-smp-cached.md` |
| Returning diagnostic and copied reports | `boot/a733/native-startup-probe.S`, `tools/run-native-startup-probe.py`, `checks/native-startup-probe.py` |

A power-state observation does not prove entry. ARRIVED does not prove RELEASED,
and RELEASED does not prove interrupt readiness, cache coherence or scheduling.
Capture the state needed to distinguish those boundaries, including the matching
core and epoch. Raw ESR/FAR values outside an observed exception path may be
reset or stale contents; do not interpret them as a captured fault.

## GIC-600 power and CPU power are separate protocols

The pinned A733 map places the distributor at `0x03400000` and redistributor
region at `0x03460000`. Discover core affinity through `GICR_TYPER`; the current
controller checks eight ordinary `0x20000` frames. Validate its assumptions
against the observed frame capabilities and last-frame bit. A model's shuffled
frame order and one board's observed order are not interchangeable guarantees.

CPU PPU ON does not establish redistributor readiness. Before SGI/PPI state
access, the GIC-600 protocol waits for equal group transition bits in
`GICR_PWRR`, writes zero to request power for the selected core (`RDAG=0`), and
confirms `RDPD=0`. A rejected request returns to the group check within the
original deadline. An uncertain access retains ownership.

Use the pinned [Arm implementation](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/drivers/arm/gic/v3/gic-x00.c)
and [GIC-600 power specification](https://documentation-service.arm.com/static/5e7ddddacbfe76649ba53034),
section 3.6.1. Identification/discovery accesses precede power-on in the reference
driver; that does not permit arbitrary SGI/PPI access to a powered-down frame.
Observed `RDPD=0` means access is permitted by that power state, not that ownership,
pending interrupt state or cleanup has been established.

Preserve enabled GIC distributor affinity-routing bits during cleanup. Do not
attempt to restore them from one to zero. The returning monitor's admission and
the [kernel evidence](../../../docs/a733-kernel.md) explain the applicable
security-state and interrupt-group requirements.

## Match diagnostics to the exact executable

The startup overlay patches a particular frozen ELF. Its addresses, displaced
instructions, structure offsets and hash checks are one binary's ABI. A rebuilt
native image requires fresh symbol/disassembly verification and affected fixture
updates; never transplant old patch addresses into a new build.

At a checkpoint inside C, reload core, epoch and mailbox identities from validated
storage. Callee-saved registers may hold values assigned by that C function until
it returns; they need not match the earlier assembly frame. Observe the target's
actual SCTLR/TCR banks; a fixture
that copies primary reset values into every secondary conceals that assumption.

Before extending execution, list newly attempted writes and their restoration
requirements. Capture original state before the first write. Restore a target's
interrupt state while it is accessible, and complete the required sleep/power
handshake before CPU shutdown. The primary needs both a matching shutdown
acknowledgment and independently observed OFF before shared cleanup.

A synchronous-exception handler cannot recover an MMIO load that never completes.
If no report returns, mark the location unknown. Choose a narrower observation or
a source-backed correction; do not declare a suspected instruction the cause.
Keep proven entry/return evidence and test only the newly crossed boundary.
