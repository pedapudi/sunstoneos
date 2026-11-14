# Partitioned FreeRTOS scheduling with topology-driven placement

Status: **qualified on the board** (finite runs) for the owner scheduler, the control
interrupt path and the unmasked task-lock wait; **host-tested** for the wake and
publication models; **open** for per-object locks in this tree, one-shot timers,
capacity hints, topology-driven placement and reservations.

Each CPU owns its scheduling state. A task, queue, semaphore, mutex, event group,
stream buffer, notification or timer API call keeps its FreeRTOS signature and
semantics. Drivers keep direct ISR-to-task notifications. The application kit
adds topology queries and work splitting. The fork is the default kernel in SMP
images (`SUNSTONE_PARTITIONED_SCHEDULER=ON`); `-DSUNSTONE_PARTITIONED_SCHEDULER=OFF`
builds the global FIFO-lock kernel as the control.

The [reviewed research](scheduler-research.md) ranks removal of shared scheduler
ownership first. ghOSt contributes sequence-tagged observations, validated remote
commands and interrupt coalescing. Linux contributes queued activation and
target-side work. These mechanisms execute inside the kernel; no policy-agent
task sits between a caller and local dispatch.

## Qualified figures

All figures are maxima over finite board runs on port 3-3 at the approved CPU
operating points, with the recorded mimus recognition (eight workers, exact
output) as the load. They are observations, not worst-case bounds. Each row
points to a record in [the scheduler evidence](../evidence/scheduler.jsonl).

| Result | Figure | Record |
| --- | --- | --- |
| Control path (interrupt above the API mask) wake latency, 10 kHz | maximum 3.6 µs, p99.99 3.0 µs, over 1 254 587 periods; jitter maximum 3.4 µs | `scheduler-owner-exit-guard-validation` |
| Task path (woken task) wake latency, 1, 2 and 4 kHz | maximum 23.5 to 31.5 µs, p99.99 19.7 to 24.5 µs; the 10 µs target is not met | `scheduler-long-runs-kernel-verdict`, `scheduler-task-path-wake-tail` |
| Model cost of the interrupts | 0.02 to 0.06 percent of recognition time per kHz | `scheduler-robot-target-results` |
| Fork against global kernel, recognition time | 110.394 against 110.853 ms over a 25 s soak | `scheduler-long-runs-kernel-verdict` |
| Longest masked span under the mimus load | 33 µs (fork), 31 µs (stock); 82 to 96 percent of every span above 5 µs is lock wait | `1793601476379926752-0c3dd3cd`, `1793602023862614320-8f9bd37f` |
| Unmasked task-lock wait | halves the time in long masked spans; recognition time +0.14% (stock), +0.97% (fork) | `1793603800991531488-e4485ccd` |
| Idle pull on the default fork | 3939 to 6722 pulls per 25 s; recognition time unchanged | `1793617020998598953-a6ecdfc1` |

## Acceptance compares each live stage with the unmodified kernel

Acceptance requires no shared scheduler-state lock for local wake or context switch (an uncontended local notification path also takes no object lock). It also requires loaded
ready-to-running p99 below 10 µs and workload mask and lock-wait maxima below 25 µs, recorded as p50, p99 and maximum per CPU and phase. Further requirements are correct immediate preemption
including publication concurrent with priority lowering and WFI entry; no scheduler timer interrupt without a deadline or equal-priority slice competitor; and eight-core QEMU soak, linked-kernel stress and exact mimus recognition after each live stage. Last, a measured capacity ratio, weighted-versus-equal splitting and
big-pinned-versus-unpinned latency at the vendor top points. Observed maxima are not
worst-case proofs, and every analytic bound names its admission assumptions. The
task path misses its 10 µs target, and the capacity, splitting and idle-wakeup items
are open.

## A forked mechanism preserves the public APIs and static storage

The pinned upstream kernel is `97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2`.
Reproducible patches are the fork boundary; downloaded upstream source stays in
the cache. Owned code lives under `kernel/scheduler/`. `configSS_PARTITIONED_SCHEDULER`
selects the fork. The partitioned patches apply to a generated source tree that
already carries the SMP patches (`cmake/scheduler-partitioned.cmake`).

| Source | Change | Preserved contract |
| --- | --- | --- |
| `patches/freertos-partitioned-tasks.patch`, `-api.patch` | Replace the global ready and delayed lists with owner operations; route remote control; convert every blocking and priority-change path | Task handles, priority numbering, affinity APIs, notification semantics, static task allocation |
| `patches/freertos-partitioned-objects.patch` | Route `queue.c`, `event_groups.c`, `stream_buffer.c` and `timers.c` critical sections through `ssKernelObjectEnter` and `ssKernelObjectExit` (compatibility protection by the two global locks) with owner wake publication | API signatures, ISR restrictions, queue data ordering, timeout results |
| `patches/freertos-partitioned-layout.patch` | Extend internal layouts and the matching `StaticTask_t`, `StaticQueue_t`, `StaticEventGroup_t`, `StaticStreamBuffer_t` | Static allocation sizes are checked; the TCB stack pointer stays first |
| `patches/freertos-partitioned-port-asm.patch` | Owner entry and exit at interrupt exit; local interrupt exclusion | Context frame, SIMD/FPU save policy, EL0/EL1 paths, current-task register ABI |
| `platform/a733-smp-cached/` | Dispatch the reschedule SGI and PPI30 to the local owner | Existing startup and restoration contracts |
| `include/FreeRTOSConfig.h` | Fork feature selection, static limits | Eight fixed priorities; 1 ms `TickType_t` units; preemption enabled |

No Linux GPL implementation is copied into the mechanism. The fork retains
upstream MIT notices. [The source inventory](scheduler-research-sources.json)
records provenance and licenses of the research references.

The static limit is 128 registered tasks including idle and service tasks.
Registration consumes a fixed slot; deletion releases it after all owners
acknowledge outstanding references. The limit sets the storage and batch
bounds. Compile-time checks compare internal and public static-object size and
alignment, TCB stack-pointer offset zero, port current-task and FPU offsets, and
cache-line alignment of ticket fields and notification words. Optional
configurations whose API branches are not converted (MPU wrappers, tickless
idle, stats formatting) are compile errors. Every task has at most one blocking
deadline. The owner deadline heap has 128 entries; slice expiry and the
software-timer service's next wake are separate local candidates.

## CPU topology is immutable data; observed operating state is separate

The scheduler consumes a validated topology view at boot. SoC data holds CPU
identity, domains and caches. Board data selects admitted CPUs, reserved CPUs
and default task masks. Online state, observed identity and applied operating
point are separate runtime records.

| Data | Fields and units | Owner and validation |
| --- | --- | --- |
| CPU identity | Logical ID, full MPIDR affinity, MIDR, physical-core and SMT-sibling IDs | SoC description; each CPU's captured MPIDR and MIDR is compared with its entry slot |
| Capacity | Relative capacity, fastest admitted physical core = 1024; provenance, workload, confidence | Calibration record; unknown is explicit and never becomes an A76/A55 MHz ratio |
| Performance domain | Member CPUs, clock and rail domain, OPPs for the observed bin, applied OPP | SoC description references the clock owner's tables and readback |
| Energy | Per-OPP power with units and provenance; unknown permitted | Energy placement stays disabled without valid costs |
| Cache | Level, type, size, line size, sharing CPU mask | Checked against CLIDR and CCSIDR observations |
| Interrupt eligibility | Device-IRQ-capable mask and excluded targets | Interrupt controller and board policy; full affinity for GIC routing |
| Board policy | Admitted, reserved, default service and application masks | `drivers/board/orangepi_zero3w.c` |

The A733 record is `platform/a733/cpu_topology.c`: six A55 and two A76 CPUs in one
DynamIQ cluster with private L1/L2 and a shared DSU L3. CPU-L and CPU-B have
distinct clock domains; CPU-L and DSU share DCDC5 and CPU-B uses DCDC3. A55 slots
are 0 to 5 and A76 slots 6 and 7, with the core index in MPIDR Aff1. See
[identity and cache evidence](../cpu-coherence.md), [clock and rail ownership](cpu-top-opp.md)
and [routing](../cpu-continue.md). A queued qualification captures logical ID,
MPIDR, MIDR and cache geometry on all eight CPUs before capacity results are
interpreted. Synthetic symmetric and three-cluster tables select the same
generic algorithms. SMT siblings are not added as independent throughput without
a shared-resource calibration.

