# Scheduler step timing

Status: implemented and host-tested. Physical results qualify the image
revisions in the evidence records. Scheduler latency targets remain unmet.
[The evidence records](../evidence/scheduler-latency-measurement.jsonl) contain
the predictions, queue outcomes and host-check results.
The CPU latency application records scheduler command, control-drain,
interrupt-exit and lock-wait intervals when `SUNSTONE_SCHED_LATENCY_TRACE` is
enabled. The recorder uses the architectural virtual counter and preserves raw
ticks. The result contains its counter frequency.

## The reference sequence preserves task ownership and interrupt ordering

The FreeRTOS reference is revision
`97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2`, pinned by
[`sources.lock.json`](../../sources.lock.json). Its provenance and MIT license
are recorded in [the source inventory](../sources.md). The cached files are
under `~/.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/`.

| Reference file | SHA-256 |
| --- | --- |
| `tasks.c` | `6de295f333f31818de546d6eb72b877c5a6964980f14f80bc1e17b19b7ff9fc4` |
| `portable/GCC/ARM_CR82/port.c` | `a50f39eba3ae851582abe69820a3f99ec3bd36cdbe53e2d33e4c4ddd29339cbe` |
| `portable/GCC/ARM_CR82/portASM.S` | `58d09b6ce2e19189db7e02b278ade9d7cf9a4bbe0411117ebf5cfd0973f1a69c` |

Upstream `vTaskSuspend` enters a critical section, takes the global task and
interrupt locks, removes the task from its scheduling and event lists, and
marks it suspended. It clears notification wait state and requests local
rescheduling or remote eviction before leaving the critical section.

The [partitioned adapter](../../kernel/scheduler/freertos.c) serializes general
control changes through a per-task FIFO. Self-suspension borrows the current
task's execution lifetime and publishes atomic suspension and saved-frame bits
before masking. It leaves no counted reference on a stack that might remain
suspended. Under the mask it changes only the calling task's local queue state
and enters the mandatory saved-frame selection. It drains no control records.
Resume clears logical suspension while preserving the saved-frame bit; only
the owner clears that bit after the frame is saved. Migration and deletion
also require their saved-context acknowledgments.
An existing caller lock defers selection until outer release while preserving
the local invalid state. Its deferred command record covers the local commit;
it does not claim to time the later switch.

General maintenance attempts at most `SS_OWNER_BATCH` records, sixteen in the
default single-pass configuration. A visited bitmap spans the maintenance
entry, so a reissued hint cannot cause another visit in that entry. FIFO,
notification, object and administration acquisition use one nonblocking
attempt. A busy resource parks its record until the matching release signals
it. Parking publishes the pending and parked bits before the resource cookie;
registration then checks the resource again to close a concurrent release.
The record preparation admits API interrupts with both the priority mask and
`DAIF.I` open. Short local queue and object-list commits raise the API mask.
Nested interrupt exits retain their pending work and cannot reenter the owner
or switch its task while preparation is active.

The control unlock signals waiters before reference release or retirement
callbacks. A second event follows that work when present; a record without it
needs only the unlock event. Deferring the first event across callbacks can
strand a waiter, so these events are not combined across records. The
diagnostic timestamps both boundaries. General nonself handle admission and
publication still use the API mask; their FIFO wait has no established
twenty-five-microsecond bound.

The upstream interrupt entry saves general and floating-point registers,
increments nesting, acknowledges the interrupt, and calls the C handler. It
then masks IRQ entry, signals end of interrupt, unwinds nesting, switches tasks
when required, and restores context. The native port adds owner maintenance
after end of interrupt and before selection. Its priority-mask and active-owner
guards exclude reentry. Control preparation opens the API mask between local
commits; the remaining owner queue and deadline work uses the API mask.
A clean interrupt wake can switch directly when a higher-priority task is
already ready and no control, repair, due deadline or expired slice requires
maintenance. Saved-frame selection and its final publication checks still run.
The detailed IRQ trace covers the priority-mask guard, eligibility check and
maintenance or direct-switch decision. It records saved-frame selection
separately. The partitioned SGI handler leaves the yield decision to owner
exit; an empty SGI does not request selection. Existing tick and ISR yield
requests remain valid. An API yield inside a private object section is deferred
until outer release, so port restoration cannot reopen its interrupt lock.

CPU0 alone advances global scheduler time. Its timer handler processes at most
three overdue ticks per hardware interrupt and publishes due-owner work.
It marks local rescheduling directly and sends SGIs only to remote due owners;
it does not call owner maintenance inside the tick. The diagnostic records the
complete C dispatch interval, including the latency workload's wake hook.
That hook admits API interrupts between its eight notifications. Selection
waits for the outer interrupt to finish. Interrupt-exit owner work has separate
timestamps. The A733 interrupt and CPU identity rules
are in [the multicore reference audit](../a733-multicore-reference-audit.md).

