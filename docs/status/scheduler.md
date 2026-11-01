# Scheduler traversal is bounded; real-time qualification remains open

**Status: implemented.**

**Board-qualified scope:** The returning CPU controls complete 64 idle and
128 loaded wake samples per core on all eight CPUs. One eight-worker recorded
mimus input preserves exact CRCs and text and returns normally. These records
establish those bounded workloads; scheduler timing exceeds its budgets and
the partitioned fork remains unimplemented.

The eight-core A733 FreeRTOS port implements direct notification wake-ups,
priority preemption, affinity, mutex priority inheritance and wait-for-interrupt
(WFI) idle.
The scheduler runs at 1 kHz with time slicing and eight priorities.
CPU0 owns the only periodic scheduler timer. Tickless idle and a general
interrupt-driven sub-tick deadline API are unimplemented.

[The scheduler design](../designs/scheduler-realtime.md) records source anchors,
the bounded time-slice change, physical maxima and the required sleep protocol.
Lists longer than sixteen tasks use conservative rescheduling; shorter lists
retain affinity filtering. Notification and wake-up interfaces remain compatible.

## Service priorities fit within the current eight levels

Larger numbers preempt smaller numbers on an eligible core. Application
manifests supply their priority; the table describes current services and
reserves the available levels. It does not enforce an application policy.

| Priority | Current use or reservation | Source |
| ---: | --- | --- |
| 0 | Kernel idle tasks, one pinned to each core | [cached configuration](../../platform/a733-smp-cached/FreeRTOSConfig.h:13) |
| 1 | Diagnostic memory/background load; available for background application work | [latency workload](../../apps/latency/workload.c:171) |
| 2 | CPU latency application's main task | [latency manifest](../../apps/latency/manifest.json:4) |
| 3 | PMIC owner, NPU application client and recorded mimus board application | [board tasks](../../apps/board/main.c:339), [NPU tasks](../../apps/os/npu.c:294), external application's manifest |
| 4 | USB, radio, watchdog supervision, thermal, audio, Bluetooth, GPU and DMA service work | [board tasks](../../apps/board/main.c:339), [thermal](../../apps/os/thermal.c:319), [GPU](../../apps/os/gpu-runtime.h:4), [DMA](../../apps/os/dma.c:58) |
| 5 | Type-C and power owners | [board tasks](../../apps/board/main.c:339) |
| 6 | Available for a bounded urgent service with a demonstrated need | [priority count](../../include/FreeRTOSConfig.h:38) |
| 7 | Returning CPU terminal tasks and highest-priority latency probes | [CPU return](../../apps/os/cpu-return.c:60), [latency workload](../../apps/latency/workload.c:169) |

Eight levels cover these classes, but several services share priority four.
Their execution budgets and affinities still require combined-load evidence.
Default OS affinity covers the six A55 cores. Explicit placements admit the
two A76 cores. The latency workload pins its probe and memory task to each
physical core; CPU0's timer wakes remote probes through production SGIs.

The pinned upstream `xTaskPriorityInherit` raises a contended mutex holder and
reschedules an eligible core if that ready holder is not running. Ordinary
release restores base priority only after the holder releases its last mutex.
Timeout disinheritance adjusts a holder with one outstanding mutex. These are
the simplified upstream semantics. Full transitive inheritance and a priority
ceiling protocol require different mechanisms. Avoid nested mutex dependencies
on a bounded response path.
Cross-core priority inversion with a high-priority waiter and medium-priority
interferers still needs an isolated physical test.

## Physical timing and idle counts remain incomplete

Queue entries `1793560470911857163-ce5f22bf` and
`1793560934366050732-4e45b5ea` return normally on port 3-3 with 64 idle and
128 loaded ready-to-running samples on every core. Both fail masking or
lock budgets. Their maxima cannot establish an improvement or repeatability.
Percentiles and complete IRQ-to-task latency remain unavailable.

| Observation | Before | After |
| --- | ---: | ---: |
| Recorded CPU0 timer IRQs, complete recording window | 332 | 329 |
| Recorded secondary timer IRQs, each core, complete window | 0 | 0 |
| CPU0 timer IRQs during an isolated idle window | Unmeasured | Unmeasured |
| Idle power or temperature change | Unmeasured | Unmeasured |

The complete recording windows include startup and both workload phases;
they are different lengths. The totals do not measure idle power savings.
CPU0 continues taking its periodic timer while idle.

The linked tick fixture passes ten cases. The supervised, resident and
resident-audio builds, kernel check and eight-core multithreaded QEMU soak
pass under `~/.cache/sunstoneos/build/scheduler-rt-1101/integration/`.
The two-core kernel and register-context checks also pass. The check index
verifies 800 checks. Host results do not qualify physical timing.

Recorded eight-worker recognition returns normally in queue entry
`1793561595474856811-579a99b6`. Both original CRCs (`ac657a3b`, `a7ade80e`),
raw text `there was silence` and formatted text `There was silence.` match
the host reference. The external application's CPU-manifest correction is
mimus commit `8308b79aa`; its separate branch still requires integration.
The queue records `returned` after an audited offline correction of the legacy
JSON expectation wrapper. The original mismatch and frozen inputs remain
preserved. The amendment has SHA-256
`3175283ed44282b93358fa3eea247534cdf9488b793c64eb0e55b128001a707c`.
Exact reference comparison includes both CRCs, raw/formatted text, marks and
capitalization. The [design note](../designs/scheduler-realtime.md) records the
capture and the preceding application-mapping watchdog failure.

The [scheduler research](../designs/scheduler-research.md) has an independent
source-accuracy review. Two independent reviews approve
[the partitioned design](../designs/scheduler-partitioned.md). It specifies
owner-only runqueues, bounded remote wake publication, notification transactions,
topology-based placement, one-shot timing and staged physical comparisons.
These approvals do not establish implementation or board performance.

Remaining acceptance work comprises phase-separated percentile capture,
tickless timekeeping, sub-tick deadlines, isolated priority inheritance,
idle-power measurement and a sustained physical eight-core soak. The recorded
recognition result covers one input. The scheduler change remains physically
unaccepted while timing budgets and sustained scheduling remain unqualified.
