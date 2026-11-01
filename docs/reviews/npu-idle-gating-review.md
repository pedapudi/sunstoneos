# Review package for NPU internal clock gating

**Status: awaiting board comparison.** The three independent code reviews
(simplicity, performance and correctness) approved the first revision with
required changes. The changes are made and host-tested. No gating image has run
on a board.

## Scope

The coordinator approved internal clock gating only: the NPU register window at
`0x03600000` and the vendor VIPLite gate helpers. The change touches no rail, no
PCK4 domain, no PLL, no voltage and no top-point clock. The NPU stays at 852 MHz
and 800 mV. Gating is opt-in. A queue that does not call
`npu_runtime_queue_power_start()` creates no owner task and behaves as before.

**Diff ranges.** The first review covered `383bfb8a..bc0eef5f`. The revision is
`bc0eef5f..` the branch tip named in the closing report, on
`work/npu-fastpath-1101`.

## Changes required by the reviews

| Item | Change |
| --- | --- |
| Simplicity 1 | `struct npu_idle_part` (`include/npu_idle.h`) holds the register offsets, bit patterns, full-idle word, idle delay, resume budget and settle bound. `include/a733_npu_idle.h` is the only place with A733 values. `npu_idle.c` and the queue adapter's register whitelist read the descriptor; the checks pass it through modeled memory. The interrupt-controller offsets in `platform/a733/npu_irq.c` belong to that platform file and are unchanged. |
| Simplicity 2 | The `can_idle` and `ready` callbacks, their NULL branches and fixture models are deleted. The EL0 wiring adds the predicate back with a kick when the ring drains. |
| Simplicity 3 | The transient SUSPENDING and RESUMING phases are removed. The failure record carries the step (suspend or resume) beside the phase. |
| Correctness 4 | A quiet query result of 0 means "not now": the owner counts it and rearms 1 ms ahead. Only -1 is retained. |
| Correctness 5 | `npu_runtime_queue_submit()` admits and submits in one raw lock span and returns pending (0) without failing. The benchmark uses it, and a host case counts the lock spans. |
| Correctness 6 | After a resume the owner calls the quiet query before unlocking. An interrupt that follows is retained with site `RESUME_IRQ`; a later one keeps the generic site. |
| Correctness 7 | A resume over the budget always records its elapsed time and a counter. Only a strict queue (the benchmark images) retains it. |
| Performance 8 | A busy queue arms the owner's alarm one idle delay ahead. The completion interrupt wakes no task and reuses the completion timestamp for the idle time. |
| Performance 9 | A third image (`--idle none`) creates no power owner. The control must stay within the limits of this baseline. |
| Performance 10 | Each window and group reports the idle-task runtime. Non-idle time is the length minus that runtime, beside the charged CPU time. Interrupt time that lands during the idle task is charged to the idle task by the scheduler counter, so neither figure includes it. |
| Performance 11 | Each image runs five windows of 12,000 jobs (about 450 ms and 45 owner wakes each). The comparison uses medians and reports the spread. One image per variant is used instead of interleaving, because the gating policy does not act during a continuously busy window. |

Recommended items done: the completion hook passes `q->observed`; the placeholder
zeros are removed from the report; the priority query and its kernel
configuration are dropped (the caller guarantees the owner outranks the
producer); history-narrating text in the design is replaced by present-tense
statements. Not done: merging `checks/npu-idle-report.py` into
`apps/npu-finite/idle-compare.py`. The check supplies synthetic reports and the
tool compares real ones, so they stay separate.

## Invariants

1. **Only the descriptor's registers change.** The adapter writes only the clock
   and shader registers and reads only those and the status register. It refuses
   every other offset, including the destructive event word.
2. **No read after the final suspend write.** While the clocks are suspended the
   only read is the clock register at resume entry, which must equal the stored
   gate value with the gate bit set. Every other readback follows the ungate
   pulse and a settle of at least 1 µs.
3. **Transitions run under the raw local interrupt lock.** Every register access
   of a transition executes with IRQs masked. A settle wait is bounded.
4. **Gate only when idle.** The owner gates when the queue is empty, completions
   equal submissions, the last completion left full idle, the lifetime window is
   open and the interrupt controller is quiet. A stale or busy alarm never gates.
5. **Admission never touches a suspended device.** It records one resume request,
   returns pending and changes no job, command or fence.
6. **Every anomaly is retained.** The first site, phase, step, offset, value and
   expectation are preserved, the failure flags are set, and no retry or further
   register access follows.
7. **An interrupt while suspended, or with no job, is a fault** retained without
   device access, attributed to the resume when the post-resume quiet query found
   one pending.
8. **Close resumes first** and fails, keeping its ownership, if the clocks are not
   running.
9. **The control is the same owner without writes.** Its wake schedule matches the
   gating image while the queue is busy.
10. **Submit and completion gain no task hop.** A completion adds one timestamp
    store; the owner observes it at its next alarm.

## Questions for each reviewer

- **Performance.** The owner still preempts the producer about every 10 ms under
  load. The 105% p99 and 110% maximum limits apply to jobs of about 10 µs. The
  baseline comparison measures this cost. Is a different priority or pinning
  needed if it exceeds the limits?
- **Correctness.** The shared virtual timer composition in the finite images (PPI
  27 is installed by the EL3 monitor; the FreeRTOS tick uses the physical timer on
  PPI 30) is checked only by the control and baseline runs.

## Host evidence

Directories are under `~/.cache/sunstoneos/build/npu-fastpath-1101/review-evidence2/`.

| Check | Result |
| --- | --- |
| `npu-idle/` (`checks/npu-idle.py`) | Nine portable groups: delayed MMIO, access-limited suspended reads, every injected failure, and over-budget resumes in both modes |
| `npu-queue-handoff/` (`checks/npu-queue-handoff.py`) | The existing handoff cases plus sixteen owner cases and nine relink-based regression mutations |
| `npu-idle-report/` (`checks/npu-idle-report.py`) | Baseline, control and gating decoding, the three-way comparison, and twenty corrupted reports; the producer is not executed |
| `npu-latency/` (`checks/npu-latency.py`) | The latency producer and decoder, unchanged without the idle benchmark |

## Limits of the evidence

- The scheduler, the shared deadline facility and the interrupt controller are
  modeled. Timing is modeled counter ticks, not silicon.
- The producer in `idle-client.h` executes only on the board. Its in-image checks
  retain the run on a violated expectation. The decoder refuses a report that
  breaks a bound.
- The operating system image does not call `npu_runtime_queue_power_start()`. The
  EL0 ring predicate belongs to the EL0 work.
- Safe shared-SRAM access while gated and recovery from a fault while suspended
  have no evidence. The PMIC provider reads no DCDC2 current or power, so there is
  no energy evidence.

## Board plan

The baseline, control and gating images are built from
`apps/npu-finite/build.py --latency queue --idle none|off|gate`. Each runs 1
warmup job, 1,000 legacy jobs, five windows of 12,000 jobs and 100 cold jobs after
each of 9, 11 and 100 ms idle gaps (61,301 jobs). The baseline runs first, then the
control, and the gating image runs only if both return normally with matching
outputs. `apps/npu-finite/idle-compare.py` applies the limits and prints the
tables. The first runs after the boards are replugged confirm the startup fix and
should not be mixed with these.
