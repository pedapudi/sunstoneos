---
name: sunstone-a733-multicore
description: Diagnose or change A733 secondary CPU startup, GIC-600 initialization, memory coherence and FreeRTOS SMP in SunstoneOS, using the documented physical checkpoints and pinned native-image contracts.
---

# Advance multicore startup from the last observed boundary

Multicore diagnosis, implementation and review may be delegated; only the
coordinator runs board tests, following the
[board operations skill](../sunstone-a733-board/SKILL.md).
Use [current startup evidence](../../../docs/a733-smp-supervised.md) to establish
what has run; a successful instruction model is not a physical checkpoint.
Read the [Arm/Linux comparison](../../../docs/a733-multicore-reference-audit.md)
before changing the unobserved interrupt-initialization interval. Linux delegates
CPU power-on to PSCI firmware; its kernel driver alone is not the direct EL3
startup contract. Distinguish required ordering from permitted reference choices.

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

Validate required implemented bits, not an assumed full register width.
`GICR_IGROUPR0` bits for unimplemented PPIs are read-as-zero/write-ignored
(Arm IHI 0069G 12.11.12). Core 1 returned `0xfff0ffff` after an all-ones write;
requiring `0xffffffff` caused a verified startup refusal. The continuation checks
all SGIs and its timer PPIs 29/30 while configuring all implemented sources.
Models must include unimplemented-bit behavior and still reject missing required
bits. A later consumer of another PPI needs its own required-bit validation.

Preserve enabled GIC distributor affinity-routing bits during cleanup. Do not
attempt to restore them from one to zero. The returning monitor's admission and
the [kernel evidence](../../../docs/a733-kernel.md) explain the applicable
security-state and interrupt-group requirements.

Preserve enabled GIC system-register interfaces during cleanup. Arm IHI 0069G
section 12.1.6, pages 12-193 and 12-194, makes a one-to-zero transition of
`ICC_SRE_EL3.SRE`, `ICC_SRE_EL2.SRE` or Secure `ICC_SRE_EL1.SRE`
unpredictable. Saving an earlier zero does not authorize restoring it after
startup enables the interface. The Nonsecure EL1 exception does not apply to
these other banks. Existing returning helpers record the earlier SRE value
and keep the enables set; qualify permitted restoration fields separately.

Treat CPU-interface access, interrupt delivery and redistributor wake as separate
operations. Compare their ordering with the pinned
[TF-A CPU-interface implementation](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/drivers/arm/gic/v3/gicv3_main.c).
Do not infer that waking before enabling system-register access explains a hang;
the reference enable path uses that order. Inspect the executing exception level,
security state, SRE controls and group enables at the actual checkpoint.

