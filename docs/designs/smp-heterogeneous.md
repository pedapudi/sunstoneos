# Design: one cached SMP base, lighter scheduler locks and A55/A76 task placement

Status: the cached-base consolidation, the first port change (items 1 to 5) and the placement header are **implemented** and checked in emulation only. Acquire and release locks (item 6) are **implemented** as FIFO ticket locks, which are **qualified on the board** in [the scheduler locks status](../status/scheduler-locks.md). PMR writes without `DSB` (item 7) are **open**. Section numbers refer to [performance-architecture.md](../performance-architecture.md).

The observe and supervised images build from the cached diagnostic's source list. The SMP port drops supervisor calls, locks and barriers that protect no shared data. Compile-time constants name the A55 cluster (cores 0–5), the A76 pair (cores 6 and 7) and the core of each role.

## Data flow and ownership

- **Build.** Only `cmake/a733-smp-cached.cmake` patches `port.c`, `portASM.S`, `portmacro.h` and `tasks.c`. The observe and supervised images, like `cmake/a733-smp-cached-devices.cmake`, take the cached target's `SOURCES`, replace `boot.c`, `continue.S`, `diagnostic.c` and the admission file, and add their USB and publication sources. Each keeps its identity tag, contract and result magic, and its pinned hashes of the cached `boot.c` and `continue.S`.
- **Scheduler state.** Each core owns its critical nesting, interrupt nesting, yield request, FPU flag and lock-ownership bits. The task and ISR locks are the only port state that several cores write.
- **Core type.** Admission verifies it once: `valid_report()` in `platform/a733/cpu_entry.c` and `a733_cpu_coherence_plan()` in `platform/a733/cpu_coherence.c` refuse any started core whose `MIDR_EL1` is not Cortex-A55 (part 0xD05) for cores 0–5 or Cortex-A76 (part 0xD0B) for cores 6 and 7. Under the scheduler the type is a fixed function of the core index, so nothing reads `MIDR` again.
- **Image identity.** The port patches alter every cached-family build identity, so board evidence stays bound to the identity it ran.

## Port changes

Items 1 to 5 remove instructions and keep every barrier on a path that still touches shared data.

1. **Core index and current task in `TPIDRRO_EL0`.** Bits [47:0] hold the current TCB address, bits [55:48] the core index, and bit 56 is set when the task runs at EL1. The register is UNKNOWN at reset, so the entry code of the cached images (`platform/a733-smp-cached/continue.S`) and the eight-core diagnostic (`a733_smp_pre_scheduler`) clears it before the first kernel call. The context-restore path writes all three fields for the task it resumes, and `prvSelectHighestPriorityTask` writes the TCB field on its own core.
   - While the TCB field is zero, `ucPortGetCoreID` and `xTaskGetCurrentTaskHandle` take the supervisor-call path, so QEMU, the instruction models and every check that starts an image part-way work without an entry write.
   - Once the field is set, `ucPortGetCoreID` is `MRS`, `UBFX`, `RET`, and `xTaskGetCurrentTaskHandle` returns the TCB field with one `MRS`. A task's own TCB does not change when it migrates, so the read needs no mask.
   - An EL0 task can read the register; it holds that task's own handle and core, which the API already returns.
