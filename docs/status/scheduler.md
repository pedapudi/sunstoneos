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

## Host-tested components support a private fork; production uses the control

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
| 0 | Kernel idle tasks, one pinned to each core | [cached configuration](../../platform/a733-smp-cached/FreeRTOSConfig.h#L13) |
| 1 | Diagnostic memory/background load; available for background application work | [latency workload](../../apps/latency/workload.c#L171) |
| 2 | CPU latency application's main task | [latency manifest](../../apps/latency/manifest.json#L4) |
| 3 | PMIC owner, NPU application client and recorded mimus board application | [board tasks](../../apps/board/main.c#L339), [NPU tasks](../../apps/os/npu.c#L294), external application's manifest |
| 4 | USB, radio, watchdog supervision, thermal, audio, Bluetooth, GPU and DMA service work | [board tasks](../../apps/board/main.c#L339), [thermal](../../apps/os/thermal.c#L319), [GPU](../../apps/os/gpu-runtime.h#L4), [DMA](../../apps/os/dma.c#L58) |
| 5 | Type-C and power owners | [board tasks](../../apps/board/main.c#L339) |
| 6 | Available for a bounded urgent service with a demonstrated need | [priority count](../../include/FreeRTOSConfig.h#L38) |
| 7 | Returning CPU terminal tasks and highest-priority latency probes | [CPU return](../../apps/os/cpu-return.c#L60), [latency workload](../../apps/latency/workload.c#L169) |

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


## Series B converts the complete scheduler behind a disabled option

The series-B contract replaces legacy scheduling for every task. A live fork
must exclude the legacy selector and global ready list. The production build
keeps `configSS_PARTITIONED_SCHEDULER` at its default zero. The incomplete API
patch is exercised only by a private linked host fixture, not by native images.
The control kernel remains buildable.

The private adapter implements owner priority FIFOs and CLZ selection,
notification/block-frame transitions, event waits and local delayed heaps.
Creation assigns single-core affinity directly and otherwise chooses the
allowed core with the fewest assigned tasks, breaking ties by lowest index.
CPU0's periodic tick remains; remote due deadlines request their owner.
Control and registry modules provide fixed storage and lifetime references.
These are partial implementation results, not complete public API coverage.

The [API inventory](../../checks/scheduler-kernel-api.json) lists all 94 public
task entry points: 34 route to the private adapter and 60 remain pending.
It distinguishes compile-only paths from behavior-tested paths. The private
patch changes 35 bodies including a creation helper. The live CMake source
chain does not apply that API patch. Port IRQ-exit/yield hooks, object retry
protection, priority inheritance and remaining API conversions block enablement.
Commits `79ed0c90` and `35d35753` preserve this work coherently.

Series B intentionally keeps a ready task on its busy home core even if another
core is idle. Idle pull belongs to series D. Concurrent linked-kernel stress
must count observations of that condition; that measurement is still absent.
No topology-placement or idle-pull implementation is part of this series.
The topology worker owns the unfinished application wiring in `30dc2d80`.

### Publication and retirement changes have focused host evidence

Commit `8eacaa3e` adds an atomic bucket summary as the logical wake-list head.
A publisher performs slot and head release ORs, then the dispatch/SGI handshake.
The old head identifies empty-to-nonempty publication, including nonurgent work.
The owner detaches the head before scanning slot hints and rechecks all pending
work before dispatch. Hint bits never authorize task activation by themselves.
The native `ss_owner_publish` body contains 60 static instructions and no LL/SC
retry. Registry lookup, pin/release and backend callback bodies are excluded.
This count is not a cycle or latency measurement.

The final reference release now executes a DMB before checking closed admission,
paired with the closer's DMB before checking reference retirement. A finite
negative control reproduces a missed retirement notification without that
barrier. A separate negative omits the empty-head SGI and strands nonurgent
hints without an owner interrupt. The migration negative reproduces an early
old-owner acknowledgment followed by a delayed old-route publication.

The exhaustive suite passes 50 cases, including sixteen expected negative
controls; peak resident memory is 199,636 KiB. Owner, notification, registry
and deadline sanitizer/native checks pass. The private adapter compiles for
AArch64 with the converted optional branches selected. Its linked sanitizer
fixture passes 10,000 remote wakes around block-frame eviction, local/event
waits, remote timeout delivery and zero compatibility-lock acquisitions in the
measured local scheduling loops. It links real FreeRTOS task/list code but
advances owner checkpoints explicitly. It does not run concurrent task stacks,
so it cannot establish the required eight-core QEMU or board behavior.
Evidence resides in `series-b/` beneath the task cache.

### Migration routing remains the first correctness blocker

The independent ordering review approves the wake helpers within their scope
and verifies the specific control-slot reuse correction in `35d35753`.
Control FIFO counters persist across incarnation changes; registration uses
that FIFO, and retirement callbacks carry an incarnation checked under
administration. The threaded duplicate-consumer/reuse fixture passes.

The review withholds whole-adapter approval. Control code can remember an old
owner's empty inbox while an old-route publisher still holds a reference.
A later reference-count snapshot does not close routing admission or require
a fresh drain acknowledgment. Migration must serialize old-epoch publisher
admission against transfer, close that epoch, wait for its publishers, then
obtain a fresh old-owner drain acknowledgment. Unconditional forwarding alone
does not establish the retirement or reuse contract. The model demonstrates
the early-ack race; the actual routing-epoch fix remains open.

The review is `series-b/ordering-review/review.md`, SHA-256
`3122dc34a51870affd3a1bdde168c2773dddd70e9781aa4461e74bed9a9dc00d`.
The same review must examine the completed migration and port integration
before a board run.

The supervised production OS baseline builds with the fork disabled; its log
is `series-b-baseline.log`. The current two-core legacy QEMU check passes 256
queue exchanges, affinity, floating-point context, mutex exclusion and
preemption; evidence is `series-b/control-qemu/`. The check index verifies
843 checks. The full integration gate, mixed-affinity
concurrent QEMU stress, all-core fork soak and physical comparisons remain due.
No new board entry or after measurement exists for series B. The loaded wake
p50/p99/maximum baseline is 69.292/159.958/159.167 microseconds on core six;
maximum task-lock wait is 139.333 microseconds. Per-core baseline values remain
in `partitioned/control-quantiles-summary.json`.

## Finish series B before enabling the fork

1. Fix routing-epoch admission and the fresh old-owner migration acknowledgment.
   Extend the actual control fixture with delayed old publishers and a second
   transfer; keep incarnation and pending-hint retirement separate.
2. Complete the 60 pending task APIs and priority-inheritance paths. Pin before
   every TCB read, route state changes to the owner, and exclude legacy
   ready/delayed/suspended list manipulation under the enabled option.
3. Convert queue, semaphore, mutex, event, stream and timer retry paths to
   compatibility object protection. That lock never permits a remote owner-list
   edit. Preserve the FIFO port lock worker's acquire/release implementation.
4. Connect the complete generated fork and owner IRQ-exit/voluntary-yield port
   hooks. Preserve frame/TLS/trace/runtime accounting and CPU0-only global time.
   Run mixed pinned/multi-core-affinity tasks at competing priorities, timeout
   races, deletion with pending hints and migration under real eight-core QEMU.
   Record local scheduling lock counts and idle-core/home-core imbalance.
5. Complete the independent ordering review and full integration gate. Merge
   coordinator-landed lock/topology/probe changes through `bringup`. Submit the
   unchanged latency workload at the existing CPU operating points and compare
   every core against its baseline; then run exact mimus recognition. Never
   retry an image that fails to return without a concrete correction.

### Recovery commits and remaining review scope

The registry/layout commits `5db91643`, `e0136632`, deadline commit `a45da836`
and control commit `2bf3e230` are integrated as private-fork foundations.
`a45da836` copies `390b8880`; they are one implementation. Retirement hook
`e83b200b` is integrated as `f109a9af`, with the last-release ordering repair
in `8eacaa3e`. The API conversion and exact pending list live in `79ed0c90`.
The reviewed reuse correction is `35d35753`. Host compilation and isolated
checks do not approve the incomplete live adapter.

The earlier adapter recovery `00293293` and superseded measurement prototype
`7598e945` remain recovery points. Do not reapply their source over the current
fork. The old IRQ-origin recorder commit `8065115f` is not integrated here;
coordinate recorder changes with the lock/latency worker and take them through
`bringup`. No board, voltage or storage approval is pending for this private
host work. The coordinator lands branches; no push is performed here.

### Images and outstanding validation

No prepared native board image is waiting for a first run. The three native
latency images already have queue entries: `control-before/`, `control-after/`
and `partitioned/control-quantiles/`. The latest image's generated README is
`partitioned/control-quantiles/os-image/sunstone-a733-os-latency/README.md`.
The topology worker owns the capacity probe and its application wiring; no
capacity image is prepared by this series. The supervised baseline is a host
build check; its generated README is
`integration/gate-os-sup-scheduler-rt-1101-517b6e1e82/os-image/README.md`.
Do not submit an old image again for an unchanged hypothesis.

The earlier supervised, resident and resident-audio builds, linked-kernel
checks, register-context checks and eight-core QEMU soak pass under
`integration/`. Those checks precede the new host components and unfinished
integration. The final three baseline builds, integration gate and check-index
verification remain due after implementation. The current status-parser check writes to `series-b/progress/`; it does not
validate scheduler timing.

### Shared files and coordination

- Scheduler ownership covers `patches/freertos-*.patch`,
  `platform/a733-smp-cached/`, `include/FreeRTOSConfig.h`,
  `kernel/scheduler/`, and scheduler CMake wiring. Coordinate edits to the public
  application kit in `include/sunstone_app.h` with application maintainers.
- `work/sched-lock-1101` owns task/ISR lock acquire and release in
  `patches/freertos-smp-c.patch` and latency attribution instrumentation. Scheduler
  owner-entry/exit and conditional-SGI additions use separate hunks. Merge its
  coordinator-landed changes from `bringup`; do not edit its implementation.
- `work/sched-topology-1101` owns topology tables, application query wrappers
  and the native capacity probe/measurement. Merge its coordinator-landed
  changes from `bringup`; this series does not edit those files.
- The probe integration worker, `work/probe-shared-1101`, owns
  `platform/a733/probe.*`. Do not replace its entry code.
- The CPU worker owns CPU clock policy and mimus CPU optimization on
  `work/cpu-perf-os-1031` and external mimus `cpu-perf-1031`. Coordinate the
  topology and work-split API through the partitioned design. The OS builds
  mimus through `MIMUS_DIR` and contains no application-specific scheduler code.
- Queue submission belongs to the board runner. Existing CPU operating points
  are the scope; any new voltage/clock point or storage write needs coordinator
  approval. No such approval is pending for the preserved work.
