# Design: one cached SMP base, lighter scheduler locks and A55/A76 task placement

Status: the consolidation and the first port change are **implemented and
measured in emulation**; [docs/smp.md](../smp.md#port-operations-avoid-supervisor-calls-and-shared-lines)
records the results. The second port change waits for the board session listed
below, and the placement header waits for its `apps/os` consumer. Section
numbers refer to [performance-architecture.md](../performance-architecture.md).
Figures marked *counted* come from reading the pinned FreeRTOS port with the
project patches: uncontended, `configASSERT` enabled, critical nesting from 0
to 1 and back.

The implementation differs from this design in three places:

- The entry code clears `TPIDRRO_EL0` instead of writing the core index and
  bit 56. While the TCB field is zero, `ucPortGetCoreID` also takes the
  supervisor call, so QEMU, the instruction models and every check that
  starts an image part-way keep working without an entry write.
- The eight-core diagnostic clears the register in `a733_smp_pre_scheduler`,
  which this package owns, instead of in `platform/a733/cpu_continue.S`.
  `platform/qemu-smp/start.S` and `checks/cached-device-reservations.S` need
  no change. The only edit outside the package is the storage-maintenance pin
  of the cached `continue.S` hash.
- Measured in QEMU, the EL1 critical-section pair retires 296 instructions and
  the same-core switch 1,320, above the 100 and 800 targets below; the four
  lock calls account for most of the pair.

The observe and supervised images build from the cached diagnostic's source
list. The SMP port drops supervisor calls, locks and barriers that protect no
shared data. Compile-time constants name the A55 cluster (cores 0–5), the A76
pair (cores 6 and 7) and the core of each role.

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
- **Core type.** Admission verifies it once: `valid_report()` in
  `platform/a733/cpu_entry.c` and `a733_cpu_coherence_plan()` in
  `platform/a733/cpu_coherence.c` refuse any started core whose `MIDR_EL1` is
  not Cortex-A55 (part 0xD05) for cores 0–5 or Cortex-A76 (part 0xD0B) for
  cores 6 and 7. Under the scheduler the type is a fixed function of the core
  index, so nothing reads `MIDR` again.

## Port changes

The first change removes instructions and keeps every barrier on a path that
still touches shared data; the emulator checks qualify it.

1. **Core index and current task in `TPIDRRO_EL0`.** Bits [47:0] hold the
   current TCB address, bits [55:48] the core index, and bit 56 is set when
   the task runs at EL1. The register is UNKNOWN at reset, so each core's entry
   code writes `core << 48 | 1 << 56` beside `msr tpidr_el1`, before the first
   kernel call: `platform/a733-smp-cached/continue.S`,
   `platform/a733/cpu_continue.S` for the `a733-smp` image, and
   `platform/qemu-smp/start.S`. The context-restore path writes all three
   fields for the task it resumes. `prvSelectHighestPriorityTask`, the only
   writer of `pxCurrentTCBs[core]` once the scheduler runs, also writes the TCB
   field on its own core.
   - `ucPortGetCoreID` stays an out-of-line function; its body becomes `MRS`,
     `UBFX`, `RET`, replacing `SVC 109`.
   - `xTaskGetCurrentTaskHandle`, which `tasks.c` calls for every use of
     `pxCurrentTCB`, returns the TCB field with one `MRS` when the field is
     non-zero. A task's own TCB does not change when it migrates, so the read
     needs no mask. While the field is still zero, before the core has
     resumed a task, the function keeps the masked array read.
   - An EL0 task can read the register; it holds that task's own handle and
     core, which the API already returns.
2. **EL1 tasks mask directly.** `portDISABLE_INTERRUPTS`,
   `portENABLE_INTERRUPTS`, `uxPortSetInterruptMask` and
   `vPortClearInterruptMask` test bit 56 and, when it is set, run in line the
   PMR write and barriers of `SVC 107`, `108`, `110`, `111` and `112`. The
   inline sequence never writes `DAIF`, so the caller's `DAIF.I` is kept, as
   the SVC path keeps it by returning the caller's `SPSR`. Pre-scheduler code
   runs with `DAIF` = 0x3c0 and an ISR runs with `I` set; the upstream FromISR
   bracket, which ends with `MSR DAIFCLR, #2`, would unmask both. EL0 tasks
   keep the SVCs. An EL0 task that reaches the EL1 sequence traps on
   `MSR ICC_PMR_EL1` into the existing fault path.
3. **SGI handler without a critical section.** `FreeRTOS_SGI_Handler` only sets
   this core's yield flag, with `DAIF.I` still set from exception entry; no
   other core writes the flag. Its PMR mask pair and ISR lock go;
   `vTaskSwitchContext` still takes both locks at IRQ exit.
4. **One mask in the tick handler.** `portENTER_CRITICAL_FROM_ISR` already
   masks, so the outer `portSET_INTERRUPT_MASK_FROM_ISR` pair goes.
5. **Per-core cache lines.** `ullPortTaskHasFPUContext` and `ullOwnedByCore`
   become one 64-byte-aligned structure per core, indexed with `LSL #6`. Each
   lock word and its recursion count get their own line (section 2.6).
   `ullCriticalNestings`, `ullPortInterruptNestings` and `ullPortYieldRequired`
   keep their symbols and 8-byte stride, because code outside this package
   reads or writes them by name (listed under outside changes). The cost: a
   critical-section or interrupt entry or exit that writes a nesting word
   first takes ownership of the line if another core wrote it since. QEMU does
   not model that transfer, so only the supervised board session shows it.

The second change weakens barriers and merges only after board qualification
([shared-helpers.md](../shared-helpers.md)):

6. **Acquire and release locks.** Take with `LDAXR`/`STXR`, release with
   `STLR`. Waiters loop on `WFE` and `LDAXR` after `SEVL`; the releasing store
   clears their exclusive monitors, which sets the event register. Only the
   holder touches the recursion count and only its core touches its ownership
   bits, so `prvGet64` and `prvSet64` drop `DSB SY`. The `DMB SY` after
   acquire and the `DSB SY`, `SEV` and `ISB` in release go.
7. **PMR writes without `DSB` or `DAIF` bracket.** Masking is one
   `MSR ICC_PMR_EL1`: IHI 0069G §12.1.6 makes PMR writes self-synchronizing,
   so no interrupt below the new mask is taken once the write executes.
   Unmasking is `MSR` then one `ISB`. §12.1.6 covers only masking, and the
   `ISB` makes an interrupt held pending by the old mask arrive at the unmask
   point instead of at a later instruction. The SVC handlers and FromISR
   functions drop `MSR DAIFSET` and `DAIFCLR`: taking the SVC or IRQ already
   set `PSTATE.I`, and the bracket served memory-mapped `GICC_PMR` on GICv2.
   `DSB SY` goes because the Redistributor observes PMR only when
   `ICC_CTLR_ELx.PMHE` = 1 (§12.1.6), and `continue.S` line 77 writes
   `ICC_CTLR_EL3` = 0. An implementation may make PMHE RAO/WI (§12.2.6), so
   `continue.S` reads `ICC_CTLR_EL3` back and faults if bit 6 is set.
   `Interrupt_Routing_Mode` 0 in every `GICD_IROUTER` write of
   `platform/a733-smp-cached/irq_table.c` stays as a second guard.

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
- Each core's `TPIDRRO_EL0` is written only by that core at EL1.
- SGI generation keeps `DSB ISHST` before `MSR ICC_SGI1R_EL1`, as
  `gic_ipi_send_mask` does in the pinned Linux v6.17 `irq-gic-v3.c`.

## Placement constants

`include/a733_smp_placement.h` holds preprocessor constants only, for
`xTaskCreateStaticAffinitySet`. `A733_SMP_A55_MASK` is 0x3F.
`A733_SMP_A76_MASK` (0xC0) is defined only under the compile definition
`A733_A76_ADMITTED`, so a use without admission fails at build time. The
service core is 0; the recognition and inference cores are 6 and 7 with
admission, and 2 and 3 without, matching `cmake/a733-os.cmake` and
[recognition-dispatch.md](../recognition-dispatch.md). A `_Static_assert`
keeps every mask inside `(1 << configNUMBER_OF_CORES) - 1`.

The header lands in the same change as its first consumer: in `apps/os`,
`_Static_assert(OS_RECOGNITION_CORE == A733_SMP_RECOGNITION_CORE)`, with
`cmake/a733-os.cmake` passing `A733_A76_ADMITTED` to the compiler. The CMake
role mapping stays the single table, and the assertion stops the header from
drifting from it. Until the `apps/os` owner takes that edit, the header is not
added.

| Hardware fact | Source | Status |
| --- | --- | --- |
| Cores 0–5 `arm,cortex-a55`, 6–7 `arm,cortex-a76` | `sun60iw2p1.dtsi`, linux-orangepi `2ac08e8` (pinned); MIDR check at admission | Cited; checked on every start |
| `TPIDRRO_EL0` is read-only at EL0, writable at EL1, UNKNOWN at reset | A55 TRM B2.2 lists it; Arm ARM DDI 0487 not pinned | Unproven; entry code writes it |
| `LDAXR`/`STLR` ordering; event on monitor clear | Arm ARM DDI 0487, not pinned | Unproven; second change |
| PMR writes self-synchronizing; Redistributor sees PMR only with PMHE = 1; PMHE may be RAO/WI | IHI 0069G §12.1.6, §12.2.6 (pinned) | Cited; second change, PMHE read back |

The implementation pins DDI 0487 and the Cortex-A76 TRM in `sources.lock.json`.

## Reuse and removal

Reused: the PMU region module `platform/a733/cpu_pmu.c` and the build pattern
of `checks/cpu-pmu-smp.py`; `tools/check-smp.py`; the Unicorn model
`checks/a733-smp-cached.py`; the irq table; the admission MIDR check; the role
mapping in `cmake/a733-os.cmake`. Removed: the observe and supervised kernel
copies and CMake, and the supervisor calls, locks and barriers that the port
changes list.

## Budgets and measurement

`checks/smp-port-cost.py` (new) builds a two-core image on `platform/qemu-smp`
with the SMP patches, one task at EL0 and one at EL1, and counts
`INST_RETIRED` per operation with the PMU region module under QEMU
`-icount shift=0`, `cortex-a55` and `cortex-a76`. The Unicorn model of the
cached eight-core image counts `SVC`, `DSB`, `DMB`, `ISB` and lock acquisitions
per operation. Both run on the base tree first to set the missing baselines.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Critical-section pair, EL1 task: instructions; `SVC` | Not measured; 4 (counted) | ≤ 100; 0 (section 3) | QEMU |
| Critical-section pair, EL0 task: instructions; `SVC` | Not measured; 4 (counted) | ≤ 180; 2 | QEMU |
| `DSB SY` / `DMB SY` / `ISB` per pair | 26 / 8 / not counted | 0 / 0 / 1 after the second change | Unicorn |
| One `pxCurrentTCB` read from a task: `SVC`, `DSB SY`, `ISB` | 3, 2, 2 in a critical section; 3, 4, 4 outside (counted) | 0, 0, 0 | Unicorn |
| Notify give, then take with the count set, EL1 task: `SVC`, `DSB SY`, `ISB` | Take alone 18 to 21 `SVC` from six or seven `pxCurrentTCB` reads (counted); rest not measured | 0, 0, one per critical-section pair after the second change | Unicorn |
| Same-core context switch after a notification, FPU state included | Not measured; 24 `DSB SY` in two lock pairs (counted) | ≤ 800 instructions; 0 `DSB SY` after the second change | QEMU, Unicorn |
| SGI0 handler: `SVC`, `DSB SY`, `ISB`, lock pairs | 2, 21, not counted, 1 (counted) | 0, 0, 0, 0 | Unicorn |
| Tick on core 0, nothing woken: `SVC`, `DSB SY` | 2, 21 (counted) | 0, 18; then 0, 0 after the second change | Unicorn |
| Cross-core wake round trip, instructions on both cores | Not measured | ≥ 30% below the base tree | QEMU |
| Port cache lines written by several cores | Five per-core arrays and two lock words share lines | One line per lock; FPU flags and ownership bits per core; nesting and yield arrays unchanged | ELF symbols |
| Patched kernel sets for cached, observe, supervised | 3 | 1 | Build tree |
| CMake lines in those three files | 325 | ≤ 200 | `wc -l` |
| ELF bytes changed by the consolidation outside `a733_smp_build_id` | — | 0 | Byte comparison |

Gates on base and changed trees: `tools/check-smp.py` cases `kernel`,
`context` and `soak8` (ten trials, on `cortex-a55` and, through a new `--cpu`
option, `cortex-a76`); the mimus SMP parity checks; the Unicorn eight-core
run. QEMU and Unicorn reset `TPIDRRO_EL0` to 0, so the Unicorn model seeds it
on every core with a value whose core field is 0xFF; a missing entry write
then fails. The model also fails on any write that sets PMHE in `ICC_CTLR_EL1`
or `ICC_CTLR_EL3`, and on any `GICD_IROUTER` write with bit 31 set. The
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
- The `tasks.c` arrays `pxCurrentTCBs` and `xYieldPendings` keep their layout;
  other cores write them only under the kernel locks.

## Board evidence and outside changes

- Before the second change merges: one supervised eight-core session with
  SGI, tick and floating-point progress on every core.
- Tasks on cores 6 and 7 with coherent shared memory, through the native
  mimus image once its owner adopts the roles; A55 against A76 timing only in
  sessions scheduled under [validation.md](../validation.md).
- The implementation delivers one `primary.patch` with the `TPIDRRO_EL0` entry
  write in the reserved `platform/a733/cpu_continue.S` and in
  `platform/qemu-smp/start.S`, and the same write with bit 56 clear in
  `checks/cached-device-reservations.S`. That harness emulates `SVC 107` and
  `108`, so the clear bit keeps the kernel on them; its `SVC 109` branch
  becomes unused.
- Readers by name that need no edit, because the symbols and 8-byte stride
  stay: `include/a733_task_context.h` and
  `drivers/storage/a733_mmc_platform_native.c` read both nesting arrays;
  `checks/a733-hdmi-native.c`, `hdmi-framebuffer.c` and
  `audit-security-watchdog-rtc.py` define them as stubs; `lradc-native.py`,
  `displayport-composed.py` and `npu-supervised.py` read and write them;
  `primary-cache-probe.py` and `tools/prepare-primary-cache-probe.py` alias
  `ullPortInterruptNestings` and write `ullPortYieldRequired`.
- `checks/mimus-supervised-native.py` and `usb-host-irq-native.py` hook
  `ucPortGetCoreID`. It stays a function, and with the TCB field at the
  emulator's reset value 0, `xTaskGetCurrentTaskHandle` still calls it, so
  both hooks keep intercepting every core-index read.
- Adoption belongs to the owners of `apps/mimus` (recognizer and worker, today
  cores 0 and 1), `apps/os` (the assertion and compile definition above), the
  GPU and NPU workers and `apps/board`. Turning on `A733_A76_ADMITTED` (CPU-B
  clock and rail, A76 startup controls) belongs to the primary agent.