2. **EL1 tasks mask directly.** `portDISABLE_INTERRUPTS`, `portENABLE_INTERRUPTS`, `uxPortSetInterruptMask` and `vPortClearInterruptMask` test bit 56 and, when it is set, write `ICC_PMR_EL1` in line, followed by `DSB SY` and `ISB`. The inline sequence never writes `DAIF`, so the caller's `DAIF.I` is kept, as the supervisor-call path keeps it by returning the caller's `SPSR`. Pre-scheduler code runs with `DAIF` = 0x3c0 and an ISR runs with `I` set; the upstream FromISR bracket, which ends with `MSR DAIFCLR, #2`, would unmask both. EL0 tasks keep the supervisor calls, and an EL0 task cannot reach the inline write because the restore path clears bit 56 for it.
3. **SGI handler without a critical section.** `FreeRTOS_SGI_Handler` only sets this core's yield flag, with `DAIF.I` still set from exception entry; no other core writes the flag. `vTaskSwitchContext` takes both locks at IRQ exit.
4. **One mask in the tick handler.** `portENTER_CRITICAL_FROM_ISR` already masks, so the tick handler has no outer `portSET_INTERRUPT_MASK_FROM_ISR` pair.
5. **Per-core cache lines.** `xPortCoreStates` holds each core's FPU-context flag and lock-ownership bits in one 64-byte-aligned structure per core, and `xPortLocks` gives each lock word and its recursion count their own line. `ullCriticalNestings`, `ullPortInterruptNestings` and `ullPortYieldRequired` keep their symbols and 8-byte stride, because drivers and checks outside the port read or write them by name. A critical-section or interrupt entry or exit that writes a nesting word first takes ownership of the line if another core wrote it since. QEMU does not model that transfer, so only a board session shows it.

The second change weakens barriers ([shared-helpers.md](../shared-helpers.md)).

6. **Acquire and release locks (implemented).** Take with `LDAXR` or `LDADDA` and release with `STLRH` on a FIFO ticket word. Waiters loop on `WFE` and `LDAXRH` after `SEVL`; the releasing store clears their exclusive monitors, which sets the event register. Only the holder touches the recursion count and only its core touches its ownership bits, so `prvGet64` and `prvSet64` are plain accesses. `SUNSTONE_LOCK_TAS_CONTROL` builds the test-and-set lock that serves as the control.
7. **PMR writes without `DSB` or `DAIF` bracket (open).** Masking would be one `MSR ICC_PMR_EL1`: IHI 0069G §12.1.6 makes PMR writes self-synchronizing, so no interrupt below the new mask is taken once the write executes. Unmasking would be `MSR` then one `ISB`; §12.1.6 covers only masking, and the `ISB` makes an interrupt held pending by the old mask arrive at the unmask point. The SVC handlers and FromISR functions would drop `MSR DAIFSET` and `DAIFCLR`, because taking the SVC or IRQ already set `PSTATE.I` and the bracket served memory-mapped `GICC_PMR` on GICv2. `DSB SY` would go because the Redistributor observes PMR only when `ICC_CTLR_ELx.PMHE` = 1 (§12.1.6) and `continue.S` writes `ICC_CTLR_EL3` = 0. An implementation may make PMHE RAO/WI (§12.2.6), so `continue.S` would read `ICC_CTLR_EL3` back and fault if bit 6 is set. `Interrupt_Routing_Mode` 0 in every `GICD_IROUTER` write of `platform/a733-smp-cached/irq_table.c` stays as a second guard. The patches still issue `DSB SY` and `ISB` after each PMR write.

All seven edit `patches/freertos-smp-c.patch`, `patches/freertos-smp-asm.patch` and `patches/freertos-smp-macro.patch`.

## Interrupts, memory and synchronization

- Cross-core wake-ups use SGI0, the 1 kHz tick stays on core 0, and lock waiters sleep in `WFE`. Each role's device interrupt (GPU INTID 95, the NPU interrupt) routes to the role's core, so completion wakes the waiter without an SGI.
- All state stays in the shared write-back mapping; no device memory, DMA buffer or cache maintenance is added. Physical coherence between the A55 and A76 clusters is a board result ([a733-smp-cached.md](../a733-smp-cached.md)).
- Each core's `TPIDRRO_EL0` is written only by that core at EL1.
- SGI generation keeps `DSB ISHST` before `MSR ICC_SGI1R_EL1`, as `gic_ipi_send_mask` does in the pinned Linux v6.17 `irq-gic-v3.c`.

## Placement constants

