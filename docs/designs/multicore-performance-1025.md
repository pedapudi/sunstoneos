# Design: lighter SMP lock, interrupt, time and tick paths

Status: **implemented and measured in emulation**. Six changes remove
barriers, supervisor calls, locks, SGIs and interrupts that protect no shared
state, in the FreeRTOS SMP port, the SMP newlib runtime, the platform counter
clock and the cached eight-core platform. Each follows the reference
implementation named in its section. The newlib lookup and the counter clock
are on `work/mc-perf-a-1025` and can merge on their own. The lock
bookkeeping, IRQ exit, wake placement and late-tick changes build on that
branch and merge only after the board session in
[the multicore performance runbook](../board-runbooks/multicore-performance-session.md).
Fair spinlocks, cache-line relayout, a different `-march` and an IRQ entry
without the floating-point save also need a supervised board session and are
not part of this design.

The port sources are the patches `patches/freertos-smp-c.patch`,
`patches/freertos-smp-asm.patch` and `patches/freertos-smp-tasks.patch`, which
`cmake/a733-smp-cached.cmake` applies to the pinned FreeRTOS-Kernel
`97f50c0e`; `cmake/smp.cmake` applies the first two for the QEMU targets.
Line numbers below refer to the patched files.

## Measurement

Three tools measure the base tree and each change:

- [checks/smp-port-cost.py](../../checks/smp-port-cost.py) builds a two-core
  QEMU image and counts `INST_RETIRED` under `-icount shift=0`, where the count
  is exact, on `cortex-a55` and `cortex-a76` cores. Each figure is the least
  of 16 samples minus an empty region, or a mean over 256 round trips or 64
  wakes. The check now also counts:
  - an SGI that an EL1 task sends to its own core, from the send to the
    task's next instruction;
  - one uncontended counter clock read;
  - an unpinned task that each tick wakes while both cores are idle, also in
    an image whose two cores have different types;
  - `__getreent`, in a second image built with the SMP newlib runtime.
- [checks/a733-smp-cached.py](../../checks/a733-smp-cached.py) `--count` runs
  the cached eight-core diagnostic in the instruction model and gives the
  median instructions, `SVC`, `DSB`, `DMB` and `ISB` per call and per IRQ,
  from vector entry to exception return. `--late-ticks 5` puts the counter
  five periods past the pending tick deadline while core 0 has no IRQ in
  progress. The model now treats the timer PPI as level-sensitive, as the GIC
  does, and reports `CNTP_CTL_EL0.ISTATUS`; it used to deliver a tick IRQ again
  after its EOI when the handler spanned a scheduling quantum.
- [checks/clock-counter-contention.c](../../checks/clock-counter-contention.c)
  runs the counter clock on 16 host threads, 200,000 calls each, with
  modeled counter reads.

QEMU on an x86-64 host and the instruction model execute the real
instructions, but neither reproduces the Arm memory model. QEMU runs on a
host whose stores are seen in order, and the model interleaves the cores one
quantum at a time over shared host memory. Neither can show a missing or
misplaced barrier, so the ordering argument for the counter clock below rests
on reasoning until a board session tests it. The same holds for the lock
bookkeeping and the IRQ exit barriers: the emulators show which instructions
go, and only the board session can show that the remaining barriers order
what the reasoning below says they order.

## Results

Each column gives the value after that change and all changes to its left.
QEMU figures are from `cortex-a55` cores; `cortex-a76` cores give the same
figures except where noted.

