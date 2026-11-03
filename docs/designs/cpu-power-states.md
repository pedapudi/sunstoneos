# Design: idle cores wait in WFI; core power-off waits for a consumer

Status: **WFI idle is implemented and host-tested** in the cached runtime and
the instruction model. **Core 0 single-core WFI is qualified on the board.**
The all-core idle and wake run is open. Core power-off through EL3 is open and
deferred. The mechanism and its configuration are in
[cpu-power.md](../cpu-power.md); section numbers refer to
[performance-architecture.md](../performance-architecture.md).

## Idle tasks execute WFI

Every FreeRTOS idle task calls `a733_cpu_idle()`, a `static inline` function in
`include/a733_cpu_power.h`: `DSB SY` then `WFI`, as section 1.2 permits. The
kernel calls the passive idle hook from both idle loops, so the image
configuration maps only that hook to the function; enabling both hooks would
execute two `WFI` per active idle iteration.

- **EL0 WFI.** Idle tasks run at EL0, so `SCTLR_EL1.nTWI` (bit 16) is set
  beside `nTWE` in `platform/a733-smp-cached/continue.S`; otherwise EL0 `WFI`
  traps with exception class `0x01` (Arm DDI0616A.a, section E3.2).
- **Wake sources.** New work reaches an idle core as SGI0, because
  `prvYieldForTask` treats a core running an idle task as preemptible by any
  task. Core 0 also wakes on timer PPI30.
- **Idle affinity.** `configIDLE_AFFINITY` is 1, so the active idle task stays
  on core 0, which the tick wakes; a self-deleted task is reclaimed within one
  tick of core 0 becoming idle.
- **Time slice.** `patches/freertos-smp-tasks.patch` changes the time-slice
  check in `xTaskIncrementTick` to yield a core running an idle task only when
  more than `configNUMBER_OF_CORES` tasks are ready at idle priority, the test
  `configIDLE_SHOULD_YIELD` already uses. The idle-priority list always holds
  all eight idle tasks, so without the patch each tick sends SGI0 to seven idle
  cores. Round-robin among application tasks is unchanged.

No EL3 code changes, and the image's EL3 vectors keep treating every SMC as a
terminal report. The images that build the patched `tasks.c` are
`cmake/a733-smp-cached.cmake`, `cmake/a733-smp-observe.cmake` and
`cmake/a733-smp-supervised.cmake`.

## Qualified figures

| Figure | Budget | Result | Evidence |
| --- | --- | --- | --- |
| SGIs to idle secondaries per tick, no ready task | 0 | 0 in 64 ticks (model) | `cpu-power-states.jsonl`: `cpu-power-states-model-budgets` |
| Instructions per idle secondary in the window | 0 | 0 (model) | same record |
| Core 0 timer wake-ups per second | 1,000 | 1,000.7 (model) | same record |
| Idle loop growth per iteration | at most 6 instructions | 2 | same record |
| `xTaskIncrementTick` static instructions | one compare | 265 against 241; 24 register instructions replace seven SGIs and seven context switches per tick | same record |
| Core 0 single-core `WFI` on the board | wake every tick | 1,000 wakes in 1.0 s, longest gap 1.0 ms, active cycles 0.17% | `cpu-power-states.jsonl`: `cpu-power-states-wfi-core0-board` |

The model has no A733 power, cache or interconnect timing, so its figures show
interrupts sent and instructions executed, not energy or wake-up latency.
`PMCCNTR_EL0` stops while the core waits in `WFI`, so it measures only active
time.

## Limitations and open work

1. The all-core run (record `cpu-power-states-all-core-idle-wake`): EL0 `WFI`
   with `nTWI`, woken by SGI0 and PPI30 on all eight cores under the
   scheduler, with the SGI counts above. The application is
   `apps/cpu-runtime-qualification`.
2. Tickless idle on core 0 waits for services that poll on ticks (section 1.3).
3. Core retention, PPU policy and GIC hardware wake from OFF are undocumented
   in the manual, and the A733 TF-A port defines no idle states.
4. L3 partial power-down has no consumer. Cluster and DSU power-down needs
   core 0 off, which the running OS never allows: the A733 has one DynamIQ
   cluster holding all eight cores.

## Deferred: core power-off and power-on through EL3

