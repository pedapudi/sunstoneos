# Design: idle cores wait in WFI; core power-off waits for a consumer

Status: **WFI idle implemented in the instruction model**; the image change
awaits the scheduler owner, and no board has executed it. The implementation,
its measurements and its differences from this design are in
[cpu-power.md](../cpu-power.md). Core power-off remains deferred. Section
numbers refer to `docs/performance-architecture.md`.

The implementation differs from the text below in three points. The idle
function is `static inline` in `include/a733_cpu_power.h`, and the image
configuration maps the passive idle hook to it, because the kernel calls that
hook from both idle loops; enabling both hooks would execute two `WFI` per
active idle iteration. `configIDLE_AFFINITY` is set, so the active idle task
stays on core 0, which the tick wakes; a self-deleted task is reclaimed within
one tick of core 0 becoming idle. The time-slice condition is a patch to the kernel's
`tasks.c`, `patches/freertos-smp-tasks.patch`, because
`patches/freertos-smp-c.patch` changes only the port.

Today an idle core never sleeps. The cached eight-core image disables both
FreeRTOS idle hooks, so idle tasks spin. Core 0 alone takes the 1 ms tick, and
its time-slice check in `xTaskIncrementTick` yields every core that runs an
idle task, because the idle-priority ready list always holds all eight idle
tasks: 7 SGI0 interrupts and 7 context switches per tick with no work
(counted).

This design ships one change: idle cores execute `WFI`. Powering secondary
cores off and on through EL3 is deferred until a consumer needs it; the last
section keeps the hardware record and the conditions for its return.

## Idle tasks execute WFI

Both idle hooks call `a733_cpu_idle()` in `platform/a733/cpu_power.c`:
`DSB SY` then `WFI`, as section 1.2 permits. Idle tasks run at EL0, so
`SCTLR_EL1.nTWI` (bit 16) must be set beside `nTWE`, or EL0 `WFI` traps with
exception class `0x01` (Arm DDI0616A.a, section E3.2).

`prvYieldForTask` treats a core running an idle task as preemptible by any
task, so new work reaches an idle core as SGI0; core 0 also wakes on timer
PPI30. The time-slice check changes to yield a core running an idle task only
when more than `configNUMBER_OF_CORES` tasks are ready at idle priority, the
test `configIDLE_SHOULD_YIELD` already uses. Round-robin among application
tasks is unchanged. The spinning idle loop is removed. No EL3 code changes,
and the image's EL3 vectors keep treating every SMC as a terminal report.

## Performance budgets

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| SGIs to idle secondaries per tick, no ready task | 7 (counted) | 0 | `sgis` counters in `checks/a733-smp-cached.py` and the QEMU eight-core model, over 64 ticks |
| Instructions per idle secondary between wake-ups | Its full emulation quantum (counted: spin) | 0 in `WFI`; idle loop grows by at most 6 instructions | Per-core instruction count added to the same model |
| Core 0 idle wake-ups per second | 1,000 | 1,000 | Same model |
| Added work per tick in `xTaskIncrementTick` | 0 | One list-length compare | Disassembly |

## Changes outside this package

For the primary agent, as implemented and measured:

- `platform/a733-smp-cached/FreeRTOSConfig.h`: enable the passive idle hook,
  map it to `a733_cpu_idle` and set `configIDLE_AFFINITY`.
- `platform/a733-smp-cached/continue.S`: set `SCTLR_EL1.nTWI`.
- `patches/freertos-smp-tasks.patch` (new): the idle time-slice condition in
  `tasks.c`.
- `cmake/a733-smp-cached.cmake`, `cmake/a733-smp-observe.cmake` and
  `cmake/a733-smp-supervised.cmake`: build the patched `tasks.c`.
- `cmake/a733-smp-observe.cmake`, `cmake/a733-smp-supervised.cmake` and
  `cmake/a733-storage-maintenance.cmake`: record the new `continue.S` digest
  that each compares before applying its private `continue.patch`. All three
  private patches still apply.

