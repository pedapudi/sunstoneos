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

## Real-time cores may need to exclude streaming work

Status: **open decision** for the robotics target, which runs camera and lidar
pipelines beside control loops.

Streaming memory traffic evicts the scheduler's working set from the private
caches and the shared L3 between ticks. In [the latency cause
measurement](scheduler-latency-measurement.md#cause-capture-separates-release-barrier-and-line-transfer-costs)
(evidence entry `1794504237145636168-9aa4ebd9`), each core of the latency fixture
streams 1 MiB in the busy phase. A busy CPU0 tick then averages 314,510 cycles
with 220 attributable L3 refills, about 1,268 cycles per refill. The same tick in
the idle phase, with no streaming on any core, averages 36,541 cycles (about
20 µs at 1794 MHz) with one L3 refill. Both figures include the measurement's own
hooks.

Two mechanisms can keep the working set resident:

- **Core isolation**, a scheduler policy: real-time cores run no streaming work,
  and bulk producers and consumers run only on the other cores. A real-time core
  then evicts only its own lines. The idle-phase tick bounds what isolation alone
  can give: about 36.5k cycles with the hooks. Streaming on other cores still
  evicts shared L3 lines, so the bound holds only while the real-time core's
  working set fits in its private L1 and 128 KiB L2. Device DMA into memory that
  a real-time core reads is a separate question.
- **L3 partitioning** by DSU scheme ID: a reserved way group keeps the
  real-time cores' lines in L3 whatever the other cores stream. It takes 256 KiB
  (25 percent) of the L3 from the other cores per group. It needs EL3 enable bits
  and per-core scheme registers. It does not protect a real-time core from its
  own streaming unless the scheme changes on each context switch.

Isolation alone was measured with streaming confined to CPUs 4-7 and CPUs 0-3
idle between samples (build option `SUNSTONE_LATENCY_STREAM_MASK=0xf0`, evidence
entry `1794597393456307577-46a485eb`; the footprint cuts and the owner SGI fix are
in both images). Against all eight cores streaming (entry
`1794508626388886966-ae6d2457`):

| Busy-phase mean over complete ticks | All cores stream | CPUs 4-7 stream | Idle phase |
| --- | ---: | ---: | ---: |
| Whole tick, cycles | 212,455 | 76,410 | 33,304 |
| Whole tick, L3 refills | 140.1 | 26.5 | 0.7 |
| CPU0's own release, cycles | 61,627 | 7,839 | 2,227 |
| Releases to CPUs 1-3, cycles each | 10,269-30,250 | 4,947-7,519 | 2,616-2,680 |
| Releases to CPUs 4-7, cycles each | 7,819-11,970 | 4,250-8,671 | 2,636-2,672 |
| Outside the releases and scans, cycles | 46,703 | 23,096 | 10,840 |
| CPU0's longest busy wake | 228 µs | 93.1 µs | 28.8 µs (idle wake) |

Isolation removes about three quarters of the busy tick's added cost but does not
reach the idle tick. About 26 lines per tick that the idle tick finds in the
shared L3 come from DRAM while other cores stream, at about 1,670 cycles each
beyond the idle cost. Those lines are spread over every segment, including the
releases to idle cores. They are what an L3 partition would keep, so the
partition measurement is the next step for the remaining 43,000 cycles.

The stream mask is a measurement build option. In a product the real-time core
set should come from the board and application declarations, so that the
scheduler places streaming work away from the declared cores; it should not be a
build option.

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
microsecond deadline or a notification on its index. A fixed table of 16 slots
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