| Metric | Base | newlib state | Counter clock | Lock bookkeeping | IRQ exit | Wake placement | Late ticks |
| --- | --- | --- | --- | --- | --- | --- | --- |
| QEMU: critical-section pair, EL1 task | 296 | 296 | 296 | 274 | 274 | 274 | 274 |
| QEMU: critical-section pair, EL0 task | 368 | 368 | 368 | 346 | 346 | 346 | 346 |
| QEMU: notification given and taken | 656 | 656 | 656 | 612 | 612 | 612 | 612 |
| QEMU: same-core switch to a woken task | 1,320 | 1,320 | 1,320 | 1,254 | 1,254 | 1,255 | 1,255 |
| QEMU: cross-core round trip, EL1 tasks, both cores | 11,976 | 11,976 | 11,976 | 11,412 | 11,394 | 11,404 | 11,404 |
| QEMU: cross-core round trip, EL0 tasks, both cores (note 1) | 14,148 | 14,169 | 14,162 | 13,569 | 13,536 | 13,540 | 13,540 |
| QEMU: SGI to the sending core, IRQ round trip | 683 | 683 | 683 | 661 | 658 | 658 | 658 |
| QEMU: `__getreent`, EL1 task and EL0 task | 96, 96 | 7, 7 | 7, 7 | 7, 7 | 7, 7 | 7, 7 | 7, 7 |
| QEMU: uncontended counter clock read | 69 | 69 | 62 | 62 | 62 | 62 | 62 |
| QEMU: tick wakes of an unpinned task on the tick's core, of 64 | 0 | 0 | 0 | 0 | 0 | 64 | 64 |
| QEMU: the same when the two cores have different types, of 64 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| QEMU: instructions per such wake, both cores (note 1) | 5,308 | 5,308 | 5,308 | 5,101 | 5,090 | 4,733 | 4,733 |
| Model: SGI IRQ that switches context: instructions; `DSB`, `ISB` | 904; 29, 7 | 904; 29, 7 | 904; 29, 7 | 880; 6, 7 | 875; 5, 5 | 879; 5, 5 | 881; 5, 5 |
| Model: tick IRQ that returns without a switch: instructions; `DSB`, `ISB` | 465; 21, 12 | 465; 21, 12 | 465; 21, 12 | 454; 10, 12 | 451; 9, 10 | 451; 9, 10 | 456; 9, 10 |
| Model: `vTaskExitCritical` from an EL0 task: instructions; `SVC`, `DSB` | 260; 1, 18 | 260; 1, 18 | 260; 1, 18 | 246; 1, 4 | 246; 1, 4 | 246; 1, 4 | 246; 1, 4 |
| Model: SGIs sent by ticks that woke tasks, over the run (note 2) | 138 | 138 | 138 | 141 | 149 | 157 | 157 |
| Model: six due tick periods: tick IRQs; tick handler calls; instructions (note 3) | 6; 6; 5,654 | 6; 6; 5,654 | 6; 6; 5,654 | 6; 6; 3,837 | 6; 6; 2,706 | 6; 6; 2,706 | 2; 6; 1,848 |
| Host: counters up to 21 ticks apart on 16 threads | Faults | Faults | No fault | No fault | No fault | No fault | No fault |
| Host: one caller stopped after its counter sample | Faults | Faults | No fault | No fault | No fault | No fault | No fault |

Notes:

1. These two figures vary by up to about 30 instructions between runs whose
   measured path is unchanged, because a different instruction count earlier
   in the run changes how QEMU interleaves the two cores' lock waits. On
   `cortex-a76` cores the EL0 round trip reads 14,162 at the base and 13,561
   after the last change.
2. Every task of the cached diagnostic is pinned to one core, so wake
   placement cannot change which core it signals. The SGI total varies with
   the order in which the model interleaves the cores.
3. The instruction total depends on which of the tick IRQs switch context,
   which varies between runs.

The QEMU EL1 critical-section pair falls by 22 instructions, the 22 `DSB SY`
that the lock bookkeeping change removes. The SGI to the sending core loses
the 22 `DSB SY` of its context switch's two lock pairs and then the three
barriers of the IRQ exit. The late-tick guard adds 5 instructions to an
on-time tick IRQ: it reads `CNTP_CTL_EL0` and tests `ISTATUS` before and after
the tick handler call.

