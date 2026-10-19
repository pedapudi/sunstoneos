# Multicore startup must satisfy both firmware and kernel contracts

A physical diagnostic identified an incorrect interrupt-group readback check.
Core 1 returned `GICR_IGROUPR0 = 0xfff0ffff`; the driver required `0xffffffff`
and retained a verification failure. The corrected driver permits unimplemented
PPI bits and now completes local initialization, restoration, shutdown and ROM
return on core 1. All seven secondaries now also pass production CPU preparation, local interrupt
initialization, restoration and shutdown under
`secondary-local-probe-20261018/physical-110/`. Cached EL3/EL1 entry, SMC return
and exact translation restoration also pass separately on every secondary under
`secondary-cache-probe-20261018/physical-111/`. Concurrent cached exchange across
a single owned page also passes on all seven secondaries under
`secondary-coherence-probe-20261018/physical-113/`. Timer PPI30 and peer SGI0
delivery also pass on every secondary under
`secondary-interrupt-probe-20261018/physical-114/`, followed by quiescence and
restored shutdown. CPU0 production local interrupt setup and cached EL1 entry
also pass under `primary-el1-probe-20261018/physical-121/`, with controls and saved
DRAM restored before responsive ROM return. CPU0 timer PPI30 and self-directed
SGI0 delivery also pass under `primary-interrupt-probe-20261018/physical-002/`,
with disabled delivery, empty pending/active state and restored controls before
ROM return. The actual FreeRTOS IRQ handler also passes register checks, cached
stack/nesting writes and complete owned-page cleaning under
`primary-context-probe-20261018/physical-005/`. Its application callback is bounded
and requests no context switch. All eight CPUs also complete the production pre-scheduler barrier and 256
lock-protected increments under
`all-core-pre-scheduler-20261018/entry-corrected/physical-008/`, followed by
complete owned-page cleaning, restored controls and matching shutdown/OFF.
All eight cores also physically complete FreeRTOS task construction and initial
idle-task context restoration, followed by full writable-memory cleanup and
restored ROM return. Evidence: `all-core-first-task-20261018/physical-010/`.
Task switching and sustained eight-core FreeRTOS scheduling remain unqualified.

This comparison is for developers changing A733 startup. It distinguishes
architectural requirements, reference implementation choices and physical
observations. Reference agreement supports an implementation; it does not prove
that the board executed it successfully.

## Linux delegates power-on to firmware

