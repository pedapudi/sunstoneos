# Scheduler research and host components; kernel integration remains incomplete: historical status record 2026-11-01

This log holds the prose evidence of [the current status](../status/scheduler.md) as it stood on the date above. It is not updated. Board results that have queue entries are in the records `docs/evidence/scheduler.jsonl`.

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
reference. The reference uses external CPU application manifest correction `8308b79aa`.
Current application pins are maintained in `apps/mimus/source.json`.

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

The [API inventory](../../checks/scheduler-kernel-api.json) lists 94 public
task entry points: 82 have private routes, including all 81 selected by the
validation configuration. Definition checks verify the intended helper at each
function definition. The Clang audit follows calls and address-taken callbacks;
154 reachable functions have no legacy ready/delayed/suspended-list access.
Twelve optional APIs remain compile-time rejected: dynamic allocation, MPU,
tickless operation and formatted statistics. These guards prevent a configured
API from silently entering the legacy selector. The live CMake chain still
uses the control kernel; no production fork is enabled.

Private object-source patches route queue, semaphore, event, stream and timer
retry paths through compatibility object protection. Public task critical
sections serialize callers through the retained task/ISR locks and disable
local preemption; ISR critical sections use the ISR lock. Other owners continue
dispatching. Global scheduler suspension uses the separate acknowledged gate.
The unsupported port scheduler-end operation asserts directly rather than
running legacy idle-task deletion.

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

The exhaustive suite passes 52 cases, including seventeen expected negative
controls; peak resident memory is 200,364 KiB. The actual routing fixture pauses
a publisher across two transfers. Removing epoch validation from a generated
copy makes that same fixture reject stale admission. The positive sanitizer
fixture also covers an admitted delayed publisher, fresh inbox drain, continued
new-route publication and deletion before adoption. Its native object contains
neither LL/SC loops nor the test pause hook.

The private adapter compiles for AArch64 in both modes, with all four object
sources selected. Linked sanitizer cases pass 10,000 remote notification races,
public startup/reset, static creation/reuse, migration, object waits and timer
commands, nested critical sections, runtime statistics, preemption modes,
TLS/errno and trace hooks. Measured local scheduling loops acquire zero
compatibility locks. These fixtures advance owner checkpoints explicitly;
they do not execute concurrent task stacks.

CPU0 reads owner-published slice deadlines and sends only due slice requests.
Six secondary-core FIFO rotations pass without timeout deadlines. Single-task,
idle and preemption-disabled owners receive no slice SGIs in those cases.
The first local equal-priority wake arms a slice without another scheduling IRQ.
The fixtures record one scripted observation of ready work on a busy home core
beside an idle core. A concurrent imbalance frequency remains unmeasured.

Ready trace hooks run in the completing caller before wake publication/SGI;
public entry/return hooks preserve single evaluation. Cross-core current-task
queries use atomic snapshots. An independent owner entry can occur between
logical completion and the producer hook. Concurrent QEMU must validate origin
attribution and sample completeness before latency-image submission.
Evidence resides in `series-b-round2/` beneath the task cache.

### Routing epochs delimit migration publishers and fresh acknowledgments

Each task has a packed owner/epoch word and two publisher-count banks, with a
closed flag in the same atomic word as each count. A caller holds its lifetime
pin, samples the route, registers once, executes a DMB and validates the word.
An accepted token covers hint publication and SGI completion. Rejection uses
one bounded activation broadcast to all owners, with no retry or shared lock.
The lifetime pin covers that broadcast. Owner inspection and adoption validate
the actual wait outcome; a hint never authorizes activation by itself.

Migration closes the old bank before detaching the task. TRANSFERRING prevents
either source nor destination from enqueueing it early. The destination restores
local deadline/state ownership and publishes the new route after adoption.
The source acknowledges only after its closed bank reaches zero and its live
and detached hints drain. New-route publishers and unrelated lifetime pins do
not postpone that acknowledgment. Reopening a bank preserves transient rejected
readers, preventing lost decrements and stale two-bank admission.

The independent ordering review approves the control module and private glue
through `2820f2f5`, including broadcast fallback, retirement, atomic current-task
snapshots and slice-deadline publication. It also approves the permanent FIFO
and incarnation-checked retirement callback correction. The review does not
approve live port wiring, concurrent stacks or physical timing.
It is `series-b-round2/review/review.md`, SHA-256
`9ffc9af95a82236b5f21210a16ddb1ad01a3dbc473556771c41537f67d0390ad`.

The full control-kernel integration gate passes at revision `0d0d831c`, including
the supervised/resident variants, legacy eight-core soak, indexed host checks
and check index. Evidence is `integration/gate-scheduler-b-round2/`.
This validates the disabled control, not concurrent execution of the fork.
Subsequent slicing/trace changes affect only the private fork and pass its
focused checker. The source includes `bringup` through `a96c8c1a`, including
the runtime-deadline repair for application waits; future images must retain it.

No series-B board entry or after measurement exists. The port 3-3 baseline
loaded wake p50/p99/maximum is 69.292/159.958/159.167 microseconds on core six;
maximum task-lock wait is 139.333 microseconds. Per-core baseline values remain
in `partitioned/control-quantiles-summary.json`.
The lock worker's matched port 3-1 references are test-and-set entry
`1793575947324491365-4f50a160` and tickets entry
`1793576137408460593-a3232b16`. Their queue results both record `returned`,
normal return and ROM return. They are another chip from the port 3-3 baseline
and are not partitioned-kernel results. The lock worker owns their per-core
timing analysis and instrumentation.

## Finish series B before enabling the fork

1. Connect the generated task/object fork and owner IRQ-exit/voluntary-yield
   port hooks. Preserve the lock worker's acquire/release hunks and the current
   task/FPU/critical/TLS ABI, including native EL0 service calls. Propagate the
   fork configuration and private static-task header through the application kit.
   Link without legacy dispatch paths and verify the real newlib layout.
2. Complete functional edge cases before concurrent admission: clean up TLS
   when static-task registration fails at capacity, and verify public control
   requests against the no-object-lock/IRQ-enabled acknowledgment precondition.
   Then run mixed pinned/multi-core-affinity tasks under real eight-core QEMU,
   including competing priorities, wake/block/timeout, pending deletion, repeated
   migration and slicing. Record scheduling lock counts, ready-origin fidelity
   and busy-owner/idle-core frequency. Idle pull stays D.
3. Complete the port integration portion of the ordering review, repeat affected
   checks and the full gate for the completed fork, and keep the control build
   available. Merge coordinator-landed lock/topology/probe changes from
   `bringup`, retaining the startup-wait fix.
4. Submit the unchanged latency workload at existing CPU operating points on
   port 3-3, respecting shared queue ownership. Compare per-core wake and lock
   distributions against the pinned same-board control, then run exact mimus.
   Do not repeat an image that fails to return without a concrete correction.

### Recovery commits and remaining review scope

The registry/layout commits `5db91643`, `e0136632`, deadline commit `a45da836`
and control commit `2bf3e230` are integrated as private-fork foundations.
`a45da836` copies `390b8880`; they are one implementation. Retirement hook
`e83b200b` is integrated as `f109a9af`, with the last-release ordering repair
in `8eacaa3e`. The current API conversion and selected configuration are recorded in
`checks/scheduler-kernel-api.json`. Routing-epoch closure is `53549546`, its
executable/model races are `652cb8e5`, and private API/trace coverage reaches
`2820f2f5`. The reviewed reuse correction is `35d35753`. Host compilation and
isolated checks do not approve live port integration.

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
