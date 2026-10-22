# Design: one cached SMP base, lighter scheduler locks and A55/A76 task placement

Status: **design, no code**. Section numbers refer to
[performance-architecture.md](../performance-architecture.md). Figures marked
*counted* come from reading the pinned FreeRTOS port with the project patches:
uncontended, `configASSERT` enabled, critical nesting from 0 to 1 and back.

The observe and supervised images build from the cached diagnostic's source
list. The SMP port drops supervisor calls, locks and barriers that protect no
shared data. A placement interface puts a task on the A55 cluster (cores 0–5),
the A76 pair (cores 6 and 7) or a named core, using the core type in
`MIDR_EL1`.

## Data flow and ownership

- **Build.** Only `cmake/a733-smp-cached.cmake` patches `port.c`, `portASM.S`,
  `portmacro.h` and `tasks.c`. The observe and supervised files stop
  re-applying the same five patches into their own directories. Like
  `cmake/a733-smp-cached-devices.cmake`, they take the cached target's
  `SOURCES`, replace `boot.c`, `continue.S`, `diagnostic.c` and the admission
  file, and add their USB and publication sources. Each keeps its identity tag,
  contract and result magic, and its pinned hashes of the cached `boot.c` and
  `continue.S`. The cached `SOURCES` list is unchanged, so images derived from
  it need no edit.
- **Scheduler state.** Each core owns its critical nesting, interrupt nesting,
  yield request, FPU flag and lock-ownership bits. The task and ISR locks are
  the only port state that several cores write.
- **Core type.** In `a733_cached_el1`, before the pre-scheduler barrier, each
  core stores `MIDR_EL1` once in its own 128-byte `a733_cached_runtime` slot
  (`reserved[0]` becomes `midr`). Core 0 reads the eight values when it creates
  tasks.

## Port changes

The first change removes instructions and keeps every barrier on a path that
still touches shared data; the emulator checks qualify it.

1. **Core index without a supervisor call.** The context-restore path writes
   `TPIDRRO_EL0` = core index (bits 0–7, from `TPIDR_EL1`), plus bit 8 when the
   task runs at EL1: four instructions per switch. `portGET_CORE_ID()` becomes
   one `MRS TPIDRRO_EL0` at either level, replacing `SVC 109`, which runs twice
   per critical section and twice per tick or SGI handler.
2. **EL1 tasks mask directly.** `portDISABLE_INTERRUPTS` and
   `portENABLE_INTERRUPTS` test bit 8 and run the PMR sequence in line; EL0
   tasks keep `SVC 107` and `SVC 108`. An EL0 task that reaches the EL1
   sequence traps on `MSR ICC_PMR_EL1` into the existing fault path.
3. **SGI handler without a critical section.** `FreeRTOS_SGI_Handler` only sets
   this core's yield flag, with `DAIF.I` still set from exception entry; no
   other core writes the flag. Its PMR mask pair and ISR lock go;
   `vTaskSwitchContext` still takes both locks at IRQ exit.
4. **One mask in the tick handler.** `portENTER_CRITICAL_FROM_ISR` already
   masks, so the outer `portSET_INTERRUPT_MASK_FROM_ISR` pair goes.
5. **Per-core cache lines.** `ullCriticalNestings`, `ullPortInterruptNestings`,
   `ullPortYieldRequired`, `ullPortTaskHasFPUContext` and `ullOwnedByCore`
   become one 64-byte-aligned structure per core, indexed with `LSL #6`. Each
   lock word and its recursion count get their own line (section 2.6).

The second change weakens barriers and merges only after board qualification
([shared-helpers.md](../shared-helpers.md)):

6. **Acquire and release locks.** Take with `LDAXR`/`STXR`, release with
   `STLR`. Waiters loop on `WFE` and `LDAXR` after `SEVL`; the releasing store
   clears their exclusive monitors, which sets the event register. Only the
   holder touches the recursion count and only its core touches its ownership
   bits, so `prvGet64` and `prvSet64` drop `DSB SY`. The `DMB SY` after
   acquire and the `DSB SY`, `SEV` and `ISB` in release go.