**Why deferred.** No image has a caller. The recognition core pair (cores 6
and 7) stays in use until the boot ends (`docs/recognition-dispatch.md`), and no
other consumer powers a core off and on again within one boot. The gain over
`WFI` idle is not measured. The merged power-authority design relies on the OS
issuing no PSCI `CPU_OFF` (`docs/designs/power-authority.md`), and
`checks/pmic-tree.py` fails if that function identifier appears in the sources.

**Trigger for return.** All of: a named consumer that powers a core off and on
again within one boot and runs on a core it never powers off; a power or
leakage budget per core measured against `WFI` idle; and a change to the
power-authority design and `checks/pmic-tree.py` agreed with their owner.

**Hardware record.** The register sequences, sources and per-step board status
are in the record `cpu-power-states-core-power-off-hardware`. First power-on
passes on every secondary through `platform/a733/cpu_start.c`; a second
power-on in one boot is unproven. The GIC and core power-down sequences pass in
startup diagnostics, and warm entry passes for cold entry only. No sequence is
qualified from a running scheduler.

**Requirements on a returning design.**

- **One state machine.** Only the EL1 per-core word tracks a transition, and
  each transition has one named writer. EL3 keeps no compare-and-swap state:
  the monitor section is Device-nGnRnE at EL3 (AttrIndx 0 under `MAIR_EL3`),
  where exclusives and atomics are not guaranteed. EL3 records use plain
  stores ordered by `DSB`, with one writer per word. PSCI `ALREADY_ON` and
  `ON_PENDING` are not returned.
- **Off request.** The request transition happens inside the port SVC with
  IRQs masked, for the core named by `tpidr_el1`. The SVC refuses unless the
  current task is that core's idle task and `ullCriticalNestings[core]` is 0.
  After writing the request, the owner sends one SGI0 with release ordering,
  because an idle core waits in `WFI` and nothing else wakes it.
- **Scheduler.** Every remote-yield path skips a core that is not online:
  `prvYieldCore`, the affinity search in `prvSelectHighestPriorityTask` and
  the time-slice loop in `xTaskIncrementTick`. A displaced task whose affinity
  includes an off core runs on an online core within one tick.
- **Admission.** Before the request, the owner disables every SPI routed to the
  core (`GICD_ICENABLER`, checked against the route table) and confirms that
  every task pinned only to that core is blocked or suspended with no open
  service window. A build check refuses an unconditional SPI route to a core
  that can power off.
- **`CPU_OFF` failure.** The target records OFF only after `ChildrenAsleep`
  succeeds. On a timeout, EL3 clears `ProcessorSleep`, re-enables the
  CPU-interface groups and returns `DENIED`; the SVC resumes the saved idle
  context, and the target writes ONLINE itself. The owner calls `CPU_ON` only
  after it observes OFF.
- **`CPU_ON` caller.** EL3 accepts `CPU_ON` only from core 0, which meets the
  core-0 admission of `a733_cpu_start_init` with its true MPIDR and serializes
  all callers. EL3 never waits for OFF: before OFF it returns not-ready, and
  the owner retries from EL1 with a blocking delay. The worst interrupt-masked
  time per SMC on core 0, with the delayed and catch-up ticks it causes, gets a
  budget row.
- **Warm path.** `CPU_ON` stores a warm deadline and the epoch in the target's
  EL3 record. Warm entry uses that deadline for coherence, continue and the EL3
  and EL1 enables, skips the all-core record barriers and the mailbox checks,
  and waits in `WFE`, bounded, for core 0's POWERED record and `SEV` before it
  touches `0x07050200 + n*4`. SMC entry and warm entry set `SP_EL3` to a
  per-core monitor stack. PSCI identifiers dispatch before the vector stub
  writes `x18`, and the SMC preserves registers as SMCCC requires.
- **Unreturned core.** WDT0 does not reset the board when a pinned core stops,
  because core 0 keeps renewing it. The owner calls `board_fail` when `CPU_ON`
  fails or the core is not ONLINE within a stated bound.
- **Budgets and checks.** Budget rows cover power per core against `WFI`, SGIs
  per off request (1) and the masked time above. A host check
  models each refusal, the `DENIED` path and the warm handoff. The QEMU
  eight-core model shows a core in `WFI` powering off after one SGI.