The pinned upstream Linux baseline is revision
`e5f0a698b34ed76002dc5cff3804a61c80233a7a`, the `v6.17` release.
Its [PSCI implementation](https://github.com/torvalds/linux/blob/e5f0a698b34ed76002dc5cff3804a61c80233a7a/arch/arm64/kernel/psci.c)
passes the target affinity and physical secondary entry to firmware's `CPU_ON`.
PSCI is the Power State Coordination Interface. Linux does not implement the
A733 power-controller sequence in that function.

The pinned board kernel's
[device tree](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi)
selects PSCI for all eight CPUs and assigns affinities `0x000` through `0x700`.
This is board-kernel evidence, not a claim that this entire device tree is
upstream. It corroborates SunstoneOS's Aff1 core index.

Linux's [boot contract](https://github.com/torvalds/linux/blob/e5f0a698b34ed76002dc5cff3804a61c80233a7a/Documentation/arch/arm64/booting.rst)
requires consistent timer setup, a shared coherency domain and initialized
exception-level controls. It also specifies higher-level GIC system-register
access controls. SunstoneOS enters directly from ROM and must provide the
firmware preparation that a Linux boot chain supplies.

The A733-specific power and entry sequence is compared with the published
monitor helpers in [CPU startup](cpu-start.md). The experimental A733 TF-A port
is supplementary evidence; its incomplete power-management functions do not
qualify a complete firmware replacement.

## EL3 must initialize the priority mask before Nonsecure delivery

Arm IHI 0069G section 4.8.1 specifies that, with `SCR_EL3.FIQ=1`, a Secure
priority mask below `0x80` makes Nonsecure `ICC_PMR_EL1` reads return zero and
writes ignored. SunstoneOS's cached entry selected that routing but initialized
PMR only after entering Nonsecure EL1. That write could therefore leave all
interrupts masked. The pinned TF-A `gicv3_cpuif_enable` initializes PMR from
Secure state before handing control back.

The production entry now initializes PMR at EL3 and checks that its readback is
in the Nonsecure-accessible range. The returning interrupt diagnostic uses the
previous frozen ELF, so its EL3 helper supplies the correction and restores the
original mask after delivery stops. The instruction model starts with Secure
PMR zero and implements ignored Nonsecure writes, priority-bit rounding and
Nonsecure read scaling. This corrects an omission in the earlier model; source
review alone does not locate the earlier full-launch failures.

Section 4.8.4 also specifies that Nonsecure priority accesses expose half the
Secure priority levels. A maximum Nonsecure mask can round down to `0xf0`.
The diagnostic therefore sets timer PPI30 to Nonsecure priority `0xa0`, verifies
it and restores the original priority word before shutdown. SGI0's configured
Secure priority `0xf0` appears as Nonsecure `0xe0`; the diagnostic requires a
Nonsecure PMR greater than `0xe0` before enabling delivery.

The new handler uses only reserved registers and the existing shared page. It
requires exactly one timer interrupt and one peer SGI per secondary. All
participants finish sending and receiving before masking IRQs, disabling their
timers and interrupt groups, and requiring empty pending/active state. Cache
cleaning and independently observed OFF remain prerequisites for shared cleanup.
This narrow diagnostic does not establish general task-context handling or
FreeRTOS scheduling.

The cached GIC-600 errata notice, SDEN-892601 version 12.0, limits erratum
2384374's affected packets to SPIs and LPIs. Both remain disabled in this
SGI/PPI-only diagnostic. Erratum 1014611 can defer another SGI/PPI until the
previous interrupt is deactivated; the handler uses combined EOI/deactivation
before returning and does not depend on nested delivery. These exclusions do
not qualify shutdown after general peripheral interrupt use.


The corrected interrupt diagnostic passes physically under
`secondary-interrupt-probe-20261018/physical-114/runtime/`. All seven targets
handle one timer interrupt and one peer SGI, then report disabled delivery and
empty pending/active state. The physical page, target restoration/OFF, shared
restoration and saved DRAM all match. ROM returns as `003:115`; the runner exits
and its queue claim closes. The result demonstrates this dedicated interrupt interval. The full FreeRTOS
scheduler and general context-switch handler remain unqualified.

## EL0 lock waits require permission to execute WFE

The linked FreeRTOS `vPortRecursiveLock` executes `SEVL; WFE` and repeats
`WFE` while another core holds the lock. The scheduler instruction inventory
observed this path on all eight cores at EL0. Startup previously selected
`SCTLR_EL1=0x30d01805`, leaving `nTWE` (bit 18) clear.

Arm's [DDI0616A.a register description](https://documentation-service.arm.com/static/62015c6c965f7d118e3f5f4c),
section E3.2, printed pages 391–392, specifies that a blocking EL0 WFE can trap
with exception class `0x01` in this configuration. The first WFE after SEVL can
complete immediately; a subsequent wait can trap. The FreeRTOS synchronous
handler accepts SVC (`0x15`) and otherwise branches to its permanent abort loop.
This establishes an incompatible startup policy, but does not prove that a WFE
trap caused the earlier physical failure.

Cached startup now sets `nTWE` before entering EL1. The resulting enabled
control is `0x30d41805`. This permits the port's existing wait instruction;
other EL0 controls remain unchanged. The scheduler check requires this permission
at every observed EL0 WFE. It does not simulate the event register or claim
that every WFE with `nTWE=0` traps.

Evidence under `scheduler-el0-access-20261018/` includes the linked instruction
inventory, pinned Arm document, rejection of the old image at its actual lock
wait, and the corrected image completing 32 floating-point rounds per core,
background work, inter-core interrupts and snapshot publication in the model.
All eight cores reach permitted EL0 waits. No physical launch of this correction
has occurred. Task selection, runtime memory ownership and physical scheduling
still require their own observations.

## Power, wake and interrupt delivery have separate requirements

| Boundary | Reference requirement | SunstoneOS evidence and remaining gap |
| --- | --- | --- |
| CPU release | Program the target entry and architecture, complete the SoC power request, then execute the secondary entry. | All seven secondaries pass entry, release, production CPU preparation and shutdown. |
| Redistributor power | GIC-600 requires the selected frame's `GICR_PWRR` handshake before interrupt-state access. | The portable controller completes the pinned Arm sequence physically for the already-powered frames of all seven secondaries. A transition from powered-off remains unqualified. |
| Redistributor wake | Clear `ProcessorSleep` and observe `ChildrenAsleep` clear before ordinary CPU-interface writes. | The controller implements this wait. All seven secondaries pass wake and return-to-sleep physically with delivery disabled. |
| System-register access | Enable the appropriate SRE controls and respect security-state banking. | Production EL3-to-EL1 code configures these controls. The returning diagnostic has a separate, narrower EL3-only setup. The separate interrupt diagnostic now establishes SGI0/PPI30 delivery. |
| Interrupt delivery | Configure local groups, priorities, enables and CPU-interface controls before receiving interrupts. | All seven secondaries pass local configuration and restoration physically with delivery disabled. The later diagnostic also passes SGI0/PPI30 delivery on every secondary; scheduling remains unqualified. |
| Shutdown | Disable CPU-interface groups, synchronize, complete the sleep handshake, then power down. Applicable errata remain part of the sequence. | Earlier core-1 shutdown passed without enabling delivery. The later SGI0/PPI30 diagnostic passes bounded quiescence before shutdown; general peripheral interrupt shutdown remains unqualified. |

The [GIC-600 manual](https://documentation-service.arm.com/static/5e7ddddacbfe76649ba53034),
section 3.6.1, and pinned
[TF-A implementation](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/drivers/arm/gic/v3/gic-x00.c)
specify the power handshake. Check the effective implementation: TF-A's generic
power hooks are weak functions overridden by its GIC-600 driver.

The [upstream GIC driver](https://github.com/torvalds/linux/blob/e5f0a698b34ed76002dc5cff3804a61c80233a7a/drivers/irqchip/irq-gic-v3.c)
enables SRE in `gic_starting_cpu`, then calls `gic_cpu_init`. That function wakes
the redistributor, configures local interrupts and initializes CPU-interface
state. Its lack of an A733 power sequence is consistent with the firmware
boundary above; copying that omission into direct EL3 startup would be wrong.

Arm's [GIC architecture specification](https://documentation-service.arm.com/static/601412d54ccc190e5e681269),
IHI 0069G section 11.1, exempts SRE writes from the prohibition on CPU-interface
writes while the redistributor sleeps. It also permits saving CPU-interface
state while the interface is quiescent. The
[software overview](https://documentation-service.arm.com/static/5f1068720daa596235e7f6ef),
section 4.2.1, repeats the SRE exception. TF-A wakes the redistributor before
enabling SRE; Linux enables SRE earlier. These sequences do not establish that
an early SRE write or the diagnostic's group-enable reads cause the failure.

## Returning diagnostics distinguish verification refusals from stalls

The original failing overlay added EL3 SRE setup and group-enable reads before calling
the portable interrupt controller. It then initializes and restores local
interrupt state before requesting shutdown. A missing report can result from
an explicit retained refusal, an exception, a stalled access or failed cleanup.
The returning refusal diagnostic below now identifies a local group-verification
failure. Earlier attempts without reports retain their observation limits.

The source trace through `boot/a733/native-local-interrupt-probe.c` and its
assembly caller identifies the following distinct outcomes:

| Interval | Current behavior | Observation limitation |
| --- | --- | --- |
| Initial identity and baseline checks | Rejects unexpected core, timer, exception level or local interrupt state. Some baseline refusals return through shutdown. | An invalid initial identity returns zero and parks the target. |
| SRE and group-enable checks | Enables EL3 system-register access, reads it back and requires all physical interrupt groups disabled. | Readback or group-enable refusal returns zero and parks, just as an unresolved controller failure does. |
| Portable controller | Discovers all eight frames, checks core control, requests redistributor power, wakes it and configures local interrupts. | Controller phase and error are copied only after polling terminates. A stalled access cannot publish them through this path. |
| Restoration | Disables local interrupts, restores saved configuration, waits for sleep and restores core wake control. | Any failed check parks the target; the host receives no result without the subsequent shared return. |

The assembly caller branches to `park` whenever the C probe returns zero.
Consequently, missing USB return does not distinguish a software refusal from
a hardware stall. A diagnostic improvement must make those outcomes observable
without returning shared hardware to ROM while a secondary might still access
it. Source review alone does not establish which outcome occurred.

The prepared stop-before-power diagnostic still executes the SRE setup and
group-enable reads. Its boundary is the first redistributor power write, not
the first new operation. A failed result there cannot identify that power write
as the cause, because the diagnostic refuses the write before issuing it.

Before the next physical attempt, trace those added operations and every
refusal/cleanup path against the frozen executable. Require a specific question,
the source-backed preconditions for each newly executed operation, and a returned
observation that distinguishes the relevant outcomes. Preserve the successful
entry and local-read evidence. Do not change register order merely because
another reference uses a different permitted order.

The interface-only profile now asks whether core 1 can enable EL3 GIC
system-register access, read back SRE and observe disabled interrupt groups,
then complete the already demonstrated shutdown. It stops before invoking the
portable controller and writes no redistributor or wake-control register during
the new interval. The compiled function confirms this boundary. Successful
observation is a separate result field; baseline refusal cannot count as success.
Unexpected SRE readback or an enabled group retains the target without shutdown.

Three focused instruction cases pass for this profile: successful observation
with matching OFF/return, enabled-group refusal and SRE readback refusal.
System registers, the interrupt controller and power transitions remain modeled.
Physical execution also passes under `native-cpu-interface-20261018/native/runtime/`.
SRE changes from `0x9` to `0xf`; both physical group-enable reads are zero.
The controller and local cleanup write counts remain zero. Matching shutdown,
independently observed OFF, shared-state restoration and saved DRAM comparison
all pass; ROM returns as `003:094`. This establishes the interface interval for
the observed state. It does not identify the earlier initialization failure.
Controller discovery and core wake-control preparation also pass physically under
`native-before-power-current-20261018/native/runtime/`. The callback refuses the
first power write before issuing it. The local bank and wake control restore;
matched shutdown/OFF and ROM return complete as `003:095`.

The next profile completes the production power handshake and refuses the first
WAKER write before MMIO. Its compiled callback records this distinct boundary.
Cleanup requires the original PWRR and unchanged local bank, then restores core
wake control before shutdown. Three instruction cases pass: normal completion,
one rejected power request and retained refusal on a changed baseline. A fixture
repair advances modeled time after any secondary parks without acknowledgment;
only the affected refusal case was rerun.

Physical execution passes under `native-before-wake-20261018/native/runtime/`.
The controller reaches phase 8 with three attempted writes: core wake control,
PWRR zero and the refused WAKER write. PWRR remains `0x100`, WAKER remains `6`,
and the saved local interrupt state matches. Matching shutdown, observed OFF,
shared-state restoration and saved DRAM comparison pass; ROM returns as
`003:096`. This establishes the handshake for an already-powered frame, not a
power-off-to-on transition. A subsequent wake/sleep diagnostic passes under
`native-wake-sleep-20261018/native/runtime/`. The production controller clears
ProcessorSleep and observes ChildrenAsleep clear, then refuses the first local
interrupt-configuration write. Cleanup rechecks disabled CPU groups, records
awake WAKER `0`, restores WAKER `6` and waits for sleep. Local state and core wake
control restore; matching shutdown/OFF, shared restoration and saved DRAM
comparison pass. ROM returns as `003:097`. Three instruction cases cover normal
return and retained wake/sleep timeouts. The model now lets a running secondary
finish after the primary parks; only the affected sleep-timeout case was rerun.

## Required interrupt bits replace an invalid full-width equality

The returning diagnostic now preserves the controller's last register access
and can report a configuration refusal after complete restoration. It permits
that cleanup only after completed wake, within the original deadline, and before
any CPU interrupt delivery. Cleanup failure still prevents shutdown and return.
Three focused cases cover success, restored verification refusal and retained
cleanup mismatch.

The physical result under `native-local-config-report-20261018/native/runtime/`
records phase 23 (`VERIFY_GROUP`), error 4 and a completed read of
`0x03490080` returning `0xfff0ffff`. The controller had written all ones, but
bits 16–19 remained zero. Full cleanup succeeded, the secondary reached OFF,
and ROM returned as `003:098`.

Arm IHI 0069G section 12.11.12, page 12-644, specifies that unimplemented
interrupt bits in `GICR_IGROUPR0` read as zero and ignore writes. The pinned
Linux and TF-A initializers write all ones without requiring full-width equality.
SunstoneOS now checks mask `0x6000ffff`: all SGIs and timer PPIs 29 and 30 used
by this startup. It still writes all ones to assign every implemented local
source to nonsecure Group 1. Future consumers must validate their required
interrupts when enabling them.

The host sanitizer check accepts three supported masks and rejects each missing
required SGI/timer bit. Two native instruction cases cover the observed mask and
a missing required source. Physical execution under
`native-local-group-mask-20261018/native/runtime/` completes the controller at
phase 26 with no error. All eleven initialization writes and seven cleanup writes
complete; original local state, core wake control and translation controls restore.
Matching shutdown/OFF, shared restoration and saved DRAM comparison pass, and
ROM returns as `003:099`. The last controller read observes SGI0 priority `0xf0`.
This fixes the observed local-initialization refusal. It does not establish
interrupt delivery, cache coherence or scheduling.

Source snapshots and hashes are under
`~/.cache/sunstoneos/evidence/multicore-reference-audit-20261018/`.
The external inputs are pinned in `sources.lock.json`; no Linux code executes
as the SunstoneOS runtime.

## Publication must cover the executable physical address space

Arm's [TrustZone guide](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/TrustZone%20for%20Armv8-A.pdf),
sections 3.4–3.5, distinguishes Secure and Nonsecure physical addresses in cache
tags. With the MMU disabled, EL3 addresses are Secure. Maintenance by virtual
address acts on the translated physical address space. Secure set/way operations
can affect both spaces; their coverage of system caches is a separate question.

SunstoneOS's frozen publication helper executes `DC CVAC` with the EL3 MMU off.
The subsequent cached EL3 code mapping selects Nonsecure physical addresses.
That publication therefore does not establish invalidation of an inherited
Nonsecure code alias. The pinned Linux boot contract also requires an image
without stale instruction entries and publication to the point of coherency;
it warns that system caches can require maintenance by address rather than
set/way. See its
[cache requirements](https://github.com/torvalds/linux/blob/e5f0a698b34ed76002dc5cff3804a61c80233a7a/Documentation/arch/arm64/booting.rst).

The first cached-EL1 attempt returned only the EL3 result despite a verified
new upload. The corrected diagnostic maintains Nonsecure code and EL1 table
aliases through temporary EL3 mappings while executing Secure noncacheable code.
It now completes production cached EL1 entry, SMC return, exact restoration and
shutdown physically under `native-ns-publication-20261018/`. This resolves the
missing interval for that diagnostic. The earlier attempt has no instruction
trace that proves exactly which stale entry it consumed.

The instruction model now checks security-selected maintenance and rejects a
missing Nonsecure code invalidation. Its cache model remains scoped; a shared
coherent byte array alone cannot verify publication across physical address
spaces. The corrected helper admits known clean immutable aliases, not arbitrary
dirty runtime state. General warm replacement still needs explicit data ownership
and cleanup before uploads.

The second board's returning PMIC probe executed its newly added voltage-selector
read. That supports the particular boot-CPU transition used there. Device-image
admission must assess its own startup and prior cache ownership; the CPU result
does not invalidate unrelated physical evidence.

## Concurrent data writes require matching cache cleaning

The returning concurrent diagnostic borrows the boot CPU's first EL1 stack page.
The boot CPU remains at EL3, and native BSS initialization and the accepted
Nonsecure publication cover the page before any secondary starts. Production
translation tables map the page as Nonsecure, write-back, inner-shareable memory.
Every secondary performs 32 exclusive atomic increments and 32 ordered payload
transfers using release/acquire synchronization.

Each participant publishes its final store before the all-participant completion
barrier. It then cleans and invalidates every line through the Nonsecure mapping
and completes `DSB SY`. No Normal-memory store follows. This follows the pinned
[TF-A range-flush sequence](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/lib/aarch64/cache_helpers.S).
Arm's cached TrustZone guide, sections 3.4–3.5, explains why maintenance must
address the physical security state used by the writers; its source and hash
are in `cpu-audit/reference-cache.json` under the local source cache.

Physical execution passes under `secondary-coherence-probe-20261018/physical-113/`.
Every secondary completes its cache cleaning, restoration and observed shutdown.
Only then does the uncached primary copy the physical page; all 4,096 bytes match
the expected epoch-bound result. This establishes the tested exchange and
cleanup protocol. It does not qualify arbitrary dirty OS data or DMA reclamation.
