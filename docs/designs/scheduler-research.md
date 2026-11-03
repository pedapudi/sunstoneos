# Scheduler ownership, delegation and heterogeneous placement

Status: **research** (a literature and source review). Its conclusion is
implemented as [the partitioned kernel](scheduler-partitioned.md), which
[scheduler status](../status/scheduler.md) qualifies.

The intended reader knows operating systems, concurrency and basic real-time scheduling.
A priority inversion is time when lower-priority work prevents a ready higher-priority
task from running. A hard real-time claim requires a bound under stated workload
assumptions, beyond observed percentiles. [Brandenburg, dissertation, chapters
2–3][dissertation]

The direction the review supports is owner-only per-core ready queues, queued remote wakeups, short
per-object locking, one-shot timers and topology-driven placement. This is a SunstoneOS
engineering conclusion from the evidence below. It is not a performance result from
another operating system.

## Evidence separates board observations, source facts and predictions

**Paper** identifies a primary paper checked for the stated mechanism or result.
**Code** identifies an implementation checked at a pinned revision. **Secondary**
identifies reporting that does not establish implementation status. **Inference**
identifies a proposed SunstoneOS adaptation or predicted effect. Each table
distinguishes these categories. Downloaded references, revisions, sections, hashes and
licenses are recorded in [the source inventory](scheduler-research-sources.json).
References are read-only research inputs; no implementation from these papers is
imported.

