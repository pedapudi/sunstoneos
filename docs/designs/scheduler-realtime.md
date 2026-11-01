# Bound scheduler work before changing global timekeeping

Status: **bounded time-slice traversal is implemented and reviewed; scheduler
qualification is incomplete.** This note addresses the eight-core A733 port
and the pinned FreeRTOS kernel. The reader is assumed to know interrupt
handling, task priorities and mutual exclusion. Current evidence and the
service priority map are in [scheduler status](../status/scheduler.md).

## The global tick already has one owner

[The configuration](../../include/FreeRTOSConfig.h:35) selects preemption,
time slicing, eight priorities and a 1 kHz tick.
[CPU0 programs the physical generic timer](../../platform/a733-smp-cached/platform.c:122).
Only [CPU0 private peripheral interrupt 30 (PPI30) advances global time](../../platform/a733-smp-cached/platform.c:146).
Secondaries receive scheduler software-generated interrupts, called SGIs,
and already [execute wait-for-interrupt (WFI) when idle](../../include/a733_cpu_power.h:23).
The idle-priority time-slice rule avoids SGIs when only the eight kernel
idle tasks are ready. The system therefore has one periodic scheduler timer,
rather than eight periodic timers.

The [late-tick path](../../platform/a733-smp-cached/platform.c:133) processes
at most three periods per IRQ. Further overdue periods retain the timer's
level assertion. Each acknowledgment advances the original compare phase.
Raising the tick rate would increase this work and lock traffic. Keep 1 kHz
while introducing a separate interrupt-driven path for sub-tick deadlines.

## Time slicing has a task-count-independent traversal bound

The native task patch filters time slices by ready task affinity. It caches
one admitted-core mask for each distinct running priority. Its original list
traversal examines every ready task at those priorities while the port holds
the global ISR lock. Static allocation places no bound on the number of tasks.

[The length check](../../patches/freertos-smp-tasks.patch:90) limits traversal
to lists containing at most twice the core count. With eight cores, a list
of sixteen entries can contain one running task and one waiter per core.
Longer lists immediately request rescheduling on every core running that
priority. This retains upstream time slicing and covers every unexamined
affinity. It can send unnecessary SGIs when the tasks are pinned elsewhere.
The selected task still follows the normal priority and affinity rules.

At most seven non-idle priorities can occur in this configuration. Each
distinct priority is examined once, so a tick visits at most 112 entries.
The small core and priority loops are also bounded. Timeout expiry and task
selection retain their separate task-count-dependent work; this change does
not establish a bound for the complete tick or context switch.

The [linked-kernel fixture](../../checks/scheduler-timeslice.py) executes
`xTaskIncrementTick` from the native ELF. It substitutes core identity and
SGI publication, then checks actual yield flags and global tick advancement.

| Ready-list case | Before: entries / instructions | After: entries / instructions |
| --- | ---: | ---: |
| Eight running tasks | 8 / 292 | 8 / 294 |
| Sixteen entries, competing affinity on one core | 16 / 389 | 16 / 391 |
| Seventeen entries | 17 / 402 | 0 / 354 |
| 1,024 entries | 1,024 / 10,472 | 0 / 354 |

These are instruction-model counts for the complete fixture tick, including
its substituted call boundaries. They establish bounded traversal. Board
cycles and CPU cost per operation remain unmeasured. The two extra instructions
on short lists pay for the length threshold. Ten cases cover exact termination, a final new
affinity, unrelated running priorities, excluded affinities and idle tasks.
An independent reviewer approves the source and reproduces all ten cases.

## Board observations expose remaining timing failures

The existing CPU-only latency application supplies 64 idle and 128 loaded
wake samples on each core. Its memory workers read and rewrite private 1 MiB
extents. CPU0's timer IRQ notifies the pinned tasks; remote cores run through
the production SGI and context-switch paths.

Both images use CPU-L at 1794 MHz/1000 mV and CPU-B at 2002 MHz/1050 mV.
The generic counter runs at 24 MHz. The comparison introduces no operating point.

