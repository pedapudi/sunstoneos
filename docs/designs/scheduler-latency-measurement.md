# Scheduler step timing

Status: implemented and host-tested; the instrumented CPU workload returns
normally on port `3-3`. Scheduler latency targets remain unmet.
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

The [partitioned adapter](../../kernel/scheduler/freertos.c) retains the task
handle and serializes the desired change through a per-task FIFO lock. It
publishes a pending bit, orders publication before observation, and requests a
software interrupt when required. The owner consumes the change under the
kernel API interrupt mask and repairs linked event-list metadata outside the
control lock. A running current task with an unlinked event item and no event
reference completes its metadata repair directly. A task suspending itself
consumes only its own request before refreshing the slice, releasing the handle,
restoring the mask and yielding. The saved-frame maintenance acknowledges its
off-CPU fence. Other commands retain the general drain and repair path.
Additional checks bind the handle to its lifetime,
restrict list writes to the owner, and require saved-context acknowledgments
before migration or deletion completes.
Handle admission and control publication retain the API mask. An interrupt
cannot wait on the interrupted caller's control FIFO, and a suspended or
deleted caller cannot retain a lifetime reference on its abandoned stack.

Each low-level control drain consumes at most `SS_OWNER_BATCH` records.
`ssKernelControlDrain` calls that drain twice, around event-list repair.
Pending control bits participate in the maintenance continuation check, so
requests beyond a batch retain a later owner entry. Control
lock release completes the serving and applied stores and signals waiters
before the next record can wait on another FIFO. A second event follows
post-unlock reference release or retirement callbacks; a record without that
work needs only the unlock event. The diagnostic timestamps these boundaries.

The upstream interrupt entry saves general and floating-point registers,
increments nesting, acknowledges the interrupt, and calls the C handler. It
then masks IRQ entry, signals end of interrupt, unwinds nesting, switches tasks
when required, and restores context. The native port adds owner maintenance
after end of interrupt and before selection. Its priority-mask guard excludes
reentry into an interrupted owner operation. Maintenance admits only interrupts
above the kernel API priority while it drains controls and wake requests,
serves idle-work requests, expires deadlines, and refreshes the slice.
A clean interrupt wake can switch directly when a higher-priority task is
already ready and no control, repair, due deadline or expired slice requires
maintenance. Saved-frame selection and its final publication checks still run.
The detailed IRQ trace covers the priority-mask guard, eligibility check and
maintenance or direct-switch decision. It records saved-frame selection
separately. A result of zero can still precede a switch when the assembly port
observes an existing yield request.

CPU0 alone advances global scheduler time. Its timer handler processes at most
three overdue ticks per hardware interrupt. The diagnostic records the complete
C dispatch interval, including the latency workload's wake hook. Interrupt-exit
owner work has separate timestamps. The A733 interrupt and CPU identity rules
are in [the multicore reference audit](../a733-multicore-reference-audit.md).

## Records retain complete maxima and expose missing observations

Every CPU retains the longest command and longest self-suspend command, with
timestamps around the handle reference, publication, control drain and slice
refresh. Separate per-step maxima need not belong to the same command. The
longest drain retains its individual control records; the longest interrupt
exit retains its maintenance-step totals.

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

Version four links each busy wake to the latest completed selection of its
task before the captured task-entry timestamp, then finds that selection's
interrupt exit. Events after task entry cannot become its selecting event.
The report retains the longest busy wake, fast, slow and unmatched sample
counts, and the number and work of intervening interrupt exits. A missing
required history entry is explicit. An intervening interrupt leaves the wake
unmatched for single-exit attribution while preserving its selecting event.

The application waits until every memory worker is suspended before the wake
tasks freeze their per-CPU recorders. This includes the simultaneous suspends
after memory traffic ends. The final wake-task suspends occur after freezing.
The task-state queries take each worker's control FIFO and add diagnostic
contention during shutdown.
The report appends a versioned, aligned tail to the existing latency result;
the diagnostic manifest declares a 128 KiB output capacity.

The timer reads and record copies add work. Instrumented durations include that
cost, and no constant overhead is subtracted. The existing workload takes 64
idle and 128 busy wake samples per CPU. It supports exact observed maxima and
phase percentiles, but cannot establish a 99.99th-percentile bound.

## Startup and workload use distinct timing boundaries

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
