# Scheduler research and host components; kernel integration remains incomplete

**Status: implemented.**

**Board-qualified scope:** Eight-core CPU controls complete 64 idle and 128
loaded wake samples per core, including queue entry
`1793566204991504422-8a47d893`, which returns normally but exceeds timing budgets.
Recorded mimus recognition preserves exact CRCs and text in entry
`1793561595474856811-579a99b6`. These are bounded workload results. The
partitioned scheduler, heterogeneous placement and tickless timing have no
physical qualification and are not enabled.

## The active kernel still serializes scheduling globally

The eight-core A733 FreeRTOS port implements direct notification wake-ups,
priority preemption, affinity, simplified mutex priority inheritance and
wait-for-interrupt (WFI) idle. It uses one global ready list, shared task and ISR
locks, eight priorities and a 1 kHz tick. CPU0 takes periodic timer interrupts;
secondaries take none. CPU0 tickless idle and a general sub-tick deadline API
remain open. The bounded traversal change does not remove lock contention.

The [research document](../designs/scheduler-research.md), commit `3a9e4db0`,
includes primary-source research on ghOSt and heterogeneous scheduling, with an
independent accuracy review. The
[partitioned design](../designs/scheduler-partitioned.md), commit `84c6ff0c`,
has independent correctness and performance approvals. Their source inventories
and review hashes accompany the documents. Design approval permits staged
implementation; it does not qualify code or board timing.

The design uses owner-only per-core runqueues, bounded remote wake publication,
sequence-validated state, conditional SGIs and topology tables. Fixed priority
comes first. Per-core EDF with constant-bandwidth servers follows measured
fixed-priority, object-lock and timer stages. Dedicated spinning agents are not
the default because they consume a core and idle power.

## Host-tested components are integrated but not connected to scheduling

- Topology tables, validation and capacity-proportional work splitting:
  `b307b961`, `78166181`, `2bb69689`. Host cases cover A733, symmetric,
  three-cluster and SMT descriptions. A733 capacities and energy costs remain
  uncalibrated; clock rates do not substitute for measured capacity.
- Wake-protocol models: `e11e42f8`, `0f08a8bd`. Exhaustive abstract interleavings
  include negative controls for lost wakes and stale notification observations.
  These models do not constitute a complete AArch64 memory-model proof.
- Owner queues and notification transactions: `25ea7f84`, `d888faff`,
  `d3b23ae9`. Focused sanitizer and native-instruction checks pass with
  independent review. These helpers do not yet replace FreeRTOS scheduling.
- Architectural observation and fixed-work capacity probe: `3f90b3d4`,
  `af190b70`, `a030384e`. Host checks cover cache-selector ordering, legacy and
  extended cache geometry, scalar and NEON digests, and clock-epoch rejection.
  Physical MPIDR mapping and the A76/A55 capacity ratio remain unmeasured.
- Phase-separated per-core latency histograms: `d1c8317b`, `f6705937`.
  Host checks and review cover phase closure, report layout and legacy decoding.
  The board baseline below exercises this recording path.

Application topology wiring is preserved in **work-in-progress commit
`30dc2d80`**. The option `SUNSTONE_A733_CPU_TOPOLOGY` defaults to OFF.
The wrappers, boot observation hook and capacity-image selection need focused
wrapper tests, a native link and independent integration review. In particular,
resolve the OPP-table symbol when topology is enabled without the CPU clock
service. No native capacity image is built or qualified from this wiring.

Evidence paths below are relative to
`~/.cache/sunstoneos/build/scheduler-rt-1101/` unless stated otherwise.
Correctness reports and raw fixtures reside in
`partitioned/archived-review-correctness/`; topology and capacity reviews reside
in `partitioned/archived-review-performance/`. The design performance report is
`partitioned/performance-review.md`. Research accuracy evidence resides in
`research/review-evidence/`. Review reports retain their recorded contents and
hashes; generated evidence stays outside tracked source.

## The measured global-kernel baseline exceeds workload budgets

Entry `1793566204991504422-8a47d893`, port 3-3, returns normally with seven
secondaries off and ROM return confirmed. The queue outcome is
`result-mismatch`; the strict timing expectation fails. This is a baseline
measurement, not a scheduler improvement or a passing qualification run.

