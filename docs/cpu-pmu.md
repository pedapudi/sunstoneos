# Per-core cycle and event counts for measurement regions

Status: **implemented** and checked under instruction emulation and on QEMU
Cortex-A55 and Cortex-A76 cores. No image links the module yet, and no
result comes from the board. Event counting needs a change to the EL3
continuation, described under [Prerequisite](#prerequisite-mdcr_el2-gives-el1-the-event-counters),
before any image uses it. The design and its budgets are in
[designs/cpu-pmu.md](designs/cpu-pmu.md), and the measured figures in
[designs/cpu-pmu-measurements.md](designs/cpu-pmu-measurements.md).

[platform/a733/cpu_pmu.c](../platform/a733/cpu_pmu.c) measures a region of
code on one core. At the start of the region it zeroes six event counters
and reads the cycle counter (`PMCCNTR_EL0`) and the generic counter
(`CNTPCT_EL0`). At the end it reads them again and writes the differences
into a 64-byte record. Board A/B runs use the record to compare cycles,
instructions and cache and TLB events between two versions of a workload.

## A pinned EL1 task calls begin and end

[include/a733_cpu_pmu.h](../include/a733_cpu_pmu.h) declares two calls:

```c
struct a733_cpu_pmu_region region;      /* on the caller's stack */
struct a733_cpu_pmu_record record;      /* in the caller's result buffer */
if (a733_cpu_pmu_begin(&region) == A733_CPU_PMU_OK) {
    work();
    a733_cpu_pmu_end(&region, &record); /* record.status says whether it counts */
}
```

The caller must meet three conditions:

- **It runs at EL1.** The task-privilege hook of the SMP port
  (`npu_service_task_privileged` in
  [patches/freertos-smp-privileged.patch](../patches/freertos-smp-privileged.patch))
  starts the NPU, DMA and pool workers at EL1. In the board image
  ([apps/board/main.c](../apps/board/main.c)), the hook also starts the tasks
  that `board_application_task_privileged` selects, such as a board-test
  bundle's workload task, at EL1. At EL0 the first `DAIF` or PMU access traps, because
  `PMUSERENR_EL0` is zero, and the EL1 synchronous-exception handler panics.
  An EL0 client gets a record from an EL1 worker that measures its own job
  and returns the record in the job's result.
- **Its affinity mask holds only the current core.** Begin reads the mask with
  `vTaskCoreAffinityGet(NULL)` and refuses any other mask. The scheduler then
  never runs the task on another core. No other task may change a measured
  task's affinity inside a region: a task moved to another core and back
  passes every check and returns a wrong record.
- **It is a task.** Interrupt handlers do not call begin, because
  `vTaskCoreAffinityGet` is a task-level kernel call.

The counters count everything that runs on the core inside the region,
including other tasks and interrupt handlers. A/B runs therefore keep the
measured core otherwise idle. Regions exclude SMC calls, because whether time
in the EL3 monitor is counted depends on the SoC's debug authentication
signals.

## The record

The record is little-endian and 64 bytes long:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | `format` | 1 |
| 1 | `core` | Core that ran end; 0xff when end refused its arguments |
| 2 | `counters` | Event counters used: min(`PMCR_EL0.N`, 6) |
| 3 | `supported` | Bit i set: event i is implemented and has a counter |
| 4 | `overflow` | Bit i set: event i occurred at least 2³² times; no count |
| 5 | `status` | `enum a733_cpu_pmu_status` |
| 6 | `reserved0` | 0 |
| 8 | `midr` | `MIDR_EL1` |
| 12 | `cntfrq` | `CNTFRQ_EL0` |
| 16 | `cycles` | `PMCCNTR_EL0` difference |
| 24 | `ticks` | `CNTPCT_EL0` difference |
| 32 | `events[6]` | 32-bit counts of `INST_RETIRED` (0x08), `L1D_CACHE_REFILL` (0x03), `L2D_CACHE_REFILL` (0x17), `BUS_ACCESS` (0x19), `DTLB_WALK` (0x34) and `ITLB_WALK` (0x35) |
| 56 | `reserved1` | 0 |

A count is zero unless its event is supported and did not overflow. On a
refusal, every field other than `format`, `core` and `status` is zero.
`decode` and `measured_rate` in [checks/cpu-pmu.py](../checks/cpu-pmu.py)
decode a record and apply the rate rule of [cpu-clock.md](cpu-clock.md),
`cycles × cntfrq ÷ ticks`. An event counting four per cycle at an estimated
2 GHz overflows after about 0.5 s.

## Begin configures the PMU once per core

Begin configures the core's PMU when the core's configured flag is clear or
`PMCR_EL0.E` reads 0. A warm reset of the core clears `E` (Cortex-A55 TRM
D5.4), and no other code in an image writes `E` as 0 or writes the event
counters. Configuration, with IRQs masked:

1. Reads `PMCR_EL0.N` and uses counters 0 to min(N, 6) − 1. N = 0 is refused
   with `A733_CPU_PMU_NO_COUNTERS`.
2. Reads `PMCEID0_EL0` and `PMCEID1_EL0`. Event e below 0x20 is implemented
   when bit e of `PMCEID0_EL0` is set, and event 0x20 + k when bit k of
   `PMCEID1_EL0` is set (TRM D5.2 and D5.3).
3. Writes each `PMEVTYPER<n>_EL0` with its event number and all filter bits
   zero, and `PMCCFILTR_EL0` with zero: every counter counts at EL0 and EL1.
4. Writes `PMINTENCLR_EL1` for bits 0 to 5 and 31, so no overflow raises the
   PMU interrupt (TRM C2.5), and `PMUSERENR_EL0` as zero, so EL0 access traps.
5. Writes `PMCNTENSET_EL0` for the used counters and the cycle counter, then
   `PMCR_EL0` with `E` and `LC` set and `D` clear. `P` and `C`, the counter
   resets, are written as zero, and `DP` and `X` keep their values.
   `a733_cpu_cycles_enable` configures the cycle counter the same way, so
   either may run first.
6. After one `ISB`, reads back each written register once. Only when all match
   does it set the configured flag and store `MIDR_EL1` and `CNTFRQ_EL0` in
   the core's state; otherwise begin refuses with
   `A733_CPU_PMU_CONFIG_MISMATCH` and configures again on the next call.

Each core has one 64-byte state line in `.bss` (512 bytes for eight cores),
written only by that core with IRQs masked. The module therefore takes no lock
and issues no `DMB` or `DSB`. A caller that passes a record to another core
uses a queue, a task notification or a release store.

## The configured path costs 28 register accesses

On a configured core, begin and end each make 14 system-register accesses,
`MSR DAIFSet` included, and one `ISB`:

- **begin:** `DAIF` read, `DAIFSet`, `TPIDR_EL1` (the port's logical core
  index), `PMCR_EL0`, six `PMEVCNTR<n>_EL0` writes from counter 5 down to
  counter 0, `PMOVSCLR_EL0`, `ISB`, `PMCCNTR_EL0`, `CNTPCT_EL0`, `DAIF`
  restore;
- **end:** `DAIF` read, `DAIFSet`, `ISB`, `PMEVCNTR0_EL0`, `PMCCNTR_EL0`,
  `CNTPCT_EL0`, `TPIDR_EL1`, `PMEVCNTR5_EL0` down to `PMEVCNTR1_EL0`,
  `PMOVSSET_EL0`, `PMCR_EL0`, `DAIF` restore.

Neither reads `CurrentEL` or a configuration register, and neither issues a
`DSB`, `DMB` or `SVC`. The counters are accessed directly by number, never
through `PMSELR_EL0`, so an interrupt cannot change which counter an access
reaches. Begin's one kernel critical section, inside `vTaskCoreAffinityGet`,
runs before any counter is zeroed.

The counted window runs from begin's write of counter 0 to end's read of
counter 0. Counter 0 counts `INST_RETIRED`, so begin zeroes it last and end
reads it first. Begin tail-calls a leaf function that zeroes the counters, so
no stack frame is restored inside the window; the leaf receives the saved
`DAIF` value in the region's `ticks` field, which it then overwrites. End
checks only its arguments before it reads counter 0, and reads the other
counters only after it confirms that it runs on the beginning core. On QEMU,
an empty region counts 20 instructions.

## Every refusal leaves the core usable

Each refusal returns a status before any device access and retains nothing
(class 1 of section 1.8 of [performance-architecture.md](performance-architecture.md)).
End writes a record with the status in every case where the record pointer
is not null.

| Status | Raised by | Condition |
| --- | --- | --- |
| `BAD_ARGUMENT` | begin, end | A null region or record |
| `NOT_PINNED` | begin | The affinity mask is not exactly the current core |
| `NO_COUNTERS` | begin | `PMCR_EL0.N` reads 0 |
| `CONFIG_MISMATCH` | begin | A configuration register read back another value |
| `NOT_BEGUN` | end | Begin refused the region; no register is read |
| `CORE_CHANGED` | end | End runs on another core, which happens only when another task changed the measured task's affinity |
| `GENERATION_CHANGED` | end | Another region began on the core and zeroed its counters; nested regions are refused this way |
| `DISABLED` | end | `PMCR_EL0.E` reads 0, as after a warm reset |

Before its core check, end reads counter 0 of the core it runs on. Every core
where begin succeeded has that counter, and the continuation programs every
core alike, so a moved task still reads an existing register.

## Prerequisite: MDCR_EL2 gives EL1 the event counters

`MDCR_EL3` is `0x10000000` in both continuation paths: `TPM` is 0, so EL1 PMU
accesses do not trap to EL3. `MDCR_EL2` is written as zero in
[platform/a733/cpu_continue.S](../platform/a733/cpu_continue.S) and
[platform/a733-smp-cached/continue.S](../platform/a733-smp-cached/continue.S).
That sets `HPMN` to 0, which reserves every event counter for EL2 and is
CONSTRAINED UNPREDICTABLE on Armv8.2 cores. The vendor monitor instead writes
`PMCR_EL0.N` into `HPMN`. Both continuation files must read `PMCR_EL0.N` at EL3
and write it into `MDCR_EL2.HPMN` with `TPM`, `TPMCR` and `HPME` zero, and
`checks/a733-cpu-continue.py` must expect `MDCR_EL2` = 6. Those files belong to
the CPU startup work. Until the change lands, `checks/cpu-pmu.py` reports the
module cases and then fails on the continuation sources. No image may call the
module before then. With `HPMN` = 0, `PMCR_EL0.N` should read 0 at EL1, which
begin refuses. Because `HPMN` = 0 is CONSTRAINED UNPREDICTABLE, an
event-counter access could instead trap and end in a panic.

## Validation

`checks/cpu-pmu.py OUTPUT` compiles the module with the board image's SMP
flags (`-mgeneral-regs-only`) and the eight-core configuration, links it with
a stub `vTaskCoreAffinityGet`, and runs it under Unicorn. The code hook
emulates every `MRS` and `MSR`, because Unicorn's CPU model implements only
four event counters. Its 20 cases cover:

- the first configuration: event numbers, filters, `PMINTENCLR_EL1`,
  `PMUSERENR_EL0`, `PMCNTENSET_EL0` and `PMCR_EL0` values, no `P` or `C`
  write, the counter zeroing order and one readback;
- the record of a region with modeled counts, and the measured rate;
- the configured path: the access list above, no readback, `CurrentEL`, `DSB`,
  `DMB` or `SVC`, and memory limited to the stack, the region, the record and
  the core's own state line, also on core 7;
- an overflowed event, unimplemented events, `N` = 4 with counters 4 and 5
  never accessed, and `N` = 0;
- a readback mismatch in each of six registers, after which the next begin
  configures again; reconfiguration after `E` reads 0; `E` cleared inside a
  region;
- refusals for a two-core mask, another core's mask, core 8, an empty mask, a
  final-core mismatch, a nested region and null arguments, each with `DAIF`
  restored, and `DAIF` kept masked when the caller had masked it;
- the record, region and state layouts from the object's debug information.

It also verifies the hashes of the sources pinned in
[checks/cpu-pmu-sources.json](../checks/cpu-pmu-sources.json) and lists the two
not yet fetched: the Arm Architecture Reference Manual (DDI 0487) and the
Cortex-A76 TRM. They cover the `ISB` placement, direct counter access, `HPMN` =
0 on Armv8.2 cores, and the A76 event numbers and counter count.

`checks/cpu-pmu-smp.py --output OUTPUT` builds
[checks/cpu-pmu-smp.c](../checks/cpu-pmu-smp.c) with the module on
`platform/qemu-smp`, the SMP port patches and the privileged-task patch, and
runs it on two cores under `-icount shift=0`, once with `-cpu cortex-a55` and
once with `-cpu cortex-a76`. Each core measures at the same time as the other.
On both cores the check requires:

- an empty region of at most 24 instructions;
- a loop of 2,000 instructions counted as 2,000 more than the same region
  without iterations;
- a nonzero cycle difference;
- the core, `MIDR_EL1`, `CNTFRQ_EL0`, counters used and supported mask that
  QEMU reports. On core 0 it
requires the nested-region, cleared-enable, two-core-mask and moved-task
refusals. QEMU implements `INST_RETIRED` and none of the other five events.

Results are in [designs/cpu-pmu-measurements.md](designs/cpu-pmu-measurements.md).

## Physical evidence still needed

Each item needs a scheduled board session under [validation.md](validation.md),
after the `MDCR_EL2` change:

1. Configure on cores 0 and 6 in a supervised session; record `PMCR_EL0`,
   `PMCEID0_EL0` and `PMCEID1_EL0`, and confirm that no exception occurs.
2. Measure empty-region cycles and instructions on cores 0 and 6.
3. Measure a loop of known length on both core types, and compare cycles per
   tick with the CPU clock report.
4. Confirm whether `L2D_CACHE_REFILL` is implemented on the A733's A55 cores.