| Maximum, microseconds | Before | After |
| --- | ---: | ---: |
| Ready insertion to execution, idle, all cores | 71.917 | 66.042 |
| Ready insertion to execution, loaded, all cores | 111.458 | 133.333 |
| Ready insertion to execution, idle, remote cores | 60.833 | 59.000 |
| Ready insertion to execution, loaded, remote cores | 98.375 | 94.708 |
| Timer assertion to C dispatch | 5.375 | 5.917 |
| IRQ-masked recorded interval | 264.958 | 265.417 |
| ISR lock acquisition / outer hold | 7.458 / 25.042 | 6.125 / 22.958 |
| Task lock acquisition / outer hold | 122.917 / 12.417 | 177.875 / 11.500 |

Both runs return normally with complete samples. Both fail the existing
25 µs masking and lock budgets. The ordinary workload does not exercise the
long-list fallback. The higher loaded wake and task-lock-wait maxima prevent
a claim of physical performance improvement. A single pair of executions
does not establish repeatability or explain that variation.

The [ready-list timestamp](../../apps/latency/workload.c:50) starts after
notification locking and insertion. It does not measure the full interval
from IRQ assertion to task execution. The recorder retains count, maximum
and first-over-budget context; it contains no percentile samples. Its lock
and mask records also include task creation and readiness waits before the
idle/load phases. Symbolization places the largest CPU0 masking sample at
`prvAddNewTaskToReadyList`. Those records cannot isolate loaded lock contention.
Masked and held intervals exclude the recorder's closing bookkeeping and
unlock tail, as [the measurement contract](../status/latency.md) explains.

Before is queue entry `1793560470911857163-ce5f22bf`, image SHA-256
`478364d6e009daf30810ba6b07490443cc6694309c70ae9902e8ea78e5c208fd`,
from Git `49fde433`. After is entry `1793560934366050732-4e45b5ea`, image
SHA-256 `21c8564d781a1b62614e4201e0f92ad01d37a0cc72c2c7945998be9e09dc466c`.
The after image contains the scheduler change committed as `d2c8de05`;
it is built before that commit, with the base revision still in build identity.
Each entry's `run/` directory contains its log, output and return report under
`~/.cache/sunstoneos/board-queue/entries/`. The runner records its own
`bringup` revision, `f9375cb7`, separately from these image sources.

## Recorded recognition preserves the reference

Queue entry `1793561595474856811-579a99b6` returns normally with eight inference
workers. The captured feature CRC is `ac657a3b`, log-probability CRC is
`a7ade80e`, raw text is `there was silence` and formatted text is
`There was silence.`. The record contains 90 frames and 57,440 samples.
Reference comparison of CRCs, text, marks and capitalization passes against
the existing host-reference JSON. Inference takes 334,670 µs for this input;
it is a parity check, not a scheduler throughput comparison.

The image SHA-256 is
`312b33297d79f70b9b81c321e1818eb6a9f27d8e746336b78b6668f33da4ccaf`.
Its OS scheduler contains `d2c8de05`. The application manifest correction is
committed separately as mimus `8308b79aa` on `work/scheduler-parity-1101`.
The application worktree is under
`~/.cache/sunstoneos/build/scheduler-rt-1101/mimus-app/`.

That correction removes unused GPU grants from the CPU profile. Without it,
entry `1793561131232973768-80ec5a72` stops at the application pin check before
scheduler startup, then returns through the watchdog. Exit detail 10 comes
from [the application pin predicate](../../apps/board/main.c:326). A GPU-write
region requires privileged writes to be excluded, while the CPU-only image
maps that reservation writable. CPU and GPU manifest-generation host checks
cover the correction. Model hashes and region addresses remain fixed.

The corrected run's queue outcome is `result-mismatch` because the submission
reuses a legacy `mode/text` JSON expectation. The current runner treats that
file as literal text. The intended formatted text is present, and independent
reference comparison passes. No repeat run is needed to recover those bytes.
The comparison, file hashes and expectation-format diagnosis are preserved in
`~/.cache/sunstoneos/build/scheduler-rt-1101/mimus-parity.json`.
These results establish one recorded-input pass; sustained physical scheduling
and broader recognition inputs remain unqualified.

