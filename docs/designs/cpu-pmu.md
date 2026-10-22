# Design: per-core cycle and event counts for performance measurement

Status: **planned**; no code exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

Board A/B runs requested by other designs need cycle counts and cache and TLB
event counts per core: `CPU_CYCLES`, `DTLB_WALK` and `ITLB_WALK` in the
[memory architecture](memory-architecture.md), and cycles in the
[power authority](power-authority.md) and [NPU exact sum](npu-exact-sum.md)
designs. The existing `a733_cpu_cycles_sample`
([drivers/clock/a733_cpu_cycles_native.c](../../drivers/clock/a733_cpu_cycles_native.c))
reads only the cycle counter. This package adds a measurement region at EL1
on Cortex-A55 r2p0 (cores 0–5) and Cortex-A76 r4p1 (cores 6 and 7). A
region reads the cycle counter and six event counters at its start and end,
and the module writes the per-core differences into a fixed 64-byte record.

## Interface and data flow

`include/a733_cpu_pmu.h` declares two calls, implemented in
`platform/a733/cpu_pmu.c`:

- `a733_cpu_pmu_begin(struct a733_cpu_pmu_region *)` runs on the core to be
  measured. It configures the core's PMU if the configuration differs from the
  table below. It then zeroes the six event counters, clears their overflow
  flags and stores the core number, a per-core generation, `PMCCNTR_EL0` and
  `CNTPCT_EL0` in the caller's region.
- `a733_cpu_pmu_end(const struct a733_cpu_pmu_region *, struct a733_cpu_pmu_record *)`
  reads the counters and the overflow flags and writes one record.

The record is little-endian and fills one 64-byte line: format version, core,
counters used, supported-event mask, overflow mask, status, `MIDR_EL1`,
`CNTFRQ_EL0`, 64-bit cycle and counter-tick differences, and six 32-bit
event differences in the order `INST_RETIRED` (0x08), `L1D_CACHE_REFILL`
(0x03), `L2D_CACHE_REFILL` (0x17), `BUS_ACCESS` (0x19), `DTLB_WALK` (0x34)
and `ITLB_WALK` (0x35). The caller owns the region (on its stack) and the
record (in its result buffer). A board-test bundle or the USB0 evidence
channel copies the record once into its response, which is the copy those
paths already make for every result. The module allocates nothing, touches no
MMIO and does not involve the USB0 code.

The counters count everything on the core, including other tasks and
interrupt handlers that run inside the region. A measured task is therefore
pinned with `xTaskCreateStaticAffinitySet` or `vTaskCoreAffinitySet`, and the
A/B runs keep the measured core otherwise idle.

## Register sequence and sources

Pinned sources:

- the Cortex-A55 TRM 100442_0200_00 (r2p0),
  `~/.cache/sunstoneos/sources/cpu-continue/cortex-a55-trm.pdf`, SHA-256
  `1f71b74d1cf1cbf3c86dab85d1407c45355ab5015d12d68afc0dc64107f7dd97`;
- TF-A `0892ad4d` `include/arch/aarch64/arch.h`, under `cpu-entry/`;
- the vendor EL3 monitor `monitor.fex` from orangepi-build `a95129bf`, with
  its disassembly in `cpu-startup/monitor.disasm`.

Configuration, once per core and again only when a readback differs:

1. Read `PMCR_EL0.N` and use counters `0 … min(N, 6) − 1`. TRM D5.4 gives
   N = 6 on the A55 and states that `MDCR_EL2.HPMN` limits the counters that
   Non-secure EL1 controls. At Non-secure EL1, N reads as `HPMN`
   (unproven, see below).
2. Read `PMCEID0_EL0` and `PMCEID1_EL0`. Event e < 0x20 is implemented when
   bit e of PMCEID0 is set, and event 0x20 + k when bit k of PMCEID1 is set
   (TRM D5.2 and D5.3; `L2D_CACHE_REFILL` exists only if an L2 cache is
   present). An unimplemented event is left out of the supported mask.
3. Write `PMEVTYPER<n>_EL0` with the event number and all filter bits zero,
   and `PMCCFILTR_EL0` with zero. Both then count at EL0 and EL1 and not at
   EL2, the same filter `a733_cpu_cycles_enable` uses.
