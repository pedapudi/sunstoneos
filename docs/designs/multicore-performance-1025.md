# Design: lighter SMP lock, interrupt, time and tick paths

Status: **implemented**; measured in emulation and **host-tested**. A first board session on port 3-3 ran cleanly but did not qualify the cycle saving, because its measurement timed lock contention. The revised session of [the multicore performance runbook](../board-runbooks/multicore-performance-session.md) is **open**.

Six changes remove barriers, supervisor calls, locks, SGIs and interrupts that protect no shared state, in the FreeRTOS SMP port, the SMP newlib runtime, the platform counter clock and the cached eight-core platform. Each follows the reference implementation named in its section. Fair spinlocks beyond the ticket lock, cache-line relayout, a different `-march` and an IRQ entry without the floating-point save need a supervised board session and are not part of this design.

The port sources are `patches/freertos-smp-c.patch`, `patches/freertos-smp-asm.patch` and `patches/freertos-smp-tasks.patch`, which `cmake/a733-smp-cached.cmake` applies to the pinned FreeRTOS-Kernel `97f50c0e`; `cmake/smp.cmake` applies the first two for the QEMU targets. Reference line numbers, derivations and the full result tables are in `docs/evidence/multicore-performance-1025.jsonl` (record `multicore-performance-1025-design-derivations`).

## Measurement

- [checks/smp-port-cost.py](../../checks/smp-port-cost.py) builds a two-core QEMU image and counts `INST_RETIRED` under `-icount shift=0`, on `cortex-a55` and `cortex-a76` cores: critical-section pairs, notifications, the same-core switch, cross-core round trips, an SGI to the sending core, one counter clock read, the tick wakes of an unpinned task (also in an image whose two cores have different types) and `__getreent` in an image built with the SMP newlib runtime.
- [checks/a733-smp-cached.py](../../checks/a733-smp-cached.py) `--count` runs the cached eight-core diagnostic in the instruction model and gives the median instructions, `SVC`, `DSB`, `DMB` and `ISB` per call and per IRQ. `--late-ticks 5` puts the counter five periods past the pending tick deadline while core 0 has no IRQ in progress. The model treats the timer PPI as level-sensitive, as the GIC does, and reports `CNTP_CTL_EL0.ISTATUS`.

QEMU on an x86-64 host and the instruction model execute the real instructions but do not reproduce the Arm memory model, so neither can show a missing or misplaced barrier. The ordering arguments below rest on reasoning until a board session tests them.

## Lock bookkeeping without `DSB`

`vPortRecursiveLock` reads and writes two kinds of bookkeeping word through `prvGet64` and `prvSet64`: a lock's recursion count, which only the lock's holder accesses, and a core's ownership bits, which only that core accesses. Both are plain `volatile` accesses, so 22 of the 24 `DSB SY` of a critical-section pair go. A core observes its own accesses in program order. Between holders, the releasing core writes the recursion count before its release store, and the next holder reads it after its acquire; a Linux arm64 spinlock orders its holders' data the same way (`queued_spin_lock`, `queued_spin_unlock`). The kernel locks are FIFO ticket locks with acquire and release ordering ([smp-heterogeneous.md](smp-heterogeneous.md), item 6).

## newlib state from the task register

`__getreent` (`runtime/newlib_smp.c`) returns the reentrancy structure inside the current task's control block, which bits [47:0] of `TPIDRRO_EL0` hold once a core has resumed a task (`xTaskGetCurrentTaskHandle` reads it with one `MRS`; see [smp.md](../smp.md#port-operations-avoid-supervisor-calls-and-shared-lines)). A task's own block does not change when it migrates, so the read needs no mask; ESP-IDF returns the current task's block without a lock for the same reason. The structure's offset comes from `StaticTask_t`, and `platform/qemu-smp/freertos_tasks_c_additions.h` asserts that it equals the control block's. While the register's task field is zero, `__getreent` takes supervisor call 118, which returns the global state before the scheduler starts.

## Counter clock without a lock

`a733_counter_clock_now` (`platform/a733/counter_clock.c`) reads the counter after `ISB`, as Linux `__arch_counter_get_cntpct` does, with no lock and no mask. The shared history only rises: a caller raises it to its own sample with a compare-and-swap, or reports the history when its sample is not above it. A failed swap reloads the history and samples again, so a stalled caller cannot lower the history or delay another caller. The history word keeps its own cache line, apart from the fault flag that every caller reads.

Two cores read `CNTPCT_EL0` at slightly different times, so a sample taken after another core's can be a tick lower. The provider absorbs any backward step of up to 100 microseconds, whatever its cause, and reports the history instead; only a larger step faults it. A real counter regression smaller than that bound is not detected, and the bound must stay far above the skew between the A55 and A76 counter reads. The history uses relaxed atomics: every caller's result lies between the history it loaded and the history after its swap, and coherence orders all accesses to that one word, so a call that happens after another returns no earlier time. That argument covers a single location and needs no barrier; a board session must still confirm it on the A55 and A76 cores.

## IRQ exit barriers

`FreeRTOS_IRQ_Handler` in `portASM.S` omits three barriers:

- The `ISB` after `MSR DAIFSET, #2`: a `DAIFSet` write masks IRQs from the next instruction, as Linux `kernel_exit` and `local_irq_disable` rely on. The mask is needed because the FromISR critical section of the tick handler ends with `MSR DAIFCLR`.
- The `DSB SY` and `ISB` before `ERET` on the exit that returns to the interrupted task. `ERET` uses the `SPSR_EL1` and `ELR_EL1` just written and is a context synchronization event, which also completes the EOI. Linux `kernel_exit` and TF-A `el3_exit` return with no barrier between.
- The `DSB SY` and `ISB` on the exit that switches context. `portSAVE_CONTEXT` reads `SPSR_EL1` and `ELR_EL1` back with `MRS`, which observes the writes in program order, and another core that resumes the saved task is ordered by the kernel locks that `vTaskSwitchContext` takes. `portRESTORE_CONTEXT` keeps its own `DSB SY` and `ISB` after the PMR write.

One `DSB SY` stays before `MSR ICC_EOIR1_EL1`, so a handler's device write that clears a level interrupt completes before the EOI; no reference shows it unneeded with these device drivers. No `ISB` follows the EOI because the next dependent instruction is the `ERET`, or the `ISB` after the PMR write on the switching exit. The SGI handler's per-core count (`platform/a733-smp-cached/platform.c`) is a relaxed load and store, because only its own core writes it.

## Wake placement

`prvYieldForTask` chooses the core running the lowest priority below the woken task's. An idle calling core, once chosen, keeps a tie against another idle core of its own type: its yield needs no SGI, and the task runs where its waker's data is cached. An idle core of another type still takes the tie, so an idle Cortex-A76 wins over an idle Cortex-A55. Ties between busy cores, and ties that the calling core does not hold, go to the last such core. Linux `select_idle_sibling` returns the target CPU when it is idle and its capacity fits.

`configCORES_SHARE_TYPE(a, b)` names the cores of one type and defaults to all cores alike. `platform/a733-smp-cached/FreeRTOSConfig.h` defines it from `A733_SMP_A55_MASK` in `include/a733_smp_placement.h`, which every cached image includes; the cached and storage build identities hash that header. A task that may run on every core and is woken by the tick on core 0 still goes to idle core 7. The rule changes placement for tasks confined to one cluster, such as the OS image's tasks without an explicit placement, which stay on the A55 cores.

## Late ticks in up to three calls per IRQ

The tick acknowledgement advances the compare value by one period, so a tick IRQ taken N periods late would leave the compare value in the past and the timer would fire N more times in a row. The tick IRQ in `platform/a733-smp-cached/platform.c` therefore:

- reads `CNTP_CTL_EL0.ISTATUS` and processes no tick when the timer condition no longer holds, as Linux's timer handler acts only on `ISTATUS`;
- after the first tick handler call reads `ISTATUS` again; when the condition still holds, the IRQ was late, and it makes at most three tick handler calls in all (`TICK_CALLS_PER_IRQ`), as `hrtimer_interrupt` retries at most three times, with one comparison of the counter against the next deadline plus a period deciding whether a third call is due;
- advances the compare value by one period per call, so the next deadline stays in phase and the kernel's tick count equals the elapsed periods;
- masks IRQs again before each further call, because the tick handler's FromISR critical section clears `PSTATE.I`. The level-sensitive timer interrupt is taken again for any period still due.

## Emulated figures

Record: `docs/evidence/multicore-performance-1025.jsonl`, `multicore-performance-1025-emulation-results`. QEMU, `cortex-a55` cores.

| Operation | Base | After all six changes |
| --- | --- | --- |
| Critical-section pair, EL1 task, instructions | 296 | 274 |
| Critical-section pair, EL0 task, instructions | 368 | 346 |
| Same-core switch to a woken task, instructions | 1,320 | 1,255 |
| Cross-core round trip, EL1 tasks, both cores | 11,976 | 11,404 |
| `__getreent`, EL1 or EL0 task | 96 | 7 |
| Uncontended counter clock read | 69 | 62 |
| Tick wakes of an unpinned task on the tick's core, of 64 | 0 | 64 |
| Six due tick periods in the model: tick IRQs; instructions | 6; 5,654 | 2; 1,848 |

The soak, runtime, kernel and context cases passed ten of ten trials on `cortex-a55` and `cortex-a76` cores, and the contention check passed under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer (record `multicore-performance-1025-correctness-checks`). Eighteen built images change identity (record `multicore-performance-1025-image-identities`), so board evidence stays bound to the identity it ran.

## Limitations and open work

- The first board session (record `multicore-performance-1025-session-1`) completed all six runs with 32 rounds and progress on every core and a late tick that behaved as modeled, but its cycle criterion failed: the eight measurement tasks timed their pairs at the same time and measured lock contention.
- Open: the revised session, which times one core at a time while the others wait in `WFE`, takes 32 samples of 64 pairs per core with a 95% confidence interval, ends the SGI timing when the core's handler has counted the SGI, and measures SGI, tick and floating-point progress on all eight cores, the skew between A55 and A76 counter reads and a forced late tick on core 0 (record `multicore-performance-1025-session-revised`). `SUNSTONE_A733_PERF_SESSION` adds the measurements to the cached images (`platform/a733-smp-cached/perf_session.c`).
- The board session does not measure wake placement, which acts only between idle cores; its evidence is the QEMU and model figures.