Check the observed GIC revision and applicable erratum conditions when changing
interrupt delivery or shutdown. The narrow returning diagnostic's exclusion of
[erratum 2384374](https://documentation-service.arm.com/static/6287e27d41e28926d04306ec?token=)
depends on its disabled interrupt groups, SPI enables and LPIs. Revisit that
reasoning before enabling delivery; it does not qualify general shutdown.

Initialize the Secure view of `ICC_PMR_EL1` at EL3 before entering Nonsecure EL1.
With `SCR_EL3.FIQ=1`, a Secure mask below `0x80` makes Nonsecure reads return
zero and Nonsecure writes ignored (IHI 0069G section 4.8.1). An EL1 write of
`255` alone therefore does not establish interrupt admission. The pinned TF-A
`gicv3_cpuif_enable` initializes the Secure mask. Verify the effective Nonsecure
mask before delivery. Models must represent this restriction and the implemented
priority bits, rather than treating PMR as an unrestricted shared variable.

Nonsecure priority accesses expose half the Secure priority levels (IHI 0069G
section 4.8.4). Five implemented Secure priority bits can leave only four in the
Nonsecure view. A maximum mask can round down to `0xf0`; an interrupt at that
same priority remains masked. Set a known admitted priority, preserve neighboring
fields and restore the original value during returning diagnostics.

Validate interrupt groups from the Secure monitor. With `GICD_CTLR.DS=0`,
Nonsecure accesses to `GICD_IGROUPR` are read-as-zero/write-ignored (IHI 0069G
section 12.9.13). A Nonsecure runtime check that requires its group bit to read
as one will reject a correctly configured interrupt. Preserve the monitor's group
validation and check the runtime-accessible priority, route and enable state.

## Match diagnostics to the exact executable

The startup overlay patches a particular frozen ELF. Its addresses, displaced
instructions, structure offsets and hash checks are one binary's ABI. A rebuilt
native image requires fresh symbol/disassembly verification and affected fixture
updates; never transplant old patch addresses into a new build.

When a fixture models a whole function, give that hook precedence over an
instruction hook at the same address. A boundary function may begin with `MRS`.
Inspect the executed interval for inlined `CNTPCT_EL0` reads: replacing only
`a733_smp_counter` leaves those reads on the emulator's unrelated clock.

Verify that the executed path contains the proposed correction. Editing the
controller source does not change a frozen ELF called by an overlay. Trace the
build's generated sources, patches and linked objects before claiming coverage.
The cached image compiles the portable continuation controller through
[`cmake/a733-smp-cached.cmake`](../../../cmake/a733-smp-cached.cmake) and
[`continuation-core.patch`](../../../platform/a733-smp-cached/continuation-core.patch).
Reuse that split when needed instead of maintaining a diagnostic copy of the driver.

At a checkpoint inside C, reload core, epoch and mailbox identities from validated
storage. Callee-saved registers may hold values assigned by that C function until
it returns; they need not match the earlier assembly frame. Observe the target's
actual SCTLR/TCR banks; a fixture
that copies primary reset values into every secondary conceals that assumption.

Trace counter setup through the executed startup before interpreting CNTFRQ or
converting deadlines. The cached secondary entry writes CNTFRQ before calling C;
a model must execute that write rather than reuse the ROM's initial declaration.
Keep raw ticks and microseconds distinct. Model read-only status bits separately
from writable controls, including GICR_WAKER.ChildrenAsleep; a write value is not
the resulting hardware status.

Before extending execution, list newly attempted writes and their restoration
requirements. Capture original state before the first write. Restore a target's
interrupt state while it is accessible, and complete the required sleep/power
handshake before CPU shutdown. The primary needs both a matching shutdown
acknowledgment and independently observed OFF before shared cleanup.

A synchronous-exception handler cannot recover an MMIO load that never completes.
If no report returns, mark the location unknown. Choose a narrower observation or
a source-backed correction; do not declare a suspected instruction the cause.
Keep proven entry/return evidence and test only the newly crossed boundary.

## Keep the global scheduler tick separate from local timer delivery

The cached port routes CPU0 PPI30 to `FreeRTOS_Tick_Handler`, which advances
FreeRTOS global time. Routing every secondary timer through that same callback
would advance global time multiple times. Keep one global tick owner unless
changing the scheduler's timekeeping contract with corresponding evidence.
A secondary hardware timer may use a separate local callback.

The finite qualification now demonstrates one timer interrupt on each secondary
through the actual IRQ entry, task frame restoration and EOI path. It publishes
armed state while interrupts are masked, before the first-task `SVC 106`.
The timer can become pending before that SVC restores task interrupt state.
Check that ordering, integer and floating-point frame preservation, effective
priority, status before disabling the timer, and final masked delivery counts.
Missing or duplicate delivery must prevent successful cleanup. Restore the
saved Secure PPI30 priority while the redistributor remains accessible.
The current physical result is in the hardware matrix; it does not qualify
periodic local preemption, idle wakeup or sustained scheduling by itself.

## Check warm-image visibility across security mappings

A successful upload comparison proves the loader's data view, not a later cached
instruction view. Before reusing code addresses, establish publication and
invalidation for the physical address space selected by the executable mapping.
EL3 maintenance with its MMU off targets Secure addresses; it cannot establish
coverage of an inherited Nonsecure alias merely because the numeric address
matches. See the [reference audit](../../../docs/a733-multicore-reference-audit.md).

Keep changed-interval observations explicit. A valid shutdown and ROM return can
coexist with a missing new interval. Treat that interval as unobserved and review
code visibility before repeating its launch. The instruction model's shared byte
array does not model security-tagged cache lines or system-cache retention.

The returning diagnostic now supplies a physically demonstrated publication path
in `boot/a733/native-ns-publication.S`: temporary Secure executable/Device data
mappings plus a nonexecutable Nonsecure maintenance alias, followed by exact
control restoration. Reuse its accepted evidence for unchanged inputs. Its
`DC IVAC` scope assumes clean immutable code/table lines and excludes dirty
runtime data. Do not generalize that assumption to a running OS or use broad
cache discard to avoid proving ownership. The host saves both temporary table
pages as part of the existing exclusive RAM reservation.

## Separate interrupt delivery, context restoration and task selection

A stackless IRQ diagnostic does not exercise the production FreeRTOS handler's
cached stack or nesting-state writes. Trace its actual vector, saved general/FP
state, per-core indexing and every callback before extending execution. The
application dispatcher can depend on initialized scheduler and deadline state;
calling it from an earlier checkpoint can fail before testing context restoration.
A bounded diagnostic callback must not be described as the complete application.

The returning production-handler interval requires no pending task switch and
cleans all owned stack, callback and nesting-state pages through Nonsecure
mappings before cache disable. Its success does not establish task selection or
the full scheduler's larger write set. Derive that set from actual stores and
complete synchronization before any CPU or ROM transition.

Mask interrupts before taking final delivery counts. A handler can complete
between a timeout decision and masking; stale counts can falsely report an
undrained interrupt. Reconcile the final counts with pending/active state before
classifying the result or attempting cleanup.
