# Multicore startup must satisfy both firmware and kernel contracts

The current failure is unresolved. Core 1 has physically reached EL3, completed
CPU preparation and returned through verified shutdown. Its local interrupt
snapshot also returned successfully. The later initialization attempt returned
neither a report nor ROM access. That result does not identify a failing register.

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

## Power, wake and interrupt delivery have separate requirements

| Boundary | Reference requirement | SunstoneOS evidence and remaining gap |
| --- | --- | --- |
| CPU release | Program the target entry and architecture, complete the SoC power request, then execute the secondary entry. | Core 1 entry, release, CPU preparation and shutdown passed. Other cores remain unqualified. |
| Redistributor power | GIC-600 requires the selected frame's `GICR_PWRR` handshake before interrupt-state access. | The portable controller implements the pinned Arm sequence. The observed core-1 frame permits access, but the initialization attempt has no returned result. |
| Redistributor wake | Clear `ProcessorSleep` and observe `ChildrenAsleep` clear before ordinary CPU-interface writes. | The controller implements this wait. Physical wake completion has not been observed. |
| System-register access | Enable the appropriate SRE controls and respect security-state banking. | Production EL3-to-EL1 code configures these controls. The returning diagnostic has a separate, narrower EL3-only setup. Neither establishes secondary interrupt delivery. |
| Interrupt delivery | Configure local groups, priorities, enables and CPU-interface controls before receiving interrupts. | Software checks pass; physical per-core delivery and scheduling remain unqualified. |
| Shutdown | Disable CPU-interface groups, synchronize, complete the sleep handshake, then power down. Applicable errata remain part of the sequence. | Earlier core-1 shutdown passed without enabling delivery. This does not qualify shutdown after normal interrupt use. |

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

## Diagnose the actual unobserved interval

The failing overlay adds EL3 SRE setup and group-enable reads before calling
the portable interrupt controller. It then initializes and restores local
interrupt state before requesting shutdown. A missing report can result from
an explicit retained refusal, an exception, a stalled access or failed cleanup.
There is no evidence selecting one of those causes.

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
Evidence is under `native-cpu-interface-20261018/`; physical execution is pending.
This diagnostic does not identify a cause if the new interval cannot return.

Source snapshots and hashes are under
`~/.cache/sunstoneos/evidence/multicore-reference-audit-20261018/`.
The external inputs are pinned in `sources.lock.json`; no Linux code executes
as the SunstoneOS runtime.
