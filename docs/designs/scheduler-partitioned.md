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
fork while qualification retains a buildable current-kernel control.

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
