# Design: per-core cycle and event counts for performance measurement

Status: **implemented** in [platform/a733/cpu_pmu.c](../../platform/a733/cpu_pmu.c)
and described in [cpu-pmu.md](../cpu-pmu.md). **Qualified on the board** on
core 0 (Cortex-A55); **host-tested** on Cortex-A55 and Cortex-A76 models. The
A76 cores 6 and 7 are open on the board. Section numbers refer to
[performance-architecture.md](../performance-architecture.md).

Board A/B runs need cycle counts and cache and TLB event counts per core:
`CPU_CYCLES`, `DTLB_WALK` and `ITLB_WALK` in the
[memory architecture](memory-architecture.md), and cycles in the
[power authority](power-authority.md) and [NPU exact sum](npu-exact-sum.md)
designs. The module measures a region at EL1 on Cortex-A55 r2p0 (cores 0–5)
and Cortex-A76 r4p1 (cores 6 and 7): a region reads the cycle counter and six
event counters at its start and end, and the module writes the per-core
differences into a fixed 64-byte record. It is linked by the board-bundle
applications `apps/board-bundles-cpu` and `apps/board-bundles-power`.

## Interface and data flow

`include/a733_cpu_pmu.h` declares two calls:

- `a733_cpu_pmu_begin(struct a733_cpu_pmu_region *)` runs on the core to be
  measured. It refuses unless the calling task's affinity mask,
  `vTaskCoreAffinityGet(NULL)`, holds only the current core. It then masks
  IRQs, reads `PMCR_EL0`, and configures the core's PMU only when the core's
  configured flag is clear or `PMCR_EL0.E` reads 0. It zeroes the six event
  counters, clears their overflow flags and stores the core number, a per-core
  generation, `PMCCNTR_EL0` and `CNTPCT_EL0` in the caller's region.
- `a733_cpu_pmu_end(const struct a733_cpu_pmu_region *, struct a733_cpu_pmu_record *)`
  reads the counters and the overflow flags and writes one record.

The record is little-endian and fills one 64-byte line: format version, core,
counters used, supported-event mask, overflow mask, status, `MIDR_EL1`,
`CNTFRQ_EL0`, 64-bit cycle and counter-tick differences, and six 32-bit event
differences in the order `INST_RETIRED` (0x08), `L1D_CACHE_REFILL` (0x03),
`L2D_CACHE_REFILL` (0x17), `BUS_ACCESS` (0x19), `DTLB_WALK` (0x34) and
`ITLB_WALK` (0x35). The caller owns the region (24 bytes, on its stack) and the
record. The module allocates nothing and touches no MMIO.

Only an EL1t task calls begin and end: a task that the image's task-privilege
hook (`npu_service_task_privileged` in
[patches/freertos-smp-privileged.patch](../../patches/freertos-smp-privileged.patch))
starts at EL1. At EL0 the first `DAIF` or PMU access in begin traps
(`PMUSERENR_EL0` is 0) and ends in the EL1 synchronous-exception panic.
Interrupt handlers do not call begin, because `vTaskCoreAffinityGet` is a
task-level kernel call.

- **Board-test bundles** measure in their own workload task, started at EL1t
  through `board_application_task_privileged` and created on one core with
  `xTaskCreateStaticAffinitySet`; `board_usb0_task` copies the record once into
  the bundle's response.
- **EL0 clients** get a record only from an EL1t worker they already call, such
  as the DMA or NPU service worker, which measures its own job and returns the
  record in that job's result.

The counters count everything on the core, including other tasks and interrupt
handlers that run inside the region, so A/B runs keep the measured core
otherwise idle.

## Register sequence

Sources: Cortex-A55 TRM 100442_0200_00 (r2p0), TF-A `0892ad4d`
`include/arch/aarch64/arch.h`, and the vendor EL3 monitor from orangepi-build
`a95129bf` (hashes and paths in `checks/cpu-pmu-sources.json`, verified by the
host check). Configuration runs when begin finds the configured flag clear or
`PMCR_EL0.E` equal to 0. The one event that makes a configured core stale is a
warm reset of that core, which sets `PMCR_EL0.E` to 0 (TRM D5.4); no other code
in an image that links this module writes `E` as 0 or writes the event counter
registers, and `a733_cpu_cycles_enable` writes the same cycle-counter values.

1. Read `PMCR_EL0.N` and use counters `0 … min(N, 6) − 1`. TRM D5.4 gives N = 6
   on the A55; `MDCR_EL2.HPMN` limits the counters that Non-secure EL1
   controls. Begin refuses N = 0.