`include/a733_smp_placement.h` holds preprocessor constants for `xTaskCreateStaticAffinitySet`. `A733_SMP_A55_MASK` is 0x3F. `A733_SMP_APPLICATION_CORE` and `A733_SMP_APPLICATION_WORKER_CORE` are 6 and 7 when the compile definition `A733_A76_ADMITTED` is set, and 2 and 3 otherwise. The service core is 0, matching `cmake/a733-os.cmake` and [recognition-dispatch.md](../recognition-dispatch.md). A preprocessor check rejects any mask or core outside `configNUMBER_OF_CORES`. `apps/os/start.c` asserts `OS_RECOGNITION_CORE == A733_SMP_APPLICATION_CORE`, so the CMake role mapping stays the single table and the header cannot drift from it. Turning on `A733_A76_ADMITTED` (CPU-B clock and rail, A76 startup controls) belongs to the CPU clock work ([cpu-a76-dvfs.md](cpu-a76-dvfs.md)).

| Hardware fact | Source | Status |
| --- | --- | --- |
| Cores 0–5 `arm,cortex-a55`, 6–7 `arm,cortex-a76` | `sun60iw2p1.dtsi`, linux-orangepi `2ac08e8` (pinned); MIDR check at admission | Cited; checked on every start |
| `TPIDRRO_EL0` is read-only at EL0, writable at EL1, UNKNOWN at reset | A55 TRM B2.2; Arm ARM DDI 0487 | Entry code clears it |
| `LDAXR`/`STLR` ordering; event on monitor clear | Arm ARM DDI 0487 | Used by the ticket locks |
| PMR writes self-synchronizing; Redistributor sees PMR only with PMHE = 1; PMHE may be RAO/WI | IHI 0069G §12.1.6, §12.2.6 | Cited; item 7 is open |

`sources.lock.json` pins DDI 0487 and the Cortex-A76 TRM.

## Qualified and measured figures

Every figure comes from QEMU instruction counts, the Unicorn model of the cached eight-core image, or the built ELF files; none comes from the board. Record: `docs/evidence/smp-heterogeneous.jsonl`, `smp-heterogeneous-port-cost-emulation` and `smp-heterogeneous-consolidation-bytes`.

| Operation | Base | Measured | Target |
| --- | --- | --- | --- |
| Critical-section pair, EL1 task, instructions | 487 | 296 | 100 |
| Critical-section pair, EL0 task, instructions | 487 | 368 | 180 |
| Current-task read, instructions | 170 | 11 | 0 `SVC`, `DSB`, `ISB` (met) |
| Same-core switch after a notification, instructions | 2,779 | 1,320 | 800 |
| SGI0 handler, instructions | 479 | 15 | 0 `SVC`, `DSB`, `ISB`, locks (met) |
| Tick on core 0: `SVC`, `DSB` | 12, 36 | 7, 32 | 0, 18 |
| Cross-core notification round trip, both cores | 29,236 | 12,191 | at least 30% below base (58%) |

## Limitations and open work

- The EL1 pair and the same-core switch miss their targets because the four `vPortRecursiveLock` calls (about 170 of the 296 pair instructions) keep their assertions and barriers. The EL0 pair also keeps two supervisor calls.
- The seven supervisor calls in the tick are `SVC 117` in `vInterruptCore`: the tick interrupts EL0 tasks, whose bit 56 is clear. An in-line SGI send from interrupt context would remove them and needs its own design.
- The observe and supervised images differ from the previous build in 433 and 1,018 bytes outside `a733_smp_build_id`, all in `configASSERT` file-name strings and the addresses that point at them.
- Open: item 7 (PMR writes without `DSB`), after a supervised eight-core session with SGI, tick and floating-point progress on every core (record `smp-heterogeneous-supervised-session`, not run).
- Open: tasks on cores 6 and 7 with coherent shared memory through the native mimus image, and A55 against A76 timing under [validation.md](../validation.md).

Not part of this design: masking from EL0 through `SCTLR_EL1.UMA` (it moves masking from GIC priority to `DAIF` and edits reserved startup code), lazy FPU switching unless the context switch exceeds its budget, migration between clusters and clock-aware placement (no consumer). The `tasks.c` arrays `pxCurrentTCBs` and `xYieldPendings` keep their layout; other cores write them only under the kernel locks.