7. **PMR writes with `ISB` only.** IHI 0069G §12.1.6: a `DSB` makes a PMR value
   observable by the Redistributor, and that observation does not affect
   masking in the CPU interface. §2.3 and §12.2.6 (`ICC_CTLR_EL1.PMHE`) limit
   the Redistributor's use of PMR to 1-of-N SPI routing. Every route in
   `platform/a733-smp-cached/irq_table.c` writes `GICD_IROUTER` with
   `Interrupt_Routing_Mode` 0. The PMR sequences in the SVC handlers and the
   FromISR mask functions therefore keep `ISB` and the `DAIF` bracket and drop
   `DSB SY`.

All seven edit `freertos-smp-c.patch`, `freertos-smp-asm.patch` and
`freertos-smp-macro.patch`; `freertos-a733-smp.patch` still applies by offset.

## Interrupts, memory and synchronization

- No new interrupt or poll: cross-core wake-ups stay SGI0, the 1 kHz tick
  stays on core 0, and lock waiters sleep in `WFE`. Each role's device
  interrupt (GPU INTID 95, the NPU interrupt) routes to the role's core, so
  completion wakes the waiter without an SGI.
- All state stays in the shared write-back mapping; no device memory, DMA
  buffer or cache maintenance is added. Physical coherence between the A55 and
  A76 clusters remains a board result ([a733-smp-cached.md](../a733-smp-cached.md)).
- `MIDR` records are plain stores before `a733_smp_pre_scheduler`, whose
  release stores and acquire loads on `probe_arrived` and `probe_finished`
  publish them to core 0.
- SGI generation keeps `DSB ISHST` before `MSR ICC_SGI1R_EL1`, as
  `gic_ipi_send_mask` does in the pinned Linux v6.17 `irq-gic-v3.c`.

## Placement interface

`include/a733_smp_placement.h`, header only. Each call returns an affinity
mask for `xTaskCreateStaticAffinitySet`, or 0 when refused; callers
`configASSERT` it, so a refusal stops the image before the task exists
(section 1.8, class 1). Resolution reads eight words at task creation and is
on no hot path.

- `a733_smp_cluster(A733_SMP_A55 or A733_SMP_A76)`: cores whose `MIDR` part
  number is 0xD05 or 0xD0B.
- `a733_smp_core(n)`: one core, `n` below 8.
- `a733_smp_role(role)`: the table below. With the build option
  `A733_A76_ADMITTED`, which `cmake/a733-os.cmake` already reads and
  `cmake/a733-smp-cached.cmake` will declare, the recognition pair is cores 6
  and 7, and the call refuses unless both report 0xD0B. Without it the pair is
  cores 2 and 3, as in [recognition-dispatch.md](recognition-dispatch.md). The
  definition is added only when the option is on, so default identities stay.

| Role | With `A733_A76_ADMITTED` | Without |
| --- | --- | --- |
| Service: USB0, thermal, DMA, watchdog | 0 | 0 |
| Recognition, GPU worker, GPU interrupt | 6 | 2 |
| Second inference worker, NPU worker, NPU interrupt | 7 | 3 |

| Hardware fact | Source | Status |
| --- | --- | --- |
| `MIDR_EL1` PartNum is bits [15:4]; 0xD05 is Cortex-A55 | Cortex-A55 TRM B2.89 (pinned) | Cited |
| 0xD0B is Cortex-A76 | A76 TRM not pinned; cores 6 and 7 read `0x414fd0b1` on the board ([cpu-start.md](../cpu-start.md)) | Board-observed |
| Cores 0–5 `arm,cortex-a55`, 6–7 `arm,cortex-a76`; `reg` is MPIDR Aff1 | `sun60iw2p1.dtsi`, linux-orangepi `2ac08e8` (pinned) | Cited |
| `TPIDRRO_EL0` is read-only at EL0, writable at EL1 | A55 TRM B2.2 lists it; Arm ARM DDI 0487 not pinned | Unproven |
| `LDAXR`/`STLR` ordering; event on monitor clear | Arm ARM DDI 0487, not pinned | Unproven; second change |
| PMR masking needs no `DSB` without 1-of-N routing | IHI 0069G §2.3, §12.1.6, §12.2.6 (pinned) | Cited; second change |

The implementation pins DDI 0487 and the Cortex-A76 TRM in `sources.lock.json`.

## Reuse and removal