## Records retain complete maxima and expose missing observations

Every CPU retains the longest command and the self-suspend with the longest
interval from the mask call to its C selection marker.
General commands record reference, publication, drain and
slice boundaries. Direct self-suspension instead records the unmasked borrowed
reference and publication, then mask admission and local commit, frame save and
C selection. The field named `masked_c_ticks` starts before the mask call and
includes admission; it cannot establish continuous exclusion without a matching
exception-span record. It also excludes recorder finalization and final port
restoration. Separate per-step maxima need not belong to the same command.
The longest drain retains its individual records; the longest interrupt exit
retains its maintenance totals.

The optional exception-span recorder retains API exclusion through the final
software return hook. It includes intervals with `DAIF.I` clear while PMR still
excludes API interrupts. Its generic FromISR close can defer when `DAIF.I` is
set, without a later endpoint when the port opens both masks. A serialized
endpoint when PMR admits API interrupts and I is clear remains required.
Intervals beginning at the batch-end remask cover subsequent masked work.

The application declares storage for sixteen waits per CPU above five
microseconds, sixteen recent interrupt exits and selections per CPU, and 512
CPU0 timer intervals. Overflow counters identify omitted
records; the longest wait is retained independently. These capacities limit
diagnostic storage and do not limit scheduler work. Generation-checked atomic
holder metadata identifies the holder observed when a waiter arrives.
Unmatched releases, competing predecessors and incomplete timer coverage leave
the cause unclassified. A timer overlap establishes observed interference,
without attributing the complete wait to that interrupt. The actual task-lock
FIFO reservation and acquisition timestamps distinguish contention from the
surrounding wrapper work. An empty FIFO does not establish the cause of that
remaining work.

Version five links each busy wake to the latest completed selection of its
task before the captured task-entry timestamp, then finds that selection's
interrupt exit. Events after task entry cannot become its selecting event.
The report retains the longest busy wake, fast, slow and unmatched sample
counts, and the number and work of intervening interrupt exits. A missing
required history entry is explicit. An intervening interrupt leaves the wake
unmatched for single-exit attribution while preserving its selecting event.
CPU0's longest wake retains the enclosing window of sixteen IRQs and sixteen
selections, including interrupt IDs, interrupted state and owner intervals.
Atomic generation checks reject overwritten history rows in one bounded copy;
an incomplete copy remains explicit. Only task identity capture uses the API
mask; history copying runs with that mask open.

The application waits until every memory worker is suspended before the wake
tasks freeze their per-CPU recorders. This includes the simultaneous suspends
after memory traffic ends. The final wake-task suspends occur after freezing.
The diagnostic acknowledgment query waits for suspended execution and the
owner-cleared saved-frame bit. A logical task-state query alone cannot
establish that boundary.
The report appends a versioned, aligned tail to the existing latency result;
the diagnostic manifest declares a 128 KiB output capacity.

The timer reads and record copies add work. Instrumented durations include that
cost, and no constant overhead is subtracted. The existing workload takes 64
idle and 128 busy wake samples per CPU. It supports exact observed maxima and
phase percentiles, but cannot establish a 99.99th-percentile bound.

## Per-core PMU windows retain separate event banks

`SUNSTONE_SCHED_PMU` adds counters around saved-frame C selection, a control
record body and CPU0's instrumented tick handler. It owns the PMU exclusively;
legacy span counters, application PMU accounting and control qualification
cannot share this profile. Arming follows the application's top-clock check.
Only the measured EL1 CPU configures or reads its counters. Capability and
configuration checks follow [the CPU PMU interface](../cpu-pmu.md).

The six event counters require two banks. Both contain L1 data refill, L1
instruction refill, L2 refill, data TLB walk and instruction TLB walk. The sixth
counts attributable L3 refill (`0x2a`) in one bank and backend stalls (`0x24`)
in the other. The cycle counter runs at full rate with 64-bit overflow. Core
capability bits identify unsupported events. L3 refill is a core-attributable
event, not a whole-cluster traffic counter.

Each window uses a synchronous stack-object address as its scope. Admission
masks local IRQ entry until the scope and internal sequence are published.
Counter enable follows all capture-state stores. The end entry validates the
core and scope, then masks IRQs across counter freeze and endpoint reads.
It restores the prior IRQ state before its first spill or result store.
A nested rejected scope cannot end the outer window. Rejection
updates no recorder state while the outer counters run, so its count remains
explicitly unavailable.