Each row below reports the core with the largest exact maximum for that metric
and phase. Percentiles are conservative histogram upper endpoints, with sixteen
bins per octave; they can exceed the exact maximum. Cores are not pooled.
Times are microseconds.

| Phase and metric | Core | p50 | p99 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Idle ready to running | 6 | 47.958 | 90.625 | 87.208 |
| Loaded ready to running | 6 | 69.292 | 159.958 | 159.167 |
| Idle task-lock wait | 6 | 3.625 | 63.958 | 74.208 |
| Loaded task-lock wait | 6 | 10.625 | 111.958 | 139.333 |
| Loaded task-lock hold | 1 | 3.792 | 11.958 | 14.208 |
| Loaded ISR-lock wait | 0 | 0.167 | 7.292 | 10.125 |
| Loaded ISR-lock hold | 0 | 1.125 | 11.958 | 16.708 |
| Loaded interrupt masking | 7 | 8.292 | 79.958 | 115.833 |

Initialization task-lock wait reaches 151.417 microseconds and interrupt masking
reaches 264.542 microseconds. Workload masking and lock waiting exceed the
25 microsecond budget. Complete IRQ-to-task origin capture is not in this
baseline; its pending implementation needs review and a new control run.

- Image SHA-256:
  `49c60251609cb0c0f004590716d31bc5ad054cb43014a33083545fd3b48600a1`.
- Output SHA-256:
  `36d653012e99c8feab4e9163e71a80ee54d3945d7188bb3c89572ab30ba4bcd8`.
- Per-core distributions: `partitioned/control-quantiles-summary.json`.
  Complete decoded capture: `partitioned/control-quantiles-decoded.json`.
- Authoritative queue result:
  `~/.cache/sunstoneos/board-queue/entries/1793566204991504422-8a47d893/run/result.json`.

The earlier controls `1793560470911857163-ce5f22bf` and
`1793560934366050732-4e45b5ea` return normally but fail timing budgets.
Their task-lock wait maxima are 122.917 and 177.875 microseconds; loaded wake
maxima are 111.458 and 133.333 microseconds. Those captures lack percentiles.
The latency worker's CPU control `1793559978973913698-7f7e36e0` records workload
masking of 62.750 microseconds and task-lock wait of 73.417 microseconds.
Workload and recording differences prevent attributing cross-run differences to
an implementation improvement.

CPU0 timer counts are 332 and 329 over the two earlier complete recording
windows; secondary counts are zero. Window lengths differ and include startup
and load. Isolated idle wake-ups per second, idle power and energy per job
remain unmeasured. No tickless power improvement is claimed.

## Recorded recognition preserves the reference output

Entry `1793561595474856811-579a99b6` returns normally with feature CRC
`ac657a3b`, log-probability CRC `a7ade80e`, raw text `there was silence` and
formatted text `There was silence.`. Marks and capitalization match the host
reference. The external CPU application manifest correction, mimus commit
`8308b79aa`, still needs integration by its maintainer or the coordinator.

The queue records `returned` after an audited offline repair of the legacy
JSON expectation wrapper. The original expectation, mismatch and frozen inputs
remain preserved. The amendment SHA-256 is
`3175283ed44282b93358fa3eea247534cdf9488b793c64eb0e55b128001a707c`.
It resides beneath the entry at
`corrections/expectation-format-20261101/amendment.json`.
The [existing design note](../designs/scheduler-realtime.md) records the exact
reference and the earlier application-mapping watchdog failure. This result
covers one input and predates the unfinished scheduler fork.

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


## Resume implementation and validation in this order

1. Complete the disabled topology integration in `30dc2d80`. Resolve its clock
   table dependency, test public queries with missing and valid observations,
   and build the native capacity application. Obtain an independent feature
   review before enabling it. Capture full MPIDR/cache identity and the fixed
   scalar/NEON throughput on both CPU types through the queue.
2. Review the unmerged commits in the table below. The retirement notification
   needs particular attention to the last-reference release versus close race.
   IRQ-origin recording needs validation against the actual timer compare.
   Merge only approved components into the scheduler branch.
3. Finish the FreeRTOS adapter in recovery commit `00293293` on
   `work/scheduler-freertos-fork-1101`. Convert every enabled task API, connect
   control/deadline/retirement paths, and add IRQ-exit and explicit-yield port
   hooks. Keep the fork disabled until linked-kernel cases and eight-core QEMU
   stress pass and an independent code reviewer approves it.