Reused: the PMU region module `platform/a733/cpu_pmu.c` and the build pattern
of `checks/cpu-pmu-smp.py`; `tools/check-smp.py`; the Unicorn model
`checks/a733-smp-cached.py`, which already gives each core its `MIDR`; the irq
table; `a733_cpu_index`. Removed: the observe and supervised kernel copies and
CMake; `SVC 109`; the barriers in `prvGet64` and `prvSet64`; the SGI handler's
lock and masks; the tick handler's outer mask.

## Budgets and measurement

`checks/smp-port-cost.py` (new) builds a two-core image on `platform/qemu-smp`
with the SMP patches, one task at EL0 and one at EL1, and counts
`INST_RETIRED` per operation with the PMU region module under QEMU
`-icount shift=0`, `cortex-a55` and `cortex-a76`. The Unicorn model of the
cached eight-core image counts `SVC`, `DSB`, `DMB` and lock acquisitions per
operation. Both run on the base tree first to set the missing baselines.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Critical-section pair, EL1 task: instructions; `SVC` | Not measured; 4 (counted) | ≤ 100; 0 (section 3) | QEMU |
| Critical-section pair, EL0 task: instructions; `SVC` | Not measured; 4 (counted) | ≤ 180; 2 | QEMU |
| `DSB SY` / `DMB SY` per pair | 26 / 8 (counted) | 0 / 0 after the second change | Unicorn |
| Same-core context switch after a notification, FPU state included | Not measured; 24 `DSB SY` in two lock pairs (counted) | ≤ 800 instructions; 0 `DSB SY` after the second change | QEMU, Unicorn |
| SGI0 handler: `SVC`, `DSB SY`, lock pairs | 2, 21, 1 (counted) | 0, 0, 0 | Unicorn |
| Tick on core 0, nothing woken: `SVC`, `DSB SY` | 2, 21 (counted) | 0, 18; then 0, 0 after the second change | Unicorn |
| Cross-core wake round trip, instructions on both cores | Not measured | ≥ 30% below the base tree | QEMU |
| Port cache lines written by several cores | Five per-core arrays and two lock words share lines | One line per lock, nothing else | ELF symbols |
| Patched kernel sets for cached, observe, supervised | 3 | 1 | Build tree |
| CMake lines in those three files | 325 | ≤ 200 | `wc -l` |
| ELF bytes changed by the consolidation outside `a733_smp_build_id` | — | 0 | Byte comparison |
| Placement resolution | — | ≤ 40 instructions, at task creation only | Host check `checks/smp-placement.c` |

Gates on base and changed trees: `tools/check-smp.py` cases `kernel`,
`context` and `soak8` (ten trials, on `cortex-a55` and, through a new `--cpu`
option, `cortex-a76`); the mimus SMP parity checks; the Unicorn eight-core run,
which also compares each recorded `midr` with the modeled value. The
consolidation lands as its own commit so the byte comparison isolates it. The
port changes then alter every cached-family build identity; the cached,
observe and supervised documents record old and new identities, and board
evidence stays bound to the identity it ran.

## Left out

- Masking from EL0 through `SCTLR_EL1.UMA`: it removes the last two `SVC` per
  pair, but moves masking from GIC priority to `DAIF` and edits reserved
  startup code.
- Lazy FPU switching, unless the context switch exceeds its budget.
- One object set compiled once for every cached-family image: build time
  only, and eight CMake files outside this package filter today's list.
- One launcher for observe and supervised: board tools and evidence name each.
- Migration between clusters and clock-aware placement: no consumer.
- A mixed-cluster QEMU machine: `virt` gives all cores one type; the host check
  and the Unicorn model cover mixed `MIDR` values.
- The `tasks.c` arrays `pxCurrentTCBs` and `xYieldPendings` keep their layout;
  other cores write them only under the kernel locks.

## Board evidence and outside changes

- Before the second change merges: one supervised eight-core session with
  SGI, tick and floating-point progress on every core.
- Tasks on cores 6 and 7 with coherent shared memory, through the native
  mimus image once its owner adopts the roles; A55 against A76 timing only in
  sessions scheduled under [validation.md](../validation.md).
- Adoption belongs to the owners of `apps/mimus` (recognizer and worker, today
  cores 0 and 1), `apps/os` (`OS_RECOGNITION_CORE`), the GPU and NPU workers
  and `apps/board`. Turning on `A733_A76_ADMITTED` (CPU-B clock and rail, A76
  startup controls) belongs to the primary agent. No reserved file changes.