## Lock bookkeeping without `DSB`

`vPortRecursiveLock` reads and writes two kinds of bookkeeping word through
`prvGet64` and `prvSet64` (`port.c` 1696–1715): a lock's recursion count, which
only the lock's holder accesses, and a core's ownership bits, which only that
core accesses. Each wrapped its access in `DSB SY`, which made a
critical-section pair execute 24 `DSB SY`. The wrappers are now plain
`volatile` accesses, and 22 of the 24 go.

A core observes its own accesses in program order, so neither word needs a
barrier on its own core. Between holders, the releasing core writes the
recursion count before the `DMB SY` and the gate store in `prvSpinUnlock`, and
the next holder reads it after the `DMB SY` that follows a successful
`prvSpinTrylock`. A Linux arm64 spinlock orders its holders' data the same
way, with the acquiring compare-and-swap of `queued_spin_lock` and the release
store of `queued_spin_unlock` (Linux v6.17 `include/asm-generic/qspinlock.h`
107–129). The message-passing tests of the Arm Architecture Reference Manual
(DDI 0487, appendix K11, which this project has not cached) require no more.
The two `DSB SY` that remain are in `prvSpinUnlock`, one per lock release,
before the `SEV` that wakes a waiter in `WFE`.

## newlib state from the task register

`__getreent` (`runtime/newlib_smp.c`) took supervisor call 118, which masks
IRQs and indexes `pxCurrentTCBs` by core. Once a core has resumed a task,
bits [47:0] of `TPIDRRO_EL0` already hold that task's control block, which EL0
can read, and `xTaskGetCurrentTaskHandle` reads it with one `MRS`
([docs/smp.md](../smp.md#port-operations-avoid-supervisor-calls-and-shared-lines)).
`__getreent` now returns the reentrancy structure inside that block. A task's
own block does not change when it migrates, so the read needs no mask; ESP-IDF
v5.3.1 returns the current task's block without a lock for the same reason
(`components/freertos/esp_additions/freertos_tasks_c_additions.h` 849–867).
The structure's offset comes from `StaticTask_t`, and
`platform/qemu-smp/freertos_tasks_c_additions.h` asserts that it equals the
control block's. While the register's task field is zero, before the core has
resumed a task, `__getreent` keeps supervisor call 118, which returns the
global state before the scheduler starts.

## Counter clock without a lock

`a733_counter_clock_now` (`platform/a733/counter_clock.c`) serialized every
caller behind a ticket lock with interrupts masked and faulted the provider
when a sample fell below the last one. Two cores read `CNTPCT_EL0` at slightly
different times, so a sample taken after another core's can still be a tick
lower; serialized behind that core, it faulted the provider. A caller stalled
while holding its ticket also faulted every waiter after 100 microseconds.

Each call now reads the counter after `ISB`, as Linux
`__arch_counter_get_cntpct` does (`arch/arm64/include/asm/arch_timer.h`
179–189), with no lock and no mask. The shared history only rises: a caller
raises it to its own sample with a compare-and-swap, or reports the history
when its sample is not above it. A failed swap reloads the history and samples
again, so a stalled caller cannot lower the history or delay another caller.
Each sample is taken after the history it is compared with was loaded, so a
lower sample shows skew between cores. The history word keeps its own cache
line, apart from the fault flag that every caller reads.

The provider now absorbs any backward step of up to 100 microseconds, whatever
its cause, and reports the history instead; only a larger step faults it. A
real counter regression smaller than that is no longer detected. The bound
must stay far above the measured skew between the A55 and A76 cores'
counter reads, which no board session has measured yet.

The history uses relaxed atomics. Every caller's result lies between the
history it loaded and the history after its swap, and coherence orders all
accesses to that one word, so a call that happens after another returns no
earlier time. That argument covers a single location only and needs no
barrier; the board session must still confirm it on the A55 and A76 cores.
The contention check adds the skew, stalled-caller and skew-bound cases; the
base provider faults in the first two.

## IRQ exit barriers

The IRQ exit (`portASM.S`, `FreeRTOS_IRQ_Handler`) executed `DSB SY` and `ISB`
after `MSR DAIFSET`, and `DSB SY` and `ISB` after writing `SPSR_EL1` and
`ELR_EL1` on both of its exits. Three barriers go:

- The `ISB` after `MSR DAIFSET, #2`. A `DAIFSet` write masks IRQs from the
  next instruction: Linux `kernel_exit` masks with `disable_daif`
  (`arch/arm64/kernel/entry.S` 335–337; `arch/arm64/include/asm/assembler.h`
  37–39) and `local_irq_disable` with `msr daifset, #3`
  (`arch/arm64/include/asm/irqflags.h` 52–57), both without `ISB`. The mask
  is needed because the FromISR critical section of the tick handler ends with
  `MSR DAIFCLR`.
- The `DSB SY` and `ISB` before `ERET` on the exit that returns to the
  interrupted task. `ERET` uses the `SPSR_EL1` and `ELR_EL1` just written and
  is a context synchronization event, which also completes the EOI. Linux
  `kernel_exit` writes `ELR_EL1` and `SPSR_EL1` and returns with no barrier
  between (`entry.S` 412–461); its `dmb sy` before `eret` is the Cortex-A77
  erratum 1508412 workaround, which neither A733 core needs. TF-A `el3_exit`
  does the same at EL3 (`lib/el3_runtime/aarch64/context.S` 656–681, TF-A
  `0892ad4d`).
- The `DSB SY` and `ISB` on the exit that switches context. `portSAVE_CONTEXT`
  reads `SPSR_EL1` and `ELR_EL1` back with `MRS`, which observes the writes in
  program order, and another core that resumes the saved task is ordered by
  the kernel locks that `vTaskSwitchContext` takes. `portRESTORE_CONTEXT`
  keeps its own `DSB SY` and `ISB` after the PMR write.

One `DSB SY` stays before `MSR ICC_EOIR1_EL1`, so a handler's device write
that clears a level interrupt completes before the EOI. Linux's
`gic_eoi_irq` has no such `DSB` (`drivers/irqchip/irq-gic-v3.c` 719–722); it
stays because no reference shows it unneeded with these device drivers.
Linux follows the EOI write with `isb()` (line 722) because the kernel keeps
running after it and must see the priority drop before it unmasks. This exit
needs no `ISB` there: the next instruction that depends on the EOI is the
`ERET`, which is itself a context synchronization event, and on the switching
exit the `ISB` after the PMR write in `portRESTORE_CONTEXT` precedes it too.

The SGI handler's per-core count (`platform/a733-smp-cached/platform.c`) was
an atomic add. Only its own core writes it, so it is now a relaxed load and
store, which other cores' relaxed loads still read untorn.

## Wake placement

`prvYieldForTask` (`tasks.c` 933–945) chooses the core running the lowest
priority below the woken task's. Its `<=` comparison gave a tie to the last
such core, so when several cores were idle the task went to the
highest-numbered idle core and cost an SGI. An idle calling core, once chosen,
now keeps a tie against another idle core of its own type: its yield needs no
SGI, and the task runs where its waker's data is cached. An idle core of
another type still takes the tie, so an idle Cortex-A76 still wins over an
idle Cortex-A55. Ties between busy cores, and ties that the calling core does
not hold, still go to the last such core. Linux `select_idle_sibling` returns
the target CPU when it is idle and `asym_fits_cpu` accepts its capacity
(`kernel/sched/fair.c` 7751–7753).

`configCORES_SHARE_TYPE(a, b)` names the cores of one type and defaults to all
cores alike. The cached A733 configuration
(`platform/a733-smp-cached/FreeRTOSConfig.h`) defines it from
`A733_SMP_A55_MASK` in `include/a733_smp_placement.h`, which every cached
image now includes; the cached and storage build identities hash that header.
In QEMU every one of 64 tick wakes of an unpinned task now runs on the tick's
core, and both cores retire 7% fewer instructions per wake. In an image whose
configuration gives the two QEMU cores different types, none does.

On the A733, a task that may run on every core and is woken by the tick on
core 0 still goes to idle core 7, as before. The rule changes placement for
tasks confined to one cluster, such as the integrated OS image's tasks
without an explicit placement, which stay on the A55 cores.

## Late ticks in up to three calls per IRQ

The cached platform's tick acknowledgement advances the compare value by one
period. A tick IRQ taken N periods late therefore left the compare value in
the past, and the timer fired N more times in a row. The tick IRQ
(`platform/a733-smp-cached/platform.c`) now works as follows:

- It reads `CNTP_CTL_EL0.ISTATUS` and processes no tick when the timer
  condition no longer holds, as Linux's timer handler acts only on `ISTATUS`
  (`drivers/clocksource/arm_arch_timer.c` 671).
- After the first tick handler call it reads `ISTATUS` again. When the
  condition still holds, the IRQ was late and at least one more period is
  due. It makes at most three tick handler calls in all, as
  `hrtimer_interrupt` retries at most three times (`hrtimer.c` 1921–1922),
  so one comparison of the counter with the next deadline plus a period
  decides whether a third call is due, where `hrtimer_forward` divides
  (`kernel/time/hrtimer.c` 1042–1074). The level-sensitive timer interrupt
  is taken again for any period still due.
- Each call advances the compare value by one period, so the next deadline
  stays in phase. Linux programs its next event from the current count
  instead (`arm_arch_timer.c` 741–757); keeping the phase keeps the kernel's
  tick count equal to the elapsed periods.
- A counter read taken early can only lower the count of due periods. The
  first call relies on the IRQ mask of exception entry; the further calls
  mask IRQs again, because the tick handler's FromISR critical section
  clears `PSTATE.I`.

## Correctness evidence

Each check ran on the final sources of each branch on 2026-10-25.

On `work/mc-perf-a-1025`:

- `tools/check-smp.py --case runtime` passed ten of ten trials on
  `cortex-a55` and on `cortex-a76` cores.
- The contention check passed with AddressSanitizer and
  UndefinedBehaviorSanitizer, 16 threads and 200,000 calls each, and with
  ThreadSanitizer, 16 threads and 20,000 calls each.
- The integration gate, label `mc-perf-a-1025-1`, passed.

On `work/mc-perf-1025`:

- `tools/check-smp.py --case soak8 --trials 10` passed ten of ten trials on
  `cortex-a55` and on `cortex-a76` cores. The QEMU SMP targets compile the
  unpatched `tasks.c`, so the soak also ran on an eight-core image built with
  `patches/freertos-smp-tasks.patch` and passed ten of ten trials on both core
  models.
- `--case runtime`, `--case kernel` and `--case context` passed ten of ten
  trials on both core models.
- The integration gate `tools/integration-gate.py`, with label
  `mc-perf-1025-2`, built revision `e11f793a`: the central images listed
  below, the supervised and resident OS images and the QEMU runtime models. It
  passed the eight-core soak with ten trials and the mimus SMP parity check.
  Its log is `~/.cache/sunstoneos/build/gate-mc-perf-1025-2/summary.txt`.

On both branches, the cached eight-core instruction model completed the
diagnostic on all eight cores in every measured state, with and without the
late tick. The pure-host checks that `checks/check-index.py` maps to the
changed files passed. Three exceptions also fail on the unchanged base:
`audit-build-safety-configure.py` finds no `ld.lld` on the configure path,
`provider-clock-reference.py` fails to compile its fixture's `twi.c`, and
`gpu-texture-frame.py` uses `PVR_GPU_SERVICE_BURST_STEPS`, which
`include/pvr_gpu_service.h` does not define.

One `cortex-a76` soak of the board-gated sources failed its second trial at
`checks/smp8-soak.c` line 104, the controller's one-second wait for a moved
task, while ten QEMU instances ran at once. Without `-icount`, guest time
follows the host clock. The same soak passed ten of ten trials alone. Run five
times each under the same load, the base image failed at the same line in four
runs and this branch's image in three. The failure is therefore host load,
and the gate and the soaks above ran without other QEMU instances.

Evidence and the fetched reference files, with their revisions and hashes,
are under `~/.cache/sunstoneos/evidence/mc-perf-1025/`.

## Images that change

The base and this branch were built with the same configuration. Every built
image that links the changed port, kernel, platform, counter clock or newlib
sources differs between the two builds, 18 in all:

- Cached-family runtime images: `a733-smp-cached-diagnostic`,
  `a733-smp-observe`, `a733-smp-supervised`,
  `sunstone-a733-smp-cached-devices`, `sunstone-a733-storage-maintenance`,
  `sunstone-a733-board`, `sunstone-a733-camera`, `sunstone-a733-gpu-render`,
  `sunstone-a733-hdmi`, `sunstone-a733-mimus-native`, and
  `sunstone-a733-os` in its supervised and resident variants.
- The uncached eight-core diagnostic `a733-smp-diagnostic`, which applies the
  same port patches.
- Emulator models: `dma-startup-model`, `npu-startup-model`,
  `smp-runtime-model`, `smp8-soak-model` and `smp-mimus-model`.

`kernel-check` and `a733-kernel-check`, which link none of those sources, are
byte-identical. The cached-family images built only under other options
derive from the same sources and change as well; the gate did not build them.
They are the other seven `sunstone-a733-os` variants and the Bluetooth, I2S,
mimus board, GPU display, HDMI, camera and board application, accelerator,
DSI scan-out, eMMC read and single-core images. `images.json` in the evidence
directory lists each ELF's SHA-256 before and after. The cached-family build
identities change as follows, so board evidence stays bound to the identity
it ran:

| Image | Identity before | Identity after |
| --- | --- | --- |
| Cached diagnostic | `81e4db08ac51800aa5806e6ba332e1eac11cf107711b84fca1fb7042000c6dba` | `502b6b3e1e6ff13ea216aafd2e37eb6d51c789fd14137f11952e242ffb303a64` |
| USB observer | `1887b8c94d89b97d8a338831f14778482a1ba89f4c9b9c5c92b72fa91405c4f3` | `269ecc3deb5d28366ff25a262559d5a4f9cde97447c37dafa76f38c5a55385b4` |
| Supervised observer | `5db6b0b64d0bc0b25274ccc2595563bed7ef8a7b7139dd33b1a8693466eb6410` | `8f3f3fa9c7ad5076374e3b90ab2dd4666fc5fd0bbb9e79dddc2d67878afc6e1b` |
| Cached device image | `7eb74ccb183ce6baf2df88c3c91568563b38bc0451228c014e7599148bc703cd` | `585f19991e4020bce3f5b11a1fa5a09b2841a115e2186ce8b19cdbc328a38c33` |

## Board evidence still needed

[The multicore performance runbook](../board-runbooks/multicore-performance-session.md)
lists the measurements: SGI, tick and floating-point progress on all eight
cores with the lock bookkeeping and IRQ exit changes, PMU cycle counts for the
removed barriers, the skew between A55 and A76 counter reads and a forced late
tick on core 0. The compile definition `SUNSTONE_A733_PERF_SESSION` adds those
measurements to the cached images (`platform/a733-smp-cached/perf_session.c`).
The measurements keep every core busy, as the profile's qualified runs did,
so the board session does not measure wake placement, which acts only
between idle cores; its evidence is the QEMU and model figures above. The
runbook proposes, and the profile's models pass, a variant of the
measurement phase that idles the cores to measure it.