## Board evidence needed

In a scheduled board session: EL0 `WFI` with `nTWI`, woken by SGI0 and PPI30,
on all eight cores under the scheduler, with the SGI counts above.

## Left out

- **Tickless idle on core 0:** services still poll on ticks (section 1.3).
- **Core retention, PPU policy and GIC hardware wake from OFF:** the manual
  documents neither the PPU nor the hotplug registers, and the A733 TF-A port
  defines no idle states.
- **L3 partial power-down:** no consumer.
- **Cluster and DSU power-down:** the A733 has one DynamIQ cluster holding all
  eight cores (user manual section 5.1.5.4, printed page 250: `C0_CPU0` to
  `C0_CPU7` under one `C0_RST_CTRL`; figure 5-1 shows one L3 and snoop
  filter). Cluster power-down needs core 0 off, which the running OS never
  allows.

## Deferred: core power-off and power-on through EL3

**Why deferred.** No image has a caller. The recognition core pair (cores 6
and 7) stays in use until the boot ends (`docs/recognition-dispatch.md`: "the
session lasts until the boot ends"), and no other consumer powers a core off
and on again within one boot. The power gain over `WFI` idle is not measured.
The merged power-authority design relies on the OS issuing no PSCI `CPU_OFF`
(`docs/designs/power-authority.md`, supply verification), and
`checks/pmic-tree.py` fails if that function identifier appears in the
sources.

**Trigger for return.** All of: a named consumer that powers a core off and
on again within one boot and runs on a core it never powers off; a power or
leakage budget per core measured against `WFI` idle; and a change to the
power-authority design and `checks/pmic-tree.py` agreed with their owner.

**Hardware record.** No sequence below is qualified from a running scheduler.

| Step | Sequence | Source | Board status |
| --- | --- | --- | --- |
| `CPU_ON` | Require PPU status OFF (`0x07052008 + n*0x1000`, bits 3:0 = 0); write and verify entry words (`0x08001004`, `0x08001008 + n*0x1000`) and AArch64 bit 0 of `0x08001000 + n*0x1000`; set mode bit 0 (`0x07050220 + n*4`), then control bit 0 (`0x07050200 + n*4`); wait for status 8 (ON); restore control and mode | Published board monitor helpers at file offsets `0xc4a0`, `0xc298`, `0xc314`; TF-A A733 `sunxi_pwr_domain_on` (`InfernoEmbedded/arm-trusted-firmware@5bc9605`, `plat/allwinner/common/sunxi_native_pm_ncat2.c`); implemented as `platform/a733/cpu_start.c` | First power-on passes on every secondary. A second power-on in one boot is **unproven** |
| `CPU_OFF`, GIC | Disable CPU-interface groups; set `GICR_WAKER.ProcessorSleep`; wait for `ChildrenAsleep` | Arm IHI 0069G, `GICR_WAKER`; TF-A `gicv3_rdistif_mark_core_asleep` | Passes with delivery disabled (`native-wake-sleep-20261018`) |
| `CPU_OFF`, core | Set `CPUPWRCTLR_EL1.CORE_PWRDN_EN`, `DSB`, `ISB` (Cortex-A76 erratum 2743102); clear mode bit 0; set control bit 1 (GIC wake-up disable); `DSB SY`; `WFI` | TF-A `cortex_a55_core_pwr_dwn`, `cortex_a76_core_pwr_dwn` (`ARM-software/arm-trusted-firmware@0892ad4`); TF-A A733 `sunxi_cpu_off`; monitor helper at `0xc2f0` | Passes in startup diagnostics on every secondary (`secondary-local-probe-20261018/physical-110`) |
| Warm entry, GIC | Clear control bit 1; wake the redistributor; configure SGI0 | `platform/a733/cpu_continue.c`; TF-A `sunxi_pwr_domain_on_finish`; monitor helper at `0xc224` | Passes for cold entry only |

Both TF-A power-down functions issue no cache maintenance before `WFI`, so
they rely on hardware cleaning of the private caches; the Cortex-A55 r2p0 and
Cortex-A76 r4p1 manuals must confirm this before any code. The published
monitor also clears the low four bits of nine undocumented registers at
`0x07051000`–`0x07059000` (file offset `0x4e88`).

**Requirements on a returning design.**

- **One state machine.** Only the EL1 per-core word tracks a transition, and
  each transition has one named writer. EL3 keeps no compare-and-swap state:
  `a733_cached_records` and the rest of the monitor section are
  Device-nGnRnE at EL3 (AttrIndx 0 under `MAIR_EL3`), where exclusives and
  atomics are not guaranteed, and that mapping keeps MMU-off warm entry
  consistent with MMU-on access. EL3 records use plain stores ordered by
  `DSB`, with one writer per word. PSCI `ALREADY_ON` and `ON_PENDING` are not
  returned.
- **Off request.** `configIDLE_AFFINITY` is 1, so each passive idle task
  stays on its own core. The request transition happens inside the port SVC
  with IRQs masked, for the core named by `tpidr_el1`. The SVC refuses unless
  the current task is that core's idle task and `ullCriticalNestings[core]`
  is 0. After writing the request, the owner sends one SGI0 with release
  ordering, because an idle core waits in `WFI` and nothing else wakes it.
- **Scheduler.** Every remote-yield path skips a core that is not online:
  `prvYieldCore`, the affinity search in `prvSelectHighestPriorityTask` and
  the time-slice loop in `xTaskIncrementTick`. A displaced task whose
  affinity includes an off core runs on an online core within one tick.
- **Admission.** Before the request, the owner disables every SPI routed to
  the core (`GICD_ICENABLER`, checked against the route table) and confirms
  that every task pinned only to that core is blocked or suspended with no
  open service window. A build check refuses an unconditional SPI route to a
  core that can power off.
- **`CPU_OFF` failure.** The target records OFF only after `ChildrenAsleep`
  succeeds. On a timeout, EL3 clears `ProcessorSleep`, re-enables the
  CPU-interface groups and returns `DENIED`; the SVC resumes the saved idle
  context, and the target writes ONLINE itself. The owner calls `CPU_ON` only
  after it observes OFF, so `DENIED` and `CPU_ON` cannot interleave.
- **`CPU_ON` caller.** EL3 accepts `CPU_ON` only from core 0. Core 0 meets
  the core-0 admission of `a733_cpu_start_init` with its true MPIDR and
  serializes all callers; the design names the lease value it passes. EL3
  never waits for OFF: before OFF it returns not-ready, and the owner retries
  from EL1 with a blocking delay. The worst interrupt-masked time per SMC on
  core 0, with the delayed and catch-up ticks it causes, gets a budget row.
- **Warm path.** `CPU_ON` stores a warm deadline and the epoch in the
  target's EL3 record. Warm entry uses that deadline for coherence, continue
  and the EL3 and EL1 enables, and skips the all-core record barriers and the
  mailbox checks. Warm entry keeps the cold path's handoff: it waits in `WFE`,
  bounded, for core 0's POWERED record and `SEV` before it touches
  `0x07050200 + n*4`. SMC entry and warm entry set `SP_EL3` to a per-core
  monitor stack. PSCI identifiers dispatch before the vector stub writes
  `x18`, and the SMC preserves registers as SMCCC requires.
- **Unreturned core.** WDT0 does not reset the board when a pinned core
  stops, because core 0 keeps renewing it. The owner calls `board_fail` when
  `CPU_ON` fails or the core is not ONLINE within a stated bound.
- **Budgets and checks.** Budget rows cover power per core against `WFI`,
  SGIs per off request (1) and the masked time above.
  `checks/cpu-power-states.c` models each refusal, the `DENIED` path and the
  warm handoff. The QEMU eight-core model shows a core in `WFI` powering off
  after one SGI.
