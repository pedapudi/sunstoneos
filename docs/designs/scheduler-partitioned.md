# Partitioned FreeRTOS scheduling with topology-driven placement

Status: **proposed; implementation and board qualification are pending.**

This design keeps the FreeRTOS task, queue, semaphore, mutex, event-group,
stream-buffer, notification and timer APIs. Each CPU owns its scheduling state.
The application kit adds optional placement hints, topology queries and work
splitting. Drivers continue using direct ISR-to-task notifications.

The [reviewed research](scheduler-research.md) ranks removal of shared scheduler
ownership first. ghOSt contributes sequence-tagged observations, validated remote
commands and interrupt coalescing. Linux contributes queued activation and
target-side work. These mechanisms execute inside the kernel; no policy-agent
task sits between a caller and local dispatch.

## Acceptance compares each live stage with the current kernel

The current kernel has task-lock waits of 177.875 µs and loaded ready-to-running
latency of 133.333 µs in entry `1793560934366050732-4e45b5ea`. The CPU control
records a 62.750 µs workload mask interval in notification-take and 73.417 µs
task-lock waiting on core 6 in entry `1793559978973913698-7f7e36e0`.
[Source decode and measurement limits](../status/latency.md)

Acceptance requires:

- No shared scheduler-state lock for local wake or context switch. The
  uncontended local notification path also acquires no object lock. Same-object
  contention can take its FIFO fallback; record that count and wait separately.
  An unconditional zero-object-lock claim remains outside this stage's guarantee.
- Loaded ready-to-running p99 below 10 µs; workload mask and lock-wait maxima
  below 25 µs. Record p50, p99 and maximum per CPU and phase.
- Correct immediate preemption by a higher-priority eligible task, including
  publication concurrent with priority lowering and WFI entry.
- No scheduler timer interrupts without a deadline or equal-priority local
  slice competitor; no increase in per-core idle wakeups.
- Eight-core QEMU soak, linked-kernel stress and exact mimus recognition after
  each live stage. Source compilation and isolated helper tests are insufficient.
- A measured capacity ratio, weighted-versus-equal application splitting and
  big-pinned-versus-unpinned latency at the existing vendor top points.

Observed maxima are not worst-case proofs. Every analytic bound names its
admission assumptions; every instruction estimate remains provisional until
linked-code counting and board measurement replace it.

## A forked mechanism preserves the public APIs and static storage

The pinned upstream kernel is
`97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2`. Reproducible patches remain the
fork boundary; downloaded upstream source stays in the cache. New owned code
lives under `kernel/scheduler/`. `configSS_PARTITIONED_SCHEDULER` selects the
fork, and `SUNSTONE_PARTITIONED_SCHEDULER` is ON by default (the long runs on the board put the fork ahead on the task path and equal on model time; see the scheduler status). `-DSUNSTONE_PARTITIONED_SCHEDULER=OFF` builds the current-kernel control.

| Existing source | Required change | Preserved contract |
| --- | --- | --- |
| `patches/freertos-smp-tasks.patch` | Replace global ready/delayed-list manipulation with owner operations; route remote control; adapt every blocking and priority-change path | Task handles, priority numbering, affinity APIs, notification semantics, static task allocation and trace events |
| New `patches/freertos-partitioned-objects.patch` | Replace kernel-wide critical sections with object locks and owner wake publication in `queue.c`, `event_groups.c`, `stream_buffer.c`, `timers.c` | Existing API signatures, ISR restrictions, queue data ordering and timeout results |
| New `patches/freertos-partitioned-layout.patch` | Extend internal task/object layouts and matching `StaticTask_t`, `StaticQueue_t`, `StaticEventGroup_t`, `StaticStreamBuffer_t` descriptions | Static allocation sizes remain checked; TCB stack pointer remains first |
| `patches/freertos-smp-c.patch`, `freertos-smp-macro.patch`, `freertos-smp-asm.patch` | Add local interrupt exclusion, owner entry/exit, compare and conditional SGI operations | Context frame, SIMD/FPU save policy, EL0/EL1 paths and current-task register ABI |
| `platform/a733-smp-cached/` | Dispatch SGI and PPI30 to the local owner; preserve GIC completion and return cleanup | Probe entry belongs to the probe integration stream; existing startup/restoration contracts remain authoritative |
| `include/FreeRTOSConfig.h` | Fork feature selection, static limits and one-shot compatibility clock | Eight fixed priorities initially; 1 ms `TickType_t` units; preemption enabled |

No Linux GPL implementation is copied into the FreeRTOS mechanism. The fork
retains upstream MIT notices. Research references establish algorithms and
their limits; [the source inventory](scheduler-research-sources.json) records
provenance and licenses.

The default static limit is 128 registered tasks, including idle and service
tasks. Registration consumes a fixed slot; deletion releases it only after all
owners acknowledge outstanding references. Increasing this limit changes the
storage and batch bounds and requires corresponding validation.

Compile-time checks compare internal and public static-object size/alignment,
TCB stack-pointer offset zero, port current-task/FPU offsets, and cache-line
alignment of ticket fields and notification words. Control and partitioned
patches apply to separate generated source trees; neither mutates the cached
upstream checkout. Every task has at most one blocking deadline. The owner
heap has 128 entries; slice expiry and the software-timer service's next wake
are separate local compare candidates. Reservation storage is added only in E.

## CPU topology is immutable data; observed operating state is separate

The scheduler consumes a validated topology view at boot. SoC data contains CPU
identity, domains and caches. Board data selects admitted CPUs, reserved CPUs
and default task masks. Online state, observed identity and applied operating
point are separate runtime records.

| Data | Fields and units | Owner and validation |
| --- | --- | --- |
| CPU identity | Logical ID, full MPIDR affinity, MIDR/microarchitecture, physical-core and SMT-sibling IDs | SoC description; compare each CPU's captured MPIDR/MIDR against its entry slot |
| Capacity | Maximum relative capacity, fastest admitted physical core = 1024; provenance, workload and confidence | Calibration record; unknown is explicit and cannot silently become an A76/A55 MHz ratio |
| Performance domain | Member CPUs, clock domain, rail domain, available OPPs for the observed bin, applied OPP | SoC description references the existing clock owner's tables and readback |
| Energy | Per-OPP measured or vendor-specified power, units and provenance; unknown permitted | Energy placement stays disabled without valid costs; voltage/frequency pairs alone do not establish energy |
| Cache | Level, instruction/data/unified type, size, line size and sharing CPU mask | SoC data checked against admitted CLIDR/CCSIDR observations; private versus shared is explicit |
| Interrupt eligibility | Device-IRQ-capable mask and excluded/reserved targets | Interrupt controller and board policy; use full affinity for GIC routing |
| Board policy | Admitted, reserved, default service and application masks | `drivers/board/orangepi_zero3w.c`; reservations describe actual consumers |

The A733 SoC record belongs in `platform/a733/cpu_topology.c`. It describes six
A55 and two A76 CPUs in one DynamIQ cluster, with private L1/L2 and shared DSU
L3. CPU-L and CPU-B have distinct clock domains; CPU-L and DSU share DCDC5.
CPU-B uses DCDC3. Existing descriptions record A55 slots 0–5 and A76 slots
6–7, with the core index in MPIDR Aff1 and Aff0 zero.
[Identity and cache evidence](../cpu-coherence.md),
[clock/rail ownership](cpu-top-opp.md), [routing](../cpu-continue.md).

The scheduler does not assume that recorded slot order proves a new board's
mapping. A queued qualification captures logical ID, MPIDR, MIDR and cache
geometry on all eight CPUs before interpreting capacity results. Synthetic
symmetric and three-cluster tables must select the same generic algorithms.
SMT tables identify siblings; their capacities are not added as independent
physical-core throughput without a shared-resource calibration.

For bin vf0400, the existing top points are CPU-L 1794 MHz, CPU-B 2002 MHz
and DSU 1274 MHz. Other bins use their own table. Placement observes
`include/a733_cpu_clock.h` and `apps/os/cpu.c`; it never writes PLL, PMIC or
thermal state. No new voltage or frequency is needed for this design.
[Vendor derivation and sequence](cpu-top-opp.md)

## Per-core runqueues have one writer and constant-time selection

Each cache-line-aligned `ss_sched_cpu` contains:

- Eight FIFO ready lists, an eight-bit ready bitmap and the current task.
- Local interrupt-exclusion and preemption depth, pending-reschedule state and
  the published dispatch status.
- A fixed incoming-wakeup set, a separate remote-control mailbox and lifetime
  acknowledgments.
- A fixed-capacity deadline heap, its next compare, a slice deadline and local
  accounting timestamps.