4. Write `PMINTENCLR_EL1` for bits 0–5 and 31 so that no overflow raises
   nPMUIRQ (TRM C2.5). Write `PMUSERENR_EL0` as zero so that EL0 access traps;
   its reset value is UNKNOWN (TRM table D5-1).
5. Write `PMCNTENSET_EL0` for the used counters and bit 31. Set `PMCR_EL0.E`
   and `LC` and clear `D`, never writing `P` or `C`, and keep `DP` and `X`
   (TRM D5.4). The cycle counter is then configured as
   `a733_cpu_cycles_enable` configures it, so either call may come first. An
   `ISB` follows, and the module reads the registers back.

Each region uses `PMEVCNTR<n>_EL0` and `PMEVTYPER<n>_EL0` directly and never
uses `PMSELR_EL0`, so an interrupt between a select and an access cannot
redirect it. Begin and end mask IRQs for their register accesses and issue one
`ISB` before reading the counters. The module never resets the cycle counter;
it zeroes the event counters at begin, so an event's overflow flag
(`PMOVSSET_EL0`) is set at end only when the event occurred at least 2³²
times. That event is then reported in the overflow mask with no count. An
event counting four per cycle at 2 GHz (estimated) needs about 0.5 s to
overflow.

**Not proven by a pinned source:**

- the A76 event numbers and counter count; the Cortex-A76 TRM (100798, r4p1)
  is not pinned;
- the `ISB` placement around counter reads, direct counter access, and the
  behavior of `MDCR_EL2.HPMN = 0` on Armv8.2 cores, all of which come from the
  Arm Architecture Reference Manual (DDI 0487), also not pinned;
- whether time spent in the EL3 monitor counts in the event counters.
  `MDCR_EL3.SPME` is 0, so Secure event counting depends on the SoC's debug
  authentication signals. Regions therefore exclude SMC calls.

The implementation pins DDI 0487 and the A76 TRM in
`checks/cpu-pmu-sources.json`, with hashes that the host check verifies.

## EL2 and EL3 controls

Both continuation paths write `MDCR_EL3 = 0x10000000` (`MDCR_MTPME_BIT`,
which is `MDCR_EL3_RESET_VAL` in TF-A arch.h line 796). `TPM` (bit 6) is 0,
so EL1 PMU accesses do not trap to EL3, and `SCR_EL3 = 0x535` makes EL1
Non-secure, where counting is permitted. `MDCR_EL3` needs no change.

`MDCR_EL2` needs a change. [platform/a733/cpu_continue.S](../../platform/a733/cpu_continue.S)
line 92 and [platform/a733-smp-cached/continue.S](../../platform/a733-smp-cached/continue.S)
line 103 write zero, which sets `HPMN` (bits 4:0, TF-A arch.h line 816) to 0.
EL1 then owns no event counter, and on Armv8.2 cores `HPMN = 0` is
CONSTRAINED UNPREDICTABLE. The vendor monitor instead writes
`PMCR_EL0.N | HPMD | HLP` (monitor.disasm `0x48004880`–`0x48004890`). **Request
to the primary agent:** at EL3, read `PMCR_EL0.N` and write it into
`MDCR_EL2.HPMN`, keeping `TPM`, `TPMCR` and `HPME` zero, in both files. In
`checks/a733-cpu-continue.py` (lines 196–197), the expected `mdcr_el2`
becomes 6 for the modeled `PMCR_EL0`. Until that change lands, the host check
of this package fails on the continuation sources, so no image uses event
counters on a core where `HPMN` is 0. An access trapped at EL2 or EL3 would
end in the EL1 synchronous-exception panic rather than a refusal.

## Interrupts, memory and synchronization

No PMU interrupt is used. Counts are read only at begin and end, and a region
costs no wake-up and no tick wait (section 2 item 1 does not apply because the
module waits for nothing).

The module keeps one 64-byte-aligned state line per core in `.bss`, 512 bytes
in all, in normal write-back memory. Each line holds the configured flag, the
counters used, the supported mask, `CNTFRQ_EL0` and the generation, and only
its own core writes it, with IRQs masked (section 2 item 6). PMU registers are
per core. No data crosses cores inside the module, so it uses no lock, `DMB`
or `DSB`. A caller that passes a record to another core uses a FreeRTOS queue,
a task notification or a release store, which order the record's bytes.

Each begin increments the generation of its core. End refuses the region with
a status code in these cases:

- the reading core differs from the beginning core (migration);
- the generation changed, meaning another region began on that core and reset
  its counters;
- `PMCR_EL0.E` reads 0, as after a warm reset of the core (TRM D5.4).

Begin also refuses outside EL1. Every refusal happens before any device
access and retains nothing (section 1.8 class 1). Nested regions on one core
are refused through the generation check.

## Reuse and removal

The cycle configuration matches `a733_cpu_cycles_enable`, so the two
coexist. `a733_cpu_cycles_enable` and `a733_cpu_cycles_sample` have no caller
outside `checks/cpu-clock-check.py`. Once this module lands, their owner can
remove them and the Unicorn section of that check; `a733_cpu_clock_timing`
stays, and the host decoder applies the rate rule of
[docs/cpu-clock.md](../cpu-clock.md) to a record's cycles and ticks. The
private `pmu_start` and `pmu_read` helpers in
`checks/memory-architecture-smp.c` and `checks/device-memory-capacity.c`
stay, because their inline two-instruction read is shorter than a call into
this module. The module uses none of the shared helpers. It has no MMIO,
ranges or mappings, and `a733_task_context_*` requires zero critical nesting,
which would refuse measurements inside critical sections.

## Budgets

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| Instructions retired in an empty region (begin's counter writes to end's counter reads) | None for this module; the fixture's inline `ISB`, `MRS` pair is about 2 (counted) | At most 24 on the A55 and A76 models | QEMU `-icount shift=0`, `INST_RETIRED` of back-to-back begin and end on each core |
| System-register accesses per begin and end, configured path | `a733_cpu_cycles_sample` pair: 22 accesses and 2 `ISB` for the cycle counter alone (counted) | At most 40 accesses and 2 `ISB`; no `DSB`, `DMB` or `SVC` | Unicorn access log in `checks/cpu-pmu.py`; QEMU `-d in_asm` trace |
| Locks, allocations, MMIO, wake-ups per region | Not applicable | Zero | Unicorn memory log: only the stack, the record and the core's own state line |
| Lines written by more than one core | Not applicable | Zero | Unicorn address log; static assertion of 64-byte alignment |
| Memory | Not applicable | Record 64 B; state 8 × 64 B; region at most 32 B | Static assertions; `nm` sizes in the host check |
| Empty-region cycles on cores 0 and 6 | Not measured | Provisional: at most 150 on the A55 and 100 on the A76 (estimated) | Board session |

## Checks

`checks/cpu-pmu.py` (host) runs the AArch64 object under Unicorn with modeled
system registers, as `checks/cpu-clock-check.py` does. It checks the event
encodings and filters, the `PMCNTENSET`, `PMINTENCLR` and `PMCR` values, the
absence of `P` and `C` writes, `N` below 6, unimplemented events, overflow
marking and every refusal. It decodes the record layout and checks the
`MDCR_EL2` write in both continuation sources.

`checks/cpu-pmu-smp.py` builds `checks/cpu-pmu-smp.c` on
`platform/qemu-smp` with two cores, once with `-cpu cortex-a55` and once with
`-cpu cortex-a76`, under `-icount shift=0`. Each core runs a pinned task that
measures a loop of K instructions while the other core measures its own loop.
The check requires `INST_RETIRED` = K plus the empty-region count on both
cores, a nonzero cycle difference, the correct core and MIDR, and a supported
mask equal to QEMU's `PMCEID` bits. It also requires the migration and
nested-region refusals.

## Left out

Sampling and PMU interrupts, EL0 access, caller-selected events, chained
64-bit event counters, the DSU cluster PMU, external debug (APB) access and
multiplexing of more than six events are left out. No current design requests
any of them, and each adds interrupt binding or shared state.

## Board evidence

Each item needs a scheduled board session under
[docs/validation.md](../validation.md), after the `MDCR_EL2` change:

1. Configure on cores 0 and 6 in a supervised session; record `PMCR_EL0`,
   `PMCEID0_EL0` and `PMCEID1_EL0`, and confirm that no exception occurs.
2. Measure empty-region cycles and instructions on cores 0 and 6.
3. Measure a K-instruction loop on both core types, and compare cycles per
   tick with the CPU clock report.
4. Confirm whether `L2D_CACHE_REFILL` is implemented on the A733's A55 cores.