The report retains the maximum-cycle invocation per core, kind and bank, or an
error sample. Its sample count includes every completed invocation. Banks are
different invocations and are never joined into one sample. Tick windows
alternate banks; other kinds select the bank from the core index. A missing
bank is distinct from an unsupported counter. Raw empty-window references are
retained without automatic subtraction. A blank may span less than one generic
counter tick while still counting CPU cycles.

Selection's trace helpers lie outside its PMU window. Control and tick windows
include their existing latency observers; the new PMU result writes follow
counter freeze. The tick window includes the fixture's notifications and the
production handler. It differs from a wake's remaining handler time after task
readiness. Port restoration and the final mask observer are outside C selection.
Cycle-to-counter ratios have boundary skew and do not establish continuous
frequency between the application's clock checks.

Each pinned wake worker freezes its own PMU recorder before publishing its
parked acknowledgment. The controller acquires all eight acknowledgments before
copying the 11,712-byte report. A snapshot without all freezes or with an active
scope is marked incomplete. The decoder exposes capabilities, overflow, missing
banks, invalid samples and clock ratios without turning their zeros into
measurements.

The idle hook executes `DSB SY; WFI`. Suspend, selection and cross-core
notification contain no cache maintenance or TLB invalidation. The application
space hook may change TTBR0 with an ASID; it issues no TLBI. Boot, terminal
retirement, optional invalidation diagnostics and device DMA maintenance have
separate paths. Source inspection does not establish cache retention in WFI.
CPU0's latency tick has eight possible direct notification targets, the pinned
wake tasks. Other OS waits become ready through deadlines rather than these
eight notification calls.

## Cause capture separates release, barrier and line-transfer costs

`SUNSTONE_LATENCY_CAUSE` tests two explanations for the backend stalls in
CPU0's tick. Under the first, cache lines that other cores own migrate to
CPU0 on each remote release, so cost follows the lines touched and the
line-transfer time. Under the second, a DSB that follows a GIC write waits for
that device write to complete, so cost follows the count of such barriers. The
profile owns the cycle counter exclusively and refuses the scheduler PMU
windows, span PMU recording, PMU accounting, control qualification and
interrupt counting. Each CPU enables its own cycle counter, with EL0 reads
permitted, in its first interrupt after arming.

The capture has three parts.

1. **Tick segments.** Around each of the fixture's eight notifications the
   tick records the release interval, the SGI sub-interval (from the owner
   barrier before the SGI to the end of the SGI write sequence) and the
   interrupt-mask restoration at the end of the notification. Around each
   core's step in the scheduler tick's deadline scan it records the scan
   interval and any SGI that the scan forces. The report retains the
   longest tick in which all eight targets were released, with absolute
   cycle stamps, and keeps per-target counts, totals and maxima for the idle
   and busy phases.
2. **Barrier benchmark.** After the workload stops, CPU0 runs one sample per
   tick of fifteen instruction sequences with IRQs masked: an empty body,
   ISB, DSB ISH, DSB SY, a store alone and followed by each DSB, an
   `ICC_SGI1R_EL1` write alone, followed by each DSB, and followed by ISB and
   each DSB, and an unchanged `ICC_PMR_EL1` write alone and followed by DSB
   SY. Each case has 64 raw samples between ISB-synchronized counter reads.
   SGI cases send the production reschedule SGI to CPU1, and the next sample
   waits until CPU1 has taken it.
3. **Line exchange.** CPU0 passes one write-back cache line back and forth
   256 times with CPU1 (same core type) and with CPU6 (Cortex-A76), after 256
   local store-and-load references. CPU0 times each round trip on its own
   counter; half a round trip estimates one transfer and includes the peer's
   polling. One 250 ms deadline bounds the exchange.

Event counters 0 to 4 count L1 instruction refills (`0x01`), L1 data refills
(`0x03`), L2 data refills (`0x17`), attributable L3 data refills (`0x2a`) and
backend stall cycles (`0x24`) at EL0 and EL1. Each tick segment records their
differences beside its cycle stamps; the counters are 32 bits wide, so every
difference is taken modulo 2^32. The report keeps these differences for the
retained tick and, per phase, sums over complete ticks (all eight releases and
scans): whole tick, each release, each scan, and the remainder outside them.

The report occupies 16,432 bytes and the exchange report 3,392 bytes; a
64-byte footer gives both offsets. The profile declares a 147,456-byte output
limit to hold them. All recorded intervals are raw: hook
bookkeeping stays inside them, and the benchmark's empty body and an empty
release-hook pair give the overhead references. Every hook is compiled only
when the option is set, so production images carry none of this code.