For bin vf0400 the top points are CPU-L 1794 MHz, CPU-B 2002 MHz and DSU
1274 MHz. Placement reads `include/a733_cpu_clock.h` and `apps/os/cpu.c`; it never
writes PLL, PMIC or thermal state. The scheduler does not yet use this table for
placement (see Open work); the application kit exposes it.

## Per-core runqueues have one writer and constant-time selection

Each cache-line-aligned `ss_sched_cpu` (`struct ssKernelCPU`) contains:

- Eight FIFO ready lists, an eight-bit ready bitmap and the current task.
- Local interrupt exclusion and preemption depth, pending-reschedule state and
  the published dispatch word.
- A fixed incoming-wake set, a separate remote-control mailbox and lifetime
  acknowledgments.
- A fixed-capacity deadline heap, its next compare, a slice deadline and local
  accounting timestamps.

The structure is laid out in cache lines by writer. Owner-only state (ready
lists, current task, detached batches, cursors) shares no line with state other
CPUs write (incoming wake words, dispatch word, control mailbox, and the slice
and next-deadline snapshots that CPU0's tick reads). Compile-time assertions
enforce the grouping. A masked scheduler entry then misses only on lines its own
work needs; with the A733's caches cold after a 1 MiB sweep, each avoidable miss
adds about a DRAM access to a span that the 25 µs bound limits.

Only that CPU's task or IRQ scheduler entry changes its ready lists, current task
or deadline heap, with local API interrupts masked. No remote CPU writes a list
link. Running tasks are absent from the ready FIFO. An interrupted current task
returns to the head of its priority unless it yields or uses up a slice, when it
joins the tail. A newly ready task joins the tail. Selection takes CLZ of the
nonzero bitmap, then removes one list head.

Time slicing runs only while a non-idle task has a local equal-priority
competitor. The quantum is 1 ms. Higher-priority preemption does not wait for a
slice. An idle task has no slice deadline and executes WFI. The first implementation
calls fixed-priority selection directly; a policy interface is added when EDF/CBS
has a consumer and an admission test.

## A fixed-slot remote wake list avoids unbounded producer retry loops

The logical multi-producer wake list uses one pending bit per task slot, per
target CPU and priority: 128 bytes per CPU at 128 slots and eight priorities, plus
an atomic summary word that forms the list head. A publisher release-ORs its task
bit, then release-ORs its bucket bit into the head. The old head identifies
empty-to-nonempty publication. The owner acquire-detaches the head, then detaches
slot words and visits bits with CLZ. The bit is a level-triggered hint to inspect
authoritative state; it carries no generation and cannot authorize activation.

An intrusive Treiber stack would let a producer retry indefinitely under
continuous competition, so the fixed-slot representation is a constraint: producer
instructions stay bounded without allocation, node reclamation or head-pointer
retry. A slot may reach an older inspection before its delayed head publication;
the later empty-bucket inspection is harmless. Repeated notifications coalesce
activation; their values and counts remain in the notification object.

Priority FIFO order is the owner's enqueue order; publications from different
remote CPUs have no total order among themselves. Within a detached word the
owner uses a rotating slot cursor so no slot number has permanent preference.

Native AArch64 publication requires LSE atomics (`ID_AA64ISAR0_EL1.Atomic`;
the build uses `-march=armv8.1-a+lse` and rejects LL/SC retry loops). Host
fixtures use native lock-free atomics. An architecture without bounded native
word read-modify-write needs a separately reviewed publication primitive.

Each owner keeps detached words in a local pending set until processed. The cursor
spans all 128 slots of a priority, including both words; fresh publications merge
without resetting it, so a continuously replenished first word cannot overtake an
unvisited second word. A priority change republishes a hint at the new priority and
forces an owner command and SGI, including for detached work.

One entry activates at most sixteen tasks (`SS_OWNER_BATCH`), checking for
higher-priority incoming work between activations. On batch completion it
dispatches the highest ready task and posts one local continuation SGI if
unprocessed hints remain. It completes IRQ exit and restores interrupts between
batches. It never runs lower-priority application work while known
higher-priority activation remains. At a fixed priority a slot already pending at
a sweep's start is inspected within eight sixteen-slot batches, plus
higher-priority admitted work; an urgent slot has a finite activation bound only
with a stated higher-priority arrival bound. Ready-to-running time starts at
publication, including inbox processing. A self-SGI storm fails admission.

## Conditional SGIs use a publication handshake as well as status words

Each CPU publishes a naturally aligned 64-bit atomic dispatch word holding a
sequence, running priority and idle flag. Remote readers use it as one coherent
observation without a seqlock retry loop. The sequence identifies changes and
authorizes no owner-state modification.

1. The waker updates the notification or wins the blocked-wait transition.
2. The waker release-publishes the incoming slot bit and bucket-head bit, executes
   `DMB ISH`, then acquire-loads the target's dispatch word.
3. If the old head was empty, the task outranks the published current task, or the
   target is idle, the waker requests a reschedule SGI. Equal or lower-priority
   publications into a nonempty head coalesce.
4. The owner acquire-detaches the head at entry and inspects a bounded batch.
   Before committing dispatch it release-publishes its status, clears the SGI
   pending flag, executes `DMB ISH`, then acquire-checks all incoming and detached
   work. Remaining work requests a bounded continuation even when its priority is
   lower than the selected task.
5. An owner pass that drains hints but commits no dispatch (it selects no task and
   leaves no reschedule) still closes the handshake: it clears the SGI pending
   flag, executes `DMB ISH`, then acquire-checks the incoming and detached work,
   and takes another pass if any remains. Without this step the flag stays set
   after the pass. The next publisher finds it set and sends no SGI, so its wake
   waits for the owner's next timer deadline. A flag already clear needs no
   barrier, because a publisher that finds it clear sends its own SGI.

The two store-to-load barriers forbid both CPUs observing the old state and
omitting the wakeup. In steps 4 and 5 the owner's flag clear, `DMB ISH` and
incoming check pair with the waker's incoming OR, `DMB ISH` and flag exchange:
either the owner's check sees the publication, or the waker's exchange sees the
cleared flag and sends the SGI; acquire and release alone allow that store-buffering outcome
on AArch64. Negative model variants that remove essential ordering must produce a
lost-wakeup trace.

```text
waker: release OR slot hint
       old_head = release_OR(incoming_head[target], bucket_bit)
       DMB ISH
       status = acquire_load(dispatch[target])
       if old_head == 0 or urgent(status) or idle(status):
           if exchange_acq_rel(sgi_pending[target], 1) == 0:
               DSB ISHST; send target SGI

owner: acquire exchange incoming_head[self] with zero
       merge already detached work and process bounded control/activation
       release_store(dispatch[self], next_priority_or_idle_with_sequence)
       release_store(sgi_pending[self], 0)
       DMB ISH
       acquire_check live incoming words, local detached words and control hints
       if any work remains: request bounded selection/continuation
       else: commit selected task or final masked WFI sequence

owner pass that drained hints and commits no dispatch:
       if relaxed_load(sgi_pending[self]) != 0:
           release_store(sgi_pending[self], 0)
           DMB ISH
           if acquire_check(incoming and detached work): take another pass
```

The final incoming check and WFI occur with `DAIF.I` masking IRQ entry and the GIC
priority mask admitting the reschedule interrupt, so a pending eligible IRQ
prevents indefinite sleep. An idle flag stays published until the owner resumes
selection. A publisher delayed after claiming the SGI flag holds a reference and
runs a bounded non-preemptive publication sequence; its redundant SGI is
harmless. A pending or active hardware SGI does not replace the final
authoritative check. The model treats flag clearing, interrupt request,
delivery and WFI wakeup as separate events.

GIC SGI target-list bits address Aff0 within a common higher-affinity tuple. A733
CPUs differ in Aff1, so delivery to several CPUs takes several SGI writes.
Completion before an SGI uses the port's `DSB ISHST` ordering plus the required
system-register synchronization ([A733 SGI contract](../cpu-continue.md)).

## Object state and owner state have separate locks and linearization points

| State | Writer and protection | Permitted takers |
| --- | --- | --- |
| Local ready links, bitmap, current task, timeout heap | Owner CPU with local API IRQ exclusion | Owner task or IRQ scheduler entry only |
| Incoming activation bits and dispatch word | Release/acquire atomics | Any publishing CPU; the owner consumes |
| Queue, semaphore, mutex metadata and event waiters | The two global FIFO ticket locks in this tree (`ssKernelObjectEnter`); one FIFO lock per object in the per-object-lock design below | Calling task or permitted ISR |
| Event-group bits and stream-buffer metadata | Same as queues | API callers; documented single-producer/consumer restrictions apply |
| Notification value and state | One-CAS fast transaction, then gated FIFO fallback on conflict | Calling task or permitted ISR; uncontended local wake takes no shared lock |
| Task wait state, generation, owner | Atomic packed state; owner commands arbitrate control changes | Waker claims a wait outcome; owner changes placement and lifetime |
| Registry, creation and deletion, public global suspend gate | Separate administration lock (`ssKernelAdminNext`/`Serving`) and acknowledged owner commands | Task slow paths; never normal wake or switch |
| Public `taskENTER_CRITICAL` | Recursive application lock plus local interrupt exclusion | Existing application and driver callers |
| Global time and the tick's deadline sampling | CPU0 alone advances time with an atomic increment and reads each owner's published deadline and slice deadline; the port's tick handler takes no global lock (`ssKernelTickEnter`/`ssKernelTickExit` only defer preemption) | CPU0's timer interrupt |

Object acquisition masks API-capable local interrupts before joining the FIFO, so
an ISR never waits on a lock held by the task it interrupted. Waiting and
ownership are non-preemptive. Waiters use SEVL/WFE with an acquire owner check;
release publishes ownership, completes the store before SEV and preserves the
check-to-WFE event-register protocol. Non-API high-priority interrupts never take
these locks. Nesting follows a checked lock order, and no object lock is held while
waiting for an owner acknowledgment. Each lock has a 32-bit ticket allocator and
serving counter on separate cache lines; outstanding tickets stay below half the
counter range. MCS locks are reserved for a demonstrated coherence bottleneck.

### Every notification action uses one atomic transaction protocol

Each notification slot is one aligned 64-bit word: value bits 0 to 31, FreeRTOS
state bits 32 to 33 (NOT_WAITING, WAITING, RECEIVED), completed-wait latch bit 34,
fallback-registration count bits 35 to 41, reserved bits 42 to 63 zero. The task's
owner wait record holds the full 64-bit wait generation and the active
notification index.

Every caller pins the permanent task descriptor, masks local API IRQs and loads
the word with acquire semantics. If the registration count is zero it computes
the transition and attempts exactly one acquire-release CAS; success is the
linearization point, and a failed CAS enters the FIFO path without retrying. The
FIFO path registers with one acquire-release LSE add to the count field, which
invalidates older zero-count snapshots and excludes later fast transactions. The
ticket owner replaces only payload bits with LSE clear and set operations that
preserve concurrent count changes, commits payload, wait intent and hint
publication, release-decrements the count, advances serving and signals SEV. At
most one non-preemptive caller per CPU participates, so eight CPUs cannot overflow
the seven-bit count. Nested calls on one slot and calls from non-API interrupts are
forbidden.

| Operation | Transaction |
| --- | --- |
| Give, set bits, overwrite, no action | Apply the action and set RECEIVED in one matched payload transaction; old WAITING also sets the completed-wait latch and requests owner inspection |
| No overwrite | Fail if the matched state is RECEIVED; otherwise replace the value and set RECEIVED (and the latch from WAITING) |
| State clear or value-bit clear | Apply the upstream transform, preserve the latch, return the old result |
| Wait entry, index setup, timeout, suspension, deletion | Always gated: inspect, clear and test state and commit the owner wait intent atomically with other slot callers |
| Wait completion | Gated consumption, decrement or bit clearing and the final timeout-boundary check, preserving upstream return values |

A successful WAITING-to-RECEIVED change sets the completed-wait latch in the same
CAS, so scheduler readiness survives later clears. Gated wait entry completes the
prior wait, clears the old latch, installs the new wait record and deadline, and
publishes WAITING and the local block intent before opening the gate. The owner
checks the latch and pending work before physically switching away. A timeout
gates the slot first: the latch wins readiness even if a later caller cleared
RECEIVED; otherwise RECEIVED or a qualifying count wins; otherwise timeout changes
WAITING to NOT_WAITING and closes that generation. The returning API still
performs the upstream final state test. A fast producer holds a descriptor
reference across snapshot, CAS and hint; task-incarnation reuse waits for
reference retirement, and wait-generation wrap requires complete reference and
deadline retirement.

Public global scheduler suspension keeps the upstream semantics through a slow
administration gate with states OPEN, ACQUIRING, HELD and RELEASING, holder
task and incarnation, nesting count and epoch. The winner publishes ACQUIRING
and sends checkpoint requests; each owner finishes its bounded local section,
acknowledges at IRQ exit or scheduler entry and defers dispatch for that epoch
while continuing timer capture and control processing. Resume publishes
RELEASING, drains pending work and releases the token; `xTaskResumeAll` reports
whether it yielded. The gate never waits for a task to run, and a global call's
duration depends on every owner's checkpoint bound, so it is measured apart from
ordinary scheduling. Internal queue and timeout code does not call public
global suspension.

### Control publication has fixed storage and does not drop ISR requests

Each permanent task descriptor holds desired control state, a request sequence,
the owner-applied sequence and separate reference and transfer records. A
per-task control ticket serializes general priority, affinity, suspend, delete
and abort changes. Self-suspension instead borrows the current execution
lifetime and publishes atomic suspension and saved-frame obligations before
the kernel raises the API mask. Its masked path cancels the caller's local
state, saves the frame and selects a replacement without draining controls.
Resume preserves the frame obligation until the owner acknowledges it.
Publishing a general composite request is one
release store followed by a hint and SGI; requests for the same desired state
coalesce, and deletion is absorbing for its incarnation. Storage cannot fill
because publication overwrites state instead of allocating command entries.
Operations that need synchronous adoption wait for the owner-applied sequence
with interrupts enabled and no object or control lock held. ISR APIs never wait
for owner progress: they commit logical state under bounded protection, publish a
hint and return the local-yield indication. Waits keep permanent
slot/incarnation/sequence cookies in 128 fixed banks (260 KiB, separate from hot
metadata), so migration cannot strand another CPU's completion records.

Per-transition rules follow from these. Creation initializes stack and TCB outside
shared exclusion, then registers the descriptor and release-publishes the initial
READY request; failure returns NULL and unwinds TLS, port, stack and TCB. Delay and
timeout helpers install or remove the owner deadline and wait generation, and keep
absolute deadlines across repeated waits. A remote task-context suspend waits for
off-CPU acknowledgment with no lock held, and an ISR resume returns immediately.
A priority setter waits for adoption outside locks, and a raise republishes pending
hints and sends an SGI. Affinity changes commit the eligible mask, then run the
acknowledged transfer; the current task cannot return on an excluded CPU. Delete
closes admission, marks DEAD, forces owner eviction and retires references before
cleanup; a self-delete never returns. Abort delay closes the blocked generation
under the object protocol and publishes the outcome.

Priority repair updates the effective priority, the object's waiter order, the
owner ready bucket, detached pending work and the urgent-SGI decision. Mutex
donation is published without taking a reverse-ranked task-control lock while the
mutex is held, and owner repair reads committed donation records without waiting
for the donor. Enabled paths exist in the build; timers, event groups, stream and
message buffers, priority APIs and abort-delay have additional builds with their
flags enabled, and an unbuilt API is not claimed converted. No internal path holds
an object lock while invoking a synchronous owner wait.

## Permanent descriptors and references make stale hints harmless

Task descriptors and incoming bitmap storage never move or disappear. A
descriptor has a task-incarnation number, TCB pointer, admission state and an
atomic reference count, all on one 64-byte line. The descriptor table is also
the handle table: a multiplicative hash of the handle selects its home slot,
registration takes the first free slot on the linear probe chain from there, and
each descriptor's key holds the handle while it is mapped. A lookup without a
collision therefore reads one line, the target's descriptor, before it pins.
Object waiters hold references; selecting an event waiter
transfers its reference to the publisher before the object lock is released, and
publishing releases it after the hint and any SGI.

A notification or control caller increments the reference count before reading
mutable task state, executes `DMB ISH`, then acquire-checks admission and
incarnation. A closed descriptor is not dereferenced. As upstream, the caller must
hold a valid task handle. Deletion closes admission with release semantics,
executes a store-to-load barrier, detaches object references and waits (with IRQs
enabled and no object lock) for admitted publishers to retire. After the count
reaches zero every owner drains its live bits, detached sets and control
references and acknowledges that incarnation; only then can the TCB and stack be
reused and the descriptor reopen with a new incarnation. The final reference
release executes `DMB ISH` before checking closed admission and notifying the
retirement coordinator; without it the closer can see a remaining reference while
the releaser sees open admission, and no event wakes reclamation.

Hints are not outcomes. An owner acquire-checks the authoritative wait state
under the object's protocol, and only the owner changes a blocked wait to ready.
Already-ready, dead and still-waiting tasks ignore stale hints. A stale bit can
reveal a valid newer completed wait but cannot invent a completion.

## Wake, block and timeout races have one outcome per wait generation

A task has separate placement state (OWNED, TRANSFERRING or CLOSED, with owner and
epoch), execution state (RUNNING, READY, BLOCKING, BLOCKED, SUSPENDED, DEAD) and
wait state (generation, reason, completion). Notification arrival uses the slot
word; other objects serialize outcomes under their object lock. Migration
preserves these outcomes.

- **Local wake:** Update the object, establish the wait completion, cancel the
  local timeout and enqueue directly; if it outranks the current task, select on
  local IRQ or critical exit. An uncontended notification-give uses one CAS.
- **Remote wake:** Under the object protocol remove the selected waiter, mark its
  generation complete and transfer its reference to the publisher. Release object
  protection before the bounded hint and SGI. The owner validates the state,
  cancels the timeout and enqueues once.
- **Block:** The owner installs PREPARING under object protection, rechecks the
  condition, installs the event waiter and local deadline, then commits BLOCKED
  before releasing protection and selecting. A producer arriving after gate release
  but before the physical switch makes the owner's final check cancel the switch,
  or its pending SGI causes immediate reentry. No list has two writers.
- **Timeout:** The owner claims TIMED_OUT only if the same generation remains
  BLOCKED, takes object protection, rechecks the generation and condition and
  detaches the event waiter. API success can still require another condition check.
- **Yield:** Put the current task at the tail of its local priority and select
  locally; no SGI or shared lock.
- **Delay:** Compute an absolute counter deadline, install it in the local heap,
  block locally and program the earliest compare. `vTaskDelayUntil` keeps its phase
  and wrap semantics in 1 ms units.

Timed blocking uses at most seven heap levels at the 128-task limit; wake
cancellation is an indexed heap removal, bounded `O(log N)` owner work.

Task deletion marks DEAD, cancels local links, detaches object waiters and
collects owner and publication acknowledgments before slot reuse or stack cleanup;
it is a slow path. A task running a destructor defers its own destructive control
requests; releasing its cleanup shield requests owner service once. Cleanup
removes the closed handle mapping before freeing recyclable memory while reserving
the old descriptor slot, so a new task can map the same address to another slot;
completion generations survive slot reuse, and static construction rejects a
buffer whose old cleanup is pending. Every latched scheduler fault records the
first transition, task slot, owner, generation and command inputs.

Queue wakes are retry permission, not a reservation: a woken task rechecks the
queue under its object lock and can reblock with a new generation while keeping
its absolute API deadline. Queue reset changes its generation and revalidates
waiters. Event-group completion captures the bits and clear-on-exit decision for
the selected generation, and all selected tasks observe that captured result.

## Migration is an acknowledged transfer, not a remote ready-list edit

An explicit affinity update changes eligibility and posts a transfer request. The
old owner validates the epoch, stops an executing task at a safe scheduler
boundary, snapshots wait outcome and timeout under the wait object's protocol,
detaches local links and release-publishes TRANSFERRING. It keeps forwarding
responsibility for that epoch and every admitted producer. The destination
acquire-validates the control record (incarnation, old and new epochs, wait state,
accounting, absolute timeout, off-CPU acknowledgment), adopts the task once, arms
any live timeout and release-publishes the new owner and epoch; that store is the
placement linearization point. It checks authoritative completion before choosing
READY or BLOCKED and acknowledges adoption. No CPU executes the task before the
old owner's off-CPU acknowledgment, and no masked interval spans an
acknowledgment wait. Cancellation or rejection returns ownership through the
same protocol.

A lifetime-pinned publisher samples the route, registers with one acquire-release
RMW on one of two participant banks (each combining closure and a count in one
word), executes a DMB and validates the sampled word. It makes one attempt; a
rejection broadcasts an epoch-neutral activation hint to all admitted owners.
The source closes its old bank before detachment; TRANSFERRING prevents an early
hint from reinserting the task on either queue. Destination adoption restores
owner-local state before release-publishing the next route, so validation rejects
a snapshot that spans two transfers. After the closed bank reaches zero, the
source checks its hints and acknowledges the epoch. An old-epoch publisher's hint
is forwarded to the current owner. Migration epochs and incarnations cannot wrap
while a corresponding reference exists.

Every task has one owner. Creation chooses the allowed core with the fewest
assigned tasks, ties to the lowest index (`ssKernelLoad`). Priority guarantees are
local to the owner: an unpinned task's allowed mask is eligibility for explicit or
idle-pull migration, not a global highest-priority-everywhere guarantee. Admission
rejects a partition assignment whose promised workload cannot fit. An
interrupt-bound task stays pinned while its device has an active job; changing its
CPU masks the source, drains completions, reroutes, publishes the new consumer and
unmasks, in device-specific owners. Migration cost includes private L1/L2 refills,
shared-L3 hits and transfer coordination; no constant cache-cost guess becomes an
admission bound.

## Idle pull moves a waiting task to an idle eligible CPU

A task that waits behind a running task of equal or higher priority can move to an
idle CPU its affinity allows. Each owner publishes `pull_targets`, the union of the
affinity masks of its queued tasks (at most eight entries per priority), and a
global bitmap marks the CPUs that run only their idle task. At the end of a service
entry an owner that keeps its current task takes each target that is idle or has
asked (`pull_requests`), picks the highest-priority waiting task that may run
there and publishes a move control. The ordinary acknowledged transfer detaches the
task and the target adopts it; no CPU edits a foreign queue. A CPU that selects its
idle task sets its idle bit and asks each owner whose targets include it, with one
coalesced bit and one SGI. A task that would preempt the running one is not pulled,
and neither is one while its core's current task is blocking, suspending or
deleted. Placement moves with the task; an affinity change moves it back.
All-idle systems exchange no balancing timer events or periodic IPIs, and a
rejection waits for a real candidate event rather than retrying.

A pinned task (affinity of one CPU) never moves; the real-time task set stays
pinned. `SS_PULL_MOVES_PER_PASS` (`SUNSTONE_PULL_MOVES_PER_PASS`, default 0, unlimited) caps the
transfers one pass starts; the cap shortens the pull pass but not the control
drains, so it stays off. Idle pull is on in the default fork. A missed pull is a
performance defect: a ready migratable task can wait behind a running task on its
own core while an allowed core idles. It cannot lose or duplicate a task, move a
pinned task, break an affinity mask or invert priority. The linked test fixtures
turn it off (`SS_KERNEL_TEST_NO_IDLE_PULL`). The cost of a missed pull is
unmeasured.

## Robot control sets the requirement: bounded worst-case response

SunstoneOS runs quadruped and biped robots with several cameras and lidar.
Real-time response ranks above throughput. Targets: worst-case wake latency of
the highest-priority task at most 10 µs; jitter of the control period at most 5% of the period (12.5 µs at 4 kHz, 5 µs at 10 kHz); no deadline miss under the full load; the model workload within 2% of its run alone. The primary rate is 4 kHz and
the stretch rate 10 kHz. A kernel is chosen if it meets the real-time targets at its
highest rate and keeps the model within 2%; among those the higher control rate wins,
then the higher model throughput. Measurements report the maximum and the 99.99th
percentile, never means. With a 5 µs jitter bound at 10 kHz, any masked or
preemption-disabled section over 5 µs is a defect.

A task woken by an interrupt runs after four delays: the interrupt waits while the
core has API-level interrupts masked, the entry and wake path run, the scheduler
selects the task, and the context switch completes. The first term is the largest
and is bounded below by the longest masked span of any kernel path on that core. On
the A733 with the memory system saturated, a span is set by dependent DRAM
refills: 94 to 98 percent of the cycles of the longest spans are backend stalls (about 35 refills at 0.5 µs each). The same spans take 2.5 to 5.5 µs with the
memory system idle. Two levers follow: shorten and split the masked sections, and
run the hardest path in an interrupt above the API mask. Fork and stock spans,
the sweeps, the lock-wait split and the task-path measurements are in the records
`scheduler-partitioned-robot-requirement`, `scheduler-robot-target-results` and
`scheduler-task-path-wake-tail`.

Intervals with `PSTATE.I` set on the control core decide the control path's latency,
in instructions:

- Interrupt entry to the C handler: 64 (33 register stores).
- Interrupt exit after the handler: about 70, with one `DSB SY`.
- Yield supervisor call: 187 in the handler; the tail takes at most 3.7 µs.
- Mask and unmask supervisor calls: 9 to 25 each, with two `DSB SY` and two `ISB`.
- `uxPortSetInterruptMaskFromISR` and its clear counterpart: seven each, with two `DSB SY`.

The last three are open for shortening. On the measured load the largest control
latency per soak is 3.3 to 3.7 µs, from the compute kernel's own memory stalls.

## Control interrupts above the API mask do not wait for kernel spans

Every kernel span is masked at the API priority, so an interrupt above that priority
is not delayed by any of them. The control class is the stored priority
`A733_GIC_CONTROL_STORED` of `include/a733_gic_priority.h`, numerically below the API
floor. `checks/scheduler-control-class.py` checks the view rules (which Non-secure
writes store which value, which values the raised mask lets through, that a read
then write keeps the value) and that the handler source calls no kernel function
and holds no priority literal.

`runtime/control_irq.c` (`SUNSTONE_CONTROL_IRQ`, default off, a measurement option)
drives it from the EL1 virtual timer (PPI 27) of cores 1 to 5, one rate each at 1, 2,
4, 8 and 10 kHz. The handler reads the counter, re-arms the compare value from the
previous deadline so the period does not drift. It records latency (entry minus the programmed deadline) and period jitter in per-core histograms with one counter tick (41.7 ns) per bin, and puts a sample into a single-producer single-consumer ring
(`runtime/control_ring.h`). A task drains the rings and checks sequence numbers; a
full ring drops the new sample and counts it. A core arms its own timer from its
first interrupt after the request, because only that core can configure its
redistributor and timer. Core 0 keeps the task-deadline timer. The comparison task
path runs on the same cores: the EL1 physical timer (PPI 30) at the API floor
(`A733_GIC_TASK_PATH_STORED`) fires half a period after the control handler and
wakes a pinned priority 5 task with `vTaskNotifyGiveFromISR`.

## Waiting for the task lock with the mask open

Most of every long masked span is queueing for the task lock with the interrupt mask
already raised. `uxPortSetInterruptMask`, `vTaskEnterCritical` and `vTaskSuspendAll`
therefore wait first and mask second. `runtime/lock_prewait.c` (`SUNSTONE_LOCK_PREWAIT`,
default off) wraps them at link time, with no protected patch change. The wrapper
reads the task lock's ticket word and, while more than one core holds or waits for
it, sleeps in `WFE` with the mask open, then calls the real function, which masks
and takes a ticket as before.

Rules. Most kernel callers on the OS image are EL0 tasks, which reach the mask
through a supervisor call and cannot read it. An EL0 task therefore waits when its core's
critical nesting count is zero and it does not hold the task lock; an EL1 task waits
when the priority mask is open and `DAIF.I` is clear. The mask reads back in the
Non-secure view (`include/a733_gic_priority.h`: the 0xff written reads back as
0xf0), so open means above the API mask, the test the port uses. An interrupt
handler, a nested critical section, a `FromISR` path and an EL0 task inside a
critical section go straight to the real function; the wrapper never lowers a mask.
A core that already holds the task lock, as a task does while it has suspended the
scheduler, does not wait. The wait takes no ticket and no lock, so an interrupt that
arrives during it runs normally. A waiter cannot take a ticket with the mask open:
an interrupt that switches tasks would leave the new task queued behind a ticket
held by a descheduled one. The wait is limited to 100 µs, checked only when an event
wakes the core, so a wait behind a long holder can last as long as the holder. After
the limit the caller masks and queues regardless, so the total wait is the limit
plus a ticket queue of at most seven holders. A burst of cores can all see the lock
nearly free and queue together, so the masked wait shrinks but remains.

## The owner exit work must not run after an interrupt above the API mask

The patched outermost interrupt exit calls `ssKernelOwnerIRQExit`. It returns 2
when the interrupted PMR excludes API interrupts or control preparation is
active. The assembly exit then neither runs maintenance nor selects a task;
pending work remains for the next ordinary maintenance entry. The active guard
also protects preparation that admits ordinary API interrupts. CPU0's tick
advances time and publishes due-owner work without calling owner maintenance.

### The mask guard leaves the owner work running after ordinary interrupts

The guard relies on `ICC_PMR_EL1` retaining the interrupted context's mask at
owner entry. A handler using the FromISR mask pair restores that value before
return. An eligible higher-priority local wake can proceed directly to saved-frame
selection when no control, object repair, due deadline or expired slice requires
maintenance. Otherwise the exit runs one service pass. The partitioned SGI
handler leaves selection to this decision; an empty SGI requests no switch.
Owner counters and the [step recorder](scheduler-latency-measurement.md) expose
the guard, service and selection paths. Optional list-integrity and stack checks
retain FreeRTOS corruption detection.

<a id="the-owner-exit-work-runs-under-the-api-mask-with-interrupts-open"></a>

### Control preparation opens both interrupt masks

The service raises PMR for local queue work and clears `PSTATE.I`. Control
preparation opens PMR as well, admitting ordinary interrupts between local
commits. FIFO, notification, object and administration acquisition each use one
nonblocking attempt. A busy record parks until its resource release signals
it. A visited bitmap spans the maintenance entry and limits it to sixteen
distinct attempts (`SS_OWNER_BATCH`); a reissued hint waits for the next entry.
Selection skips control preparation and object timeout work. Incoming-wake
drain and dispatch publication still close the saved-frame selection handshake.

Nested interrupt exits leave pending work intact while preparation is active.
The outer exit sets `PSTATE.I` before restoring PMR so an ordinary interrupt
cannot enter an unfinished outer return. The control unlock signals FIFO
waiters before lifetime callbacks. A second event follows those callbacks only
when they run; deferring the unlock event across records can strand a waiter.

These work counts do not establish time bounds. The instrumented workload
retains control records costing 7.667 to 47 µs and an owner C exit of 71.792 µs.
The sixteen-record batch cost remains unqualified, and masked work after
preparation still exceeds the 25 µs budget. The [timing records](../evidence/scheduler-latency-measurement.jsonl)
qualify these observations and the recorder's coverage limits.

## Per-object locking replaces the global locks for queues, event groups and stream buffers

Status: host-tested; the code is on branch `work/sched-tasklock-1102` and is not in
this tree, which keeps the global locks.

### Object locks: queues, event groups and stream buffers lock their own object

Every queue, event group and stream buffer section locks the object it works on and no other lock. A section that took the two global locks (the task lock and the interrupt lock) would make operations on unrelated objects queue behind each other with the interrupt mask raised, which is what the per-object locks avoid. No build option or code path selects the global locks for these sources.

Mechanism. An object hashes, by its address, to one of 64 slots (`SS_OBJECT_SLOTS`). Each slot has a task-side ticket lock T and an interrupt-side ticket lock I, the roles of the two global locks:

| Section | Holds | Mask |
| --- | --- | --- |
| Critical section of an object source (`taskENTER_CRITICAL`) | T and I of the slot | raised |
| Suspension of an object source (`vTaskSuspendAll`), across the blocking path | T of the slot | open |
| `FromISR` section | I of the slot | raised |
| Walk of an event group's waiter list (`xEventGroupSetBits`, `vEventGroupDelete`) | T of the slot for the walk; I of the slot for one waiter at a time | raised while I is held |
| Owner timing out or repairing a waiter | I of the slot recorded when the waiter blocked | raised |
| Priority donation (`xTaskPriorityInherit` and its inverse) | the donation lock, a leaf | raised |

The slot needs no field in the object, so `StaticQueue_t` and its siblings keep their layout. A task that blocks on an event list records the slot (`event_slot`) under the object's lock; the owner that later times the waiter out takes I of that slot, so it excludes the senders and receivers of the same object and nothing else. The insertion into the event list holds I of the slot as well. Without it, a waiter placing itself on a list under T alone would race with the owner of another waiter of that list, which removes its item under I alone.

Object sections cover the kernel objects' own data. The queue, event group, stream buffer and timer sources, and the per-task fields that only their own API writes, use them:

| Source | Lock key | Notes |
| --- | --- | --- |
| Queue, semaphore, mutex, event group, stream buffer, message buffer | the object | Blocking waits hold T of the slot with the mask open. |
| Timer fields: auto-reload flag, active flag, identifier | the timer | The timer API reads and writes them under the lock; the service task updates the status byte without it. |
| Task fields: application tag, task number, thread-local-storage pointers, the snapshot that `vTaskGetInfo` reads | the task control block | Each is written only by its own API; the snapshot only reads. |
| A task's event-list item value (`uxTaskResetEventItemValue`) | the slot the task last waited on | The same I that the waker and the owner use when they edit the item. |

Invariants.

- **Lock order.** Global locks, then at most one task-side slot and one interrupt-side slot per CPU (nesting is on the same object or one in the same slot, asserted), then the donation lock. No section waits for an owner acknowledgment while holding a lock; commands and publications defer to the outermost release, as before.
- **No lost or duplicated task.** A waiter leaves an event list by exactly one of two paths: the waker's `xTaskRemoveFromEventList` or `vTaskRemoveFromUnorderedEventList` under the object's T and I, or the owner's timeout, abort or detach under the same slot's I. The outcome compare-and-swap that arbitrates them, the wait generation, the pins and the deferred publication are unchanged. The interrupt-side lock of the slot orders list edits (insert, remove, priority reorder) against each other.
- **No priority inversion.** Locks are FIFO tickets, so a holder is never overtaken. Donation and its removal still hold the donation lock across the read-modify-write of the donated priority and the mutex count, and publish the repair at the outermost release. A mutex shares a slot with other objects only as a longer queue, never with an unbounded wait: every holder runs a bounded section.
- **Affinity masks.** Placement, publication, migration and idle pull are untouched; the change adds no path that moves or queues a task.
- **Event-driven.** Waiting is `WFE` on the ticket, release is `SEV`; there is no polling and no tick dependency.
- **Idle.** The idle task is still `wfi`; the change adds nothing to its path.

An event group's waker walks the waiter list: it reads an item and its successor, tests the item's wait bits, may remove the item, and moves to the successor. Under T alone, the owner of a waiter could time that waiter out, under I, between the read and the use; the walk then removed an item that had already left the list and whose list reference had been released. The walk therefore reads a waiter and decides on it under I, and it reads the next waiter only after it takes I again. The queue sources remove only the head of a waiter list, each time inside a critical section or a `FromISR` section, so they already hold I. Stream buffers and task notifications keep no waiter list, and the timer service's lists hold timers, which no owner edits. Queue sets are refused at compile time: a member queue would wake the set's waiters under the member's slot, while the set's owner times them out under the set's slot.

`xEventGroupSetBits` keeps its place in the list with a cursor: a list item on the walker's stack, linked into the list but counted in no length and owned by no task. Under I, the walker reads the waiter after the cursor, moves the cursor past it, tests its bits and wakes it if they match; then it releases I and takes it again before the next waiter. An owner that times out, aborts or detaches a neighbour of the cursor while I is free unlinks that neighbour with the ordinary list removal, which relinks the cursor. No waiter joins the list during the walk, because joining takes T, which the walker holds throughout. Owner repair re-sorts only ordered lists, and an event group's list is unordered, so no sorted insert compares against the cursor. `vEventGroupDelete` needs no cursor: it removes the first waiter under I, releases I, and repeats until the list is empty.

The cost of the walk was counted on QEMU (instructions per guest core, attributed by the core-index register) and converted to cycles from cache-line counts. The cycle figures are estimates until a board run measures them.

- **Lock hold per waiter.** The walker's own instructions under I are 15 to 32 for a waiter that does not match and up to 132 for one that it wakes, inside 114 instructions that raise the mask and take I and 150 that release I and open the mask. A waiter that does not match costs about one cache line, the next waiter's item, which the cursor link writes. A woken waiter costs about five (its item and task record, the outcome word, the neighbours it unlinks and the owner CPU's current task), and the lock's line adds one when another CPU took I since the last release. At the measured costs of a line transfer between cores (142.5 to 153.5 cycles) and of an L3 refill while other cores stream (about 1,670 cycles), and at one cycle per instruction outside the line transfers, a hold costs at most about 1,300 cycles (0.7 µs at 1794 MHz) on idle caches and about 10,400 cycles (5.8 µs) under streaming.
- **Interrupt latency.** The walking CPU takes API-level interrupts between waiters, so the walk delays an interrupt by at most one hold: about 0.7 µs on idle caches and 5.8 µs under streaming, for 4 waiters and for 119. A CPU that needs I of the same slot, to time out a waiter or to run a `FromISR` section, waits for at most one hold for each CPU queued ahead of it. With I held across the walk, the same figures were 1.7 µs and about 19 µs for 4 waiters, and 50 µs and 550 µs for 119.
- **Walk under T.** The release and retake cost 264 instructions per waiter, including two supervisor calls that write the priority mask with barriers. A walk that wakes all 119 waiters therefore executes about 47,000 instructions under T, against about 16,000 with I held across the walk. With its 595 lines at the costs above, it holds T for about 136,000 cycles (76 µs) on idle caches and about 1,050,000 cycles (585 µs) under streaming. The walker's CPU defers preemption for that long, and tasks that use an object of the same slot wait for it. The cursor bounds interrupt latency and the hold of I; it does not bound the hold of T. This bound is accepted while event groups have a few waiters each, and needs a design that also releases T between waiters if a workload puts more than a few waiters on one event group.

The walk is bounded by the tasks that can wait: at most 119 waiters, the 128 task slots (`SS_OWNER_SLOTS`) less the eight idle tasks and the caller.

The perturbation sweep of `checks/scheduler-control-linked.py` delays the walk between two waiters, with I free and the cursor parked (site `object-walk` in `kernel/scheduler/perturb.h`), in `scheduler-objects-qemu`, where one task on CPU0 sets bits for waiters on CPUs 4 and 5 that time out after two ticks. All three delays (300, 1,000 and 3,000 loop turns) pass. A walk that instead reads the next waiter before it releases I and uses it after it takes I again fails at 1,000 and 3,000 turns on the waker's assertion that it still holds the waiter's list reference, the same failure as a walk under T alone.

Host check (`scheduler-objects-qemu`, eight cores on the instruction counter). Phase 1 holds the lock of one queue open inside its critical section until six other cores have each completed twenty operations on queues of their own, or a budget of the holder's loop iterations runs out. It counts operations and iterations, not time. If the queues shared a lock, the other cores could not run, the budget would run out on all 40 holds and the check would fail; with object locks none runs out. Phase 2 mixes producers, consumers with one-tick timeouts, interrupt-level senders on every core, a mutex with inheritance, an event group, a stream buffer and a counting semaphore, and checks that no message is lost, duplicated or reordered and that the mutex excludes. Its waits are liveness bounds, 4 times the slowest value measured over every perturbation site and delay, and they, its polling interval and the interrupt-level senders' period scale with an injected delay; the message and event-group counts must reach a quarter of the fewest measured. The linked-kernel check and the other emulator cases (kernel, concurrent, soak, yield, storm, heap, runtime) pass with the change.

### Sections that keep the global locks

The public critical-section API (`taskENTER_CRITICAL`, `taskENTER_CRITICAL_FROM_ISR` and their exits, as `vTaskEnterCritical` and `vTaskEnterCriticalFromISR`) takes the two global locks and does not exclude object sections. The kernel takes no application lock inside an object section, and the order is global locks first, then one slot. These users remain:

| User | What the section protects | Why it keeps the global locks | To convert it |
| --- | --- | --- | --- |
| Driver and application critical sections (the DMA, GPU and NPU services, the board, RTC, UART, SPI, PCIe, I2S, ISP and thermal applications, the OS application and large-load code, the deadline table, the C-library lock pool, the latency recorder; 124 sections in 44 files) | Device state, request slots and tables owned by the caller, and interrupt-handler metadata. Several enclose a kernel call (a queue send or a task notification) so that a slot reservation and its publication are one transition. | The data is not kernel-object state, so no object lock can stand for it, and the interrupt handlers that share it need a lock that also masks interrupts on all cores. Object sections nest inside these sections, which the lock order allows. | Give each driver its own lock, or make its state lock-free with a single publication. This is driver work, not scheduler work. |
| The timer source's one-time initialisation (`prvCheckForValidListAndQueue`) | The timer lists and the creation of the timer queue. | It runs once, at the first timer creation or at scheduler start. Creating the queue enters the queue's own section inside it, and a CPU holds at most one slot at a time, so the enclosing section cannot be an object section. | Create the timer queue in a step that runs before any other CPU or task can create a timer, and drop the lock. |
| The timer service task's suspension (`prvProcessTimerOrBlockTask`) | The service task's sample of the time and its decision to block. | The task lock is global, not per object: timer callbacks run under the suspension when the tick count wraps, and a callback may call any object's API, so the suspension cannot be tied to the slot of one object. The OS images build without the timer service. | Run the callbacks outside the suspension, then key the suspension on the timer queue. |
| The public scheduler suspension (`vTaskSuspendAll`, `xTaskResumeAll`), used by the heap allocator and the C library's allocator lock | The heap's free list, and a stop of scheduling on every CPU while the holder runs. | It is the scheduler's own gate: every CPU's owner acknowledges it before the holder continues, so allocator calls serialize against each other and stop task switches. It is not an object lock. | Replace the allocator's exclusion with its own lock, so that an allocation no longer stops the other CPUs. |

The scheduler's own state does not use the global locks. Ready lists, deadlines, the registry and the control queues belong to one owner per CPU, and other CPUs reach them by publishing a command that the owner applies; the suspension gate above is the one place where all owners stop together. Replacing them is a redesign of the owner protocol and is not part of the object locks.



## Capacity hints guide placement without weakening priorities or affinity

Status: open. Placement today follows creation order (fewest assigned tasks) and
affinity masks; the topology table is not an input.

Task attributes will add a class (latency-critical, throughput, background,
interrupt-bound), an allowed CPU mask, a migratable flag and an estimated demand in
fastest-core capacity units. Defaults preserve existing affinity and priority
behavior; hints never authorize a disallowed CPU or let lower-priority work preempt
a higher-priority local task. Initial placement chooses the fastest eligible
available capacity for heavy, latency-critical and throughput work. Background and
housekeeping work prefer LITTLE capacity when deadlines and measured energy support
it. Interrupt-bound work prefers its consuming IRQ's eligible core, and reserved
CPUs are excluded. When all eligible CPUs are busy, deterministic least-load
placement uses normalized declared demand; admission is a separate contract.

Accounting reads the generic counter at task transitions and IRQ entry and exit,
without a periodic utilization tick. An execution EWMA per task is optional; static
application estimates bootstrap placement. Neither PELT-like tracking nor a scalar
capacity ratio predicts every integer, SIMD and cache workload, and PMU-based
classification waits for a demonstrated need. The clock owner supplies effective
applied rates and thermal restrictions; unknown transition state invalidates
rate-normalized estimates. Only background placement may compare LITTLE execution
against BIG race-to-idle, and only with valid energy costs. A continuously running
task whose demand changes is reconsidered at the next IRQ or accounting boundary,
hint update or idle pull; there is no hidden periodic utilization sampler.

## Applications query topology and split parallel work proportionally

Status: implemented and qualified on the board for the maximum-weight split;
effective-capacity splitting is open. `include/sunstone_app.h` and
`include/sunstone_cpu_topology.h` declare:

```c
int ss_cpu_topology(struct ss_cpu_info *out, size_t capacity, size_t *count);
int ss_work_split(uint64_t units, uint64_t cpu_mask, uint32_t quantum,
                  struct ss_work_range *out, size_t capacity, size_t *count);
```

`ss_cpu_info` reports logical CPU, affinity, microarchitecture, physical and SMT
relationships, cache and domain IDs, maximum and effective capacity and validity
flags; the immutable snapshot identifies its calibration and operating-point epoch.
The syscall copies a bounded table and inner compute loops retain the result. The
board table holds the top operating point only, so `ss_work_split_max()` (maximum
weights) works and `ss_work_split()` needs effective-capacity data.

The split helper assigns aligned initial ranges in proportion to effective
capacity. Ranges are half-open `[begin, end)`. Full quanta are distributed by
integer largest remainder with CPU ID as the tie-break; only the final range may
hold a shorter tail; no unit is omitted or assigned twice; empty ranges are
permitted when units are fewer than workers. Insufficient output capacity returns
the required count without writing partial ranges; arithmetic overflow and an empty
eligible mask return an error. Zero or unknown capacity returns an explicit
uncalibrated result and callers may split equally. Optional dynamic chunking uses a
bounded atomic next-chunk index in application-owned storage. A later operating-point
change makes the estimate stale without invalidating ranges. The OS supplies
topology and integer ranges; the application chooses kernels, tensor tiles and
numerical ordering, and no application header enters the OS.
`SUNSTONE_A733_CPU_TOPOLOGY` (default off) selects the topology build.

## One-shot local timers replace periodic scheduler ticks

Status: open. The fork keeps the periodic 1 ms tick: CPU0's PPI30 advances global
time (`ssKernelTick`), samples every owner's next deadline and slice deadline, and
forces owner service on a remote CPU when one is due. Secondaries execute WFI when
idle and receive only SGIs. `configUSE_TICKLESS_IDLE` is rejected at compile time.
The sub-tick wait for drivers is a separate consumer:
`sunstone_deadline_wait_until()` (`include/sunstone_deadline.h`, `runtime/deadline.c`) shares CPU0's generic-timer compare through a fixed 16-slot table (`DEADLINE_SLOTS`).

The design for the replacement: the physical generic counter defines one boot epoch,
and the compatibility tick is `floor((counter - epoch) * 1000 / counter_hz)` reduced
to `TickType_t` with upstream wrap behavior, computed without overflow by quotient
and remainder; no secondary counter increments global time. Each CPU owns PPI30 and
programs the minimum of its next timeout, slice expiry and later reservation
boundary; an empty set disables the timer. Timer IRQ processing reads the counter,
expires a bounded batch, selects urgent work and programs the next compare;
remaining expired work schedules another local entry after interrupts are restored.
EL0 callers use the privilege transition for timer programming and SGIs; the port
orders device writes, uses ISB after timer register programming and observes GIC
write-pending completion before readback. Software timers use the timer-service
task, with the next command or expiry on owner deadlines. Tickless idle needs a cross-core sleep protocol. The protocol has a privileged idle entry, a sleep generation number,
acknowledgment from every idle core, atomic cancellation of admission or a CPU0 wake
before remote callbacks run, tick correction from the counter, and no kernel lock
held across WFI or an acknowledgment wait. The upstream generic path is unusable
here (its idle check rejects more than one idle ready list, and it keeps the task
lock across the sleep).

## Primitive bounds include waiting, copying and holder progress

Let `S_r,c` be the maximum service time of object `r` on CPU `c`, including allowed
IRQ interference and release. With one outstanding internal request per CPU and
FIFO non-preemptive acquisition, `wait_r <= sum over other c of S_r,c`; with at
most eight CPUs and a uniform bound `S_r`, wait is at most `7 S_r` and exclusion at
most `8 S_r` ([MSRP assumptions](scheduler-research.md#locking-bounds-depend-on-progress-rules-and-workload-contracts)).
The sum applies to non-nested locks; where task control or mutex priority repair
nests locks the bound includes the complete wait and service of each nested lock.
Object-to-control reverse nesting is forbidden.

| Primitive | Bound and remaining obligation |
| --- | --- |
| Local ready operation, uncontended local notification-give | No shared-lock wait; bounded local instructions plus admitted higher-priority IRQ interference |
| Remote activation publication | One native release RMW plus the status and SGI handshake; coherence latency is measured |
| Queue send and receive, semaphore | FIFO bound with item-copy bytes in `S_r`; bulk copy outside a lock needs a separately proved reservation protocol |
| Notification conflict, compound owner operation | Per-slot FIFO bound after one failed fast CAS or direct gated admission |
| Event-group set, stream-buffer completion | FIFO bound with actual bytes and awakened-waiter count; wake-many is `O(k)` for `k` wakeups |
| Local application mutex | Priority inheritance (simplified upstream semantics) limits inversion only under admitted critical-section, nesting and arrival bounds |
| Cross-core application mutex | The metadata FIFO does not bound sleeping ownership; a hard bound needs a declared critical-section contract and a cross-core progress protocol |
| Global critical and suspend compatibility | Caller duration plus checkpoint and IRQ bounds; excluded from the 25 µs promise and measured separately |
| Migration and deletion | Bounded command storage and checkpoints; completion takes acknowledgments and has its own slow-path deadline |

The latency-qualified queue class uses at most 64 copied bytes per operation and
sixteen selected wakeups per batch; larger valid objects are supported but need their
own service bounds. For short internal locks, `8 S_r` plus local bookkeeping must fit
in 25 µs. WFE reduces active waiting but does not remove the waiting CPU's
non-preemptive blocking.

## Masked-span cost follows dependent cache-line refills

Instruction budgets for the owner paths are provisional until linked-code counts and
board samples replace them: ready enqueue or remove at most 64 retired instructions,
pick or yield 96, untimed local notification-give 160 before an optional switch.
Remote wake, incoming scan (at most 256 slot-word loads), timed block, timeout,
migration and context switch are counted on the linked path. no host check
reports every interrupt-masked span of the CPU latency workload on eight emulated
cores as scheduler atomic stores, read-modify-writes and atomic loads; the counts
do not convert to time because the emulator has no cache misses.
`SUNSTONE_LATENCY_PMU` adds instructions retired, cycles, L1D and L2D refills, and
backend stall cycles inside each core's longest masked spans; the counters are read
only. On the board, spans of 17 to 30 µs retire 770 to 2100 instructions with 20 to 60 L1D refills and 25 to 50 cycles per instruction. `STALL_BACKEND` is 94 to 98% of
their cycles (59 to 68% on core 3's critical section) while eight cores stream 1 MiB
sweeps. No host trace counts the barriers, SGI writes and exception instructions per
span, and whether any span contains a Device or non-cacheable access is open. A service entry with two `DSB SY` and 35 L1D refills stalls about
33 000 cycles, 0.5 µs per refill if the refills serialize behind a saturated DRAM,
whereas the barrier drain alone would need about 8 µs per `DSB`; the refill reading
fits better. Spans therefore shorten with fewer distinct lines per entry or
overlapped fetches (prefetch, not implemented). The values are in the records
`scheduler-partitioned-masked-span-instruction-mix`, `1793591168258054823-33d41fe5`
and `1793593624607852116-12606140`. PMU instrumentation compiles out when disabled.

## EDF/CBS is a second-stage policy with explicit admission

Status: open. After per-core fixed priority, object locks, migration and one-shot
timing are qualified, per-core EDF constant-bandwidth servers add reservations with
runtime, relative deadline and period. Exhaustion stops the server until its
one-shot replenishment, with accounting from the counter at transitions. The band
sits below the highest fixed-priority emergency and IRQ work and above ordinary
background work. Admission subtracts bounded higher-band demand, interrupts, object
blocking and scheduler overhead from each CPU's calibrated capacity; migrating a
reservation needs destination admission. Budget accounting and mutex inheritance
must state whose runtime is charged. GRUB reclaiming and proxy execution are later
options ([EDF/CBS research](scheduler-research.md#linux-distributes-state-but-still-uses-runqueue-locks)).

## Validation exercises ownership transitions and hardware completion

Host-tested: `kernel/scheduler/model/` holds an executable TLA+ abstraction of the
wake protocol with per-CPU publication buffers, per-location coherence, acquire
observations and DMB store-to-load constraints. It explores publication,
PREPARING and BLOCKED races, timeout winners, dispatch lowering, pending-SGI
clearing and idle entry with two producers and one owner, migration with a second
owner, and generation reuse, and checks exactly one activation. Negative models
that remove a barrier, reference or winner check must produce a lost-wakeup or
unsafe-reuse trace. Linked-kernel stress (`checks/scheduler-kernel-*.c`,
`checks/scheduler-partitioned-qemu.c`, `scheduler-yield-qemu.c`) runs real task,
notification, queue, event and timer APIs on eight concurrently executing QEMU cores. It forces wakes during block commit, timeout and remote priority changes, migrates permitted tasks and deletes and recreates static tasks while publications are outstanding. Assertions detect duplicate running ownership and
non-owner link writes. These establish properties of finite abstractions, not Arm
instruction correctness or latency bounds.

Each live board stage records an image hash, revision, queue entry, exact output and
normal return, with idle and loaded ready-to-running and IRQ-to-task p50, p99 and
maximum, lock acquisitions per operation, every lock's wait and hold, context
switch, mask duration and idle wakeups per CPU. The CPU0 periodic tick is the
idle-power baseline. Capacity calibration runs the identical fixed integer and SIMD
kernel on each CPU at the vendor top points, normalizes the fastest to 1024 and
retains workload-specific ratios. Energy per job is reported only if the power-supply
app exposes calibrated power and sampling bandwidth; voltage alone does not give
energy.

## Open work

1. Task-path wake latency to the 10 µs target (99.99th percentile 19 to 26 µs at 1, 2
   and 4 kHz). The gap from handler to task start carries the tail: control drains
   and idle-pull transfers of 5 to 17 µs and task switches up to 11 µs. Shorten the
   wake path (fewer supervisor calls and barriers, no owner pass before the switch,
   bounded drain work per exit). Isolate local mask admission, dispatch and
   recorder finalization costs. Qualify the complete bounded control batch and
   explain the CPU0 wake regression; earlier runs lack interrupt identities.
2. Bound compatibility sections that retain the global locks. Per-object locks
   are implemented and close the event-list race without an established
   task-path latency improvement.
3. Run five soaks of the global-lock images with the notification change to isolate
   its 4 kHz improvement from the per-object locks.
4. Repeat the A76 task-path soak and measure recognition time with the task path on
   those cores.
5. Control-path long runs (10^6 periods) at 1 to 8 kHz; only 10 kHz has them.
6. Bound the owner work per interrupt exit, and shorten the yield supervisor call,
   the mask supervisor calls and the FromISR mask pair.
7. An abortable queue lock that lets a waiter take its place in line with the mask
   open; tickless idle (`work/tickless-idle-1101`, changes the protected
   `patches/freertos-smp-*.patch`) with control-period jitter measured with and
   without it.
8. Sweep the eight-core soak for masked spans (the 95 to 206 µs stock spans seen
   there did not recur under the mimus load).
9. Idle pull: fix the missed pull or measure it on against off on the board.
10. One-shot timers, capacity hints with topology-driven placement, effective-capacity
    work splitting, EDF/CBS reservations, and the capacity, weighted-split,
    pinned-versus-unpinned and idle-wakeup measurements of the acceptance list.
11. Heterogeneous placement and tickless timing have no physical qualification.

## Design review

Two independent reviews (correctness; performance and progress) approve staged
implementation. Neither approval establishes physical timing. [The review
inventory](scheduler-partitioned-reviews.json) pins their reports; research accuracy
review is recorded in [the research document](scheduler-research.md).