4. Update the design's instruction-cost accounting to match the implementation:
   owner wake draining can inspect 256 words per bounded batch; the permanent
   handle registry can require 256 probes. Measure common and worst paths.
   The design's earlier scan estimate does not describe these bounds.
5. Compare each enabled stage with the latency control on the board, including
   exact IRQ-to-task origins, per-core distributions, context switches, masking
   and idle wake-ups. Run the eight-core soak and exact mimus recognition after
   each stage. Merge current `bringup` into the scheduler branch before each
   future submission; preserve the probe worker's entry code.
6. Implement and measure per-object locking, one-shot timers and capacity-aware
   placement in separate stages. Compare equal versus capacity-proportional
   mimus splitting with the CPU worker. Measure big-pinned versus unpinned
   critical tasks and energy per job only if calibrated supply data exists.
   EDF reservations follow those measurements.

### Pending code reviews and recovery branches

All branches below are local worker branches. Their host checks are author
checks; their new code has no independent approval yet. A reviewer independent
of each author supplies the feature approval. The coordinator decides what to
land in `bringup`. No new review or board run is requested during wrap-up.

| Branch suffix after `work/scheduler-` | Commit | Remaining review or integration |
| --- | --- | --- |
| `percentiles-1101` | `8065115f` | IRQ-origin packing, producer and decoder accuracy |
| `owner-core-1101` | `e83b200b` | Last-reference retirement notification and memory ordering |
| `freertos-fork-1101` | `5db91643`, `e0136632` | TCB layout, permanent registry and reclamation |
| `control-1101` | `d06e0d63` | Remote controls, global suspension rendezvous and migration |
| `deadline-1101` | `390b8880` | Deadline heap, cancellation and timeout races |
| `freertos-fork-1101` | `00293293` | Incomplete adapter; finish before review or integration |

The fork branch contains deadline commit `a45da836`, a copy of `390b8880`.
Avoid integrating duplicate changes as separate implementations. Recovery
commit `7598e945` on `work/scheduler-measure-1101` preserves a superseded
measurement prototype; do not merge it. The reviewed histogram implementation
is already integrated. Worker worktrees and generated evidence remain under
the task cache, with no active workers.

### Images and outstanding validation

No prepared native board image is waiting for a first run. The three native
latency images already have queue entries: `control-before/`, `control-after/`
and `partitioned/control-quantiles/`. The latest image's generated README is
`partitioned/control-quantiles/os-image/sunstone-a733-os-latency/README.md`.
The capacity probe has host artifacts only; its application wiring is incomplete.
Do not submit an old image again for an unchanged hypothesis.

The earlier supervised, resident and resident-audio builds, linked-kernel
checks, register-context checks and eight-core QEMU soak pass under
`integration/`. Those checks precede the new host components and unfinished
integration. The final three baseline builds, integration gate and check-index
verification remain due after implementation. The wrap-up status-parser check
writes to `wrap-up/progress/`; it does not validate scheduler timing.

### Shared files and coordination

- Scheduler ownership covers `patches/freertos-*.patch`,
  `platform/a733-smp-cached/`, `include/FreeRTOSConfig.h`,
  `kernel/scheduler/`, and scheduler CMake wiring. Coordinate edits to the public
  application kit in `include/sunstone_app.h` with application maintainers.
- The latency worker owns `runtime/latency*.c`, `include/sunstone_latency.h`,
  `apps/latency/`, timing budgets and combined-load images. Preserve its
  instrumentation and merge later latency commits before comparisons.
- The probe integration worker, `work/probe-shared-1101`, owns
  `platform/a733/probe.*`. Do not replace its entry code.
- The CPU worker owns CPU clock policy and mimus CPU optimization on
  `work/cpu-perf-os-1031` and external mimus `cpu-perf-1031`. Coordinate the
  topology and work-split API through the partitioned design. The OS builds
  mimus through `MIMUS_DIR` and contains no application-specific scheduler code.
- Queue submission belongs to the board runner. Existing CPU operating points
  are the scope; any new voltage/clock point or storage write needs coordinator
  approval. No such approval is pending for the preserved work.
