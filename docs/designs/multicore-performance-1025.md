# Design: newlib state and time reads without supervisor calls or locks

Status: **implemented and measured in emulation**. Two changes remove a
supervisor call and a lock that protect no shared state: `__getreent` reads
the running task's newlib state from the task register, and the platform
counter clock reads time without a lock. Neither changes the FreeRTOS port,
so neither changes the cached diagnostic, its observers or the eight-core
soak image. Each follows the reference implementation named in its section.

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
instructions, but neither reproduces the Arm memory model: QEMU runs on a
host whose stores are seen in order, and the model interleaves the cores one
quantum at a time over shared host memory. Neither can show a missing or
misplaced barrier, so the ordering argument for the counter clock below rests
on reasoning until a board session tests it.

## Results

| Metric | Base | newlib state | Counter clock |
| --- | --- | --- | --- |
| QEMU: `__getreent`, EL1 task and EL0 task | 96, 96 | 7, 7 | 7, 7 |
| QEMU: uncontended counter clock read | 69 | 69 | 62 |
| Host: counters up to 21 ticks apart on 16 threads | Faults | Faults | No fault |
| Host: one caller stopped after its counter sample | Faults | Faults | No fault |

Every other QEMU and model figure is the same in all three columns: a
critical-section pair retires 296 instructions from an EL1 task and 368 from
an EL0 task, and a cross-core round trip 11,976. The EL0 round trip reads
14,148 to 14,169; it varies between runs by that much because an instruction
count earlier in the run changes how QEMU interleaves the two cores' lock
waits.

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

## Correctness evidence

Each check ran on revision `9e30bfb8` on 2026-10-25:

- `tools/check-smp.py --case runtime` (`smp-runtime-model`, which uses the new
  `__getreent`) passed ten of ten trials on `cortex-a55` and on `cortex-a76`
  cores.
- The contention check passed with AddressSanitizer and
  UndefinedBehaviorSanitizer, 16 threads and 200,000 calls each, and with
  ThreadSanitizer, 16 threads and 20,000 calls each.
- The cached eight-core instruction model completed the diagnostic on all
  eight cores in each of the three states.
- The pure-host checks that `checks/check-index.py` maps to the changed files
  passed, except `audit-build-safety-configure.py` and
  `provider-clock-reference.py`. Those two also fail on the unchanged base:
  the first finds no `ld.lld` on the configure path, and the second fails to
  compile its fixture's `twi.c`.
- The integration gate `tools/integration-gate.py`, with label
  `mc-perf-a-1025-1`, built the central images listed below, the supervised
  and resident OS images and the QEMU runtime models. It passed the
  eight-core soak with ten trials and the mimus SMP parity check. Its log is
  `~/.cache/sunstoneos/build/gate-mc-perf-a-1025-1/summary.txt`.

Evidence and the fetched reference files, with their revisions and hashes,
are under `~/.cache/sunstoneos/evidence/mc-perf-1025/`.

## Images that change

The base and this tree were built with the same configuration. These images
differ between the two builds:

- Images that link the counter clock or the SMP newlib runtime:
  `sunstone-a733-storage-maintenance`, `sunstone-a733-board`,
  `sunstone-a733-camera`, `sunstone-a733-gpu-render`, `sunstone-a733-hdmi`,
  `sunstone-a733-mimus-native`, and `sunstone-a733-os` in its supervised and
  resident variants.
- `sunstone-a733-smp-cached-devices`, whose code is unchanged; its build
  identity changes from
  `7eb74ccb183ce6baf2df88c3c91568563b38bc0451228c014e7599148bc703cd` to
  `9c1b44b12396941a6ce175a4beeaa4bc9b0eaa4b2b6c2d654d692be469c167db` because
  the identity hashes the changed counter clock header.
- Emulator models: `dma-startup-model`, `npu-startup-model`,
  `smp-runtime-model` and `smp-mimus-model`.

The cached diagnostic, the USB and supervised observers and
`smp8-soak-model` are byte-identical. The images built only under other
options that link the counter clock or newlib change as well; the gate did
not build them. `images-a.json` in the evidence directory lists each ELF's
SHA-256 before and after.