2. Read `PMCEID0_EL0` and `PMCEID1_EL0`. Event e < 0x20 is implemented when bit
   e of PMCEID0 is set, and event 0x20 + k when bit k of PMCEID1 is set (TRM
   D5.2 and D5.3). An unimplemented event is left out of the supported mask.
3. Write `PMEVTYPER<n>_EL0` with the event number and all filter bits zero, and
   `PMCCFILTR_EL0` with zero, so both count at EL0 and EL1 and not at EL2, the
   filter `a733_cpu_cycles_enable` uses.
4. Write `PMINTENCLR_EL1` for bits 0–5 and 31 so that no overflow raises
   nPMUIRQ (TRM C2.5), and `PMUSERENR_EL0` as zero so that EL0 access traps.
5. Write `PMCNTENSET_EL0` for the used counters and bit 31. Set `PMCR_EL0.E` and
   `LC`, clear `D`, never write `P` or `C`, keep `DP` and `X`; an `ISB`
   follows. The module reads back each register it wrote, once, and sets the
   configured flag only when all match; otherwise begin refuses. It stores
   `MIDR_EL1` and `CNTFRQ_EL0` in the core's state line.

The configured path makes 28 system-register accesses and 2 `ISB`: 14 in begin
(`DAIF` read, `DAIFSet`, `TPIDR_EL1`, `PMCR_EL0`, six `PMEVCNTR<n>_EL0` writes,
`PMOVSCLR_EL0`, `ISB`, `PMCCNTR_EL0`, `CNTPCT_EL0`, `DAIF` restore) and 14 in end
(`DAIF` read, `DAIFSet`, `ISB`, `PMEVCNTR0_EL0`, `PMCCNTR_EL0`, `CNTPCT_EL0`,
`TPIDR_EL1`, the other five `PMEVCNTR<n>_EL0` reads, `PMOVSSET_EL0`, `PMCR_EL0`,
`DAIF` restore). Begin zeroes counter 0 (`INST_RETIRED`) last and end reads it
first, so the counted window holds as few instructions as possible; begin
tail-calls a leaf that zeroes the counters, so no stack frame is restored in
the window. End checks its arguments, and refuses a region that begin refused,
before it reads any register, and reads the other five counters only after it
confirms that it runs on the beginning core. Each region uses `PMEVCNTR<n>_EL0`
and `PMEVTYPER<n>_EL0` directly and never `PMSELR_EL0`. The module never resets
the cycle counter; an event's overflow flag is set at end only when the event
occurred at least 2³² times, and that event is then reported in the overflow
mask with no count.

## EL2 and EL3 controls

Both continuation paths write `MDCR_EL3 = 0x10000000` (`MDCR_MTPME_BIT`), so
`TPM` (bit 6) is 0 and EL1 PMU accesses do not trap to EL3; `SCR_EL3 = 0x535`
makes EL1 Non-secure, where counting is permitted. Both
[platform/a733/cpu_continue.S](../../platform/a733/cpu_continue.S) and
[platform/a733-smp-cached/continue.S](../../platform/a733-smp-cached/continue.S)
copy `PMCR_EL0.N` into `MDCR_EL2.HPMN` (keeping `TPM`, `TPMCR` and `HPME`
zero), as the vendor monitor does, because `HPMN = 0` leaves EL1 no counter and
is CONSTRAINED UNPREDICTABLE on Armv8.2 cores. `checks/a733-cpu-continue.py`
expects `mdcr_el2` = 6 for the modeled `PMCR_EL0`. An access trapped at EL2 or
EL3 ends in the EL1 synchronous-exception panic rather than a refusal.

## Interrupts, memory and synchronization

No PMU interrupt is used. Counts are read only at begin and end, so a region
costs no wake-up and no tick wait. The module keeps one 64-byte-aligned state
line per core in `.bss` (512 bytes in all, normal write-back memory) holding the
configured flag, counters used, supported mask, `MIDR_EL1`, `CNTFRQ_EL0` and
generation. Only its own core writes a line, with IRQs masked (section 2 item
6), and PMU registers are per core, so the module uses no lock of its own,
`DMB` or `DSB`. A caller that passes a record to another core uses a FreeRTOS
queue, a task notification or a release store.