The global-lock FreeRTOS SMP kernel shares ready lists, a recursive task lock and a
recursive ISR lock across eight cores, and task switching takes both locks
([port and scheduler audit](scheduler-realtime.md#the-global-tick-has-one-owner)).
Its board observations are the baseline that motivates partitioning:

| Board observation | Measured value | Scope |
| --- | ---: | --- |
| Task-lock wait | 177.875 µs maximum | Bounded-traversal CPU image; includes initialization |
| Task-lock outer hold | 11.500 µs maximum | Same image and recording window |
| Ready to running under eight-core load | 133.333 µs maximum | Same image; highest-priority pinned probes |
| Interrupt-masked duration during workload | 62.750 µs maximum on core 6 | CPU control |
| Task-lock wait during workload | 73.417 µs maximum on core 6 | Same control; other cores' initialization maxima obscure their workload maxima |

The runs are records `1793560934366050732-4e45b5ea` and `1793559978973913698-7f7e36e0`
in [scheduler.jsonl](../evidence/scheduler.jsonl) and [latency.jsonl](../evidence/latency.jsonl);
[latency status](../status/latency.md) gives the counter frequency and measurement
boundaries. Notification-take opens the 62.750 µs mask interval, which includes
kernel-lock waiting, and recorded hold intervals omit part of the unlock tail.
These are observed prefixes, not complete worst-case bounds, and the records
carry no percentiles. No table in this document substitutes another system's
latency for SunstoneOS latency.

## Partitioning removes contention by giving each scheduler exclusive state

| Model | Ownership and contention | Scheduling cost and limitation | Evidence |
| --- | --- | --- | --- |
| Partitioned | Assign each task to one CPU; each CPU schedules its own tasks | Local decisions avoid a shared ready queue. Offline assignment resembles bin packing; spare capacity on another partition does not automatically help | **Paper:** dissertation chapters 3–4; EDF comparison §1 [B1][dissertation], [B2][edf] |
| Clustered | Assign each task to a subset of CPUs; schedule globally within that subset | Limits the number of contenders and migration distance. Shared cluster state still requires synchronization | **Paper:** EDF comparison §§1, 4–6 [B2][edf] |
| Global | Eligible tasks may execute throughout the processor set | Work conservation and flexible placement require coordination and migration; a shared queue is one implementation choice, not a mathematical requirement | **Paper:** dissertation chapters 2–4 [B1][dissertation] |
| RTEMS scheduler instances | Assign processors to separate scheduler instances, including singleton partitions | Allows different scheduling policies for different CPU groups. MrsP coordinates shared resources across instances | **Documentation:** clustered configuration and MrsP [R1][rtems-cluster], [R2][rtems-key] |
| AUTOSAR multicore OS | Statically assign OS applications and their tasks to cores; activate tasks across cores through OS services | Local resources affect their own core. Cross-core spinlocks require configured ordering and protection against local interference | **Specification:** R24-11 §§7.9.3, 7.9.21, 7.9.28–29 [A1][autosar] |

The LITMUS^RT comparison includes measured scheduling and migration overhead on a
24-core Intel machine. Partitioned EDF is preferable for its hard real-time experiments;
clustered EDF is effective for its soft real-time experiments. This establishes the
importance of implementation overhead, not an A733 schedulability theorem. **Paper:**
[B1, abstract and chapter 4][dissertation], [B2, §§4–6][edf].

**Inference:** Start with singleton partitions because the A733 observations already
expose shared-lock waiting. Preserve explicit affinity. Allow a measured idle-pull path
only for tasks whose contract permits migration. A shared L3 can reduce migration cost
without removing private-cache refill or synchronization cost. [A733 cache
topology](../cpu-coherence.md)

## Locking bounds depend on progress rules and workload contracts

Let `m` be the number of CPUs accessing a resource, `n` the number of tasks, and `L` the
longest resource service time in seconds. Service time includes handoff and permitted
interrupt interference, not just the protected C body. The following bounds assume
mutually exclusive, non-nested resource requests unless stated otherwise.
Suspension-aware analysis distinguishes waiting from execution. Suspension-oblivious
analysis charges suspension as execution demand. These definitions produce different
priority-inversion bounds. **Paper:** [Brandenburg's locking review, §§2,
5.1–5.2][locking].

| Protocol | Progress and queueing rule | Blocking bound and qualifications | SunstoneOS implication |
| --- | --- | --- | --- |
| MPCP: multiprocessor priority ceiling protocol | Suspends waiters; boosts holders above ordinary tasks; priority-ordered requests | No `(m−1)L` FIFO bound. Higher-priority requests can overtake; ceilings, arrival bounds and holder interference enter response-time analysis. Priority queues admit an `Ω(mn)` suspension-aware inversion lower bound in the general model | Unsuitable as a blanket finite bound for arbitrary FreeRTOS mutex use. **Paper:** [review §§5.2.1, 5.2.3][locking] |
| MSRP: multiprocessor stack resource policy | Non-preemptive FIFO spinning for global resources; local stack resource policy | One request waits for at most one critical section per other participating CPU: `B ≤ Σ L_c ≤ (m−1)L`. A locally released task may also suffer one complete non-preemptive request, including its remote wait | Suitable for bounded internal metadata sections; include waiting in the masking budget. **Paper:** [review §4.1][locking] |
| FMLP: flexible multiprocessor locking protocol | Short resources use FIFO spinlocks; long resources use FIFO suspension with inheritance or boosting, depending on scheduling model | Partitioned short variant inherits MSRP's bound. Global long variant has at most `(n−1)L` suspension-oblivious inversion per request. This is not the bound for every FMLP variant | Specify the variant; sleeping application mutexes cannot borrow a short-lock bound. **Paper:** [review §§4.2, 5.1.2–3][locking] |
| OMLP: optimal multiprocessor locking protocol family | Bounded FIFO segments, priority tails, donation or tokens depending on variant | Global semaphore variant bounds suspension-oblivious inversion by `(2m−1)L` per request. Family members provide `O(m)` bounds under their stated analysis; this is not an unconditional wall-time wait bound | Useful if cross-partition sleeping locks justify additional protocol machinery. **Paper:** [OMLP §§2–5][omlp] |
| MrsP: multiprocessor resource sharing protocol | FIFO requests, local ceilings and helping: a waiting CPU can execute a preempted holder | One predecessor per participating CPU bounds required remote critical-section work in the non-nested case. Wall time also includes ceiling-eligible interference and helping/migration overhead; `(m−1)L` alone is insufficient | Requires explicit scheduler support and a separate proof before adopting helping. **Paper:** [review §7.2][locking]; **implementation documentation:** [RTEMS MrsP][rtems-key] |

**Inference:** With eight contenders, an MSRP request can wait for seven service times.
A 12 µs service assumption gives 84 µs of waiting and 96 µs including its own service.
Fairness alone therefore cannot establish a 25 µs masked bound. An eight-way internal
lock needs service below 3.125 µs to fit 25 µs even before other overhead. Removing
shared scheduler acquisitions has higher priority than changing their arbitration order.
The 12 µs input is illustrative; recorded holds do not establish an upper bound on
service time.

### FIFO spinlocks and locality optimizations have different contracts

First-in, first-out (FIFO) admission orders contenders by their queue insertion. It does
not bound elapsed wait if a holder or predecessor stops executing. None of these
spinlocks supplies priority inheritance or a hard real-time response guarantee by
itself.

| Lock and primary source | Admission, coherence and storage behavior | Tradeoff for the A733 runtime |
| --- | --- | --- |
| Ticket lock, [Mellor-Crummey and Scott, TOCS 1991](https://homepages.inf.ed.ac.uk/mic/PPLS/mellor-crummey.pdf), §2.2 | Fetch-and-increment assigns tickets; all waiters observe the serving counter. FIFO, with constant lock storage. Releasing the lock changes the shared line read by all waiters. Proportional backoff reduces traffic but requires a delay estimate. | Simple fair admission is attractive for a small fixed core count. It still creates coherence traffic and predecessor blocking. Measure actual contention before replacing it. |
| MCS, same paper, §2.4 | An atomic tail exchange enqueues a per-acquisition node; each waiter spins on its own flag and its predecessor releases it. FIFO after queue insertion; O(1) remote references per acquisition. Nodes must remain valid through handoff. | Local spinning can reduce shared-counter traffic. Node lifetime, task/IRQ nesting, cancellation and preemption must match the RTOS contract. The paper evaluates 8 MHz MC68000 BBN Butterfly and 16 MHz 80386 Sequent Symmetry hardware; its costs establish no A733 cycle estimate. |
| Linux qspinlock, [Linux v6.12 source](https://github.com/torvalds/linux/blob/v6.12/kernel/locking/qspinlock.c), layout comment and queued_spin_lock_slowpath | Four-byte lock combines locked, pending and encoded queue-tail fields. Per-CPU MCS nodes encode CPU identity and nesting context. The first queued waiter spins on the lock word; others spin locally. A pending fast route avoids queue setup under light contention. | The native queue path preserves queued handoff; it is not a ticket assignment contract for function-entry order. Per-CPU node reuse assumes Linux's preemption/nesting rules. The source explicitly requires atomic byte/halfword operations. Retain those invariants and Arm memory ordering in any adaptation. This is a primary implementation, not a paper claiming a platform-independent latency. |
| CNA, [Dice and Kogan, EuroSys 2019](https://arxiv.org/pdf/1810.05600), §§4–7 | MCS-derived local spinning with one word of lock state plus waiter nodes. Unlock prefers same-socket successors and moves other waiters to a secondary queue. A low-probability flush serves that queue. Thus it violates global FIFO and offers probabilistic long-term fairness, not a deterministic bypass bound. | Its locality mechanism targets multi-socket NUMA. The paper's two-socket E5-2699v3 system has 72 logical CPUs; its map benchmark reports 39% over MCS at 70 threads and 50% for update-only operations at 70 threads on that same system. A733 cache/topology benefits require measurement; socket locality is not an established A55/A76 policy. |
| ShflLocks, [Kashyap et al., SOSP 2019](https://multics69.github.io/pages/pubs/shfllock-kashyap-sosp19.pdf), §§3–4, §§6.1–6.3 | A waiter rearranges queued nodes while the holder executes, moving locality work away from unlock. NUMA policy groups same-socket waiters, intentionally changing FIFO order. Blocking variants add parking/waking and permit TAS fast-path lock stealing to avoid waiting behind a descheduled contender. Reported fairness is long-term throughput balance. | Evaluation uses eight sockets and 192 Xeon E7-8890v4 cores with hyperthreading disabled; 30-second single-lock tests pin contenders, with two per core for oversubscription. Nonblocking variants outperform stock locks by 2.8× on MWRL and 2× on lock1. These workload-specific throughput gains do not bound maximum wait or justify shuffling on eight A733 cores. |

CNA and ShflLocks are research candidates only where measured cache transfers dominate.
Their fairness measurements do not prove that a high-priority task cannot repeatedly be
bypassed. Scheduler locks also need bounded critical sections, a valid interrupt
exclusion policy and a rule preventing a waiting task from blocking the only runnable
lock holder.

An Arm adaptation must use explicit acquire/release atomics and prove all publication
and node-reuse orderings. [Paolillo et al.'s CNA weak-memory
verification](https://arxiv.org/abs/2111.15240) is relevant follow-up evidence:
pseudocode that omits fences is insufficient. Its qspinlock/LKMM finding concerns the
revisions studied there and is not a claim about all later Linux releases.

## Linux distributes state but still uses runqueue locks

Linux's queued remote-wakeup path is an implementation reference, not evidence that
Linux scheduling is lock-free. A wakeup can still take the task's PI lock and a target
runqueue lock. The pending-wakeup consumer takes its own runqueue lock while activating
tasks. **Code:** [`try_to_wake_up`, `ttwu_queue`, `sched_ttwu_pending`, Linux
v6.12][linux-core].

| Mechanism | Verified behavior | Proposed use |
| --- | --- | --- |
| Per-CPU runqueues | A runqueue belongs to a CPU and has its own lock; migration may require multiple runqueue locks | **Inference:** Make ready-queue writes exclusive to the owner, avoiding even the local shared lock. **Code:** [`rq_lock`, `ttwu_queue`][linux-core] |
| `TTWU_QUEUE` | Queue remote activation through a lockless linked list and call-single machinery; target drains and activates. The feature chooses this path conditionally, including cache separation and idle targets | **Inference:** Publish remote wakes without modifying the remote ready list; validate task lifetime and timeout races separately. **Code:** [`ttwu_queue_cond`, `__ttwu_queue_wakelist`][linux-core], [list contract][linux-llist] |
| RT push/pull and `RT_PUSH_IPI` | Push overloaded runnable RT work toward available CPUs. IRQ work serializes scans of overloaded runqueues to avoid many CPUs repeatedly taking remote locks or causing an IPI storm | **Inference:** Use event-triggered idle pull on the migration slow path. **Code:** [`rto_push_irq_work_func`, `pull_rt_task`, v6.12][linux-rt] |
| `SCHED_DEADLINE` | EDF chooses the earliest scheduling deadline; CBS enforces runtime/period bandwidth. Admission, affinity and root-domain constraints matter. GRUB reclaims inactive bandwidth when enabled | **Inference:** Add per-core reservations after the fixed-priority paths meet timing targets. **Documentation:** [algorithm, bandwidth management, reclaiming][linux-dl] |
| EEVDF | The fair class uses lag eligibility and earliest virtual deadlines to allocate service | **Inference:** Useful for general-purpose fairness; it supplies neither an application execution budget nor hard-deadline admission. **Documentation:** [EEVDF][linux-eevdf] |

### sched_ext separates policy from the scheduler core

sched_ext installs BPF scheduler operations, with dispatch queues connecting runnable
tasks to execution. The documented safety path returns tasks to the fair scheduler if
the BPF scheduler exits or encounters a detected failure. Its BPF interface is
explicitly unstable across kernel versions. These are Linux infrastructure dependencies,
not a small policy module portable directly to FreeRTOS. The final sentence is an
inference from the documented BPF and scheduler integration. [sched_ext documentation:
switching, dispatch queues and API
stability](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-ext.rst)

Jonathan Corbet’s July 16, 2026 article is a secondary report about sub-scheduler
enqueue isolation and proxy-execution compatibility proposals. Followed primary links
identify Tejun Heo’s v5 series dated July 9 and Andrea Righi’s v7 series dated July 16.
The series titles target sched_ext/for-7.3; a target branch is not proof of merging.
[LWN article](https://lwn.net/Articles/1082717/); [Heo v5 cover letter, message
20260709225041.1695495-1](https://lwn.net/ml/all/20260709225041.1695495-1-tj@kernel.org/);
[Righi v7 cover letter, message
20260716132229.61603-1](https://lwn.net/ml/all/20260716132229.61603-1-arighi@nvidia.com/).

The v5 cover letter proposes per-CPU capabilities for immediate enqueue, ordinary
enqueue and preemption, with descendants constrained by ancestor grants. It explicitly
lists task cgroup migration and tasks affined outside granted CPUs as remaining
limitations at that revision. This is proposal scope, not a claim about present defects.
[Heo v5 cover letter](https://lwn.net/ml/all/20260709225041.1695495-1-tj@kernel.org/),
capability list and “Known limitations”.

The v7 proxy series describes a waiting donor retaining scheduling eligibility while a
mutex owner executes under its scheduling context. It proposes blocked-donor admission
through `SCX_OPS_ENQ_BLOCKED`, separating the selected scheduling entity from the task
executing instructions. [Righi v7 cover
letter](https://lwn.net/ml/all/20260716132229.61603-1-arighi@nvidia.com/), background
and design.

Current-code verification: the pinned tree contains `__scx_allow_proxy_exec()`,
`scx_block_proxy_donor()`, `scx_proxy_donor_start()` and `SCX_OPS_ENQ_BLOCKED` handling.
Its Kconfig exposes `SCHED_PROXY_EXEC` under EXPERT and excludes PREEMPT_RT. Therefore
the historical article's statement that sched_ext and proxy execution are mutually
exclusive does not describe this pinned tree. This establishes source presence and
configuration conditions, not enabled distribution configurations or successful runtime
qualification. [Pinned sched_ext proxy
paths](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/kernel/sched/ext/ext.c);
[SCHED_PROXY_EXEC
configuration](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/init/Kconfig).
The pinned sub-scheduler implementation also contains capability delegation; no claim
that all historical limitations are resolved is made here. [Pinned sub-scheduler
implementation](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/kernel/sched/ext/sub.c)

Design inference: priority inheritance and proxy execution solve related
resource-blocking problems but have different accounting and task-identity contracts.
Porting donor execution would require explicit ownership of consumed runtime, migration,
lock-wait chains and admission. It is not a necessary dependency for static
heterogeneous placement. [Righi
v7](https://lwn.net/ml/all/20260716132229.61603-1-arighi@nvidia.com/); [Capacity
scheduling
requirements](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst).

### Tick suppression does not replace timed preemption

`NO_HZ_IDLE` suppresses scheduling ticks while a CPU is idle. `NO_HZ_FULL` extends
suppression to selected CPUs with a single runnable task, subject to dependencies. Linux
retains a timekeeping CPU, requires RCU callback offloading for adaptive-tick CPUs, and
documents POSIX CPU timers as a reason ticks remain active. Entry/exit accounting has
costs; full tick suppression is not equivalent to disabling all timer interrupts.
[NO_HZ: idle, adaptive ticks and RCU
implications](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/timers/no_hz.rst).

Design inference: one-shot timer programming can preserve a scheduler's next required
deadline while avoiding unnecessary periodic interrupts. The new FreeRTOS fork must
replace tick ownership with a single counter-derived epoch and separately provide local
preemption/idle wakeups; Linux configuration names do not establish those contracts in
SunstoneOS. [Local multicore
skill](../../.agents/skills/sunstone-a733-multicore/SKILL.md), “Keep the global
scheduler tick separate from local timer delivery”; [Linux adaptive
ticks](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/timers/no_hz.rst).

### PREEMPT_RT and lazy preemption address different latency costs

PREEMPT_RT replaces many spinning locks with sleeping, priority-inheritance-aware locks
and forces most interrupt work into threads. Low-level entry, scheduler and some
interrupt paths remain nonpreemptible. This reduces unpreemptible intervals; it does not
eliminate the need to measure worst-case execution and interrupt-disabled time. The last
sentence is a design inference from the stated exceptions. [PREEMPT_RT theory: locking
and threaded
interrupts](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/core-api/real-time/theory.rst).

The pinned preemption documentation defines lazy preemption as delaying a fair-class
scheduling request until return to user space or the next tick. Real-time tasks still
issue the immediate rescheduling request. Lazy preemption is therefore not PREEMPT_RT or
a different ready-queue policy. Availability depends on architecture and kernel
configuration. [Scheduler preemption: models and honoring
requests](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-preemption.rst);
[Preemption
configuration](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/kernel/Kconfig.preempt).

Design inference: importing Linux lazy-preemption terminology into FreeRTOS would hide a
different task/kernel boundary and tick contract. First specify which events demand
immediate higher-priority execution, which equal-priority work may wait, and the maximum
delay. Preserve immediate urgent-task preemption independently of capacity preferences.
[Linux lazy-preemption
contract](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-preemption.rst);
[RT capacity preference preserves
priority](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst).

## Microsecond schedulers trade kernel crossings against dedicated resources

The following results are the papers' experiments. Their different request
distributions, latency percentiles and hardware prevent a numerical ranking against the
A733. The SunstoneOS adaptation column is engineering inference.

These research results concern network services on Intel servers. The cited costs are
measurements or models from those systems; none establishes an A733 interrupt,
context-switch or scheduler cost. A service-level objective (SLO) is a latency or
throughput target. A scheduling quantum limits execution before another runnable request
can receive service.

| System and primary source | Mechanism and requirements | Measured result and experiment | Applicability to the A733 FreeRTOS runtime |
| --- | --- | --- | --- |
| [Shinjuku, NSDI 2019](https://cs.stanford.edu/~jhumphri/documents/shinjuku.pdf), §§3.1–3.3, Tables 1–2, §4.1 | Central dispatch, shared-memory messages and preemptive user contexts. Dune supplies VMX non-root execution; x86 posted interrupts remove receiver VM exits. The trusted dispatcher maps the real APIC and descriptors to avoid sender VM exits. | Table 1 reports 298 sender cycles, 1,212 receiver cycles and 1,993 total cycles for a no-op handler; the introduction reports a 110-cycle optimized context switch (Table 2 varies signal-mask and FP-state handling). The paper illustrates conversions with a 2 GHz CPU; these are distinct from the evaluation server, two E5-2658 CPUs at 2.3 GHz, 128 GB RAM and Intel 82599ES 10GbE. Scheduling experiments use quanta as short as 5 µs. | Central dispatch, short quanta and selective preemption are possible design choices. VMX, APIC and Dune are x86 mechanisms. An Arm port needs its own timer/SGI path and measured save/restore costs, including SIMD state and interrupt masking. |
| [Shenango, NSDI 2019](https://pdos.csail.mit.edu/papers/shenango:nsdi19.pdf), §§3–5, §7.3/Table 2 | One dedicated IOKernel core detects persistent packet/runqueue backlog every 5 µs and reallocates cores between application runtimes. Lightweight threads and work stealing schedule requests cooperatively; SIGUSR1 and eventfd support withdrawing and waking kernel threads. | Thread microbenchmarks report 52 ns yield ping-pong, 100 ns condition-variable ping-pong, 148 ns spawn/join and 37 ns uncontended mutex operations. Test platform: two 12-core E5-2650v4 CPUs at 2.20 GHz, first socket only, hyperthreads enabled, 64 GB RAM and Intel 82599ES 10GbE; TurboBoost, C-states and frequency scaling disabled. | Queue age, local runnable queues and explicit core grants are portable concepts. Its 5 µs allocation interval is not intra-application time slicing or an interrupt latency. FreeRTOS already owns the cores; a Linux signal/eventfd control layer would need replacement. |
| [Caladan, OSDI 2020](https://pdos.csail.mit.edu/papers/caladan:osdi20.pdf), §§5.2–5.3, Table 1, §7.3/Figure 8 | A dedicated scheduler reacts to queue delay, memory bandwidth, LLC misses and hyperthread interference. KSCHED publishes per-core commands asynchronously, batches multicast IPIs and executes target-side scheduling work. MONITOR/MWAIT wakes idle cores through cache-coherent writes. | Table 1 reports 10–20 µs decisions and typical 20 µs convergence. Rotating 22 tasks, ordinary Linux mechanisms require 43× more scheduler work and 5× greater migration latency than KSCHED. Platform: two 12-core Broadwell Xeons, first socket only, 64 GB RAM, Ubuntu 18.04/Linux 5.2.0 modified for multicast IPIs, ConnectX-5 40GbE. | Asynchronous target-side work and interference feedback are useful candidates. A55/A76 cores have no SMT siblings, so the hyperthread controller has no direct consumer. MONITOR/MWAIT and Intel monitoring controls require Arm-specific replacements and verified event ordering. |
| [Concord, SOSP 2023](https://dslab.epfl.ch/pubs/concord.pdf), §§3–4, §5.4/Table 1 | LLVM instrumentation polls a per-worker cache line; the dispatcher writes a preemption request after the quantum. Bounded local queues use Join-Bounded Shortest Queue scheduling; the dispatcher executes application work when workers are busy. Cooperation approximates precise preemption. | Across 24 Phoenix, PARSEC and Splash-2 benchmarks, instrumentation overhead averages 1.04%, maximum 6.7%. With a 5 µs target quantum, its standard deviation averages 0.29 µs, maximum 1.8 µs. Main testbed: Cloudlab c6420, reported 32 cores/64 threads with Xeon Gold 6142 at 2.60 GHz, 376 GB RAM and X710 10GbE. | Compiler-mediated safe points could serve cooperative jobs after an Arm instrumentation port. They do not bound preemption through arbitrary assembly, uninstrumented libraries or blocking kernel paths. Poll frequency, code growth and interruptible critical sections require local analysis. |
| [Skyloft, SOSP 2024](https://madsys.cs.tsinghua.edu.cn/publication/skyloft-a-general-high-efficient-scheduling-framework-in-user-space/SOSP24-Jia.pdf), §§3.2–3.3, §5.4/Table 6 | Intel Sapphire Rapids User Interrupts (UINTR), modified Linux 6.0 UINTR support and a kernel module. SENDUIPI supports remote preemption. Timer delegation sets UINV to the LAPIC timer vector and primes the descriptor's pending bitmap using self-SENDUIPI with notification suppressed; the handler primes it again. Cross-application switches still enter the kernel. | On dual 24-core Xeon Gold 5418Y CPUs at 2.0 GHz, Table 6 reports same-socket user-IPI send/receive/delivery of 167/661/1,211 cycles. User timer handling costs 642 cycles, versus 5,057 for setitimer. Receiver is a no-op handler; send/receive work and delivery latency are separate metrics, not additive stages. TurboBoost, idle states and frequency scaling are disabled. | Its measured mechanism requires Intel UINTR and LAPIC behavior. Cortex-A55/A76 and GIC-600 do not provide these instructions or descriptors. Per-core timer preemption is independently plausible on FreeRTOS, but Skyloft's user-mode delivery method and costs do not transfer. |

Concord's polling cost is also workload-dependent. Its approximate two-cycle cached poll
and approximately 150-cycle notification coherence miss (§3.1) explain an x86
implementation, not a portable cycle budget. Its separate UINTR comparison uses a
reported 192-core Xeon Platinum 8648 Sapphire Rapids server (§6), rather than the main
evaluation machine.

**Inference:** A counter-derived compatibility clock and per-core compare ownership must
replace the current CPU0 tick together. Local timers must not independently increment
the existing global tick. Context preservation and interrupt exclusion remain part of
every measured preemption cost. [Current timer
contract](../../.agents/skills/sunstone-a733-multicore/SKILL.md)

## ghOSt techniques transfer selectively to an in-kernel real-time scheduler

The [SOSP 2021 paper][ghost-paper] evaluates userspace agents controlling a Linux
scheduling class. Its local mode has one active agent per CPU; central mode has one
active agent controlling several CPUs. NR means the paper reports no isolated effect for
a technique. All applicability judgments below are **SunstoneOS inferences**, not claims
made by ghOSt's authors.

| Technique | What it does | Measured effect in the paper | Applies to SunstoneOS | Why | Where it lands in our design |
| --- | --- | --- | --- | --- | --- |
| Policy separated from mechanism | Kernel checks decisions; agents choose runnable tasks (§§2–3) | Isolated gain NR | Yes, as separation | A kernel policy boundary preserves local dispatch and existing task APIs; userspace delegation adds a hop | Fixed-priority mechanism first; EDF/CBS policy later |
| Per-CPU and centralized agents | Local or centralized decisions (§3.3) | Table 3: message delivery 725 ns local, 265 ns central; central agent already spins | Partly | Per-core ownership transfers directly; separate agent tasks do not | CPU-owned runqueues and direct caller/ISR paths |
| Shared-memory kernel-to-agent messages | Queue state changes for agents (§3.1) | Transport-only gain NR | Partly | Owner inboxes transfer remote work without sharing ready lists; they need bounded capacity and lifetime rules | Intrusive remote wake list and separate control mailbox |
| Status words with sequence numbers | Observe state without taking the writer's lock (§3.1) | Isolated gain NR | Yes | Remote observations can guide conditional SGIs; publication versus sleeping still needs a memory-ordering proof | Sequence-tagged per-core dispatch status |
| Sequence-validated transactions | Reject decisions against obsolete task/agent state (§3.2) | Local scheduling 888 ns versus CFS context switch 599 ns; isolated sequence cost NR | Partly | Validation detects stale placement; owner handoff and off-CPU acknowledgment establish migration safety | Epoch-tagged owner commands and acknowledged migration |
| Batched commits and synchronized groups | Amortize commits and interrupt requests; synchronized groups have a separate interface (§3.2) | Table 3: 1 versus 10 targets gives 1.50 versus 2.52 million theoretical dispatches/s; end-to-end time rises from 1.772 to 5.688 µs | Partly | Batch bookkeeping and repeated requests per owner. A733 Aff1 layout prevents one explicit target-list write selecting arbitrary cores | Per-target reschedule coalescing; group SGIs only for encodable affinity groups |
| BPF hooks while agents decide | Kernel pick-next hook can fill otherwise idle execution (§3.2, §5) | Isolated gain NR; §5 proposes further ring-buffer gap filling | Partly | A bounded local pick provides available work directly; adding BPF has no demonstrated consumer | Local bitmap/CLZ selection on every scheduler entry |
| Spinning global agent on a dedicated core | Continuously observes messages (§3.3) | Central delivery 265 ns; energy cost NR | No by default | Consumes CPU capacity and prevents WFI; benefit needs a request-dispatch workload | Optional later experiment with explicit core and power budget |
| Policy hot upgrade and failed-agent fallback | Discover kernel state after replacement; watchdog can return tasks to CFS (§3.4) | Isolated pause and gain NR | No immediate requirement | A single RTOS image has no separately isolated userspace agent to restart | Keep policy invariants explicit; no hot-upgrade machinery |
| Shinjuku-style preemption | Central FIFO with 30 µs slices (§4.2) | Within 5% of Shinjuku saturation throughput; slightly higher loaded tails in the mixed-duration experiment | Partly | Motivates bounded preemption and mixed short/long stress; its selected slice is workload-specific | One-shot local slice only for equal-priority competitors |

Table 3 uses dual 2 GHz Xeon Platinum 8173M processors with 28 physical cores and two
threads per core. Section 4.2 uses a different Xeon E5-2658 setup. Its mixed-duration
workload combines short requests with rare 10 ms work. These conditions do not establish
A733 instruction or energy costs. **Paper:** [§§4.1–4.2, Table 3, Figure
6][ghost-paper].

The public userspace revision is `9ca0a1fb6ed88f0c4b0b40a5a35502938efa567f`; the kernel
revision is `edd5f9490d82df24c16f90a62f7be05c6c389867`. Both postdate the paper and
corroborate mechanisms rather than its benchmark binaries. **Code:** [status-word
reads][ghost-status], [transaction validation][ghost-valid], [ordinary versus
synchronized commits][ghost-commits].

The kernel builds a reschedule mask during commits. Its x86 path can attempt IPI-free
wakeup; its non-x86 path sends target interrupts individually. **Code:**
[`ghost_send_reschedule` and commit handling][ghost-ipi]. On A733, each core's distinct
Aff1 identifies a separate higher-affinity tuple. SGI target-list bits select Aff0
within that tuple. [A733 routing
contract](../../.agents/skills/sunstone-a733-multicore/SKILL.md)

**Inference:** A status read must be paired with a protocol that handles the target
lowering its priority or entering WFI concurrently. A sequence comparison cannot
establish that a migrating task has stopped executing. Neither ordinary batching nor
synchronized group commits replace the fork's ownership proof.

## Heterogeneous placement needs measured capacity and explicit demand

### Capacity describes a CPU; utilization describes demand

Linux models original CPU capacity as work per clock multiplied by maximum frequency,
normalized against the fastest CPU. Available fair-class capacity also deducts competing
activity such as interrupts. A single scalar cannot express every workload's relative
integer, floating-point or branch performance. [Linux capacity documentation
§§1–5](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst)

The Arm capacity binding accepts normalized `capacity-dmips-mhz` values for every CPU or
none. Its guidance derives these values from Dhrystone throughput divided by measurement
frequency; boot-time topology combines them with maximum rates. A55:A76 capacity
therefore cannot be inferred from the MHz ratio alone. The final sentence is an
inference from the binding and capacity formula. [CPU capacity binding
§§2–3](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/devicetree/bindings/cpu/cpu-capacity.txt)
[Linux capacity documentation
§§1–5](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst)

Linux normalizes observed task duty cycle by current/maximum frequency and CPU/maximum
capacity. This estimates demand as if execution had occurred on the fastest CPU at
maximum frequency. Frequency invariance can use frequency-sensitive hardware counters,
such as Arm AMU, or known cpufreq transitions. PMU instruction and cache-miss counters
are not a necessary input to this baseline model. [Linux capacity documentation
§§1–5](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst)

Per-Entity Load Tracking (PELT) uses exponentially decayed execution/runnable history.
Linux describes 1024-microsecond periods and decay that halves the contribution after 32
periods, conventionally described as about 32 ms. Execution time, runnable-state
transitions and a consistent clock are its basic inputs; it does not classify the
instruction mix. The final sentence follows from the PELT update interfaces and
documented signals. [Linux schedutil: PELT, frequency/CPU
invariance](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/schedutil.rst)
[PELT implementation: ___update_load_sum and
__update_load_avg_se](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/kernel/sched/pelt.c)

The sampled Android WALT implementation tracks demand and runnable contributions in
windows, with a default nominal 20 ms window rounded to the tick period. It scales
execution time using current CPU capacity and transfers task contributions during
migration. This is a pinned historical Android implementation, not evidence that WALT is
current upstream Linux policy. [Android WALT, commit
d7311986f7fc1b7e5df44c4556e190ee24733370](https://android.googlesource.com/kernel/common/+/d7311986f7fc1b7e5df44c4556e190ee24733370/kernel/sched/walt.c),
symbols `walt_ravg_window`, `scale_exec_time`, `walt_update_task_ravg`,
`walt_fixup_busy_time`.

### Placement requires topology and a correction path

Linux marks scheduler domains that span differing capacities with `SD_ASYM_CPUCAPACITY`
and the lowest domain spanning all capacities with `SD_ASYM_CPUCAPACITY_FULL`. Wakeup
placement compares estimated demand with CPU capacity. A continuously runnable task that
outgrows a small CPU may receive no new wakeup; misfit load balancing provides a
migration opportunity toward higher capacity. [Linux capacity documentation
§§1–5](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst)

The real-time class retains priority constraints when testing capacity. Its documented
wakeup search tries to satisfy the task's minimum utilization clamp; if candidates
cannot satisfy it, strict priority wins. This is a preference within eligible CPUs, not
a reservation of a big core. [Linux capacity documentation
§§1–5](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst)

Utilization clamps express performance requests: a lower bound can avoid slow ramp-up,
and an upper bound can limit requested performance. Clamps affect supported placement
and schedutil frequency choices. They do not replace affinity, guarantee a deadline, or
prove a fixed P/E mapping. The last sentence is the design implication of their
documented hint semantics. [Linux utilization clamp
§§1–3](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-util-clamp.rst)

Energy Aware Scheduling (EAS) compares estimated energy for candidate placement using an
Energy Model and utilization. A performance domain groups CPUs sharing a
performance-state control relationship; scheduler topology and energy-domain boundaries
need not coincide. When a root domain is overutilized, EAS gives way to load balancing.
EAS requires credible energy costs and topology; capacity-aware placement alone does not
require an Energy Model. [Linux EAS: topology, energy model and
overutilization](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-energy.rst)
[Linux Energy Model: performance
domains](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/power/energy-model.rst)

### Hardware feedback adds information beyond time accounting

Intel Turbo Boost Max Technology (ITMT) exposes CPU priorities so the scheduler can
prefer cores with higher maximum turbo rates. This source describes preferred-core
ranking, not per-task instruction classification. [Linux ITMT implementation and
introductory
contract](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/arch/x86/kernel/itmt.c)

Intel describes Thread Director as hardware that monitors thread instruction mix and
core state and supplies feedback for OS placement decisions. This is a different
dependency from ordinary execution-time accounting. It is not evidence that A733 has
equivalent hardware or that Linux implements every Intel capability. [Intel Thread
Director, article 000097053, reviewed
2023-11-20](https://www.intel.com/content/www/us/en/support/articles/000097053.html)

Linux documents HFI performance and energy-efficiency capability values independently in
the range 0–255. A package table can change with operating conditions and generates
update interrupts. Its documented Linux driver relays updates through thermal
notifications; that description alone does not establish a complete Thread Director
placement policy. [Linux HFI: overview, interface and implementation
details](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/arch/x86/intel-hfi.rst)

Apple says placement considers application hints, observed workload and system state.
QoS classes describe the importance and latency sensitivity of work; background tasks
are more likely to use efficiency cores. Apple also says a thread can execute on both
core types over time. Thus “background always means E core” and “user initiated always
means P core” overstate the documented contract. [Apple: Optimize for Apple silicon with
performance and efficiency cores](https://developer.apple.com/news/?id=vk3m204o), “Use
Quality of Service classes”; [Apple: Tuning your code’s performance for Apple
silicon](https://developer.apple.com/documentation/apple-silicon/tuning-your-code-s-performance-for-apple-silicon),
QoS and parallel execution.

The HASS research scheduler ranks the relative benefit of assigning threads to faster
cores. Its static variant uses offline reuse-distance profiles; its dynamic variant uses
hardware cache-miss counters. The evaluated heterogeneity varies clock frequency, and
the authors explicitly exclude exploration of other microarchitectural differences. Its
results therefore do not measure A55/A76 scheduling. [Saez et al., Journal of Parallel
and Distributed Computing 71 (2011),
114–131](https://people.ece.ubc.ca/sasha/papers/JPDC-2011.pdf), introduction and
architectural-signature framework.

CAMP combines estimated fast-core benefit with thread-level parallelism, so sequential
phases and useful parallel work affect allocation. Its inexpensive estimator uses online
last-level-cache miss measurements rather than sampling every thread on both core types.
The OpenSolaris evaluation emulates asymmetry with per-core DVFS; this limits transfer
to genuine A55/A76 microarchitectural differences. [Saez et al., A Comprehensive
Scheduler for Asymmetric Multicore Systems, EuroSys
2010](https://people.ece.ubc.ca/sasha/papers/eurosys163-saez.pdf), introduction, speedup
estimator and evaluation method.

### A733 can start with measured capacity and existing clock ownership

The local top-OPP design identifies six A55 CPUs in CPU-L, two A76 CPUs in CPU-B, and a
separate shared DSU domain. For bin vf0400 it records default top points of 1794, 2002
and 1274 MHz respectively. These values are clock/voltage policy, not a measured
throughput ratio. [Local top-OPP design](../designs/cpu-top-opp.md), “The vendor’s top
operating points”; local source revision `0f2e3f16c3c5e4cf51081ce68c512e2190fef45c`.

The current clock interface supplies decoded domain rates and timing samples; the OS CPU
service owns the driver and calls the performance service from the thermal task. The
CPU-performance branch documents HIGH as the default and thermal policy as overriding
mode selection. Live rate switching and recovery remain physically unqualified there.
[Clock interface](../../include/a733_cpu_clock.h), `a733_cpu_clock_report` and
`a733_cpu_clock_timing`; [OS owner](../../apps/os/cpu.c), `os_cpu_clock_step`;
[CPU-performance documentation at its pinned
branch](https://github.com/pedapudi/sunstoneos/blob/2cb455da/docs/performance-modes.md)
and [resident service design at that
branch](https://github.com/pedapudi/sunstoneos/blob/2cb455da/docs/designs/resident-performance.md),
branch revision `2cb455da`.

Design inference: a scheduler topology query should consume core identity,
admitted/online state, cluster membership, calibrated maximum capacity and the clock
owner's current effective rate. Keep requested, applied and unknown transition states
distinct. A worker-pool split can use capacity weights for initial chunks and a shared
remainder queue to avoid leaving fast workers idle. Neither interface needs authority to
change clocks, voltage or thermal policy. This is a proposal derived from the preceding
capacity model and local owner contract, not an existing API or physical result. [Linux
capacity documentation
§§1–5](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst)
[Local owner](../../apps/os/cpu.c).

Design inference: establish task execution/runnable accounting and scheduling
correctness before adding PMU workload classification or EAS. A fixed-top-rate policy
can initially use calibrated static capacities. Rate changes and throttling require
accounting to observe the effective rate or invalidate its estimates. Absolute deadlines
and strict affinities remain separate contracts from performance hints. [Linux capacity
documentation
§§1–5](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-capacity.rst)
[Linux utilization clamp
§§1–3](https://github.com/torvalds/linux/blob/46fec1c3029b552ec63b2d01af9d43a386f043ea/Documentation/scheduler/sched-util-clamp.rst)

## SunstoneOS priorities follow contention, latency and idle-power evidence

The ranking below predicts which changes address the recorded failure modes. It does not
claim a measured improvement. Acceptance uses the same latency workloads and records
initialization separately from idle and loaded operation. [Board evidence and
boundaries](../status/latency.md)

| Rank | Chosen change | Expected effect and test | Power cost and guard |
| ---: | --- | --- | --- |
| 1 | Owner-only per-core ready queues with local O(1) fixed-priority selection | Remove both shared kernel-lock acquisitions from local wake and context switch. Directly targets 178 µs lock waits and 133 µs loaded dispatch maxima | Local state avoids coherence traffic; no permanent agent core |
| 2 | Queued remote wakeups, sequence-tagged per-core status and a proven sleep/preemption handshake | Reduce remote ready-list traffic and redundant SGIs. Require loaded ready-to-running p99 below 10 µs and workload mask/wait maxima below 25 µs | Send an SGI for urgent work or sleeping targets; no polling for deferred work |
| 3 | Short per-object FIFO metadata locks and owner-routed timeout/control operations | Prevent unrelated queues and notifications from serializing. Measure per-lock service, wait and masking, including unlock tails | WFE/SEV reduces polling traffic; bounds still include waiting and interrupt interference |
| 4 | Topology-based capacity placement and capacity-proportional application splitting | Give heavy and latency-critical work the fastest eligible capacity. Measure a fixed kernel on each CPU class and equal versus weighted mimus splits | Race to idle at existing vendor top points; energy-aware background placement needs an actual energy model |
| 5 | One-shot per-core timers and tick-count conversion from the generic counter | Remove CPU0's idle periodic tick and supply sub-tick deadlines without increasing tick frequency | Zero scheduler timer wakeups without a deadline or local slice competitor; preserve counter time across WFI |
| 6 | Sequence-validated migration and grouped reschedule requests | Make remote placement a retryable slow path; batch SGIs only where GIC affinity encoding permits | Bound migration frequency and private-cache refill cost; never delay urgent dispatch to accumulate a batch |
| 7 | Per-core EDF/CBS reservations | Isolate periodic budgets after the mechanism is qualified; account for blocking and higher-priority work in admission | Charge actual execution and use one-shot replenishment; no periodic accounting task |
| 8 | Dedicated centralized microsecond dispatcher | Research-only opt-in for a demonstrated request workload; no default global agent | One spinning CPU is a substantial capacity and idle-power expense on eight CPUs |

The partitioned design must resolve wake-versus-timeout ownership, notification arrival
during blocking, task deletion with queued references, migration epochs and lost wakeups
around WFI. Sequence validation alone resolves none of these lifetime and
memory-ordering questions. These are explicit proof and stress-test obligations inferred
from the proposed owner-only architecture.

The board report needs p50, p99 and maximum ready-to-running and IRQ-to-task latency,
each lock's hold and wait, interrupt masking, context-switch cost and per-core idle
wakeups. It also needs the measured A76/A55 capacity ratio, weighted-versus-equal mimus
CPU time and exact recognition output. [Validation and real-time
requirements](../../AGENTS.md), [the global-lock kernel's priorities and timekeeping](scheduler-realtime.md).

## References and verification anchors

An independent accuracy review passes on 2026-11-01. It checks the cited paper
measurements, protocol assumptions, Linux code scope, ghOSt applicability and
A733 topology limits. It verifies downloaded-source hashes and the five local
Git-revision hashes. The review establishes source accuracy, not board timing.

The links beside each claim identify the checked paper section or code symbol. The
source inventory pins downloaded bytes. Linux v6.12 anchors describe a stable
implementation; newer features use an explicitly named separate commit. The ghOSt paper
is also indexed at [Google Research](https://research.google/pubs/pub50833/).

[dissertation]: https://www.cs.unc.edu/~anderson/diss/bbbdiss.pdf
[edf]: https://people.mpi-sws.org/~bbb/papers/pdf/rtss10b.pdf
[locking]: https://arxiv.org/abs/1909.09600
[omlp]: https://people.mpi-sws.org/~bbb/papers/pdf/daes12.pdf
[rtems-cluster]: https://docs.rtems.org/docs/main/c-user/config/scheduler-clustered.html
[rtems-key]: https://docs.rtems.org/docs/main/c-user/key_concepts.html#multiprocessor-resource-sharing-protocol-mrsp
[autosar]: https://www.autosar.org/fileadmin/standards/R24-11/CP/AUTOSAR_CP_SWS_OS.pdf
[linux-core]: https://github.com/torvalds/linux/blob/v6.12/kernel/sched/core.c#L3762
[linux-llist]: https://github.com/torvalds/linux/blob/v6.12/include/linux/llist.h
[linux-rt]: https://github.com/torvalds/linux/blob/v6.12/kernel/sched/rt.c#L2111
[linux-dl]: https://www.kernel.org/doc/html/latest/scheduler/sched-deadline.html
[linux-eevdf]: https://docs.kernel.org/6.12/scheduler/sched-eevdf.html
[ghost-paper]: https://storage.googleapis.com/gweb-research2023-media/pubtools/6413.pdf
[ghost-status]: https://github.com/google/ghost-userspace/blob/9ca0a1fb6ed88f0c4b0b40a5a35502938efa567f/lib/ghost.h#L411
[ghost-valid]: https://github.com/google/ghost-kernel/blob/edd5f9490d82df24c16f90a62f7be05c6c389867/kernel/sched/ghost.c#L1268
[ghost-commits]: https://github.com/google/ghost-userspace/blob/9ca0a1fb6ed88f0c4b0b40a5a35502938efa567f/lib/enclave.cc#L502
[ghost-ipi]: https://github.com/google/ghost-kernel/blob/edd5f9490d82df24c16f90a62f7be05c6c389867/kernel/sched/ghost.c#L6484