The structure is laid out in cache lines by writer. Owner-only state (ready
lists, current task, detached batches, cursors) shares no line with state that
other CPUs write (the incoming wake words, the dispatch status, the control
mailbox words, and the slice and next-deadline snapshots that CPU0's tick reads).
Each CPU's control mailbox and cursor have a line of their own, and per-task
records start on a line boundary. Compile-time assertions enforce the grouping.
A masked scheduler entry then misses only on lines that its own work needs; the
A733 runs with caches cold after a 1 MiB sweep, where each avoidable miss adds
roughly the cost of a DRAM access to a span that the 25 µs bound limits.

Only that CPU's task/IRQ scheduler entry changes its ready lists, current task
or deadline heap. Local API interrupts are masked while changing those fields.
No remote CPU writes a list link. Running tasks are absent from the ready FIFO.
An interrupted current task returns to its priority's head unless it yields or
uses up a slice, when it joins the tail. A newly ready task joins the tail.
Selection uses CLZ on the nonzero bitmap, then removes one list head.

Time slicing runs only while a non-idle task has a local equal-priority
competitor. The compatibility quantum starts at 1 ms; measurement can justify a
different explicit setting. Higher-priority preemption does not wait for a slice.
An idle task has no slice deadline and executes WFI.

The first implementation calls fixed-priority selection and transition accounting
directly. EDF/CBS later introduces a policy interface when its periodic-service
consumer and admission test exist. That interface must remain bounded,
allocation-free and callable directly in the owner context.

## A fixed-slot remote wake list avoids unbounded producer retry loops

The logical multi-producer wake list uses one pending bit per task slot, per
target CPU and priority. At 128 slots and eight priorities this takes 128 bytes
per CPU, plus an eight-byte atomic summary word forming the logical list head.
A publisher release-ORs its task bit, then release-ORs its bucket bit into the
head. The old head identifies empty-to-nonempty publication. The owner
acquire-detaches the head before detaching slot words and visiting bits with CLZ.
The bit is a level-triggered hint to inspect authoritative state; it carries
no generation and cannot itself authorize activation.

This bounded representation replaces an intrusive Treiber stack. A Treiber
producer is lock-free but can retry indefinitely under continuous competition.
The fixed-slot representation gives bounded producer instructions without
allocation, node reclamation or head-pointer retry. A slot may reach an older
inspection before its delayed head publication. The later empty-bucket inspection
is harmless; the summary and slot bits authorize no task transition by themselves. Repeated notifications
coalesce activation; their values/counts remain in the notification object.

The priority FIFO order is the owner's enqueue order. Publications from
different remote CPUs do not establish a total FIFO order among themselves.
Within one detached word the owner uses a rotating slot cursor to avoid a
permanent slot-number preference. The FreeRTOS API's notification values and
event-waiter selection remain serialized by their own object rules.

Native AArch64 publication requires LSE atomic operations, admitted from
`ID_AA64ISAR0_EL1.Atomic`, and compile-time code generation without an LL/SC
retry loop. The board identity capture records that field. Host fixtures use
native lock-free atomics. An architecture lacking bounded native word RMW needs
a separately reviewed publication primitive; substituting an unbounded compiler
loop would violate this design.

Each owner retains detached words in its local pending set until processed.
Its cursor spans all 128 slots of a priority, including both words. Fresh
publications merge into that set without resetting the cursor. Each slot is
visited at most once per sweep; a continuously replenished first word cannot
overtake an unvisited second word. Effective-priority changes republish a hint
at the new priority and force an owner command/SGI, including detached work.

One entry activates at most sixteen tasks, checking for higher-priority incoming
work between activations. On batch completion it dispatches the highest ready
task; it may dispatch equal-priority ready work before another batch. It posts
one local continuation SGI if unprocessed hints remain. It completes IRQ exit
and restores interrupts between batches; immediate continuation can precede
application execution. It never runs lower-priority application work
while known higher-priority activation remains; successive urgent batches remain
bounded individually, while total interference depends on admitted arrivals.

At a fixed priority, a slot already pending at a sweep's start is inspected
within at most eight sixteen-slot batches, plus higher-priority admitted work.
The bound includes detached words. An urgent pending slot therefore has a finite
activation bound only with a stated higher-priority arrival bound. Measure
continuation IRQ cost and application progress; a self-SGI storm fails admission.
Ready-to-running timing starts at publication, including inbox processing.

## Conditional SGIs use a publication handshake as well as status words

Each CPU publishes a naturally aligned 64-bit atomic dispatch word containing
a sequence, running priority and idle flag. It is a single coherent observation;
remote readers do not use a seqlock retry loop. The sequence identifies changes,
but does not authorize modifying owner state.

The publication protocol is:

1. Waker updates the notification or wins the blocked-wait state transition.
2. Waker release-publishes the incoming slot bit and bucket-head bit, executes
   `DMB ISH`, then acquire-loads the target's dispatch word.
3. If the old logical head is empty, the task outranks the published current
   task, or the target is idle, the waker requests a reschedule SGI. Further
   equal/lower-priority publications into a nonempty head coalesce.
4. The owner acquire-detaches the logical head at entry and inspects a bounded
   batch. Before committing dispatch it release-publishes its status and clears
   the SGI coalescing flag, executes `DMB ISH`, then acquire-checks all incoming
   and detached work. Remaining work requests a bounded continuation even when
   its priority is lower than the selected task.

The two store-to-load barriers forbid both CPUs observing the old state and
omitting the required wakeup. Acquire/release alone does not prohibit that
store-buffering outcome on AArch64. Negative model variants remove essential ordering and must produce a
corresponding lost-wakeup trace. Redundant checks need no artificial counterexample.

The final incoming check and WFI occur with DAIF.I masking IRQ exception entry
and with the GIC priority mask admitting the reschedule interrupt. A pending
eligible IRQ prevents indefinite sleep; unmasking dispatches its handler.
The final Arm WFI/exception sequence is modeled and checked on the board.
An idle flag remains published until the owner has resumed selection.

Reschedule requests coalesce per target with an atomic pending flag. The owner
clears the flag before its final inbox check, so a concurrent publication can
request another SGI. The full-state/inbox handshake remains authoritative;
the pending flag is only an optimization. A batch never delays an urgent wake
to accumulate more targets.

The final owner sequence orders the pending clear before the same store-to-load
barrier that precedes inbox inspection:

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
```

A publisher delayed after claiming the SGI flag still holds a reference and
executes a bounded non-preemptive publication sequence. Its eventual redundant
SGI is harmless. A pending or active hardware SGI does not substitute for
the final authoritative check. The model treats flag clearing, interrupt
request, active/pending delivery and WFI wakeup as separate events.

GIC SGI target-list bits address Aff0 within a common higher-affinity tuple.
A733 CPUs differ in Aff1, so explicit delivery to several CPUs needs several
SGI writes. A future topology with common Aff1 and distinct Aff0 can batch them
into one write. Completion before SGI uses the architecture port's existing
`DSB ISHST` ordering, followed by the system-register synchronization required
by that port. [A733 SGI contract](../cpu-continue.md)

## Object state and owner state have separate locks and linearization points

| State | Writer and protection | Permitted takers |
| --- | --- | --- |
| Local ready links, bitmap, current task, timeout heap | Owner CPU with local API IRQ exclusion | Owner task/IRQ scheduler only; no shared lock |
| Incoming activation bits and dispatch word | Release/acquire atomic operations | Any publishing CPU; owner consumes |
| Queue, semaphore and mutex metadata and event waiters | One FIFO lock embedded in each object | Calling task or permitted ISR; no scheduler locks |
| Event-group bits and stream-buffer metadata | Their own FIFO object lock | API callers; preserve documented single-producer/consumer restrictions where applicable |
| Notification value/state | One-CAS fast transaction, then gated FIFO fallback on conflict; every observer follows this protocol | Calling task/permitted ISR. Uncontended local wake takes no shared lock; contention takes only the slot's object lock |
| Task wait state, generation and owner | Atomic packed state; owner commands arbitrate control changes | Waker claims a wait outcome; owner changes placement and lifetime |
| Registry, creation/deletion and public global suspend gate | Separate administration lock and acknowledged owner commands | Task slow paths; never normal wake/switch |
| Public `taskENTER_CRITICAL` compatibility | Retained recursive application lock plus local interrupt exclusion | Existing application/driver callers; kernel internals use local/object primitives |

Object acquisition masks API-capable local interrupts before joining its FIFO.
This prevents an ISR waiting on a lock held by the task it interrupted. Waiting
and ownership are non-preemptive. Waiters use SEVL/WFE with an acquire owner
check; release publishes ownership, completes the store before SEV and preserves
the check-to-WFE event-register protocol. Non-API high-priority interrupts never
take these locks. Nesting follows a checked lock order; no object lock is held
while waiting for an owner acknowledgment.

Each object has a 32-bit ticket allocator and serving counter on separate
cache lines. Native atomic fetch-add gives FIFO admission; release increments
serving and uses SEV. Outstanding tickets remain below half the counter range.
MCS is reserved for a demonstrated coherence bottleneck; its extra node and
interrupt-nesting lifetime rules currently have no measured justification.

### Every notification action uses one atomic transaction protocol

Each notification slot has one aligned 64-bit word: value bits 0–31,
FreeRTOS state bits 32–33, completed-wait latch bit 34, fallback-registration
count bits 35–41 and zeroed reserved bits 42–63.
The three states are NOT_WAITING, WAITING and RECEIVED.
The task's separate owner wait record contains the full 64-bit wait generation
and active notification index. The notification word has no truncated generation.

Every caller first pins the permanent task descriptor, masks local API IRQs,
and loads the word with acquire semantics. If its registration count is zero,
the caller computes the entire value/state transition and attempts exactly one
acquire-release CAS. Success is the linearization point; previous-value outputs
come from the matched snapshot. A no-overwrite refusal can linearize at an
acquired RECEIVED snapshot. A failed CAS enters the FIFO path without retrying.

The FIFO path registers with one acquire-release LSE add to the count field,
before obtaining a ticket or interpreting payload. Registration invalidates an
older zero-count fast snapshot and excludes subsequent fast transactions. The
ticket owner acquire-loads serving and then the latest word. It replaces only
payload bits with LSE clear/set operations that preserve concurrent count
changes. All observers honor the gate, so no API or scheduler reads the
intermediate cleared payload as stable state.

After committing payload, wait intent and required hint publication, the owner
release-decrements registration count, advances serving with release semantics,
completes publication and signals SEV. Count includes all registered waiters,
so a last-owner release cannot clear admission over a new waiter's registration.
At most one non-preemptive caller per CPU participates; nested calls on one
slot and calls from non-API interrupts are forbidden. Eight admitted CPUs
cannot overflow the seven-bit count or carry into reserved bits.

| Notification API operation | Atomic transaction or gated operation |
| --- | --- |
| Increment/give, set bits, overwrite, no action | Apply the action and set RECEIVED in one matched payload transaction; old WAITING also sets the completed-wait latch and requests owner inspection |
| No overwrite, with or without previous-value query | Fail if matched state is RECEIVED; otherwise replace value and set RECEIVED, also setting completed-wait latch when the matched state was WAITING |
| State clear or value-bit clear | Apply upstream state/value transform while preserving completed-wait latch; return the required old result |
| Wait/take entry, wait-index setup, timeout, suspension or deletion | Always gated; atomically with other slot callers inspect/clear/test state and commit the owner wait intent |
| Wait/take completion | Gated consumption/decrement/bit clearing and final timeout-boundary condition check, preserving upstream return values |

The slot word is authoritative for notification arrival. A successful change
from WAITING to RECEIVED also sets the completed-wait latch in that same CAS.
The producer does not race a second independent generic WOKEN CAS. Later
state clears, value changes and notifications preserve the latch, so they cannot
erase scheduler readiness already established by a notification.

While gated, wait entry completes the prior owner wait, clears its old latch,
tests state/value, clears entry bits as required and installs a new owner wait
record/deadline. It publishes WAITING and commits the local block intent before
opening the gate. Fast producers cannot overlap those intermediate writes. The
owner checks the latch/pending work before physically switching away. The latch
can remain set after activation; owner execution state prevents duplicate ready
insertion, and only a subsequent wait entry or retirement clears it.

A timeout gates the same slot before inspecting the matching owner wait record.
The completed-wait latch wins scheduler readiness even if another caller cleared
RECEIVED or the value. The returning API still performs its upstream final
state/value test, which can return failure/zero after such a clear. Otherwise
RECEIVED or a qualifying count wins the condition check; timeout
changes WAITING to NOT_WAITING and closes that owner generation. A later
notification remains pending for a subsequent call. Inbox consumption gates
the slot before interpreting its stable latch/state and activates only the
currently blocked owner wait once. Stale hints cannot set a completion latch
or turn WAITING into RECEIVED. The uncontended local CAS path may activate
directly under owner IRQ exclusion, retaining the latch until the next gated
wait; remote consumption uses the gated stable observation.

A fast producer holds a descriptor reference across snapshot, CAS and hint.
Its delayed successful CAS may legally linearize against a later matching state
of the same live task; a bitmap hint never carries an old generation outcome.
Task-incarnation reuse is forbidden until reference retirement. Generation wrap
of the owner wait record requires complete reference and deadline retirement.
The model includes a fast snapshot spanning a full gate interval, concurrent
registrations, both payload writes and the final decrement.

Public global scheduler suspension preserves the upstream global semantics.
It uses a slow administrative gate, per-owner scheduling checkpoints and
acknowledgments, with interrupts enabled during the acknowledgment wait.
Owners continue IRQ handling and defer dispatch while the gate is active.
Internal queue and timeout code no longer calls public global suspension.
Nested suspension belongs to the acquiring task. Resume publishes pending work
and releases the gate. A global call's duration depends on every owner's
checkpoint bound and is measured separately from ordinary scheduling.

The administration gate has OPEN, ACQUIRING, HELD and RELEASING states, with
holder task/incarnation, nesting count and epoch. A task acquires its FIFO token
with local preemption disabled but API interrupts enabled. It holds no object
or public critical lock. Competing callers wait with WFE and can acknowledge
control interrupts; their own non-preemptive wait does not mask IRQ handling.
The winner publishes ACQUIRING and sends checkpoint requests to all owners.
Each owner finishes its current bounded local/object section, acknowledges at
IRQ exit or scheduler entry, then prevents task dispatch for that epoch.
It continues timer capture and control/reference processing, deferring runnable
activations to its pending state. After all acknowledgments the gate becomes
HELD and the caller returns. Reentry by the holder increments nesting without
another rendezvous; pre-scheduler calls change only boot-local nesting.

Final resume publishes RELEASING, enables owner dispatch, signals pending owners
and releases the administrative token. The calling owner drains its pending
work before returning; `xTaskResumeAll` reports whether it performed the local
yield, matching the upstream return contract. A nested resume performs no
release. Blocking APIs and calls that can switch context remain invalid while
suspended, as upstream documents. The global gate never waits for a task to run:
its acknowledgments execute in IRQ/control context. The bound includes admitted
non-API IRQ interference, object-lock service and owner checkpoint work.

### Control publication has fixed storage and does not drop ISR requests

Each permanent task descriptor contains desired control state, a request
sequence, owner-applied sequence and separate reference/transfer records.
A per-task control ticket serializes callers changing desired priority, affinity,
suspend/delete and abort state. Publishing the composite request is one release
store followed by a control hint/SGI; owner entry acquire-loads it. Requests
for the same desired state coalesce; deletion is absorbing for its incarnation.
Storage cannot fill because publication overwrites desired state rather than
allocating command entries. Transition results and references are committed
before the caller releases protection.

Operations requiring synchronous adoption retain their request sequence and wait
for the owner-applied sequence with interrupts enabled and no object/control
lock held. A later coalesced request acknowledges all earlier requests whose
effects it subsumes. Destructive retirement and transfer records do not coalesce
until their acknowledgments complete. ISR APIs never wait for owner progress:
they commit their specified logical state under bounded object/control protection,
publish a hint and return the required local-yield indication.

Schedulable waits retain permanent slot/incarnation/sequence cookies, rather
than lifetime pins on a task stack that deletion can abandon. Deferred critical
commands move into task-owned cookie banks before interrupts are restored, so
migration cannot strand another CPU's completion records. The 128 fixed banks
reserve 260 KiB separately from hot task and owner metadata.

| Public/internal transition | Mutation and linearization | Completion, return and first live series |
| --- | --- | --- |
| Static create, affinity create | Initialize stack/TCB outside shared exclusion; register descriptor and release-publish initial READY request | Handle valid after publication; creation failure returns NULL on exhausted static registry; B |
| Dynamic create, affinity create | Use upstream stack/TCB allocation, then the same owner admission | Allocation/admission failure clears the output handle and unwinds TLS, port, stack and TCB resources; B |
| Notify, indexed notify, query/clear/take/wait and ISR variants | Notification protocol above; one matched word transaction or gated commit | Preserve all action results and previous values; ISR yield flag concerns local preemption, remote delivery sends its own SGI; B |
| Delay, periodic delay and owner timeout helpers | Install/remove owner deadline and wait generation | Repeated API waits preserve absolute deadlines and upstream wrap rules; B |
| Suspend/resume and ISR resume | Under control and active-object protection commit logical suspend state or resumed generation; release-publish desired state | Remote task-context suspend waits for owner-applied/off-CPU acknowledgment before return, with interrupts enabled and no lock held. Self-suspend returns only after resume. ISR resume returns immediately; repeated resume of a nonsuspended task does not wake it; B |
| Priority set/get and effective-priority change | Serialize desired base priority and active object-waiter ordering; publish effective priority/control repair | Task setter waits for adoption outside locks; getter observes committed desired state. A raise republishes every relevant pending hint and sends SGI; B |
| Mutex inheritance/disinheritance | Under mutex protection publish donation/withdrawal, retain upstream simplified held-mutex rules, then owner repairs ready/event placement | Local holder changes directly; remote donation forces owner control entry. Never wait for acknowledgment while holding mutex metadata; B |
| Affinity set/get | Commit eligible mask, then acknowledged transfer protocol | Getter sees committed eligibility; placement completion has separate acknowledgment. Current task cannot return on an excluded CPU; B |
| Delete and idle cleanup | Close admission and mark logical DEAD; force owner eviction; retire references before cleanup | Self-delete never returns. Remote running deletion is deferred for safe cleanup, as upstream; B |
| Abort delay, where enabled | Under active object protocol close the current blocked generation and publish retry/cancellation outcome | Preserve Boolean success/failure and final condition checks; B under its enabled test configuration |
| Global suspend/resume | Administrative state machine above | Nesting and local-yield result retained; B |
| Task state/system snapshots, handles and runtime statistics | Read stable descriptor/owner snapshots; use global gate for APIs requiring a consistent multi-owner snapshot | Diagnostic slow paths do not inspect transient gated payload or free TCBs; B |
| Queue/semaphore/mutex, event, stream/message-buffer waits and timer commands | Object-protected waiter state plus owner block/wake; kernel helpers no longer use global suspend | Correct in B under compatibility object protection; C replaces that protection with per-object locks |

Priority repair updates current effective priority, active object's waiter order,
owner ready bucket, detached pending work and the urgent-SGI decision. An
external priority setter releases task-control protection before waiting for
owner adoption. Mutex donation is published without acquiring a reverse-ranked
task-control lock while its mutex is held. Owner repair reads committed donation
records, never waits for the donating task, and rechecks pending changes before
dispatch. Timeout disinheritance and recursive mutex nesting receive explicit
linked-kernel cases.

All enabled paths above exist in B. Timers, event groups, stream/message buffers,
priority APIs and abort-delay receive additional builds with their respective
flags enabled; an unbuilt API is not claimed converted. Public blocking/control
calls retain upstream context restrictions. No internal path holds an object
lock while invoking a synchronous owner wait.

## Permanent descriptors and references make stale hints harmless

Task descriptors and incoming bitmap storage never move or disappear. A
descriptor has a task-incarnation number, TCB pointer, admission state and
native atomic reference count. Object waiters retain references. Selecting an
event waiter transfers its reference to the publisher before releasing the
object lock; publishing releases it only after the hint and any required SGI.

A notification or control caller increments the permanent descriptor's reference
count before reading mutable task state, executes `DMB ISH`, then acquire-checks admission and
incarnation. A closed descriptor is not dereferenced. As with upstream, a
caller must hold a valid task handle; calling an already deleted/reused handle
does not acquire validity through this protocol. Internal waiter references
remain valid across concurrent deletion until explicitly retired.

Deletion closes admission with release semantics, executes a store-to-load
barrier, detaches existing object references and waits for admitted publishers
to retire. Waiting occurs with IRQs enabled and no object lock held. An
increment after closure sees closed admission and touches no TCB. After the
count reaches zero, all owners drain their live bits, detached sets and control
references and acknowledge that incarnation. Only then can the TCB/stack be
reused and the descriptor reopen with a new incarnation.
The final reference release executes its own `DMB ISH` before checking closed
admission and notifying the permanent retirement coordinator. Without that
barrier, the closer can see a remaining reference while the releaser sees open
admission, leaving no event to wake reclamation.

Hints are not outcomes: an owner acquire-checks the current authoritative
wait state under its object's protocol, and only that owner changes a current
blocked wait to ready. Already-ready, dead and still-waiting tasks ignore
irrelevant hints. A stale bit may reveal a valid newer completed wait, which
is safe; it cannot invent completion. The model pauses a producer before
publication and an owner after detaching a word, then races deletion/reuse.

## Wake, block and timeout races have one outcome per wait generation

A task has separate placement state, execution state and object wait state.
Placement is OWNED, TRANSFERRING or CLOSED, with owner and full epoch.
Execution is RUNNING, READY, BLOCKING, BLOCKED, SUSPENDED or DEAD. The wait
record has a generation, reason and completion state. Notification arrival uses
the slot word above; other objects serialize outcomes under their object lock.
Migration preserves those outcomes rather than replacing them with MIGRATING.

- **Local wake:** Update the object/notification, establish its wait completion,
  cancel the local timeout, and enqueue directly. If it outranks the current
  task, select on local IRQ/critical exit. Uncontended notification-give uses
  one CAS and no scheduler or object lock; conflict uses its FIFO fallback.
- **Remote wake:** Under the object's protocol, remove its selected waiter,
  mark that generation complete and transfer its reference to the publisher.
  Release object protection before the bounded hint/SGI publication. The owner
  validates current state, cancels its local timeout and enqueues once.
- **Block:** The owner installs PREPARING while holding the object protection,
  checks the condition again, installs the event waiter and local deadline,
  then commits BLOCKED before releasing object protection and selecting.
  Notifications use the gated entry above. A producer can arrive after gate
  release but before physical switch; the owner's final check cancels that
  switch or its pending SGI causes immediate reentry. No list has two writers.
- **Timeout:** The owner claims TIMED_OUT only if the same generation remains
  BLOCKED. It obtains object protection, rechecks the generation/condition and
  detaches the event waiter before reuse. It releases object protection before
  local heap/ready work. One scheduler wait has one completion; API success
  can require another condition check, as specified below.
- **Yield:** Put the current task at its local priority tail and select from
  its own bitmap. No SGI or shared lock is needed.
- **Delay:** Compute an absolute counter deadline, install it in the local
  heap, block locally and program the earliest compare. `vTaskDelayUntil`
  retains its phase and wrap semantics in 1 ms compatibility units.

Timed blocking uses at most seven heap levels at the 128-task limit. Wake
cancellation uses an indexed heap removal; this is bounded `O(log N)` work on
the owner, not an `O(1)` ready-queue operation. It requires measurement within
the local wake budget. A timer-wheel or lazy-cancellation optimization needs a
demonstrated benefit without unbounded stale-entry cleanup.

Task deletion marks DEAD, cancels local links, detaches object waiters and
collects owner/publication acknowledgments before slot reuse or stack cleanup.
It runs as a slow path. Every latched scheduler fault records the first
transition, task slot, owner, generation and incoming command inputs.

Owner entry completes compatibility-object and timeout maintenance before
selection. New work arriving during selection requests a continuation; selection
does not acquire the global task, ISR or administration locks. Resource
destruction runs in task context without these locks or control-drain exclusion.
Idle tasks collect self-deletions; a nonself delete caller can perform cleanup
and observe its permanent completion before reusing static storage.

A task executing a destructor defers its own destructive control requests,
including admission closure and allocator-wait abort. Its cleanup shield allows
blocking, migration and priority donation. Releasing the shield requests owner
service once. Retirement similarly waits for the final reference's event instead
of renewing SGIs while that reference is live.

Cleanup removes the closed handle mapping before freeing recyclable memory,
while reserving the old descriptor slot until destruction completes. A new task
can therefore map the same allocated address to another slot. Final retirement
of the old slot cannot remove that new mapping. Permanent completion generations
survive slot reuse, and static construction rejects a buffer whose old cleanup
is still pending.

Queue wakes are retry permission, not an item/space reservation. A woken task
rechecks the queue under its object lock and can reblock with a new wait
generation while retaining the original absolute API deadline. At timeout it
performs the upstream final condition check before returning failure. Queue
reset changes its generation and wakes/revalidates affected waiters; deletion
waits for waiter/publisher references before freeing object storage.

Event-group completion captures the bits and clear-on-exit decision for the
selected wait generation under the event lock, then publishes each selected
waiter. All selected tasks observe that captured result. Timer command queues
use ordinary queue retry/deadline semantics. Linked tests cover queue condition
theft before waiter dispatch, timeout-boundary success and event-group snapshots.

## Migration is an acknowledged transfer, not a remote ready-list edit

An explicit affinity update changes eligibility and posts a transfer request.
The old owner validates the expected epoch and stops an executing task at a safe
scheduler boundary. Under its active wait object's protocol it snapshots wait
outcome and timeout, detaches local links and release-publishes TRANSFERRING.
It retains forwarding responsibility for that epoch and every admitted producer.

The destination's control record contains task incarnation, old/new epochs,
wait state, accounting, absolute timeout and the off-CPU acknowledgment. The
destination acquire-validates that record, adopts the task once, arms any live
timeout and publishes the new owner/epoch with release semantics. That store is
the placement linearization point. It checks authoritative object/notification
completion before choosing READY versus BLOCKED and acknowledges adoption.

An old-epoch publisher can finish after adoption. Its old-owner hint is
forwarded to the current owner, even if the producer never observed transfer.
Publishers observing the new owner route there directly. The old owner keeps
forwarding until all old-epoch publisher references, live/detached hints and
control records retire and both owners acknowledge. Migration epochs and task
incarnations cannot wrap while any corresponding reference exists.

The private control implementation uses a packed owner/epoch word and two
atomic participant banks. Each bank combines closure and a count in one word.
A lifetime-pinned publisher samples the route, registers with one acquire-release
RMW, executes a DMB and validates the sampled word. It makes one attempt;
rejection broadcasts an epoch-neutral activation hint to all admitted owners.
The lifetime pin remains held through every hint and SGI. Publication commits
the object outcome before route admission, so destination adoption can observe
completion even when an admitted old publisher has not sent its hint yet.

The source closes its old bank before detachment. TRANSFERRING prevents an early
hint from reinserting the task on either queue. Destination adoption restores
owner-local state before release-publishing the next route. Reopening the
alternate bank preserves counts from rejected transient registrations. Its
release orders the intervening route before a stale reader's acquiring RMW;
validation therefore rejects a snapshot spanning two owner transfers.

After the closed bank reaches zero, the source checks its live/detached hints
and acknowledges that epoch. It requests a fresh local checkpoint after draining
any remaining batch. A last closed participant posts an event instead of leaving
the destination polling for reference retirement. New-route publishers do not
hold the old epoch open. The existing lifetime/incarnation protocol separately
protects deletion and static TCB reuse.

Only the destination owns timeouts after adoption; before adoption the old
owner holds timeout decisions in the transfer record. An already-expired
deadline is checked against the object condition on adoption. Transfer preserves
notification values and object completion even if both a timer and producer
were active. No CPU executes the task before old-owner off-CPU acknowledgment.
No object lock or masked interval spans acknowledgment waits. Cancellation or
destination rejection returns ownership to the old owner through the same
acknowledged protocol; it never silently drops a request.

Idle pull examines at most the other seven owners, with an event-triggered
request and one bounded response per victim. It considers only tasks marked
migratable. It prefers a misfit task for an idle higher-capacity CPU and moves
light work down only when that improves the declared background objective.
All-idle systems exchange no balancing timer events or periodic IPIs. The idle
mask is a published topology-sized bitmap. A CPU becoming idle requests a pull;
a victim rejecting it retains that idle CPU in its interested-idle set. Task
creation, wake, effective-priority change, slice/accounting completion or a new
demand hint can advertise a migratable candidate to interested idle CPUs.
One coalesced request per victim/idle pair remains outstanding. A rejection
waits for a real candidate/eligibility event instead of a polling retry.

A continuously running task whose demand changes is reconsidered at the next
observed IRQ/accounting boundary, explicit hint update or idle pull. With no
such event, there is no promised utilization-reclassification deadline. A heavy
declared hint and initial capacity placement cover a tickless solo compute task.
No hidden periodic utilization sampler is introduced.

Priority guarantees are local to the assigned owner. An unpinned task receives
an owner at creation; its allowed mask is eligibility for explicit or permitted
idle migration, not a global highest-priority-everywhere guarantee. Existing
API signatures remain compatible; this deliberate partitioned placement contract
is exposed in the application kit. Admission rejects a partition assignment
whose promised workload cannot fit, rather than assuming spare remote capacity.

Migration cost includes private L1/L2 refills, shared-L3 hits/misses and transfer
coordination. The topology exposes actual cache sizes; no constant cache-cost
guess becomes an admission bound. Measure warm and displaced working sets,
time the owner handoff, and apply hysteresis exceeding the measured refill cost.

An interrupt-bound task remains pinned while a device has an active job.
Changing its CPU first masks the device source, drains completions, confirms
the interrupt controller's documented write completion, reroutes to an eligible
CPU, publishes the new consumer and unmasks. Device-specific owners perform
this slow path through existing IRQ APIs. Scheduler placement cannot reroute a
live device autonomously.

### Idle pull moves a waiting task to an idle eligible CPU

The fork implements the pull of the previous section for tasks that wait behind
a running task of equal or higher priority. Each owner publishes the set of
CPUs that its queued tasks may run on (`pull_targets`, the union of their
affinity masks, computed from at most eight entries per priority) and a global
bitmap marks the CPUs that run only their idle task. At the end of a service
entry an owner that keeps running its current task takes every target that is
idle or has asked (`pull_requests`), picks the highest-priority waiting task
that may run there and publishes a move control for it. The ordinary
acknowledged transfer then detaches the queued task here and the target adopts
it; no CPU edits a foreign queue. A CPU that selects its idle task sets its idle
bit and asks, with one coalesced bit and one SGI, each owner whose targets
include it. A task that would preempt the running one is not pulled, and neither
is one while its core's current task is blocking, suspending or deleted.
Placement moves with the task: its owner becomes the target, and an affinity
change moves it back.

Prediction, from the previous stranding measurement (16 of 16 forced rounds
left a ready receiver waiting 1 ms on a busy home while the eligible peer idled):
with idle pull the receiver completes on the peer within the same millisecond in
16 of 16 rounds and no round strands, and the pinned-latency and loaded-wake
distributions of the eight-core CPU control stay within the noise of the
previous control, because a CPU that is not idle never pulls.

### The recorded recognition decides whether the fork is kept

The CPU recognition image (the external mimus application, eight workers, the
recorded 3.59 s clip) is built twice from one tree, with the fork off and on.
Each variant runs twice on port 3-3 at the approved CPU points. Both must
reproduce the recorded result exactly: `feat_crc=ac657a3b`, `lp_crc=a7ade80e`
and the text `FORMATTED There was silence.`. The application pins one worker
per core and has no wake-latency instrumentation, so the comparison measures
compute time per recording and the real-time factor (compute time over audio
time, 57,440 samples at 16 kHz).

Prediction: the pinned workers rarely wake each other, so the fork changes the
real-time factor by less than 1% in either direction, and no variant regresses
by more than 1%. The fork is kept and enabled by default only if the output is
exact, the real-time factor improves by at least 2%, and nothing regresses by
more than 1%; otherwise it stays off. No audio or recognition task wake latency
is compared, because the image has no instrumentation for it that costs nothing
when disabled.

Result, port 3-3, two runs per variant, development mode: both variants returned
the exact result (`feat_crc=ac657a3b`, `lp_crc=a7ade80e`, `FORMATTED There was
silence.`). Compute time per recording is 348.7 ms (real-time factor 0.0971) with
the fork off and 366.4 ms (0.1021) with the fork on, repeatable to 0.04% within
each variant. The fork is 5.1% slower, which is a regression of more than 1%
against a predicted change of less than 1%. The prediction fails and the rule
decides: the fork stays off by default. The recognition application is pinned
and compute-bound, so the extra cost per kernel entry (the stalled masked spans
measured above) is paid without a placement gain. The fork remains an option
for workloads with unpinned tasks; the comparison found no workload that needs it.

### Where the fork loses time on the recorded recognition

The recognition engine (mimus `bcaf39ff5`, eight workers) splits each linear call
over seven pinned worker tasks. The owner sends one job through a one-slot queue
to each worker, runs its own share, then receives one completion per worker.
Every worker and the owner have an affinity of one CPU, so idle pull never moves
them, and a migration is a failure (the worker asserts its core). The same image,
built with `SUNSTONE_MIMUS_PARALLEL_TRACE`, accounts each parallel call in
counter ticks: the owner's time in the seven sends (dispatch kernel time), the
owner's own share (compute), the wait for the slowest worker beyond the owner's
share (last-finisher wait), the time from the slowest worker's finish to the
owner's return (completion path), and per worker the time from the end of its
send to the start of its job (wake to run), its compute and its completion send.

Prediction: the stock kernel's work per call is one lock-protected wake per send
and per completion. The fork replaces each with the routed path: a publication
into the owner's incoming words, an SGI, and the owner's service entry before
the worker runs; and the owner's receives go through the same path. Each routed
hop runs a masked kernel entry that costs 12 to 30 µs when the memory system is
saturated, which is the measured cost of the masked spans above. The fork
therefore loses time in the dispatch kernel time (seven sends) and in wake to run
and the completion path, and not in compute; its worker compute equals the stock
kernel's within 1%. The difference in those three parts, summed over the
recognition, accounts for the 17.7 ms. If the stock kernel's total of
synchronization and scheduling is under 2% of recognition time, the fork cannot
beat it by 2% on this workload.

Result (port 3-3, one run per kernel, engine `bcaf39ff5` from the mimus branch
`scheduler-parity-1101`, accounting compiled in, output exact): compute time per
recording is 109.6 ms with the global kernel and 110.1 ms with the fork, so the
fork is 0.5% slower on the current engine, not the 5% of the older engine. The
prediction is refuted in its mechanism. In microseconds over the 470 parallel
calls, global kernel against fork:

| Part | Global kernel | Fork |
| --- | --- | --- |
| Parallel calls, total | 80,564 | 79,672 |
| Dispatch kernel time (seven sends) | 5,381 | 5,971 |
| Owner's own share | 46,358 | 47,199 |
| Wait for the slowest worker | 21,961 | 21,264 |
| Completion path | 6,862 | 5,237 |
| Wake to run, sum over 2,658 worker calls | 7,375 | 9,625 |
| Wake to run, slowest worker per call, summed | 1,791 | 2,092 |
| Worker compute, sum | 223,596 | 224,325 |
| Outside the parallel calls (total less parallel calls) | 29,021 | 30,439 |

The fork's routed wake and completion path costs no more than the stock kernel's:
dispatch, completion and the slowest worker's wake to run sum to 14.0 ms (12.8% of the
recognition) with the global kernel and 13.3 ms with the fork. Parallel calls are
0.9 ms shorter with the fork. The difference comes from the serial part
outside the parallel calls, which is 1.4 ms (4.9%) longer with the fork, and from
the owner's own share, 0.8 ms longer, both of which run on the owner's core with
the other cores idle. No worker moved: placement errors and core changes are zero,
and idle pull cannot move a task whose affinity is one CPU. The stock kernel's
synchronization and scheduling time is 12.8% of the recognition, above the 2%
bound, so the fork can win only by removing part of it or by removing the owner's
slowdown while seven cores idle.

Interrupt accounting (port 3-3, same input and engine; interrupts taken during
the recognition, by core; every one is SGI 0 except the core 0 timer): the
owner core (6) takes 293 to 296 interrupts with the global kernel and 358 to 380
with the fork, 65 to 85 more. The other pinned cores take the same number with both
kernels, except core 3 (341 against 546), where the unpinned OS services run.
The SGI senders of the fork, by call site, show where the owner's extra interrupts
come from: the workers' completion wakes through `ssKernelPublishReference` (356 of
the 359, the same wake as the global kernel's) and 52 self-directed continuations
at the end of `ssKernelService` plus 11 from the yield path. Those 63 account for
the extra count. A pass that re-runs the service while work remains, before it
falls back to the continuation SGI, did not reduce the owner's interrupts on the
board (368 and 380) nor the time (110.24 and 110.31 ms against 109.94 and
109.73 ms with the global kernel, two runs each), so the continuations are not
the owner's loss, or they are not avoidable by repeating the pass. The owner-core
loss of 2.2 ms is therefore unexplained: the next candidates are a slower
`ssKernelPublishReference` wake that lands during the owner's compute (the same
interrupt count, a longer service) and the cache effect of the fork's larger
per-CPU structures on the owner. The pass is not kept (`SS_SERVICE_PASSES` is
1). Measured with the repeat pair, the fork is 0.4% slower than the global kernel on
the current engine and does not reach the 2% faster that enabling it would need.

### Hardware counters on the owner core separate a slower wake from cache effects

The 2.2 ms the owner core (6) loses on the recorded recognition has two candidate
causes. Slower wake: the fork's publish-and-wake service (`ssKernelPublishReference`
for the owner's seven sends, `ssKernelService` at the end of every interrupt) takes
more cycles than the stock kernel's wake. Cache effects: the fork's larger per-CPU
structures, the compat-lock lines and the extra interrupts displace the owner's
working set, so its own compute and the serial code between calls run slower with
more refills. The same image pair runs with the PMU enabled on every core at EL1
with EL0 read access (cycles, L1D refills, L2D refills, backend stall cycles; read
only). The owner reads the counters at the boundaries of four disjoint windows on
core 6 over the recognition: the sends of a parallel call, the owner's own share,
the tail (wait for the slowest worker and the completion path), and the serial code
outside the parallel calls (total less the three). The fork also accumulates the
counters inside `ssKernelPublishReference` and `ssKernelService` on core 6.

Prediction. If the wake service is slower: the fork's excess cycles are in the
send and tail windows and in the two kernel regions (at least 1.5 ms of the
2.2 ms), the owner and serial windows match the stock kernel's within 0.3 ms, and
refills per window match. If cache effects: the excess is in the owner and serial
windows (at least 1.5 ms), their L1D and L2D refills are higher in the fork by
about 10% or more, and the send and tail windows differ by at most 0.5 ms. Interrupts that
land in the owner window count toward that window, so the kernel regions' cycles
are subtracted from the owner window before it is compared. A split with no
region over 1.5 ms points to neither.

Result (port 3-3, one run per kernel, exact output; cycles of core 6, fork minus
global kernel, 2.002 GHz):

| Window | Global kernel (M cycles) | Fork (M cycles) | Difference | L1D refills | L2D refills |
| --- | --- | --- | --- | --- | --- |
| Owner's own share | 92.86 | 94.61 | +0.87 ms | +1.5% | +2.3% |
| Sends | 10.57 | 12.19 | +0.81 ms | -29% | -32% |
| Tail (slowest worker, completion path) | 31.50 | 21.08 | -5.20 ms | -47% | -55% |
| Everything else between bind and report | 241.43 | 249.38 | +3.97 ms | -0.1% | -0.6% |

The first three windows are the parallel calls and agree with the time accounting
(owner share +0.90 ms, sends +0.72 ms). The tail window counts cycles only while the
core runs: it waits in WFI, which stops the cycle counter, so its time difference is the
-2.6 ms of the time accounting. The last row is not the serial code of the timed
recognition: the window runs from the bind to the report and includes about 90 ms of
untimed work (the warm and verified passes), so it cannot hold the 1.4 ms by which the
timed serial code is longer.

Inside the fork, `ssKernelPublishReference` took 2.05 M cycles in 2,658 entries (771
cycles each) and `ssKernelService` 0.81 M cycles in 1,596 entries (508 each), 1.4 ms
together.

Neither candidate meets its prediction. The wake service is not slower: sends cost 0.8
ms more, the tail 2.6 ms less (time), a net 1.8 ms faster, and no wake window exceeds the
stock kernel's by 1.5 ms. Cache effects do not reach 1.5 ms: the owner window, where
they would show, is 0.87 ms longer with 1.5% and 2.3% more refills, not 10%, and its backend
stalls are 34% of its cycles in both kernels. The 1.4 ms by which the timed serial code
is longer is not covered by these windows. No single cause accounts for 1.5 ms of the
loss, so the stream stops with the fork off by default. The fork's faster completion path
(-2.6 ms) and its slower sends, owner share and serial code (+0.7, +0.9 and +1.4 ms)
cancel to the measured +0.4%. A counter window around the timed serial code, which the
engine would have to place inside its timed region, is the next step if the work resumes.

## Robot control sets the requirement: bounded worst-case response

SunstoneOS on this board will run quadruped and biped robots with several cameras
and lidar. The main scheduler requirement is real-time response; throughput is
second. The proposed targets, which the product owner can revise: worst-case wake
latency of the highest-priority task at most 20 µs; jitter of a 1 kHz control period
at most 50 µs; no deadline miss under full load. Measurements report the maximum
and the 99.99th percentile, never means.

### What bounds a task's response

A task woken by an interrupt runs after four delays: the interrupt waits while the
core has API-level interrupts masked (the longest masked span on that core), the
interrupt entry and the wake path run, the scheduler selects the task, and the
context switch completes. The first term is the largest and cannot be below the
longest masked span of any kernel path that runs on that core. The measured
spans on port 3-3, in microseconds, with the memory system saturated by eight
cores streaming (the CPU control and the recognition):

| Kernel | Longest masked span | Loaded wake, maximum | Task-lock wait, maximum |
| --- | --- | --- | --- |
| Global kernel, ticket locks off | 264 | 160 | 150 |
| Global kernel, ticket locks | 266 | 126 | 98 |
| Partitioned fork | 21.8 to 32.1 in the measured phases, 46.6 at the end of the run | 127 | 12 |

With the memory system idle the same spans are 2.5 to 5.5 µs. Backend stalls on
dependent DRAM refills are 94 to 98% of the cycles of the longest spans (about 35
refills at 0.5 µs each), so the span is set by memory latency under load, not by
instructions.

### Prediction

Neither kernel meets 20 µs while the recognition or any other bandwidth-heavy task
runs: the dominant source of worst-case latency is the interrupt-masked span, held
open by DRAM latency, and the fork's best case is its 22 to 32 µs measured spans plus
the 17 to 20 µs service entry, so about 50 to 80 µs. The fork's tail is two to three
times shorter than the global kernel's, because it has no global lock to queue behind
and its spans do not add; the global kernel's worst case is the sum of the holders
ahead on the ticket lock, up to about 150 µs. Without bandwidth-heavy neighbours both
meet 20 µs, as their idle spans are 2.5 to 5.5 µs.

Two levers follow, in order. First, shorten and split the spans: move work out of
the masked section when the section waits on refills (publication of wakes, the
control drain of the caller, slice refresh), and keep object critical sections to
the list edit. Second, take the hardest path out from under the mask. The kernel masks
by raising the priority mask to the API level, so an interrupt of higher priority than
that level is never delayed by a kernel span. A control loop that runs in such an
interrupt, with no kernel call, has a response independent of the scheduler; the
tasks around it use the kernel. This is the usual structure of a robot controller and
the way to reach 20 µs next to a streaming recognition.

Idle pull applies to best-effort tasks only: it must never move a task whose affinity
is one CPU, and the real-time task set stays pinned. The test workload is a pinned
1 kHz control task of the highest priority, a sensor-fusion task woken by
interrupts at a few hundred hertz, camera-like and lidar-like interrupt producers,
and the recognition and an unpinned best-effort load below them, measured on port
3-1 over at least 10^6 control periods per kernel. Tickless deadlines matter for a
periodic wake that does not come from the tick: a control task driven by the
tick has a 1 ms quantum and a precise-deadline wake does not.

### Revised targets and verdict rule

The control loop runs faster than 1 kHz and the robot runs a model beside it. The
targets are: worst-case wake latency of the highest-priority task at most 10 µs;
jitter of the control period at most 5% of the period (12.5 µs at 4 kHz, 5 µs at
10 kHz); no deadline miss under the full load. The primary rate is 4 kHz and the stretch
rate 10 kHz. The control task is swept over 1, 2, 4, 8 and 10 kHz, and the sweep
curve (jitter 99.99th percentile and maximum against rate) is recorded for both
kernels, with the tick and without it. The model workload runs in the same test:
recognition time per input on the CPU workers, and NPU jobs per second where the
image carries an NPU stream, each also alone. A kernel is chosen if it meets the
real-time targets at its highest rate and keeps the model within 2% of its run alone;
among kernels that do, the higher control rate wins, then the higher model throughput.
Any masked-span change that slows the model is weighed against the 2% bound. The
model's synchronization cost on the global kernel is 14.0 ms of 109.6 ms (dispatch
5.4, completion 6.9, slowest wake 1.8; the fork 13.3 ms), all of it in the kernel
paths a real-time change could lengthen.

With a 5 µs jitter bound at 10 kHz and a 10 µs wake bound, any masked or
preemption-disabled section over 5 µs is a defect. The sweep of such sections found in
recorded runs (the longest three per core and phase, so partial; length, core and
the call sites that open and close the span):

| Kernel | Length (µs) | Site |
| --- | --- | --- |
| Global kernel, ticket locks | 266.3 | `prvAddNewTaskToReadyList` to `vPortRestoreTaskContext` (start-up, core 0) |
| | 101.3, 95.3 | `ulTaskGenericNotifyTake` to the yield; `vTaskSuspendAll` to `ulTaskGenericNotifyTake` |
| | 31.0, 26.1, 19.3 | `os_thermal_task`; `ulTaskGenericNotifyTake`; `xQueueGetMutexHolder` |
| Global kernel, test-and-set locks | 264.8, 206.2, 191.4, 142.3, 127.7, 115.3, 107.8 | start-up; `vTaskSuspendAll`; notify take; `xTaskGenericNotify`; `vTaskDelay`; `os_thermal_task` |
| Partitioned fork | 46.6, 41.2 | `ssKernelCommand` (end-of-run suspension; command publication with the owner's drain) |
| | 31.8, 34.2 | `vTaskEnterCritical` to `ssKernelObjectExit` (queue operation of an OS service) |
| | 22.6, 21.8 | `ssKernelService` (service entry at interrupt exit) |
| | 21.6, 20.8 | `ssKernelNotify` |
| | 15.8, 18.9 | `ssKernelObjectEnter` to `ssKernelObjectExit` |
| | 5.3 to 5.9 | `ssKernelNotifyWait` |

Every entry exceeds the 5 µs bound, on both kernels. A complete sweep needs a recorder
that keeps every distinct (open, close) pair above 5 µs rather than the longest three,
run on the image that carries the OS services and drivers; that recorder is the first
tool to build. The fixes go in order of length. The fork's spans fall into three kinds:
command publication that drains its own queue under the mask (`ssKernelCommand`),
object critical sections that hold the list edit and the wake publication together
(`vTaskEnterCritical` to `ssKernelObjectExit`, `ssKernelObjectEnter`), and the service
entry. Each reduces to moving the work that waits on refills (the drain, the wake
publication, the slice refresh) out of the masked section, or splitting it into
bounded steps that unmask between them.

### The span sweep lists every masked pair before any span is fixed

Earlier rounds measured the three longest spans per kernel. Fixing spans one at a time from that list risks leaving a longer span unmeasured, so the sweep records all of them first.

The recorder keeps one entry per distinct pair of call sites, the call that raised the interrupt priority mask and the call that restored it, on each core. An entry holds the span count, the longest span and a 22-bin histogram with edges from 1 to 500 microseconds. The 99.99th percentile is the upper edge of the bin that holds that rank, and the longest span when fewer than 10^4 spans were seen. Writes are per core, so the recorder adds no shared line to the measured path. The table has 64 entries per core and counts overflow separately, so a full table is visible in the report. The image build shrinks the recorder's full histograms to 17 bins because the sweep image has no free space for them; the sweep table replaces them.

The image is the mimus OS image with the OS services and drivers linked, run under the mimus load (eight workers, three recorded inputs), once with the stock kernel and once with the partitioned fork. Pairs whose longest span exceeds 5 microseconds are listed with their symbols from the image.

Prediction. Pairs that touch the scheduler lists or a ticket lock are held open by dependent DRAM refills, so their length follows the number of distinct cache lines touched rather than instructions. On the stock kernel the longest pairs are the suspend-all spans and the notify-take spans, which walk shared lists under the task lock, at 95 to 206 microseconds as measured before. On the fork they are the kernel command, the critical section that ends in an object exit, and the service pass, at 22 to 47 microseconds. The sweep may add pairs that the earlier three-site capture could not see; any such pair above 5 microseconds is a defect under the 10 microsecond wake target and joins the fix list in length order.

### Lock wait or held work: each span is split before it is fixed

A span that does almost no work, such as the scheduler-state query on the stock kernel (31 us) or the object enter and exit pair on the fork (33 us), is probably held open by waiting for a contended kernel lock with the interrupt mask already raised, not by memory work under the lock. The two causes have different fixes. If the wait dominates, the lock is taken with the mask lowered: spin unmasked, raise the mask, try the lock, and lower the mask and retry on failure. If the held work dominates, the memory touched under the lock is reduced or moved outside the masked region.

The recorder therefore adds the time each core spent acquiring a kernel lock (the task lock and the interrupt lock) between the mask opening and the mask closing to the pair's entry. Per pair it keeps the lock wait inside the longest span, the longest lock wait in any span, and, for spans above 5 us, their count, total length and total lock wait, so the wait share of the long spans is a ratio of two sums.

Prediction. For the pairs listed above 25 us on both kernels, lock wait is at least 70% of the long-span time, because 8 cores take the same task lock and a holder's own span is itself 10 to 30 us. Pairs on core 6 that run the mimus dispatch (`board_service_progress` to `dispatch` and the yield hook) are the exception: they hold the lock rather than wait for it, and their wait share is under 30%. If both hold, the fix for the wait-dominated pairs is the unmasked spin on both kernels, and the held pairs go to the memory fixes. If the wait share is under 30% everywhere, the earlier DRAM-latency account stands and the fix is the memory work.

Dependency. How long a span stays open when several cores read memory depends on how DRAM bandwidth falls as more cores read, which the DRAM-scaling study in [DRAM bandwidth](dram-bandwidth.md) is investigating. This design does not change that study's files; its result sets how much of a held span can be removed by reducing lines touched, and a change in the interconnect settings would change the held-span lengths measured here.

### Waiting for the task lock with the mask open

The sweep shows 82 to 96% of every long span is queueing for the task lock with the interrupt mask already raised. The kernel entry points `uxPortSetInterruptMask`, `vTaskEnterCritical` and `vTaskSuspendAll` therefore wait first and mask second. `runtime/lock_prewait.c` wraps them at link time (`SUNSTONE_LOCK_PREWAIT`); no protected patch changes. The wrapper reads the task lock's ticket word and, while more than one core holds or waits for the lock, sleeps in `WFE` with the mask open, then calls the real function, which masks and takes a ticket as before.

Rules. Most kernel callers on the OS image are EL0 tasks, which run the FreeRTOS API code unprivileged and reach the mask through a supervisor call, so the first board attempt waited for under 1% of the entries. An EL0 task cannot read the mask, so it waits when its core's critical nesting count is zero and it does not hold the task lock; an EL1 task waits when it is an EL1 task with the priority mask open and `DAIF.I` clear. The mask reads back in the Non-secure view (`include/a733_gic_priority.h`: the 0xff written reads back as 0xf0), so open means above the API mask, the test the port uses, not equal to 0xff. An interrupt handler, a nested critical section, a `FromISR` path and an EL0 task inside a critical section go straight to the real function and spin at the mask they already hold; the wrapper never lowers a mask. A core that already holds the task lock, as a task does while it has suspended the scheduler, does not wait, because the queue behind its own hold can shrink only when it releases. The wait takes no ticket and no lock, so an interrupt that arrives during it runs normally and cannot deadlock against a ticket the interrupted code holds. The ticket lock stays first-come first-served once the mask is raised. The wait is limited to 100 microseconds; when the limit ends it, the caller masks and queues regardless, so the total wait is the limit plus a ticket queue of at most seven holders, and no core can lose repeatedly. A burst of cores can all see the lock nearly free and queue together, so the masked wait is not removed, only reduced.

Prediction. On the host model with eight threads, the median masked wait drops from 11.9 to 4.5 microseconds and the 99th percentile from 14.8 to 11.6 (`checks/scheduler-lock-prewait.py`, which fails on starvation). On the board, the longest masked span of each pair in the sweep falls from 30 to 34 microseconds to 10 to 15 microseconds, and its lock wait inside the span falls below 12 microseconds. The unmasked wait is at most 100 microseconds per kernel entry. The mimus recognition time stays within 2% of the sweep image without the wait, because the same lock serves the same calls in the same order. Spans still above 5 microseconds after this change are held work plus a short queue and are the input to the next fix, which shortens the hold or shares the lock less. If the longest span stays above 20 microseconds, bursts queue together and the depth threshold or a staggered wait is the next change.

### Control interrupts above the API mask do not wait for kernel spans

Every kernel span is masked at the API priority, so an interrupt above that priority is not delayed by any of them. The control class is the stored priority `A733_GIC_CONTROL_STORED` of `include/a733_gic_priority.h`, numerically below the API floor; `checks/scheduler-control-class.py` checks the view rules (which Non-secure writes store which value, which values the raised mask lets through, that a read then write keeps the value) and that the handler source calls no kernel function and holds no priority literal.

`runtime/control_irq.c` (`SUNSTONE_CONTROL_IRQ`) drives it from the EL1 virtual timer (PPI 27) of cores 1 to 5, one rate each, at 1, 2, 4, 8 and 10 kHz at once. The handler reads the counter, re-arms the compare value from the previous deadline so the period does not drift, records latency (entry minus the programmed deadline) and period jitter (the interval between entries minus the periods that passed) in per-core histograms with one counter tick (41.7 ns) per bin, and puts a sample into a single-producer single-consumer ring (`runtime/control_ring.h`). A task drains the rings and checks the sequence numbers; a full ring drops the new sample and counts it. A core arms its own timer from its first interrupt after the request, because only that core can configure its redistributor and timer. Core 0 keeps the task-deadline timer.

The comparison row is the task path on the same cores, in the same run: the EL1 physical timer (PPI 30) at the API floor (`A733_GIC_TASK_PATH_STORED`) fires at the same rate, half a period after the control handler, and its handler wakes a pinned priority 5 task with `vTaskNotifyGiveFromISR`. The task's wake latency is its start minus the deadline the interrupt served, and its jitter is the interval between task starts minus the periods that passed. This is the path an ordinary control loop takes through the kernel, including every masked span of the task lock.

Task path prediction. Its wake latency follows the kernel spans: the interrupt itself waits for any API-masked section of up to 30 us, and the notify and the switch take the task lock behind a convoy of up to seven holders, so the maximum is 30 to 60 us on both kernels, and the 99.99th percentile in the same range at the few thousand interrupts of one run. The 10 us wake target and the 5% jitter bound at 4 kHz (12.5 us) are missed; the control path carries them.

Prediction. Under the full mimus load, with the longest kernel spans at 30 us, the handler's latency does not follow them. What remains is the interrupt entry path (context save, acknowledge), the longest interval with `DAIF.I` set (the port's short mask windows and any lower-priority handler that runs with interrupts masked, such as the tick handler on core 0, which this class avoids by using cores 1 to 5), and a stalled barrier under DRAM contention. Per rate and core: maximum latency under 10 us, 99.99th percentile under 5 us (equal to the maximum when a run has fewer than 10^4 interrupts, as these short runs do), jitter no larger than the latency range, no dropped sample and no gap, no missed period. Against the targets this meets the 5% jitter bound at 1 and 2 kHz (50 and 25 us) and 4 kHz (12.5 us); at 8 and 10 kHz (6.25 and 5 us) the maximum is the open question. The recognition runs for about 0.4 s, so one run holds 400 to 4000 interrupts per core; the 10^6-period runs belong to the robot workload.

### Why the instrumented fork beat stock when the plain fork did not

The sweep images ran the recognition in 136.4 ms on the stock kernel and 127.2 ms on the fork, a fork 6.8% faster; the plain images of the speed stream measured the fork 0.4% slower (110.3 against 109.8 ms). Two explanations: the span recorder adds work inside every masked span, and a stock kernel with a longer convoy pays more for it; or the current tree differs from the earlier images. A plain pair of the current tree (no recorder, no counters), and a plain pair with the unmasked wait, separate them.

Prediction. The plain pair of the current tree takes about 110 ms on both kernels, the fork within 1% of stock, because the recorder alone explains the 26 ms that stock lost: its timestamp reads and table update lengthen every span by roughly 0.3 to 1 us and the stock kernel has about twice the long spans of the fork. The unmasked wait changes the plain times by under 1%. If the plain pair already shows the fork faster than stock by several percent, the current tree changed and the earlier verdict needs its own rerun.

### The long runs behind the kernel verdict

The first robot table rests on 200 to 2000 periods per core, so its maximum is not a worst case, and the fork's lead on the task path is the very difference the verdict would use. This round repeats the recognition for 25 seconds inside one boot (`SUNSTONE_MIMUS_SOAK_SECONDS`; the board session is 60 seconds), once quiet and repeatedly with the engine kept, then once with the normal output, so the control interrupts run under the same load for the whole soak. Five boots per kernel pool at least 10^5 periods at 1 kHz (125 000), 2 kHz and 4 kHz for the task path alone on cores 1 to 3, and 1.25 million at 10 kHz for the control path alone on core 4. The program prints each histogram's nonzero bins, so the p99.99 comes from the pooled counts, not from a per-run value. The image is the plain mimus image plus the interrupts; a plain soak image without them and a soak image with both paths on five cores give the model cost.

Verdict rule. The control path must meet the targets on both kernels. Then the kernel whose task path meets the targets at the higher rate wins; then model time.

Prediction. The task path's tail grows with the sample count: its pooled maximum is 40 to 70 us on both kernels, its p99.99 20 to 40 us, so neither kernel meets the 10 us wake target at 4 kHz and, with a convoy of seven 3.5 us holds behind it, neither meets it at 1 kHz at the 99.99th percentile. The fork's task path is lower than stock at 1 and 2 kHz by 20 to 40% at the p99.99 and equal at 4 kHz, because fewer cores queue behind one lock when the rate is low. The control path at 10 kHz keeps its maximum under 6 us on both kernels over 1.25 million periods. Model cost per handler is proportional to its rate: about 0.09% of recognition time per kHz of interrupts on stock and 0.06% on the fork, about 0.4 to 0.9 us of core time per interrupt, so a robot with a 10 kHz control loop and a 1 kHz fusion interrupt on a core pays about 1% of that core's model throughput.

### The owner exit work must not run after an interrupt above the API mask

One fork run of the long set stopped with `platform_panic` (queue entry `1793608348858278119-8f8e4282`, task path alone on cores 1 to 3 and the control path alone on core 4). What the record holds: a `configASSERT` at `FreeRTOS_TCP_WIN.c:1846` in `ulTCPWindowTxGet` (the line number is the stop detail 0x736; the panic call site is the only one in the image with that line), taken on core 4 with the program counter at `platform_panic+0x34`, the loop after the panic call; ESR 0x404 and FAR 0x43a956b2 are stale, since the stop was the software panic and not an exception. The file name, the caller and the running task were not recorded, because the panic came from a context that cannot be handed to the failure return, and that path skipped the failure note. No transcript exists, since the image never returned.

That image runs no TCP application. The only route to `ulTCPWindowTxGet` is the IP task, which exists only after the radio owner starts the network (`FreeRTOS_IPInit_Multi` from `aic8800_network_activate`). So the assertion was reached through corrupted memory or a wild jump, not through use.

Cause. The patched interrupt exit (`portASM.S`, `freertos-partitioned-port-asm.patch`) calls `ssKernelOwnerIRQExit` after the outermost interrupt, which drains owner work and may request a switch. The kernel's maintenance, object and control sections are masked at the API priority, so an interrupt at or below that priority never lands inside them. The control interrupts of this stream are above the API priority and do. Their exit then ran the owner work inside the interrupted maintenance, which asserts that it is not re-entered (`ssKernelControlDrain`: `isr_depth` and `control_draining`). The stock kernel does no such work at interrupt exit and is not affected. The heap and control-state changes came after the panic image (it had the 4 MiB heap and the first control state), and every index into the control state is bounded by construction.

Reproduction. `scheduler-partitioned-storm-qemu` runs the 8-core concurrent workload with a timer interrupt above the API mask firing every 33 us on every core (`SS_STORM`; its handler calls nothing). Before the fix it failed 4 of 4 runs with `configASSERT` at `freertos.c:1027` or `1028`. Without the interrupt the workload passes.

Fix. `ssKernelOwnerIRQExit` returns without work when `ICC_PMR_EL1` still holds the API mask, which only an interrupt above it can observe, because the exit path does not change the register. The work is left for the next ordinary exit or the tick. The tick calls the unguarded body directly. After the fix the storm case passes 18 of 18 and the kernel, concurrent, soak, runtime and heap cases pass, and 12 runs of the concurrent case pass with `configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES`. A measurement image can add that option (`SUNSTONE_LIST_CHECK`), which with `configCHECK_FOR_STACK_OVERFLOW 2` (a pattern check of every stack at each switch, already on) arms the kernel's own corruption detectors.

Lock audit. The partitioned patches change no list operation: `freertos-partitioned-api.patch`, `-tasks.patch`, `-objects.patch` and `-layout.patch` add or remove none of `vListInsert*`, `uxListRemove` or `vListInitialise`. The fork's own list operations are the four sites in `kernel/scheduler/freertos.c` on a task's event list item (timeout removal, event block, event wake, object repair); the timeout, wake and repair sites run between `ssKernelObjectEnterFromISR` and `ssKernelObjectExitFromISR`, which take the same task and ISR locks as before, and the block site is reached from `vTaskPlaceOnEventList*` inside the caller's queue critical section, as in the stock kernel. The lock split changed no lock that protects a list.

Prediction for the board. The fixed fork image (`ssKernelOwnerIRQExit` guard, panic note, plain) runs five 25 s soaks without a stop, and pools to at least 100 000 periods at 1, 2 and 4 kHz for the task path and 1.25 million at 10 kHz for the control path. The same image with the list integrity bytes runs three more without a stop. The 3 MiB heap does not drop samples: pooled counts are the periods of the soak, and the heap peak in the output stays near 107 KB.

### The mask guard leaves the owner work running after ordinary interrupts

The guard in `ssKernelOwnerIRQExit` is right only if the interrupt path leaves `ICC_PMR_EL1` as the interrupted code had it. In the generated `portASM.S` the register is written in six places, none on the interrupt path: the first-task start (`MSR ICC_PMR_EL1, X1` after choosing 255 or the API mask) and the supervisor-call handlers that mask and unmask (`portSVC_DISABLE_INTERRUPTS`, `portSVC_ENABLE_INTERRUPTS`, `portSVC_MASK_ALL_INTERRUPTS`, `portSVC_UNMASK_ALL_INTERRUPTS`, `portSVC_UNMASK_INTERRUPTS`). The interrupt path is: increment the nesting count, `MRS X0, ICC_IAR1_EL1`, mark the task register as EL1, `BL vApplicationIRQHandler`, `MSR DAIFSET, #2`, `DSB SY`, `MSR ICC_EOIR1_EL1, X0`, restore the nesting count, and, if the nesting is zero, `BL ssKernelOwnerIRQExit`. The only way the register differs at the exit is a handler that raises it through the FromISR mask functions and does not restore it, which the FromISR pair forbids.

Counters of owner-work drains by CPU (`SUNSTONE_OWNER_COUNTERS`): from interrupt exit, skipped by the guard, from the tick, and service passes run. The QEMU concurrent case, with no control interrupt, drains 232 to 444 times per CPU on CPUs 1 to 7 from interrupt exit, 5569 times on CPU 0 with 5302 more from the tick, and the guard skips none (the check asserts it). The storm case, with a control interrupt every 33 us on every CPU, still drains 3187 to 5797 times per CPU from interrupt exit, skips 421 to 3216 exits of the control interrupts, and drains 198 times from the tick on CPU 0 (the tick is a small share of its interrupts there). The board run on the fixed fork image prints the same counters per CPU: drains from interrupt exit on cores 1 to 3 and 5 to 7 must be nonzero and skips zero there, and skips appear only on core 4, which runs the control interrupt.

Prediction for the stock control path. Its tail (15.1 us over 1.0 million periods against 5.4 us on the fork) comes from an interval with `DAIF.I` set, not from a kernel span, because spans are masked by `ICC_PMR_EL1` and the control interrupt is above it. The stock supervisor-call mask handlers hold `DAIF.I` through a `DSB SY` and an `ISB` (the handler writes `ICC_PMR_EL1`, then `DSB SY`), and a `DSB SY` waits for every outstanding memory access of the core, which under eight cores of DRAM traffic can last several microseconds. The fork reaches the supervisor-call handlers less often. The worst instances the handler records show the interrupted context: an interrupted program counter just after a supervisor call return or in `portASM.S`, with the task register EL0 and `ICC_PMR_EL1` at the API mask or open.

### The owner exit work runs under the API mask with interrupts open

After the guard, the longest remaining interval with `PSTATE.I` set on the control core was the owner work at interrupt exit: `ssKernelService` ran for up to 11 us on core 4 (300 to 670 exits over 5 us in about 320 000 per soak), and a control interrupt that arrived then waited for it; the largest control latency in the earlier fixed-image runs was 12.8 us, from a run without the instance recorder. The exit now raises `ICC_PMR_EL1` to the API mask and clears `PSTATE.I`, as an interrupt service routine's critical section does, runs the work, sets `PSTATE.I`, then restores the mask. Only interrupts above the API mask can run in it.

Two properties make that safe, and the interrupt exit of the generated `portASM.S` carries the second. First, a control interrupt that lands in the work finds `ICC_PMR_EL1` at the API mask and does no owner work. Second, it must not switch tasks either: its exit would otherwise find a switch request that the outer exit has raised and consume it in the middle of the outer exit. `ssKernelOwnerIRQExit` therefore returns 2 for an interrupted masked context, and `freertos-partitioned-port-asm.patch` takes the no-switch exit on 2. The pending request stays set for the outer exit to find. The restore order matters too: `PSTATE.I` is set before `ICC_PMR_EL1` is restored, because the port's own restore clears it after the write, and an ordinary interrupt taken in that gap would be a second outermost exit inside the first (the first QEMU attempt failed 6 of 6 on that).

Proof that no switch request is lost. `scheduler-yield-qemu` wakes a priority 6 task from an API-level timer interrupt on cores 1 to 7 over a busy priority 1 task, 300 wakes per core, and in the storm build an interrupt above the API mask lands every 33 us on every core. Per core it counts the wakes raised in the interrupt (`xHigherPriorityTaskWoken` true) and the task runs that served them, and the latency in counter ticks; a wake later than half a wake period (a request that waited for the next exit) fails, and at the end raised must equal served and no request may be pending. Plain build: raised equals served on every core, none late, none pending. Storm build: the same, with 3119 exits skipped, 276 of them with a request pending, and a worst wake of 6386 ticks of virtual time against 12 500. A mutation in which the skipped exit clears the pending flag stalls the check (a QEMU timeout), and one in which the exit hook reports no request, so that the pending flag alone must carry the wake, passes with 111 skips while a request was pending. The fork therefore has two paths to a switch after an interrupt wake, the flag and the hook's report, and the skip leaves both intact.

Board. Five soaks of 25 s on the default fork with the control path at 10 kHz on core 4, and the switch-request counters in the image: all returned with exact output and no stop; 1 254 587 periods; latency maximum 3.6 us, 99.99th percentile 3.0 us; jitter maximum 3.4 us, 99.99th percentile 2.8 us (targets: 10 us and 5 us). Core 4 skipped 12 900 to 13 500 exits per soak, 1120 to 1250 of them with a request pending; the five soaks together drained 5 988 935 exits, 66 037 skipped. The board has no served counter, so the board evidence is that no soak stopped, every recognition stayed exact and the recognition time did not grow (110.853 ms against 110.394 ms for the earlier fixed image, within 0.5%).

### The task path's wake tail is the global object lock

The task path misses the 10 us wake target (maximum 23.5 to 31.5 us, 99.99th percentile 19.7 to 24.5 us) while the control path meets it, and 82 to 96% of the masked time in the long spans is waiting for the global task lock or interrupt lock. A task path wake has two parts. The handler starts late by the time its core held the interrupt off (the entry delay), and then the handler, the exit work and the switch take the gap to the task's first instruction. The recorder splits the two for the task path: the handler's start minus the deadline, and the task's start minus the handler's start, with the interrupted context of the largest entry delays.

The second measurement attributes the locks. With `SUNSTONE_OBJECT_TRACE` every acquisition of the task lock and the interrupt lock is recorded by (object, caller) on each core: count, wait, hold and waits over 5 us. A wait over 5 us is also charged to the acquisition that last took that lock before the waiter arrived, giving a table of waiter against holder by object and call site. The object is the queue, event group or stream buffer the kernel call works on; public critical sections have none.

Prediction for the entry/gap split (soaks of the unmodified fork, 25 s, task path alone at 1, 2 and 4 kHz; refuted, see the result below): the entry delay carries the tail. Its maximum is at least 80% of the latency maximum on each core, and the gap stays at or below 8 us at the maximum and 6 us at the 99.99th percentile, so the gap alone meets the 10 us target. The interrupted contexts of the largest entry delays are EL0 code at the return from a supervisor call (the mask close of a kernel call), not interrupt handlers.

Prediction for the lock attribution: the waits over 5 us are on the task lock and the interrupt lock in comparable shares, more than 60% of them are charged to a holder acting on a different object than the waiter, and the rest are the same object (the allocator's recursive mutex and one or two queues). If so, most of the tail is serialization of independent objects, which per-object locks remove; if instead the same object dominates, a per-object lock cannot help and the tail needs shorter holds.

Result of the split (two unmodified-fork soaks, entries `1793668412009966237-1c10f02f` and `1793671006348692345-8b3ce66b`, pooled; microseconds). The prediction that the entry delay carries the tail was wrong. The entry delay is short and the gap carries the tail.

| Rate | Latency p50 / p99.99 / max | Entry delay p99.99 / max | Gap p50 / p99.99 / max |
| --- | --- | --- | --- |
| 1 kHz | 5.2 / 22.0 / 23.9 | 6.3 / 10.0 | 4.8 / 21.3 / 22.7 |
| 2 kHz | 4.5 / 20.3 / 25.1 | 8.3 / 8.8 | 4.2 / 19.0 / 24.7 |
| 4 kHz | 4.0 / 27.2 / 31.1 | 9.2 / 11.3 | 3.7 / 24.2 / 25.2 |

Phase timers (`SUNSTONE_OWNER_COUNTERS`) on the wake path of cores 1 to 3, global locks (entry `1793668868450145397-9ef28fda`): the notification takes 0.9 to 1.2 us on average and at most 5.1 us; the interrupt-exit work 1.6 to 2.0 us on average, at most 18.9 us; the task switch 1.4 us on average, at most 11.4 us. Inside the exit work, the control drain exceeds 5 us in 536 of 548 148 passes on core 1 (at most 8.5 us, 13.7 us on core 2) and idle pull in 606 of 274 074 (at most 16.6 us), which are task transfers; hint inspection, deadline expiry and slice refresh never exceed 2.1 us. The rest of the gap is the exit and restore code and the task's own supervisor calls.

Object locks against global locks, same image and phase timers (entries `1793668892195242489-3b585c56` and `1793668868450145397-9ef28fda`): entry delay maxima 5.3, 4.8, 5.5 against 5.7, 5.0, 5.7 us, gap maxima 15.7, 25.0, 19.8 against 16.0, 25.8, 16.0 us, recognition time 109.43 against 109.41 ms. The object locks do not change the task path. A wake-path variant that prefetches the woken task's lines and defers idle pull past a pending switch (entries `1793669877242530330-39c2dcac`, `1793669888246106384-74781dfd`) left the gap median at 6.5, 5.7, 5.3 us against 6.8, 5.8, 5.5 and failed the object check in four runs; it was removed.

Conclusion. Locking cannot bring the task path to 10 us. The median wake already costs 4 to 7 us in kernel code on the woken core, and its tail (13 to 25 us) comes from idle-pull transfers, control drains and the exit and switch code, none of which waits for a global lock. Meeting the target needs a shorter wake path, for example fewer supervisor calls and barriers per wake, no owner pass that precedes the switch, or a core reserved from the streaming application.

### Object locks: queues, event groups and stream buffers lock their own object

Before this change every queue, event group and stream buffer section took the two global locks (the task lock and the interrupt lock), the same two locks as a public `taskENTER_CRITICAL()`. Operations on unrelated objects therefore queued behind each other with the interrupt mask raised.

Mechanism. An object hashes, by its address, to one of 64 slots (`SS_OBJECT_SLOTS`). Each slot has a task-side ticket lock T and an interrupt-side ticket lock I, the roles of the two global locks:

| Section | Holds | Mask |
| --- | --- | --- |
| Critical section of an object source (`taskENTER_CRITICAL`) | T and I of the slot | raised |
| Suspension of an object source (`vTaskSuspendAll`), across the blocking path | T of the slot | open |
| `FromISR` section | I of the slot | raised |
| Owner timing out or repairing a waiter | I of the slot recorded when the waiter blocked | raised |
| Priority donation (`xTaskPriorityInherit` and its inverse) | the donation lock, a leaf | raised |

The slot needs no field in the object, so `StaticQueue_t` and its siblings keep their layout. A task that blocks on an event list records the slot (`event_slot`) under the object's lock; the owner that later times the waiter out takes I of that slot, so it excludes the senders and receivers of the same object and nothing else. The insertion into the event list now also holds I of the slot, which closes a race the global scheme had: a waiter placing itself on a list under T alone while the owner of another waiter of that list removed it under the global I alone.

The public `taskENTER_CRITICAL()` family keeps the global locks and no longer excludes object sections. Application critical sections protect application data, not kernel objects, and the kernel took no application lock inside an object section. The timer source keeps the global locks because its sections enclose queue creation; with the timers disabled in the OS image it is not on the hot path.

Invariants.

- **Lock order.** Global locks, then at most one task-side slot and one interrupt-side slot per CPU (nesting is on the same object or one in the same slot, asserted), then the donation lock. No section waits for an owner acknowledgment while holding a lock; commands and publications defer to the outermost release, as before.
- **No lost or duplicated task.** A waiter leaves an event list by exactly one of: the waker's `xTaskRemoveFromEventList` under the object's lock, or the owner's timeout under the same slot's I; the outcome compare-and-swap that arbitrates them, the wait generation, the pins and the deferred publication are unchanged. The interrupt-side lock of the slot orders list edits (insert, remove, priority reorder) against each other.
- **No priority inversion.** Locks are FIFO tickets, so a holder is never overtaken. Donation and its removal still hold the donation lock across the read-modify-write of the donated priority and the mutex count, and publish the repair at the outermost release. A mutex shares a slot with other objects only as a longer queue, never with an unbounded wait: every holder runs a bounded section.
- **Affinity masks.** Placement, publication, migration and idle pull are untouched; the change adds no path that moves or queues a task.
- **Event-driven.** Waiting is `WFE` on the ticket, release is `SEV`; there is no polling and no tick dependency.
- **Idle.** The idle task is still `wfi`; the change adds nothing to its path.

Host check (`scheduler-objects-qemu`, eight cores on the instruction counter). Phase 1 holds the lock of one queue open inside its critical section until six other cores have each completed twenty operations on queues of their own, or a budget of the holder's loop iterations runs out. It counts operations and iterations, not time. With the global locks the others cannot run, so the budget runs out on all 40 holds and the check fails; with object locks none runs out. Phase 2 mixes producers, consumers with one-tick timeouts, interrupt-level senders on every core, a mutex with inheritance, an event group, a stream buffer and a counting semaphore, and checks that no message is lost, duplicated or reordered and that the mutex excludes. The linked-kernel check and the other emulator cases (kernel, concurrent, soak, yield, storm, heap, runtime) pass with the change.

## Capacity hints guide placement without weakening priorities or affinity

Application-kit task attributes add a class, allowed CPU mask, migratable flag
and estimated demand in fastest-core capacity units. Classes are
latency-critical, throughput, background and interrupt-bound. Defaults preserve
existing affinity and priority behavior. Hints never authorize a disallowed CPU
or preempt a higher-priority local task with lower-priority work.

Initial placement chooses the fastest eligible available capacity for heavy,
latency-critical and throughput work. Background and housekeeping prefer LITTLE
capacity when deadlines and measured energy support that choice. Interrupt-bound
work prefers its consuming IRQ's eligible core. Reserved CPUs are excluded.
When all eligible CPUs are busy, deterministic least-load placement considers
normalized declared demand and capacity; admission remains a separate contract.

Accounting reads the generic counter on task transitions and IRQ entry/exit,
without a periodic utilization tick. A cheap per-task execution/runnable EWMA
is optional; its update occurs at transitions and its decay uses elapsed time.
Static application estimates bootstrap placement. Neither PELT-like tracking nor
a scalar capacity ratio predicts every integer/SIMD/cache workload. PMU-based
classification is deferred until measurement demonstrates a need.

The clock owner supplies effective applied rates and thermal restrictions.
Unknown transition state invalidates rate-normalized estimates. Busy CPUs use
the vendor top point for their bin, subject to existing thermal protection.
WFI and the established idle-gating owner provide idle savings. Only background
placement may compare LITTLE execution against BIG race-to-idle using valid
energy costs; latency-critical and throughput classes seek fastest capacity.

## Applications query topology and split parallel work proportionally

The proposed kit interfaces are:

```c
int ss_cpu_topology(struct ss_cpu_info *out, size_t capacity, size_t *count);
int ss_work_split(uint64_t units, uint64_t cpu_mask, uint32_t quantum,
                  struct ss_work_range *out, size_t capacity, size_t *count);
```

`ss_cpu_info` reports logical CPU, affinity, microarchitecture, physical/SMT
relationships, cache/domain IDs, maximum/effective capacity and validity flags.
The immutable snapshot identifies its calibration and operating-point epoch.
The syscall copies a bounded table; inner compute loops retain the result.

The split helper assigns aligned initial ranges in proportion to effective
capacity. It conserves every work unit, obeys the eligible mask and deterministically
distributes rounding remainders. A zero/unknown capacity returns an explicit
uncalibrated result; callers may choose equal splitting. Optional dynamic
chunking uses a bounded atomic next-chunk index in application-owned storage.
Chunk size must amortize claiming overhead and preserve the application's
dependency and reduction ordering.

The CPU-performance stream owns mimus GEMM and attention optimizations on
`work/cpu-perf-os-1031` and mimus `cpu-perf-1031`. This API is the coordination
contract for that consumer: the OS supplies topology and integer ranges; the
application chooses kernels, tensor tiles and numerical ordering. No mimus
header or model dependency enters the OS. Compare equal and weighted ranges
with identical kernels, weights, inputs, thread masks and numerical results.

Work ranges use half-open `[begin, end)` endpoints. The helper receives total
units before alignment and a positive quantum. Full quanta are distributed by
integer largest remainder with CPU ID as the tie-break; only the final range
may contain a shorter tail. No unit is omitted or assigned twice. Empty ranges
are permitted when units are fewer than workers. Insufficient output capacity
returns the required count without writing partial ranges. Arithmetic overflow
and an empty eligible mask return an error. A later operating-point change
makes the estimate stale but does not invalidate ranges; the application can
refresh at the next job boundary. API version one exposes validity flags and
an epoch explicitly so the CPU-performance consumer can distinguish these cases.

## One-shot local timers replace periodic scheduler ticks

The physical generic counter defines one boot epoch. The compatibility tick is
`floor((counter − epoch) × 1000 / counter_hz)`, reduced to `TickType_t` with
upstream wrap behavior. Conversion avoids overflow with quotient/remainder
arithmetic. `xTaskGetTickCount` and its ISR variant compute time directly;
no secondary independently increments global time.

Each CPU owns PPI30 and programs the minimum of its next timeout, local slice
expiry and later reservation boundary. An empty set disables its scheduler
timer. Timer IRQ processing reads the counter, expires a bounded batch, selects
urgent work and programs the next compare. Remaining expired work schedules
another local entry after restoring interrupts. Expiration never waits for a
1 ms compatibility boundary.

EL0 callers use the existing privilege transition for timer programming and
SGIs. The port completes required device writes before dependent operations,
uses ISB after timer system-register programming, and observes GIC write-pending
completion before readback. Late compares and stale readback are modeled.
Existing returning images save/restore timer and GIC state on every CPU.

`ss_deadline_wait_until(counter_deadline)` adds sub-tick waiting through the
same owner timeout mechanism. FreeRTOS delays retain 1 ms units. Software
timers use a timer-service task and existing callback semantics, but their
next command/expiry uses owner deadlines rather than periodic tick polling.
CPU0 may sleep without stopping time or the other CPUs' deadlines.

## Primitive bounds include waiting, copying and holder progress

Let `S_r,c` be the maximum service time of object `r` on participating CPU `c`,
including allowed IRQ interference and release/handoff. One outstanding internal
request per CPU and FIFO non-preemptive acquisition give
`wait_r ≤ Σ(other c) S_r,c`. With at most eight CPUs, a uniform service bound
`S_r` gives wait at most `7 S_r` and complete exclusion at most `8 S_r`.
[MSRP assumptions and limitations](scheduler-research.md#locking-bounds-depend-on-progress-rules-and-workload-contracts)

| Primitive | Bound and remaining obligation |
| --- | --- |
| Local ready operation / uncontended untimed local notification-give | Zero shared-lock waiting; bounded local instructions plus admitted higher-priority IRQ interference. Contended notification-give uses its FIFO object fallback |
| Remote activation publication | One native release RMW plus status/SGI handshake; hardware coherence latency must be measured, not treated as zero |
| Queue send/receive, semaphore metadata | FIFO bound above, with item-copy bytes in `S_r`; queue creation records maximum item size. Bulk copy outside a lock requires a separately proved reservation protocol |
| Notification conflicts and compound owner operations | Per-slot FIFO bound after one failed fast CAS or direct gated admission; untimed local uncontended activation is direct, remote consumption honors the gate |
| Event-group set, stream-buffer completion | FIFO bound with actual bytes and awakened-waiter count in `S_r`. Wake-many is `O(k)` for `k` wakeups, bounded by registration; it is not advertised as an O(1) single wake |
| Local application mutex | Priority inheritance limits inversion only under admitted critical-section, nesting and arrival bounds; preserve FreeRTOS's documented simplified inheritance behavior initially |
| Cross-core application mutex | Metadata FIFO does not bound the application's sleeping ownership. Requires a declared critical-section contract and an explicit cross-core progress protocol before a hard bound is claimed |
| Global critical/suspend compatibility | Caller duration and all relevant checkpoint/IRQ bounds; excluded from an unconditional 25 µs promise and separately measured |
| Migration and deletion | Bounded command storage and checkpoints; completion requires acknowledgments and has a separate measured slow-path deadline |

The simple FIFO sum applies to non-nested locks. Where task control or mutex
priority repair needs ordered nesting, its service bound includes the complete
wait and service of every nested lock. Object-to-control reverse nesting is
forbidden; donation publication defers repair until its object lock is released.
The first latency-qualified queue class uses at most 64 copied bytes per
operation and sixteen selected wakeups per bounded batch. Larger valid FreeRTOS
objects remain functionally supported but need separate service bounds; they
cannot inherit that class's 25 µs guarantee. These are API resource contracts,
not additional board-image launch-admission checks.

For short internal locks, admitted `8 S_r` plus local bookkeeping must fit
25 µs. An unexplained overrun blocks that live stage. WFE reduces active
waiting but does not remove the waiting CPU's non-preemptive blocking.

## Instruction budgets are hypotheses until linked-code and board measurement

The following are implementation budgets, not measured A733 costs. `P = 8`
priorities, `W = 2` pending words per priority, `N ≤ 128` registered tasks,
and `k ≤ 16` activations per entry are fixed configuration limits.

| Path | Work counted before implementation | Initial instruction budget and measurement |
| --- | --- | --- |
| Ready enqueue/remove | Fixed list-link and bitmap operations | At most 64 retired instructions each, excluding instrumentation and context save |
| Pick/yield | One CLZ, one FIFO remove; yield adds one append | At most 96 instructions for local pick/yield body |
| Untimed local notification-give | Native atomic value/pending operations, ready append and priority test | At most 160 instructions before optional switch |
| Remote wake | Lifetime and routing pins, two hint/head publication RMWs, status/SGI handshake and reference releases | Count the complete linked producer; helper-only instruction counts exclude these pins and callbacks |
| Incoming scan | One head exchange, at most 256 slot-word loads, nonzero-word exchanges and sixteen hint validations | Count the linked path; tune the bounded batch to meet 25 µs without delaying urgent tasks |
| Timed block/wake | Object protocol plus at most seven heap levels | Count comparisons, loads/stores and object waits separately; no invented cycle conversion |
| Timeout | At most seven heap levels per expired entry plus object detach | Measure simultaneous expiry and expired-batch continuation |
| Migration | Two owner commands, off-CPU and adoption acknowledgments, cache refill | Slow-path counter interval and PMU events; no constant context-switch claim |
| Context switch | Existing complete integer/SIMD/privilege frame plus local selection | Count both FPU-active and inactive cases; measure entry-to-first-instruction on each CPU type |

`checks/scheduler-masked-spans.py` runs the CPU latency workload under the owner
scheduler on eight emulated cores and reports every interrupt-masked span as the
number of scheduler atomic stores and read-modify-writes, and of atomic loads,
that its core executed. A span that waits for another core is excluded. It
bounds both counts. The counts do not convert to time: on the A733 the cost of a
span is dominated by cache misses that the emulator does not have.

The CPU control with `SUNSTONE_LATENCY_PMU` records, for each core's three
longest masked spans per phase, the instructions retired, cycles, L1D refills and
L2D refills inside the span. First prediction, which the capture contradicted (see the next subsection): after the cache-line grouping, every
span in the measured phases retires at most 1,500 instructions (the sum of the
rows above for the longest entry), and a span longer than 15 µs at these
instruction counts shows at least 100 L1D refills, that is, its time is miss
time. A long span with few refills contradicts the prediction and points to a
wait. The counters are read only; no event interrupt, clock or voltage changes.

### Masked-span cycles are barrier and exception cycles, not memory-access cycles

The first PMU capture contradicted the miss-time prediction: spans of 17 to 30 µs
retired 770 to 2,100 instructions with 20 to 60 L1D refills, at 25 to 50 cycles
per instruction. `checks/scheduler-masked-stalls.py` executes the model images
under the QEMU GDB stub, steps one core through a span from the entry of the mask
call to the entry of the unmask call, and classifies every executed instruction.
The Normal memory of the scheduler, the control tables and the recorder is
write-back cacheable (`0x41000000` to `0x48000000`), the monitor interval
(`0x42000000` to `0x42200000`) holds no scheduler or recorder data, and the
count of loads and stores to Device or non-cacheable addresses is zero in every
span traced. The recorder reads `CNTVCT_EL0` with an ISB eight times in the
longest span, and its buffers are cacheable. Per span:

| Span (instructions) | DSB (SY) | ISB | SVC and ERET | SGI writes | LSE atomics | Exclusives |
| --- | --- | --- | --- | --- | --- | --- |
| End-of-run self-suspend, `ssKernelCommand` (2,574) | 17 (8) | 19 | 11 | 4 | 15 | 2 |
| Task critical section, `vTaskEnterCritical` (889) | 4 (4) | 4 | 4 | 0 | 0 | 4 |
| Service entry, `ssKernelService` (577) | 2 (2) | 4 | 0 | 0 | 1 | 0 |
| Notification give, `ssKernelNotify` (667) | 4 (3) | 5 | 0 | 0 | 10 | 0 |

Prediction: at most 17 barrier, SGI and exception instructions per span account
for 25,000 to 78,000 cycles, so each costs thousands of cycles. A DSB SY or DMB
cannot complete before every earlier store has reached the point of
coherence, and the load tasks keep the core's write buffer and the write-streaming
bus path full with their 1 MiB sweeps; the same instructions cost about a sixth
as much in the idle phase (about 3 µs for the same service entry, a sixth of the loaded span). The cycles are
therefore backend stall cycles with the core waiting on its own outstanding
traffic. Counting these events inside each of the longest spans confirms or
refutes it:

| Event | Number | Prediction for a span over 15 µs |
| --- | --- | --- |
| `STALL_BACKEND` | 0x24 | At least 70% of the span's cycles |
| `BUS_ACCESS` | 0x19 | Many beats, in proportion to the sweep traffic that the barriers wait for, and not to the span's instructions |
| `MEM_ACCESS` | 0x13 | At most 700 (about one per memory instruction) |
| `L1D_CACHE_REFILL` | 0x03 | At most 60, as in the first capture |
| `INST_RETIRED` | 0x08 | Unchanged from the first capture |

All five are architectural PMUv3 events. The Cortex-A55 TRM lists 0x03, 0x13, 0x19
and 0x24 (the cores report them in `PMCEID0_EL0` and `PMCEID1_EL0`; the measured
A55 values `0x7fffffff` and `0x01f0ae7f` set all four). No TRM for the Cortex-A76
is in the source cache. The capture therefore stores each core's implemented-event
mask and counts only the events that mask lists. Neither core documents a
barrier-specific event (the Armv8.2 `DSB_SPEC`, `DMB_SPEC` and `ISB_SPEC`, 0x7C to 0x7E,
are absent from the A55 table), so the prediction is tested by exclusion. A long
span with low `STALL_BACKEND` contradicts it and points to the SGI and exception
entry cost instead. A span whose cycles are mostly stalled while `BUS_ACCESS` stays low
points to waiting on another core, such as a LSE atomic on a line in another
core's cache.

Result of the capture on port 3-3 (all five events are implemented on all eight
cores): in the busy phase `STALL_BACKEND` is 94 to 98% of the cycles of every
service, notification and critical-section span except core 3's critical section
(59 to 68%), `MEM_ACCESS` is 441 to 619, `BUS_ACCESS` 320 to 960 beats and L1D
refills 23 to 38. The prediction that backend stalls dominate holds. The capture does
not separate the two causes that fit it: a service entry with two DSB SY and 35 L1D
refills stalls about 33,000 cycles, which is 0.5 µs per refill if the refills
serialize behind a saturated DRAM, and the barrier drain the prediction names would
need about 8 µs per DSB. The refill reading fits better. The spans are limited by
dependent line fetches while eight cores stream 1 MiB sweeps. They shorten only
with fewer distinct lines per entry or with overlapped fetches (prefetch), which
this stream has not implemented; the measured maxima are 12 to 29 µs in the
measured phases and 18 to 46 µs for the end-of-run task suspension.

Linked-kernel tests replace budget rows with instruction counts before board
submission. Board samples add cycles/counter duration, p50, p99 and maximum.
PMU instrumentation is optional and compiles out completely when disabled.

## EDF/CBS is a second-stage policy with explicit admission

After per-core fixed-priority scheduling, object locks, migration and one-shot
timing pass, add per-core EDF constant-bandwidth servers. Each reservation has
runtime, relative deadline and period. Exhaustion stops its server until its
one-shot replenishment; execution accounting uses the counter at transitions.

The initial band sits below the highest fixed-priority emergency/IRQ work and
above ordinary background work. Admission subtracts bounded higher-band demand,
interrupts, object blocking and scheduler overhead from each CPU's capacity.
Demand is calibrated for that CPU; migrating a reservation requires destination
admission. Budget accounting and mutex inheritance must state whose runtime is
charged. GRUB reclaiming and proxy execution are later options, not prerequisites
for the first reservation guarantee. [EDF/CBS and reclaiming research](scheduler-research.md#linux-distributes-state-but-still-uses-runqueue-locks)

## Commit series introduce one reviewable mechanism at a time

| Series | Deliverable | Host gate and live measurement |
| --- | --- | --- |
| A: topology and proof infrastructure | Topology tables/query/split helper; fixed-slot wake model and sequence/sleep model; additive percentile evidence using the latency worker's recorder | Symmetric, A733 and three-cluster fixtures; exhaustive interleavings; current-kernel board baseline, MPIDR/cache/LSE capture and capacity kernel; exact mimus control |
| B: owner runqueues and notifications | Owner state, local pick/context switch, atomic notifications and queued remote activation; object APIs retain compatibility protection until C | Actual forked linked-kernel task/notify/block stress, all-core QEMU soak, local-lock counters zero; latency idle/load and exact mimus |
| C: object protection | Replace compatibility protection with per-object locks; tune already-correct timeout, priority/inheritance/control and migration paths | Cross-core queue, notification, simultaneous timeout, deletion/reuse and migration stress; lock/mask attribution; latency and exact mimus |
| D: per-core compare and heterogeneous policy | Remove CPU0 periodic tick, enable local deadlines/slicing, capacity hints, idle pull and IRQ-owner binding | Time wrap, overdue compare, stale readback, WFI races and synthetic topology tests; idle wakeups, big/little throughput, weighted splitting, pinned latency and exact mimus |
| E: reservations | EDF/CBS, band policy and admission | Budget exhaustion/replenishment, interference and inheritance accounting; periodic service isolation and full physical regression |

Every series-B task has one owner, including a task whose affinity admits
several cores. Creation chooses the allowed core with the fewest assigned tasks;
ties select the lowest logical index. Owner changes require the acknowledged
migration transfer. There is no legacy selector or global ready list under the
option. Idle pull starts in D: a ready task can wait on its busy owner while
another core is idle. The linked stress counts observations of that condition.

Series B still needs correct owner-routed timeout handling even before C removes
object compatibility locks. A shared compatibility object lock never authorizes
remote writes to owner queues. The implementation may split a series into
smaller commits, but no live stage leaves an enabled public API half converted.

Before each board submission, merge `bringup`, resolve changes without editing
probe entry ownership, run focused checks and one baseline build. Images are
built only when the queue can accept them, using port 3-1 or 3-3 and existing
CPU operating points. A paused board stops submission and is reported.

The latency worker owns timing instrumentation and the combined-load workload.
Add phase/per-core histograms through that measurement path and merge its
updates instead of replacing it. Record percentile bin widths and conservative
quantile endpoints if histograms are used. Initialization evidence remains
separate and retained. Measure recorder-enabled overhead; disabled executable
code and storage must match the noninstrumented control.

The earlier mimus entry `1793561595474856811-579a99b6` returns exact reference
CRCs and text but has a malformed expectation. Correct it by an audited offline
re-evaluation of the existing capture and normal return. Preserve frozen inputs
and the original mismatch, append the corrected expectation/result and update
the authoritative queue record. No board retry is needed to repair that format.

## Validation exercises ownership transitions and hardware completion

The wake model uses an executable TLA+ abstraction with explicit per-CPU
publication buffers, per-location coherence, acquire observations and DMB
store-to-load constraints. It explores publication, PREPARING/BLOCKED races, timeout winners,
dispatch lowering, pending-SGI clearing and idle entry with two producers and
one owner; migration adds a second owner. It enumerates visibility/order choices permitted by the stated
acquire/release and DMB protocol, includes generation reuse and checks exactly
one activation. Negative models remove essential barrier, reference or winner checks and must
produce the corresponding lost-wakeup or unsafe-reuse trace. A sequential queue simulation
alone does not qualify as a weak-memory model.

Linked-kernel stress runs real task, notification, queue, event and timer APIs,
with multiple operations in flight. QEMU uses eight concurrently executing
cores. Cases force wakes during block commit, timeout and remote priority
changes, migrate only permitted tasks, and delete/recreate static tasks while
publications are outstanding. Instrument assertions detect duplicate running
ownership and non-owner link writes.

Each live board stage records an image hash, revision, queue entry, exact output
and normal return. Latency rows include idle/loaded ready-to-running and
IRQ-to-task p50/p99/maximum, lock acquisitions per operation, every lock's wait
and complete hold, context switch, mask duration and idle wakeups per CPU.
The CPU0 periodic-tick control is the idle-power baseline; secondaries already
have no periodic scheduler ticks. Record temperature and supply observations
only under matched workload, duration and thermal conditions.

Capacity calibration runs the identical fixed integer/SIMD kernel separately on
each CPU at the vendor top points, with matching working sets and elapsed
counter windows. Normalize fastest observed capacity to 1024, report each core
and variation, and retain workload-specific ratios. Mimus compares equal,
weighted and optionally dynamic chunks with exact CRCs/text and total CPU time.
Latency-critical placement compares pinned BIG and eligible unpinned execution
under the same eight-core load.

Energy per job is reported only if the power-supply observation app exposes
usable calibrated power and sampling bandwidth. Voltage alone does not give
energy. If unavailable, report that gap and idle wakeups/temperature separately.
Three baseline builds, the integration gate and check index run once before
the final report, with feature flags selecting the changed branches.

## Design review record

Two independent reviews approve staged implementation on 2026-11-01. The
correctness review checks notification linearization and completion, reference
retirement, migration, priority repair, SGI ordering and queue retry semantics.
The performance/progress review checks bounded batching, control progress,
placement, topology, work splitting and complete live-stage API coverage.
Both require executable models, enabled linked-kernel cases and independent
implementation review. Neither approval establishes physical timing.

[The review inventory](scheduler-partitioned-reviews.json) pins their detailed
reports. Research accuracy review is recorded separately in
[the research document](scheduler-research.md).