Begin refuses unless the calling task's affinity mask holds only the current
core; then the scheduler runs the task on no other core inside the region.
`vTaskCoreAffinityGet` enters the kernel critical section once per begin,
before any counter is zeroed: the module's only lock, outside the counted
window. Each begin increments its core's generation. End refuses with a status
code (`enum a733_cpu_pmu_status`) on a final-core mismatch (another task
changed the measured task's affinity), a changed generation (another region
began on the core, which also refuses nested regions), or `PMCR_EL0.E` reading 0.
A task moved to a second core and back inside one region passes these checks
and gets a wrong record, so no task changes a measured task's affinity during a
region; in the tree, one task sets another's affinity only at creation
(`apps/board/main.c`, `apps/spi/main.c`) and in scheduler migration checks.
Every refusal happens before any device access and retains nothing (section 1.8
class 1).

## Constraints and unproven items

- The cycle configuration matches `a733_cpu_cycles_enable`, so the two coexist.
  The helpers `pmu_start` and `pmu_read` in `checks/device-memory-capacity.c`
  stay because their inline
  two-instruction read is shorter than a call.
- Regions exclude SMC calls: `MDCR_EL3.SPME` is 0, so whether EL3 time counts
  depends on the SoC's debug authentication signals.
- Not proven by a pinned source: the A76 event numbers and counter count (the
  Cortex-A76 TRM 100798 r4p1 is not pinned), the `ISB` placement around counter
  reads, direct counter access, and the behavior of `MDCR_EL2.HPMN = 0` on
  Armv8.2 cores (Arm DDI 0487, not pinned). The host check lists DDI 0487 and
  the A76 TRM with their URLs and no hash.
- Left out: sampling and PMU interrupts, EL0 access, caller-selected events,
  chained 64-bit event counters, the DSU cluster PMU, external debug access and
  multiplexing of more than six events. No design requests any of them.

## Qualified figures

| Figure | Budget | Result | Evidence |
| --- | --- | --- | --- |
| Instructions in an empty region (QEMU A55 and A76) | at most 24 | 20 | `cpu-pmu.jsonl`: `cpu-pmu-host-budgets` |
| Loop of 2,000 instructions above the empty region | exact | 2,000 on both cores and models | same |
| System-register accesses and `ISB`, begin plus end | at most 28 and 2 | 28 and 2 | same |
| Locks, allocations, MMIO, wake-ups per region | one critical section before the window, else zero | as budgeted | same |
| Record, state, region size | 64 B, 512 B, at most 32 B | 64 B, 512 B, 24 B | same |
| Empty-region cycles, core 0 (A55) on the board | at most 150 (provisional) | 64 at the median, 131 at most | `cpu-pmu.jsonl`: `cpu-pmu-board-core0` |
| `L2D_CACHE_REFILL` on the A733 A55 | implemented | 1,024 lines count 1,031 refills | same |

QEMU implements `INST_RETIRED` and no other of the six events; its cycle
counter follows virtual time and says nothing about A733 cycles.

## Checks

`checks/cpu-pmu.py` runs the AArch64 object under Unicorn with modeled system
registers: event encodings and filters, the `PMCNTENSET`, `PMINTENCLR` and
`PMCR` values, the absence of `P` and `C` writes, `N` below 6, unimplemented
events, overflow marking, every refusal, no configuration readback on the
configured path, reconfiguration when `PMCR_EL0.E` reads 0, the record layout
and the `MDCR_EL2` write in both continuation sources.
`checks/cpu-pmu-smp.py` builds `checks/cpu-pmu-smp.c` on `platform/qemu-smp`
with two cores, once with `-cpu cortex-a55` and once with `-cpu cortex-a76`,
under `-icount shift=0`. Each core runs a pinned task that measures a loop of
2,000 instructions while the other core measures its own; the check requires
`INST_RETIRED` equal to that count plus the empty region's, a nonzero cycle
difference, the correct core and MIDR, a supported mask equal to QEMU's
`PMCEID` bits, the refusal of a two-core affinity mask at begin, the
final-core-mismatch refusal when a peer moves a pinned task, and the refusal of
a nested region.

## Open work

Record `cpu-pmu-board-a76`, in a scheduled board session
([validation.md](../validation.md)):

1. Configure on core 6; record `PMCR_EL0`, `PMCEID0_EL0` and `PMCEID1_EL0`, and
   confirm that no exception occurs.
2. Measure empty-region cycles (provisional budget at most 100) and
   instructions on core 6.
3. Measure a K-instruction loop on core 6 and compare cycles per tick with the
   CPU clock report.
