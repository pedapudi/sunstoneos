# Scheduler priorities, timekeeping and the global-lock control kernel

Status: **implemented** for the priority map, the single periodic tick, bounded
time-slice traversal in the global-lock kernel and the sub-tick deadline wait;
**open** for tickless idle. The kernel that SMP images build by default is
[the partitioned fork](scheduler-partitioned.md); the global-lock kernel described
here is its control (`-DSUNSTONE_PARTITIONED_SCHEDULER=OFF`). The reader is assumed
to know interrupt handling, task priorities and mutual exclusion. Qualification
is in [scheduler status](../status/scheduler.md) and the measurement boundaries
are in [latency status](../status/latency.md).

## Service priorities fit within eight levels

Larger numbers preempt smaller numbers on an eligible core. Application manifests
supply their priority; the table describes the services and reserves the free
levels. It does not enforce an application policy.

| Priority | Use or reservation | Source |
| ---: | --- | --- |
| 0 | Kernel idle tasks, one pinned to each core | [cached configuration](../../platform/a733-smp-cached/FreeRTOSConfig.h#L13) |
| 1 | Diagnostic memory/background load; available for background application work | [latency workload](../../apps/latency/workload.c#L171) |
| 2 | CPU latency application's main task | [latency manifest](../../apps/latency/manifest.json#L4) |
| 3 | PMIC owner, NPU application client and recorded mimus board application | [board tasks](../../apps/board/main.c#L339), [NPU tasks](../../apps/os/npu.c#L294), external application's manifest |
| 4 | USB, radio, watchdog supervision, thermal, audio, Bluetooth, GPU and DMA service work | [board tasks](../../apps/board/main.c#L339), [thermal](../../apps/os/thermal.c#L319), [GPU](../../apps/os/gpu-runtime.h#L4), [DMA](../../apps/os/dma.c#L58) |
| 5 | Type-C and power owners | [board tasks](../../apps/board/main.c#L339) |
| 6 | Available for a bounded urgent service with a demonstrated need | [priority count](../../include/FreeRTOSConfig.h#L38) |
| 7 | Returning CPU terminal tasks and highest-priority latency probes | [CPU return](../../apps/os/cpu-return.c#L60), [latency workload](../../apps/latency/workload.c#L171) |

Several services share priority four; their execution budgets and affinities
need combined-load evidence. Default OS affinity covers the six A55 cores, and
explicit placements admit the two A76 cores. The latency workload pins its probe
and memory task to each physical core, and CPU0's timer wakes remote probes through
production SGIs.

The pinned upstream `xTaskPriorityInherit` raises a contended mutex holder and
reschedules an eligible core if that ready holder is not running. Ordinary release
restores base priority after the holder releases its last mutex, and timeout
disinheritance adjusts a holder with one outstanding mutex. These are the
simplified upstream semantics; full transitive inheritance and a priority ceiling
protocol need different mechanisms, so bounded response paths avoid nested mutex
dependencies. Cross-core priority inversion with a high-priority waiter and
medium-priority interferers has no isolated physical test.

## The global tick has one owner

[The configuration](../../include/FreeRTOSConfig.h#L35) selects preemption, time
slicing, eight priorities and a 1 kHz tick. [CPU0 programs the physical generic
timer](../../platform/a733-smp-cached/platform.c#L122), and only [CPU0 private
peripheral interrupt 30 (PPI30) advances global time](../../platform/a733-smp-cached/platform.c#L146).
Secondaries receive scheduler software-generated interrupts (SGIs) and [execute
wait-for-interrupt (WFI) when idle](../../include/a733_cpu_power.h#L23). The
idle-priority time-slice rule sends no SGI when only the eight kernel idle tasks
are ready, so the system has one periodic scheduler timer.

The [late-tick path](../../platform/a733-smp-cached/platform.c#L133) processes at
most three periods per IRQ; further overdue periods keep the timer's level
assertion, and each acknowledgment advances the original compare phase. A higher tick
rate would add work and lock traffic, so the tick stays at 1 kHz and sub-tick
deadlines use a separate interrupt-driven path (below).

## Real-time cores may need to exclude streaming work

Status: **open decision** for the robotics target, which runs camera and lidar
pipelines beside control loops.

Streaming memory traffic evicts the scheduler's working set from the private
caches and the shared L3 between ticks. In [the latency cause
measurement](scheduler-latency-measurement.md#cause-capture-separates-release-barrier-and-line-transfer-costs)
(evidence entry `1794504237145636168-9aa4ebd9`), each core of the latency fixture
streams 1 MiB in the busy phase. A busy CPU0 tick then averages 314,510 cycles
with 220 attributable L3 refills, about 1,268 cycles per refill. The same tick in
the idle phase, with no streaming on any core, averages 36,541 cycles (about
20 µs at 1794 MHz) with one L3 refill. Both figures include the measurement's own
hooks.

Two mechanisms can keep the working set resident:

- **Core isolation**, a scheduler policy: real-time cores run no streaming work,
  and bulk producers and consumers run only on the other cores. A real-time core
  then evicts only its own lines. The idle-phase tick bounds what isolation alone
  can give: about 36.5k cycles with the hooks. Streaming on other cores still
  evicts shared L3 lines, so the bound holds only while the real-time core's
  working set fits in its private L1 and 128 KiB L2. Device DMA into memory that
  a real-time core reads is a separate question.
- **L3 partitioning** by DSU scheme ID: a reserved way group keeps the
  real-time cores' lines in L3 whatever the other cores stream. It takes 256 KiB
  (25 percent) of the L3 from the other cores per group. EL3 code writes the
  cluster partition register and each core's scheme register, or grants EL1 that
  access. It does not protect a real-time core from its
  own streaming unless the scheme changes on each context switch.

Isolation alone was measured with streaming confined to CPUs 4-7 and CPUs 0-3
idle between samples (build option `SUNSTONE_LATENCY_STREAM_MASK=0xf0`, evidence
entry `1794597393456307577-46a485eb`; the footprint cuts and the owner SGI fix are
in both images). Against all eight cores streaming (entry
`1794508626388886966-ae6d2457`):

| Busy-phase mean over complete ticks | All cores stream | CPUs 4-7 stream | Idle phase |
| --- | ---: | ---: | ---: |
| Whole tick, cycles | 212,455 | 76,410 | 33,304 |
| Whole tick, L3 refills | 140.1 | 26.5 | 0.7 |
| CPU0's own release, cycles | 61,627 | 7,839 | 2,227 |
| Releases to CPUs 1-3, cycles each | 10,269-30,250 | 4,947-7,519 | 2,616-2,680 |
| Releases to CPUs 4-7, cycles each | 7,819-11,970 | 4,250-8,671 | 2,636-2,672 |
| Outside the releases and scans, cycles | 46,703 | 23,096 | 10,840 |
| CPU0's longest busy wake | 228 µs | 93.1 µs | 28.8 µs (idle wake) |

Isolation removes about three quarters of the busy tick's added cost but does not
reach the idle tick. About 26 lines per tick that the idle tick finds in the
shared L3 come from DRAM while other cores stream, at about 1,670 cycles each
beyond the idle cost. Those lines are spread over every segment, including the
releases to idle cores. An L3 partition should keep them; the partition
measurement below removes about 15,900 of the remaining 43,000 cycles.

The stream mask is a measurement build option. In a product the real-time core
set should come from the board and application declarations, so that the
scheduler places streaming work away from the declared cores; it should not be a
build option.

### An L3 partition would keep the real-time cores' lines in the shared cache

Status: **measured**, one cause-build run on port 3-3 (evidence entry
`1794605350026314973-495cffbe`); the prediction missed at the whole tick's L3
refills. The build option `SUNSTONE_LATENCY_L3_PARTITION` enables the writes;
no other image writes these registers.

The A733's DynamIQ Shared Unit (DSU) reports `CLUSTERIDR_EL1` 0x41, revision
r4p1. Its L3 holds 1 MiB in 16 ways of 1024 sets with 64-byte lines (`CCSIDR`
for level 3 reads 0x707fe07a), so one way group of four ways holds 256 KiB. The
register layout below comes from the DSU Technical Reference Manual for r4p1
(Arm 100453_0401, issue 05, sections 6.3, 11.12, 11.15 and 11.18); it matches
the r3p0 manual (100453_0300) bit for bit. The cause build's capture already
reads these registers at EL3 on every core without a trap; the partition
registers and `ACTLR_EL3` read 0.

The partition registers:

- **`CLUSTERPARTCR_EL1`** (`S3_0_C15_C4_3`) is one 32-bit register for the
  cluster. Bit 4 × *id* + *g* marks way group *g* private to scheme ID *id*.
  Several schemes that mark the same group share it, and a group that no scheme
  marks is shared by all schemes. The register changes only victim selection: a
  core allocates into its scheme's private groups and the shared groups, and a
  lookup still hits in any way. Every bit resets to 0.
- **`CLUSTERTHREADSID_EL1`** (`S3_0_C15_C4_0`) holds a core's scheme ID in
  bits [2:0], one copy per core, reset 0.
- **`CLUSTERACPSID_EL1` and `CLUSTERSTASHSID_EL1`** give the scheme IDs of
  accelerator coherency port (ACP) requests and cache stashes; both reset to 0.
- **`CLUSTERPWRSTAT_EL1`** (`S3_0_C15_C3_7`) bits [7:4] show which L3 portions
  are powered: 0b1111 means all 16 ways. A powered-down portion removes its way
  group from every partition.
- **`ACTLR_EL3` bits 11 (SMEN) and 10 (TSIDEN)** grant EL1 write access to the
  partition registers and to `CLUSTERTHREADSID_EL1`. A Non-secure EL1 write also
  needs the same bit in `ACTLR_EL2`; without both grants the write traps to EL3.
  SunstoneOS runs at Non-secure EL1 with EL2 unused. The vendor monitor sets
  `ACTLR_EL3` to 0xc00, both grants, in SMC `0x8000ff50`
  ([vendor monitor](../vendor-firmware/tf-a-monitor.md)).

The measurement configures two schemes:

- `CLUSTERPARTCR_EL1` = 0x1E. Bits 1 to 3 make way groups 1 to 3 private to
  scheme 0, and bit 4 makes group 0 private to scheme 1. No group is shared.
- CPUs 0 to 3, the real-time set, write 1 to `CLUSTERTHREADSID_EL1`; CPUs 4 to 7
  keep 0. ACP requests and stashes keep scheme 0.
- The real-time cores then allocate only into group 0 (256 KiB, four ways per
  set). The streaming cores, ACP requests and stashes allocate only into groups
  1 to 3 (768 KiB). Streaming cannot evict a line that a real-time core
  allocated.
- `ACTLR_EL3` stays 0, so EL1 code cannot change the partition. An EL1 write
  traps to the image's EL3 vectors, which end the run as a fault.

The image's own EL3 code writes the registers, in `local_boot` in
`platform/a733-smp-cached/continue.c`, under a cause-build option. The runner
cannot do it: it reaches the board through FEL on CPU0 only, and
`CLUSTERTHREADSID_EL1` is a per-core system register with no memory-mapped
alias, so each core must write its own. The sequence:

1. Each core captures the registers before any write, now including
   `CLUSTERPWRSTAT_EL1`, the ACP and stash scheme IDs and
   `CLUSTERTHREADSIDOVR_EL1`. It then writes its `CLUSTERTHREADSID_EL1`, issues
   an ISB and reads it back; a mismatch clears the register and stops the boot
   with detail 0x738.
2. After all eight cores pass the barrier that follows the capture, CPU0 writes
   `CLUSTERPARTCR_EL1`, issues an ISB and reads the register back; a mismatch
   clears it and stops the boot with detail 0x739. Cores that reach EL1 first
   run without a partition for a few microseconds of startup, before any
   measurement.
3. The decoder reports the boot capture and refuses a partition record whose
   readback differs from what was written, which is 0x1E from CPU0 and the
   scheme ID from each core. It treats `CLUSTERTHREADSID_EL1` as a per-core
   value.

Two resets undo the write. Every entry to EL3 ends this image, whether a normal
return, an EL1 fault or an EL3 exception, and the shared EL3 vector entry
(`a733_cached_el3_fault` in `continue.S`) writes 0 to `CLUSTERPARTCR_EL1` and to
the entering core's `CLUSTERTHREADSID_EL1` before anything else. The image
makes no other SMC. Before every run, the runner's watchdog reset returns the
board to FEL; that is a system reset, which should return the DSU's partition
bits to 0. A hang that never reaches EL3 relies on the watchdog reset alone.
The boot capture of the next cause-build run reads the registers before any
write and must read 0 on every core. Because the EL3 clear runs first, that
reading shows that no partition persisted; it does not show which of the two
resets cleared it.

The prediction uses the isolation run (entry `1794597393456307577-46a485eb`,
streaming on CPUs 4 to 7) as its baseline, with the same image apart from the
partition. Lines that CPUs 0 to 3 allocate stay in group 0, so the segments
whose lines those cores own cost what they cost in the idle phase. Lines last
held by CPUs 4 to 7 enter the L3 under scheme 0 when those cores evict them, so
streaming still evicts them, and the releases to CPUs 4 to 7 keep their
isolation cost.

| Busy-phase mean over complete ticks | Isolation | Idle phase | Predicted with the partition | Measured with the partition |
| --- | ---: | ---: | ---: | ---: |
| Whole tick, cycles | 76,410 | 33,304 | 45,000-62,000 | 60,519 |
| Whole tick, L3 refills | 26.5 | 0.7 | 8-17 | 20.3 (miss) |
| CPU0's own release, cycles | 7,839 (3.6 refills) | 2,228 | 1,800-4,000, at most 1 refill | 6,624 (2.7 refills; miss) |
| Releases to CPUs 1-3, cycles each | 4,948-7,519 | 2,616-2,680 | 2,000-4,000, at most 1 refill each | 3,214-3,959 (0.1-0.5 refills) |
| Releases to CPUs 4-7, cycles summed | 24,866 (11.1 refills) | 10,617 | 18,500-31,000 | 24,571 (11.4 refills) |
| Outside the releases and scans, cycles | 23,096 (5.4 refills) | 10,840 | 10,000-23,500 | 16,488 (4.3 refills) |
| Deadline scans, cycles summed | 1,952 | 1,669 | 1,000-3,000 | 2,288 |
| Idle tick, cycles | 33,304 | 33,304 | 28,000-38,000 | 33,232 |
| CPU0's longest busy wake | 93.1 µs | 28.8 µs (idle wake) | at most 85 µs | 69.5 µs |
| 1 MiB sweeps on CPUs 4-7 in the busy phase, summed | 497 | none | 470-505 | 479 (107, 101, 128, 143) |

The run started from reset values: every core read `CLUSTERPARTCR_EL1` 0, scheme
ID 0 and `CLUSTERPWRSTAT_EL1` 0xf0 (all 16 ways powered) before the write. CPU0
read back 0x1E, and CPUs 0 to 3 read back scheme ID 1. The partition kept the
lines that CPUs 1 to 3 own: their releases fell to within about 1,300 cycles of
the idle cost, and the outside segment fell by 6,600 cycles. CPU0's own release
did not follow: it kept 6,624 cycles and 2.7 L3 refills per tick, which meets
the refuting outcome below for that segment. Those lines are either lines a
streaming core last held, which the streaming scheme still evicts, or lines
lost by a path other than L3 eviction; this run does not separate the two. The
streaming cores completed 3.6 percent fewer sweeps.

The point estimate is about 47,800 cycles. The idle-phase costs of CPU0's own
release (2,228 cycles), the releases to CPUs 1 to 3 (7,949) and the outside
segment (10,840), with the scans at their isolation cost (1,952), sum to 22,969.
The releases to CPUs 4 to 7 at their isolation cost add 24,866. The upper bound
assumes that all 5.4 refills of the outside segment are lines that CPUs 4 to 7
last held, which keeps that segment at its isolation cost and the tick at about
60,100 cycles. The tick saving of about 28,600 cycles is 16 µs at 1794 MHz, so
CPU0's longest busy wake should fall from 93.1 µs to about 77 µs.

Refuting outcomes, checked in this order:

- A boot capture with a nonzero `CLUSTERPARTCR_EL1` or scheme ID, or with
  `CLUSTERPWRSTAT_EL1` bits [7:4] other than 0b1111, means the run did not start
  from a reset, fully powered L3; the run says nothing about the partition.
- A readback other than 0x1E, or a scheme ID other than 1 on CPUs 0 to 3, means
  the register does not hold what was written (the boot stops with detail 0x738
  or 0x739); the run says nothing about the partition.
- A busy tick of 70,000 cycles or more, or a CPU0 release of 6,000 cycles or
  more, means the partition does not keep CPU0's lines. The remaining refills
  then come from lines other cores own or from DRAM contention, not from L3
  evictions of real-time lines.
- Releases to CPUs 4 to 7 near their idle cost would mean the partition also
  protects lines that the streaming cores last held, so the line-ownership model
  above is wrong.
- Fewer than 470 sweeps in all on CPUs 4 to 7 means the partition costs the
  streaming cores more than the replacement estimate above.

The streaming cores lose a quarter of the L3: they allocate into 12 ways
instead of 16. Each of the four streams a private 1 MiB buffer per sweep, 4 MiB
in all, so every 1024-line L3 set receives 64 streaming lines per round of
sweeps. Victim selection is not documented. Under random replacement a line
survives 64 later allocations into its set with probability (15/16)^64, about
1.6 percent, with 16 ways, and (11/12)^64, about 0.4 percent, with 12 ways. The
streams therefore read from DRAM nearly always in both cases, and their
throughput should fall by at most a few percent. In the isolation run, CPUs 4
to 7 completed 106, 102, 134 and 155 sweeps in the 0.129 s busy phase, 497 in
all (8.1 GB/s of reads and writes). The prediction is 470 to 505 in all, a fall
of at most 5.4 percent, with each core within 8 percent of its isolation count.
A larger fall would mean the streaming cores reuse L3 lines that the partition
takes from them, which robotics pipelines sharing the cluster would also lose.

Time budget: the isolation run took 21.3 s, with preparation 0.7 s, rails 3.3 s,
reset 4.8 s, DRAM 2.4 s, launch 6.5 s, capture 0.5 s, rail restoration 1.7 s
and 1.4 s in the other stages. The partition adds four register reads, one
write and one readback per core at EL3, under 1 µs, and 512 bytes of output,
under 0.01 s of readback. The expected total is 21 to 22 s, below the 55 s finish_by
and the 60 s session lifetime. The controller publishes progress every 1 ms
tick; the longest gap is the bounded 250 ms line exchange, below the 1 s
application and 3 s runner progress windows. One run follows `check` with
admitted:true, and the first miss ends the stage without a retry.

In a product, the scheme assignment would follow the real-time set that the
board and application declarations name, as for core isolation above.

### Each core's current task has its own cache line

Status: **measured**, one cause-build run on port 3-3 with the partition above
(evidence entry `1794609985926933617-7e0a73a4`); CPU0's own release met its
prediction and the whole tick missed at its L3 refills.

The SMP kernel keeps each core's running task in `pxCurrentTCBs`. Every
context switch writes the switching core's entry, and the port reads it on
every exception entry and exit. With eight 8-byte entries, two lines held all
of them. In the partition run's image, line 0x41257140 held CPU0's tick time
and the entries of CPUs 0 to 4, and line 0x41257180 held the entries of CPUs 5
to 7 with `xIdleTaskHandles[0]`. CPUs 5 to 7 wrote the second line on every
wake, and their streaming then evicted it to DRAM. The latency fixture's ready
hook read `xIdleTaskHandles[0]` in CPU0's own release, the first release of each
tick, so that release refilled the line once per tick. The first line is
refilled, if at all, by the context save at exception entry, which comes
before the measured tick begins.

`include/sunstone_current_tcb.h` gives each core a 64-byte line in the
SunstoneOS kernels: C code indexes the array with `portCURRENT_TCB(core)`, and
assembly shifts the core ID by `portCURRENT_TCB_SHIFT`. The array costs 448
more bytes. The layout belongs to the kernel that defines the array:

- FreeRTOS `tasks.c` patched for the SunstoneOS schedulers refuses to compile
  unless `portCURRENT_TCB_SHIFT` is 6, and `cmake/current-tcb.cmake` sets that
  definition on every target that compiles a generated `tasks.c`.
- The unmodified `tasks.c`, which the emulator models of the device services
  and some bring-up images use, keeps 8-byte entries, the header's default.
- A mismatched kernel and port do not link. The kernel defines the absolute
  symbol `ss_current_tcb_layout_<shift>` and references
  `ss_current_tcb_port_<shift>`; the port does the reverse, and a port built
  with shift 3 supplies the weak layout symbol that the unmodified kernel
  lacks. Both references sit in retained data, so `--gc-sections` keeps them.

`checks/current-tcb-lines.py` fails on an index stride written out in C,
assembly, Python or a kernel patch, and on an image whose array does not match
the layout its guard symbols name. It also links every pairing of kernel kind
and port shift and requires that only the matching pairs link.

The fixture's ready hook reads neither the array nor the idle-task handles.
Each core publishes its running task, and whether it is an idle task, into its
own wake slot from the `traceCURRENT_TCB_SET` hook in the switch path, and the
kernel passes the idle flag from the task's attributes. CPU0 already writes
that slot in every release. The image of the run below found the flag on the
switching core by comparing the task with every core's idle-task handle; the
current fixture takes it from the kernel.

The run used the partition image of entry `1794605350026314973-495cffbe` with
only this change applied (branch `work/sched-tcb-lines-base-1113`). The
prediction removed one DRAM refill, about 1,670 cycles, from CPU0's own release
and left the other segments unchanged.

| Busy-phase mean over complete ticks | Partition | Predicted | Measured |
| --- | ---: | ---: | ---: |
| CPU0's own release, cycles | 6,624 | 4,000-6,000 | 4,719 |
| CPU0's own release, L3 refills | 2.71 | 1.2-2.2 | 1.63 |
| Whole tick, cycles | 60,519 | 54,000-61,000 | 59,148 |
| Whole tick, L3 refills | 20.3 | 17.0-20.0 | 20.14 (miss) |
| Releases to CPUs 1-3, cycles each | 3,214-3,959 | 2,500-4,500 | 3,164-3,606 |
| Releases to CPUs 4-7, cycles summed | 24,571 (11.4 refills) | 20,000-28,000 (9.5-13.0) | 25,461 (12.35) |
| Outside the releases and scans, cycles | 16,488 (4.3 refills) | 13,500-19,500 (3.0-5.5) | 16,699 (4.37) |
| Idle tick, cycles | 33,232 | 28,000-38,000 | 32,475 |
| CPU0's longest busy wake | 69.5 µs | at most 75 µs | 86.7 µs (miss) |
| 1 MiB sweeps on CPUs 4-7, summed | 479 | 465-500 | 481 |

CPU0's own release lost the refill and 1,905 cycles. The tick lost only 1,371
cycles and 0.2 refills, because the releases to CPU4 and CPU5 rose from 3.77
and 3.38 refills to 4.13 and 3.84; one run per image cannot separate that rise
from run-to-run variation. In both runs CPU0's longest busy wake is the first
sample of the busy phase. Without that sample the longest wake falls from 48.8
to 45.1 µs and the median from 37.3 to 36.7 µs.

The refill that disappeared came from the fixture's read, not from the
kernel's own tick path. Inside the measured tick the kernel reads the first
line only after the context save and the tick-time update have brought it into
CPU0's caches, and it does not read the second line. The kernel's share of
the old layout is the exception-entry context save, which this measurement does
not cover. CPU0's own release still takes about 2,500 cycles and one L3 refill
more than in the idle phase.

### A declared real-time core set keeps a private L3 way group

Status: **approved design**, not implemented; it follows the landing of the
line-per-core change above. It replaces the measurement options
`SUNSTONE_LATENCY_L3_PARTITION` and `SUNSTONE_LATENCY_STREAM_MASK` with a
declaration in the application manifest and the board description.

#### What the partition gives and costs

The A733's DynamIQ Shared Unit (DSU) splits its 1 MiB, 16-way L3 into four way
groups of 256 KiB. `CLUSTERPARTCR_EL1` makes a group private to a scheme ID, and
each core's `CLUSTERTHREADSID_EL1` selects its scheme. A private group changes
only victim selection: a core allocates only into its scheme's groups, and a
lookup still hits in any way.

Measured on port 3-3 with streaming on CPUs 4-7 and group 0 private to CPUs 0-3
(evidence entry `1794605350026314973-495cffbe`, against isolation alone in
`1794597393456307577-46a485eb`):

- CPU0's busy tick fell from 76,410 to 60,519 cycles and from 26.5 to 20.3 L3
  refills; its longest busy wake fell from 93.1 to 69.5 µs.
- The releases to CPUs 1-3 fell to within about 1,300 cycles of their idle cost.
- The streaming cores, left with 12 of 16 ways, completed 479 sweeps of 1 MiB
  against 497, 3.6 percent fewer. A second partition run
  (`1794609985926933617-7e0a73a4`) completed 481.

The partition costs nothing at run time: EL3 writes the registers once at
start, no EL1 code touches them, and `ACTLR_EL3` stays 0, so EL1 cannot change
them.

#### The declaration

- **Application manifest**, a new optional `cpu.realtime` object:
  `{"cores": N, "l3_way_groups": G}`. N is how many cores run the application's
  real-time tasks; G is how many way groups those cores keep to themselves.
  Without the object the image writes nothing.
- **Board description**, per-part data in the board port: the real-time core
  order and the L3 geometry. For the A733 the order is the A55 cores 0, 1, 2,
  3, 4, 5. CPU0 comes first because it owns the global tick and runs every
  release. The geometry is 4 groups of 4 ways, 64-byte lines and 1024 sets,
  read on the board as `CCSIDR` 0x707fe07a.
- **Derived values**: the real-time set is the first N cores of the order; its
  cores get scheme ID 1 and every other core keeps 0. Way groups 0 to G-1 are
  private to scheme 1 and groups G to 3 to scheme 0, so `CLUSTERPARTCR_EL1` is
  the sum of 2^(4+g) for g below G and 2^g for g from G to 3: 0x1E for one
  group, 0x3C for two, 0x78 for three.

An image holds one application, and its manifest is compiled in
(`ss_application.manifest`), so the EL3 boot code reads the declaration from
the image itself. The image's build ID covers it, and the session carries no
new record.

The partition protects the real-time cores' lines only while streaming work
stays off those cores. Placing the application's streaming workers and the OS's
bulk services on the other cores is the core-isolation decision in "Real-time
cores may need to exclude streaming work". This design depends on it and does
not settle it (see the open items below). The latency fixture then streams on the cores outside the
declared set, and its stream mask becomes a value derived from its manifest.

#### Admission

The manifest generator (`tools/sunstone-manifest.py`) refuses a declaration
outside these hardware bounds, and the EL3 boot code checks the same bounds
against the board description before any write:

1. 1 ≤ G ≤ groups − 1, which is 3 on the A733. Scheme 0 keeps at least one
   group, because the other cores, accelerator coherency port (ACP) requests
   and cache stashes all allocate under scheme 0.
2. 1 ≤ N ≤ the length of the real-time order, and at least one core stays
   outside the set. With every core in scheme 1 the partition only shrinks the
   cache.
3. `CLUSTERPWRSTAT_EL1` bits [7:4] show every group powered. A powered-down
   portion removes its group from every scheme, so a claimed group that is not
   powered refuses the start.

One scheme ID serves real-time work. Changing a core's scheme on each context
switch would let one core serve both kinds of work, at an MSR and ISB per
switch; the design keeps the scheme fixed per core for the boot.

#### Applied at OS start, cleared on every exit

In `local_boot` (`platform/a733-smp-cached/continue.c`), which every OS image
runs:

1. Each core reads `CLUSTERPARTCR_EL1` and its own `CLUSTERTHREADSID_EL1`.
   A nonzero value means a partition survived a reset that should have cleared
   it; the core clears both and stops the boot with a new refusal detail.
2. Each core in the real-time set writes scheme ID 1, issues an ISB and reads
   it back (detail 0x738 on a mismatch, as now).
3. After the barrier that follows the capture, CPU0 checks the power status,
   writes `CLUSTERPARTCR_EL1` and reads it back (detail 0x739).
4. A third barrier holds every core until CPU0's write is complete. The current
   measurement code lacks it, and secondary cores can reach EL1 a few
   microseconds before the partition exists.

With no declaration, steps 2 to 4 do not run; step 1 still costs two register
reads per core.

Exits:

- Every entry to EL3 already clears both registers first, in the shared vector
  entry `a733_cached_el3_fault` (`continue.S`). That covers the normal return,
  the privileged exit SMC and `svc #119`, EL1 faults, EL3 exceptions, the
  returning lifetime's success SMC and the resident stop.
- `failed()` and `park()` in `continue.c` wait at EL3 without passing through
  the vector. A core that fails there after CPU0's write leaves the partition
  set. The design clears both registers in `failed()` before it parks.
- A watchdog reset runs no EL3 code. It is a system reset, which returns the
  DSU registers to 0; step 1 of the next start checks that.

#### What the streaming side loses

With G groups the other cores keep 16 − 4G ways: 12, 8 or 4. The measured cost
is 3.6 percent fewer sweeps with one group, for streams that reuse almost
nothing from the L3. Under random replacement a streamed line survives one
round of 4 MiB of sweeps with probability (15/16)^64, about 1.6 percent, with
16 ways and (11/12)^64, about 0.4 percent, with 12 ways.

Work that reuses the L3 loses more. A working set between 768 KiB and 1 MiB fits
in 16 ways and does not fit in 12. A camera or lidar stage that revisits a
frame tile, or CPU code feeding NPU tiles, falls in that range. No such
workload has been measured with the partition. Device DMA into memory does not
allocate in the L3 today: the drivers treat every device as non-coherent, and
nothing uses the ACP or stashing. Two or three groups have not been measured
on either side.

#### Checks

- **Host**: manifest cases that refuse G = 0, G = 4, N = 0 and N equal to every
  core, and accept the latency fixture's declaration. A host test of the EL3
  value derivation gives 0x1E, 0x3C and 0x78 and the per-core scheme IDs. A
  check that no build option names the partition or the stream mask.
- **QEMU**: the QEMU board description declares no L3 groups, so the start
  code writes nothing there.
- **Board**: one cause-build run with the latency manifest declaring four cores
  and one group. Prediction: the figures of entry
  `1794605350026314973-495cffbe` within their spread, a boot read of 0 on every
  core, and readbacks 0x1E and scheme IDs 1, 1, 1, 1, 0, 0, 0, 0.

#### Open items

- Core isolation stays an open decision. The partition does not place work:
  the scheduler still decides where streaming workers and bulk services run,
  and this design does not change that.
- The manifest declaration is also where the scheduler's real-time core set
  should come from when isolation is decided, so that one declaration names the
  cores that keep an L3 way group and the cores that streaming work avoids.
- The declaration does not pass through a session grant, as the CPU clock grant
  does: the partition changes no voltage, clock or rail, and the image's build
  ID covers the manifest.
- An application on the A76 cores has no real-time set yet; the board order
  holds only A55 cores.

## Time slicing has a task-count-independent traversal bound

Status: implemented in `patches/freertos-smp-tasks.patch` and host-tested. The
native task patch filters time slices by ready task affinity and caches one admitted-core
mask per distinct running priority. Without a bound, the traversal examines every
ready task at those priorities while the port holds the global ISR lock, and static
allocation places no limit on the task count.

[The length check](../../patches/freertos-smp-tasks.patch) limits traversal to lists
of at most twice the core count (sixteen entries on eight cores: one running task and
one waiter per core). A longer list requests rescheduling on every core running that
priority, which retains upstream time slicing and covers every unexamined affinity,
and can send unnecessary SGIs when the tasks are pinned elsewhere. At most seven
non-idle priorities occur, each examined once, so a tick visits at most 112 entries.
Timeout expiry and task selection keep their task-count-dependent work, so the
bound does not cover the complete tick or context switch.

no host check executes
`xTaskIncrementTick` from the native ELF with substituted core identity and SGI
publication, and checks yield flags and tick advancement over ten cases (exact
termination, a last-added affinity, unrelated running priorities, excluded
affinities, idle tasks). A 1,024-entry list costs 354 instructions after the check
against 10,472 before; the instruction-model counts, the board pair that compares
the change (entries `1793560470911857163-ce5f22bf` and
`1793560934366050732-4e45b5ea`) and the reviewer's reproduction are in the records
`scheduler-realtime-timeslice-traversal` in
[scheduler-realtime.jsonl](../evidence/scheduler-realtime.jsonl) and in
[scheduler.jsonl](../evidence/scheduler.jsonl). On the board the change does not
improve the loaded wake or task-lock-wait maxima, and both runs fail the 25 µs
masking and lock budgets.

The recorded recognition on the same scheduler returns the reference CRCs and
text (record `1793561595474856811-579a99b6`); it is a parity check, not a
throughput comparison.

## Tickless idle needs a cross-core sleep protocol

Status: open. Enabling the upstream generic path does not work on this port. In
the pinned upstream `tasks.c`, `prvGetExpectedIdleTime` rejects an idle ready list
longer than one (this configuration has eight idle tasks); `vTaskSuspendAll` keeps
the task lock until `xTaskResumeAll`, so sleeping within it extends a global lock
hold through the sleep. In the same file, `vTaskStepTick` requires scheduler suspension when stepping
to the next delayed-task expiry; and `xTaskGetTickCountFromISR` reads global time
with local masking. These anchors refer to the source pinned by
[the source lock](../../sources.lock.json) before project patches. The partitioned
fork rejects `configUSE_TICKLESS_IDLE` at compile time.

An idle secondary can receive a device IRQ and wake a task pinned to itself without
an SGI to CPU0, so the task could observe stale ticks and install an incorrect
timeout while CPU0's timer is suppressed. A wake request to CPU0 alone does not
close this race. The design needs a privileged EL1 idle entry, a generation number
for each sleep attempt and acknowledgment from every idle core. Remote IRQ entry
must atomically cancel admission or wake CPU0 before running callbacks. CPU0
corrects global ticks from the generic counter, restores the periodic compare and
publishes the awake generation before remote callbacks proceed. No kernel lock
remains held across WFI or an acknowledgment wait. Active-idle task reclamation and
returning-session deadlines stay live during suppression. Evidence needed:
idle-window PPI and SGI counts per core, elapsed-counter versus global-tick
agreement, pending-IRQ race coverage, repeated sleep and wake transitions, and
temperature or power.

## High-resolution deadlines use a separate compare consumer

Status: implemented. The [generic counter clock](../../platform/a733/counter_clock.c)
supplies sub-tick timestamps (24 MHz, nominal 41.667 ns period), and reading it does
not create an asynchronous timeout. `sunstone_deadline_wait_until()`
([header](../../include/sunstone_deadline.h), `runtime/deadline.c`,
`runtime/deadline-a733.c`) lets a registered task wait until an absolute
microsecond deadline or a notification on its index. A table sized for the
image's declared owner tasks (16 slots by default for standalone profiles)
shares CPU0's virtual-timer compare (`CNTV`) with the scheduler. A caller updates
its slot and CPU0 callers rearm the compare directly; secondary callers publish a
rearm request and send CPU0 an SGI, because the compare register is private to each
core. The ISR retires one due slot per interrupt and notifies its task, so the added
critical section stays bounded and further due slots keep the level-sensitive
compare pending. Registration reserves notification storage once and preserves
notification index zero. Registered tasks must not be deleted. The caller rechecks
its device predicate after every result, including expiry. Overshoot distributions,
burst expiry, cancellation and cross-core delivery are unmeasured, and the
scheduler tick rate does not satisfy this requirement.

## The global-lock kernel is the control for the partitioned fork

Per-core ready lists change migration, affinity, simultaneous wake placement,
priority inheritance and task deletion, and splitting only the port's locks would
expose shared kernel lists to concurrent mutation. [The reviewed
research](scheduler-research.md) ranks removal of shared ownership above
fairness-only changes, and [the partitioned design](scheduler-partitioned.md)
specifies the fork's local queues, publication protocol, object locking, topology
and timing. The global-lock kernel remains its buildable physical control. Every live
stage requires phase-separated percentiles, complete lock and mask attribution and
exact recognition output before a performance claim.
