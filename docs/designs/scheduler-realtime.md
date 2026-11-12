# Scheduler priorities, timekeeping and the global-lock control kernel

Status: **implemented** for the priority map, the single periodic tick, bounded
time-slice traversal in the global-lock kernel and the sub-tick deadline wait;
**open** for tickless idle. The kernel that SMP images build by default is
[the partitioned fork](scheduler-partitioned.md); the global-lock kernel described
here is its control (`-DSUNSTONE_PARTITIONED_SCHEDULER=OFF`). The reader is assumed
to know interrupt handling, task priorities and mutual exclusion. Qualification
is in [scheduler status](../status/scheduler.md) and the measurement boundaries
are in [latency status](../status/latency.md).

## Service priorities fit within eight levels

Larger numbers preempt smaller numbers on an eligible core. Application manifests
supply their priority; the table describes the services and reserves the free
levels. It does not enforce an application policy.

| Priority | Use or reservation | Source |
| ---: | --- | --- |
| 0 | Kernel idle tasks, one pinned to each core | [cached configuration](../../platform/a733-smp-cached/FreeRTOSConfig.h#L13) |
| 1 | Diagnostic memory/background load; available for background application work | [latency workload](../../apps/latency/workload.c#L171) |
| 2 | CPU latency application's main task | [latency manifest](../../apps/latency/manifest.json#L4) |
| 3 | PMIC owner, NPU application client and recorded mimus board application | [board tasks](../../apps/board/main.c#L339), [NPU tasks](../../apps/os/npu.c#L294), external application's manifest |
| 4 | USB, radio, watchdog supervision, thermal, audio, Bluetooth, GPU and DMA service work | [board tasks](../../apps/board/main.c#L339), [thermal](../../apps/os/thermal.c#L319), [GPU](../../apps/os/gpu-runtime.h#L4), [DMA](../../apps/os/dma.c#L58) |
| 5 | Type-C and power owners | [board tasks](../../apps/board/main.c#L339) |
| 6 | Available for a bounded urgent service with a demonstrated need | [priority count](../../include/FreeRTOSConfig.h#L38) |
| 7 | Returning CPU terminal tasks and highest-priority latency probes | [CPU return](../../apps/os/cpu-return.c#L60), [latency workload](../../apps/latency/workload.c#L171) |

Several services share priority four; their execution budgets and affinities
need combined-load evidence. Default OS affinity covers the six A55 cores, and
explicit placements admit the two A76 cores. The latency workload pins its probe
and memory task to each physical core, and CPU0's timer wakes remote probes through
production SGIs.

The pinned upstream `xTaskPriorityInherit` raises a contended mutex holder and
reschedules an eligible core if that ready holder is not running. Ordinary release
restores base priority after the holder releases its last mutex, and timeout
disinheritance adjusts a holder with one outstanding mutex. These are the
simplified upstream semantics; full transitive inheritance and a priority ceiling
protocol need different mechanisms, so bounded response paths avoid nested mutex
dependencies. Cross-core priority inversion with a high-priority waiter and
medium-priority interferers has no isolated physical test.

## The global tick has one owner

[The configuration](../../include/FreeRTOSConfig.h#L35) selects preemption, time
slicing, eight priorities and a 1 kHz tick. [CPU0 programs the physical generic
timer](../../platform/a733-smp-cached/platform.c#L122), and only [CPU0 private
peripheral interrupt 30 (PPI30) advances global time](../../platform/a733-smp-cached/platform.c#L146).
Secondaries receive scheduler software-generated interrupts (SGIs) and [execute
wait-for-interrupt (WFI) when idle](../../include/a733_cpu_power.h#L23). The
idle-priority time-slice rule sends no SGI when only the eight kernel idle tasks
are ready, so the system has one periodic scheduler timer.

The [late-tick path](../../platform/a733-smp-cached/platform.c#L133) processes at
most three periods per IRQ; further overdue periods keep the timer's level
assertion, and each acknowledgment advances the original compare phase. A higher tick
rate would add work and lock traffic, so the tick stays at 1 kHz and sub-tick
deadlines use a separate interrupt-driven path (below).

## Time slicing has a task-count-independent traversal bound

Status: implemented in `patches/freertos-smp-tasks.patch` and host-tested. The
native task patch filters time slices by ready task affinity and caches one admitted-core
mask per distinct running priority. Without a bound, the traversal examines every
ready task at those priorities while the port holds the global ISR lock, and static
allocation places no limit on the task count.

[The length check](../../patches/freertos-smp-tasks.patch) limits traversal to lists
of at most twice the core count (sixteen entries on eight cores: one running task and
one waiter per core). A longer list requests rescheduling on every core running that
priority, which retains upstream time slicing and covers every unexamined affinity,
and can send unnecessary SGIs when the tasks are pinned elsewhere. At most seven
non-idle priorities occur, each examined once, so a tick visits at most 112 entries.
Timeout expiry and task selection keep their task-count-dependent work, so the
bound does not cover the complete tick or context switch.

no host check executes
`xTaskIncrementTick` from the native ELF with substituted core identity and SGI
publication, and checks yield flags and tick advancement over ten cases (exact
termination, a last-added affinity, unrelated running priorities, excluded
affinities, idle tasks). A 1,024-entry list costs 354 instructions after the check
against 10,472 before; the instruction-model counts, the board pair that compares
the change (entries `1793560470911857163-ce5f22bf` and
`1793560934366050732-4e45b5ea`) and the reviewer's reproduction are in the records
`scheduler-realtime-timeslice-traversal` in
[scheduler-realtime.jsonl](../evidence/scheduler-realtime.jsonl) and in
[scheduler.jsonl](../evidence/scheduler.jsonl). On the board the change does not
improve the loaded wake or task-lock-wait maxima, and both runs fail the 25 µs
masking and lock budgets.

The recorded recognition on the same scheduler returns the reference CRCs and
text (record `1793561595474856811-579a99b6`); it is a parity check, not a
throughput comparison.

## Tickless idle needs a cross-core sleep protocol

Status: open. Enabling the upstream generic path does not work on this port. In
the pinned upstream `tasks.c`, `prvGetExpectedIdleTime` rejects an idle ready list
longer than one (this configuration has eight idle tasks); `vTaskSuspendAll` keeps
the task lock until `xTaskResumeAll`, so sleeping within it extends a global lock
hold through the sleep. In the same file, `vTaskStepTick` requires scheduler suspension when stepping
to the next delayed-task expiry; and `xTaskGetTickCountFromISR` reads global time
with local masking. These anchors refer to the source pinned by
[the source lock](../../sources.lock.json) before project patches. The partitioned
fork rejects `configUSE_TICKLESS_IDLE` at compile time.

An idle secondary can receive a device IRQ and wake a task pinned to itself without
an SGI to CPU0, so the task could observe stale ticks and install an incorrect
timeout while CPU0's timer is suppressed. A wake request to CPU0 alone does not
close this race. The design needs a privileged EL1 idle entry, a generation number
for each sleep attempt and acknowledgment from every idle core. Remote IRQ entry
must atomically cancel admission or wake CPU0 before running callbacks. CPU0
corrects global ticks from the generic counter, restores the periodic compare and
publishes the awake generation before remote callbacks proceed. No kernel lock
remains held across WFI or an acknowledgment wait. Active-idle task reclamation and
returning-session deadlines stay live during suppression. Evidence needed:
idle-window PPI and SGI counts per core, elapsed-counter versus global-tick
agreement, pending-IRQ race coverage, repeated sleep and wake transitions, and
temperature or power.

## High-resolution deadlines use a separate compare consumer

Status: implemented. The [generic counter clock](../../platform/a733/counter_clock.c)
supplies sub-tick timestamps (24 MHz, nominal 41.667 ns period), and reading it does
not create an asynchronous timeout. `sunstone_deadline_wait_until()`
([header](../../include/sunstone_deadline.h), `runtime/deadline.c`,
`runtime/deadline-a733.c`) lets a registered task wait until an absolute
microsecond deadline or a notification on its index. A table sized for the
image's declared owner tasks (16 slots by default for standalone profiles)
shares CPU0's virtual-timer compare (`CNTV`) with the scheduler. A caller updates
its slot and CPU0 callers rearm the compare directly; secondary callers publish a
rearm request and send CPU0 an SGI, because the compare register is private to each
core. The ISR retires one due slot per interrupt and notifies its task, so the added
critical section stays bounded and further due slots keep the level-sensitive
compare pending. Registration reserves notification storage once and preserves
notification index zero. Registered tasks must not be deleted. The caller rechecks
its device predicate after every result, including expiry. Overshoot distributions,
burst expiry, cancellation and cross-core delivery are unmeasured, and the
scheduler tick rate does not satisfy this requirement.

## The global-lock kernel is the control for the partitioned fork

Per-core ready lists change migration, affinity, simultaneous wake placement,
priority inheritance and task deletion, and splitting only the port's locks would
expose shared kernel lists to concurrent mutation. [The reviewed
research](scheduler-research.md) ranks removal of shared ownership above
fairness-only changes, and [the partitioned design](scheduler-partitioned.md)
specifies the fork's local queues, publication protocol, object locking, topology
and timing. The global-lock kernel remains its buildable physical control. Every live
stage requires phase-separated percentiles, complete lock and mask attribution and
exact recognition output before a performance claim.