The cache lines behind each segment come from the eight-core QEMU model.
`tools/qemu-tick-footprint.c` is a QEMU plugin that records CPU0's
instructions and data accesses between the tick hooks, keeping only ticks whose
fixture phase load returns the busy value. `tools/tick-footprint.py` splits each
tick at the release and scan hooks, resolves every data address to its variable
and field through DWARF and every instruction to its source line, and counts the
lines each segment touches first in the tick. It classes lines as operating
system, exception stack, latency fixture or diagnostic. When the busy traffic
evicts the working set between ticks, the first-touch count of a segment
estimates its refills, which the board's event counts then test.

`checks/tick-footprint-budget.py` holds these counts to
`checks/tick-footprint-budget.json`. For CPU0's own release, the largest remote
release, the largest deadline scan and the work outside them, it takes the
largest line count of each structure over 80 busy ticks and fails if any count
exceeds its budget. Data lines are physical 64-byte lines. Code lines are
counted from the start of each function, so a change that only moves a function
leaves its count alone; the board's placement can add up to one line per
function.

## The control phase retains enough samples for the requested percentile

`SUNSTONE_LATENCY_CONTROL` adds a control phase to the latency application.
`SUNSTONE_LATENCY_CONTROL_MODE=busy` retains memory traffic and measures local
control interrupts and interrupt-woken tasks on cores 1 through 7 at 1, 2, 4,
8, 10, 4 and 4 kHz. Mode `idle` stops memory traffic and measures only control
interrupts at 10 kHz on all seven cores. The controller waits twenty
milliseconds between progress checks. No ring-consumer task wakes each tick.

Every selected task path requires 10,000 accepted samples. Control paths
require 10,001 entries, providing 10,000 period-error samples. The runtime
preserves missed periods, notification errors and incomplete counts. An error
stops collection; all writers acknowledge stop and all measurement tasks
suspend before the application snapshots results and freezes mask recording.
The outer control IRQ wrapper consumes its owned PPIs before the base latency
wrapper, which continues to record CPU0's scheduler timer.

The aligned control tail occupies 17,536 bytes. Without scheduler tracing the
complete report occupies 81,408 bytes; with the declared version-five tracing
capacities it occupies 131,072 bytes. The manifest grants 131,072 output bytes
and a fifteen-second application deadline. Control qualification rejects PMU
accounting and per-span PMU recording because their counters conflict with the
task-path refill measurement.

The decoder validates identities, sample totals, histogram endpoints and
shutdown acknowledgments. It compares deadline-to-task-start latency at the
99.99th percentile with ten microseconds, and recorded mask and task-lock
maxima with twenty-five microseconds. Handler-entry delay, handler-to-task gap
and control-period jitter remain separate distributions. A result that meets
those recorded targets does not qualify startup, omitted exception work or
final-image repeatability. The restore-span flag identifies the optional
software exception interval; its entry-save prefix and post-timestamp
observer, temporary restore and exception-return work remain outside that
measurement. Physical busy sampling and matched idle-jitter runs with and
without tickless idle remain unqualified.

## Startup and workload use distinct timing boundaries

Version five carries restore-span build metadata in the upper two bits of its
tick-capacity header word; the lower 32 bits retain the capacity and the middle
bits are reserved. The validity bit distinguishes an explicit OFF value from
an older header with unknown metadata. With restore spans enabled, the startup
endpoint follows the return observer's bookkeeping and excludes the timestamp
store, final temporary register restoration and exception return. With restore
spans disabled, it remains the port restore marker. The decoder checks this
metadata against a control tail when present and keeps an older version-five
endpoint conditional when no build metadata is available.

Version four marks the primary CPU immediately before it sets the
scheduler-running flag. Assembly records each core's first context restoration
immediately before restoring the final register pair, changing stack selection
and returning from the exception. The primary start marker is the common
origin; other cores' start markers remain zero. The interval includes timer
setup and secondary readiness. The final stores, register restore and exception
return remain outside the measured interval, so it is a lower bound on masking
after the scheduler starts. Earlier report versions mark port entry and restore
entry and have a narrower scope.

Top CPU clocks are established later by the scheduled clock and thermal
services. The application verifies CPU-L 1794 MHz at 1000 mV and CPU-B 2002 MHz
at 1050 mV before and after its workload. Those checks do not establish the
clock conditions of earlier startup intervals. The approved transition sequence
is in [the CPU clock reference](../cpu-clock.md#a-transition-orders-voltage-and-frequency).