## Tickless idle needs a cross-core sleep protocol

Tickless idle is unimplemented. Simply enabling the upstream generic path
does not work on this port. In the pinned upstream `tasks.c`:

- `prvGetExpectedIdleTime`, line 4047, rejects an idle ready list longer than
  one; this configuration has eight idle tasks.
- `vTaskSuspendAll`, line 3958, retains the task lock until
  `xTaskResumeAll`, line 4089. Sleeping within that ownership extends a
  global lock hold through the sleep interval.
- `vTaskStepTick`, line 4677, requires scheduler suspension when stepping
  exactly to the next delayed-task expiry.
- `xTaskGetTickCountFromISR`, line 4253, reads global time with local masking.

These anchors refer to the cached source pinned by
[the source lock](../../sources.lock.json), before applying project patches.
[The upstream implementation](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/tasks.c)
defines these scheduler contracts.

An idle secondary can receive a device IRQ and wake a task pinned to itself.
Existing affinity selection need not send CPU0 an SGI. That task could observe
stale ticks and install an incorrect timeout while CPU0's timer is suppressed.
An asynchronous wake request to CPU0 alone does not close this race.

The proposed implementation requires a privileged exception level 1 (EL1)
idle entry, a generation number
for each sleep attempt and acknowledgment from every idle core. Remote IRQ
entry must atomically cancel admission or wake CPU0 before executing callbacks.
CPU0 corrects global ticks from the generic counter, restores the periodic
compare and publishes the awake generation before remote callbacks proceed.
No kernel lock can remain held across WFI or an acknowledgment wait.
Ready-list changes, pending IRQs, tick wrap, early wake and timeout expiry
require faithful concurrent tests. Active-idle task reclamation and returning
session deadlines must remain live during suppression.

The admission and wake traffic must be measured against the saved timer IRQs.
Required evidence includes idle-window PPI and SGI counts on each core,
elapsed-counter versus global-tick agreement, pending-IRQ race coverage, repeated
sleep/wake transitions, and observable temperature or power. Neither CPU timing
run isolates an idle counting window or measures idle power.

## High-resolution deadlines require a separate compare consumer

The [generic counter clock](../../platform/a733/counter_clock.c:16) supplies
sub-tick timestamps. A733's 24 MHz counter has a nominal 41.667 ns period.
Reading that counter does not create an asynchronous timeout. There is no
general application API for interrupt-driven sub-tick waits in this port.

A proposed fixed-capacity deadline table shares CPU0's generic timer compare
with the scheduler tick. Caller-context submission updates a resident slot
and selects the earlier of the next tick and earliest deadline. CPU0 callers
rearm its compare directly. Secondary callers publish a rearm request and send
CPU0 an SGI, because the architectural compare register is private to each core.
The ISR retires due slots and notifies their tasks directly. Registration reserves notification
storage once; it must preserve existing notification index zero. Cancellation
and task retirement require explicit ownership. Table size bounds submission
and IRQ work, and simultaneous expiries need a measured budget.

This path remains unimplemented. It requires measured overshoot distributions,
burst expiry, cancellation and cross-core delivery before driver adoption.
Increasing the scheduler tick rate does not satisfy this requirement.

## Retain shared ready lists until contention is isolated

Per-core ready lists would change migration, affinity, simultaneous wake-up
placement, priority inheritance and task deletion. Splitting only the port's
locks would expose shared kernel lists to concurrent mutation. The current
records include startup contention and cannot establish that the additional
coordination improves steady-state scheduling.

Retain the upstream lock ownership and ready lists. Phase-separated hold/wait
samples, percentiles, holder call sites, throughput and CPU cost remain needed
from the latency measurement stream. Reconsider partitioning only with those physical
measurements and the unchanged recognition reference as a regression check.
