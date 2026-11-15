# Concurrent GPU, NPU and CPU work in one OS image

Status: stage 1 implemented, not yet board-qualified. Stage 2 (DMA) is planned.

The robotics target runs camera and lidar processing on the CPU while models run on the GPU and the NPU. This design
qualifies the GPU and the NPU running jobs at the same time inside the one OS image, under a CPU streaming load, and
measures the completion latency of each accelerator: the time from its completion interrupt to the waiting task running
again. The status rows it serves are "Concurrent CPU, GPU and NPU operation" in [application.md](../status/application.md)
and "DMA, GPU and NPU completion timing" in [latency.md](../status/latency.md). The latency budgets are 250 µs for DMA and
1,000 µs each for the GPU and the NPU.

## Parts that had never run together on the board

Before stage 1, every board record exercised the GPU and the NPU in separate images:

- **The returning lifetime refused the NPU beside the GPU.** [grants.c](../../apps/os/grants.c) carried a refusal row for
  that combination, because no host check composed the two retirements. The runner launches only the returning and the
  resident lifetimes, and a resident run returns no DRAM output, so no measured combined run was possible.
- **Shared clock gates and the power-domain controller.** Both devices need the PCK-600 power-domain controller clock
  (R_CCU `+0x1ac` bit 0) and sit behind CCU bus gates in the same words: the NPU AHB gate (`+0x5c0` bit 6) beside the GPU
  AHB gate (bit 7), and the NPU MBUS gate (`+0x5e0` bit 18) beside the GPU MBUS gate (bit 16). The shared-resource manager
  ([a733_gpu_resources.c](../../drivers/gpu/a733_gpu_resources.c)) leases the PCK clock to each client and never clears a
  shared gate on release. Only the host check
  [accelerator-shared-owner.c](../../checks/accelerator-shared-owner.c) had composed the two owners.
- **The GPU PLL beside a running NPU.** Powering the GPU off and on stops and restarts PLL_GPU0 (CCU `+0x0e0`) while
  PLL_NPU (CCU `+0x2a0`) clocks the NPU. No run had read PLL_NPU across such a transition.
- **Two completion interrupts on two cores.** The GPU interrupt (IRQ95) and the NPU interrupt (IRQ97) had never been
  delivered in the same image, and their timing stamps read different counters: the GPU stamped CNTVCT and the NPU
  timing application CNTPCT.

## What the host models assume about each other

- The GPU power model and the NPU startup model each own the shared gate words alone. Neither writes the other's bit, and
  each update is a read-modify-write under its own driver's register lock. The CCU gate words take a key field on write
  (`0x010000ff` for `+0x5c0`); a write with that broad mask clears every bit the mask covers that the writer did not read.
- Each model treats the PCK-600 clock as on for as long as any lease holds it. A GPU retirement that dropped the clock
  while an NPU job held its lease would stop the NPU's power domain mid-job.
- The GPU host checks model one task per device. They do not model the NPU interrupt on another core preempting the GPU
  retirement between two steps.

The extended host check models each core as its own thread, takes the real platform register lock, and fails on two
mutants: a GPU release that drops the PCK gate, and a GPU retirement that clears NPU AHB bit 6 through the broad key mask.
The grants row is removed in the same commit as that check.

## The combined order follows the vendor sequence

The vendor kernel makes both devices' shared resources permanent before either driver loads:

- The CCU driver registers the GPU AHB and MBUS gates and the NPU AHB and MBUS gates as `CLK_IS_CRITICAL`, so the clock
  framework enables them once and never disables them ([GPU reference, section 4](../reference-backsolve-gpu.md);
  [NPU reference, kernel clock and power-domain registration](../reference-backsolve-npu.md)).
- The PCK-600 driver writes every domain's delay row, registers each domain as on and enables the PCK clock without a
  release path.

SunstoneOS keeps the same end state: the gates and the PCK clock stay on after the first device starts, and no release
clears them. The difference is the order of first use. The vendor enables everything at boot; SunstoneOS enables each gate
when the first device that needs it starts. Starting the second device then finds its gates already on and its update
changes no bit. The vendor GPU driver also leaves PLL_GPU0 running (`CLK_IGNORE_UNUSED`); SunstoneOS stops it at every GPU
close, which is the transition stage 1 measures.

## The manual warns against PLL transitions during use

The A733 User Manual V1.00, section 4.1.4.1 "Generic PLL Frequency Modulation", under its sub-heading "PLL Disable"
(page 26), states:

> It is not suggested to enable or disable the PLLs during usage, as the PLL enable/disable will lead to the disturbance
> among PLLs, which will cause system instability. When the clock is not required, it is recommended to set the
> PLL_OUTPUT_GATE bit in the PLLx_CTRL_REG as 0.

The GPU close sequence clears the output gate first, then the lock enable, the PLL enable and the LDO enable
([a733_gpu_power.c](../../drivers/gpu/a733_gpu_power.c)), and the open sequence reverses it: four writes to stop, ten to
start. Stage 1 accepts the warning for its run and measures its effect:

- An image built with `SUNSTONE_ACCEL_PLL_WATCH` reads PLL_NPU before and after every PLL_GPU0 write, inside the GPU
  platform's register lock ([a733_gpu_platform_native.c](../../drivers/gpu/a733_gpu_platform_native.c)).
- The NPU completion path reads PLL_NPU again at the first completion after such a write
  ([npu-queue.c](../../apps/os/npu-queue.c)).
- The ring's `pll` record keeps the first word read, the OR of every difference from it, the count of reads with the LOCK
  bit (bit 28) clear, and the site of the first read that differed. The application fails the run on any difference or
  lock loss, and on any NPU output that differs from the simulator reference.

## Stage 1: GPU, NPU and streaming in the returning lifetime

The application [accel-overlap](../../apps/accel-overlap/README.md) runs on the A76 cluster with seven workers:

- Core 6 runs the main task, which owns the GPU queue and waits for its completions. IRQ95 is routed to core 6.
- Core 1 runs the NPU producer, which owns the EL0 ring and waits for its completions. IRQ97 is routed to core 1.
- Cores 2, 3, 4, 5 and 7 each run a streaming worker that reads, checks and rewrites 4 MiB of DRAM per sweep.

Five phases run in order: the GPU alone, the NPU alone, both, both with streaming, and three GPU close-and-reopen cycles
while NPU jobs stay in flight under streaming. Each of the first four phases has a serial segment and a pipelined segment:

- **Serial.** 2,000 GPU jobs, one at a time, each with rotated inputs and a checked output. The NPU runs single relu jobs,
  at least 4,000 and until the GPU segment ends, each output compared with the simulator reference.
- **Pipelined.** One second at the declared depths, four GPU jobs and eight NPU jobs in flight. The last output of each GPU
  slot and the last NPU output are checked.

Both devices stamp on CNTVCT. The wake latency of a job is the counter at the wait's return minus the counter at the entry
of the completion interrupt handler, kept only when the wait blocked and the notification completed the fence. The GPU's
stamps are `gpu_queue_last_irq_counter` and `gpu_queue_last_wake_counter`; the NPU's are `struct ss_npu_queue_wake` in an
image built with `SUNSTONE_NPU_WAKE_TIMING`. Each distribution is a histogram of 2-tick buckets up to 1.365 ms, with the
count above it and the true maximum kept separately.

The run ends within its 25 s `run_us`, with a 5 s return reserve inside the 60 s returning lifetime. The application
stops at the first output mismatch, PLL_NPU difference or lock loss, or thermal throttle. The decoder checks the other
limits from the result: each phase within 3 s, at least 1,000 NPU completions while a GPU job was in flight in the two
overlap phases together, the PLL write counts, wake sample counts and the top CPU point at the start and end.

### Stage 1 predictions

Wake latency, from the interrupt handler's entry to the waiting task running:

| Phase | p50 | p99 | Maximum |
|---|---|---|---|
| GPU alone | 2.5–4 µs | 8–15 µs | at most 150 µs |
| NPU alone | 1.5–4 µs | 4–12 µs | 10–80 µs |
| Both, no streaming | within 1 µs of alone | | at most 30 µs above alone |
| Both, with streaming | 0.5–3 µs above alone | | 40–250 µs |

Throughput with both devices and streaming, against each device's own phase alone: the GPU 0–15% lower, the NPU relu jobs
5–30% lower. The PLL watch sees 42 to 52 PLL_GPU0 writes (14 for each of three cycles, plus the first power-on when the ring
opened before it), at least 21 of them with an NPU job in flight, and no change in PLL_NPU. At least 95% of the serial GPU jobs
and at least half of the serial NPU jobs give a wake sample: a relu job takes about 5.5 µs and can finish before the
producer blocks, and such a job has no wake to measure. The decoder
([host.py](../../apps/accel-overlap/host.py)) compares every value with these ranges.

## Stage 1b: the loss follows the number of CPU streams

In stage 1, five CPU workers streaming 6.5 GB/s cut the GPU job rate by 63% and the NPU relu rate by 54%. Stage 1b
separates two causes: DRAM saturation, where every master waits longer for DRAM, and arbitration, where a bus setting ranks
the accelerators behind the CPU.

### The vendor sets no quality-of-service value for these masters

The vendor kernel, U-Boot and boot0 write no NSI (the A733's bus interconnect) priority, mode or bandwidth limit for the
GPU (port 6), the NPU (port 18) or the CPU ports (19 and 20). CPU traffic reaches DRAM through the AXI2HIF bridges at
0x0a030000 and 0x0a040000 and does not pass through the NSI, so an NSI priority can rank the GPU against the NPU but not
against the CPU. Our settings already equal the vendor's for all three masters.

### Method

The streaming sweep build of accel-overlap (`SUNSTONE_ACCEL_OVERLAP_SWEEP`) runs five one-second phases. In each, the GPU
(four jobs in flight) and the NPU (eight in flight) run pipelined together, and the last output of each GPU slot and the
last NPU output are checked. The phases differ in the streaming workers: none, core 2, cores 2-4, cores 2-5 and 7 (the
stage-1 set), and core 7 alone. Cores 2-5 are Cortex-A55 cores and core 7 is a Cortex-A76. A chase task on core 0 (an A55)
follows one random cycle through the 262,144 64-byte lines of a 16 MiB region, so each load waits for the one before. It
times 64 such loads per sample, which gives the loaded CPU memory latency in every phase.

### Results

One returning run on port 3-1 at the approved points (entry `1794709286170370328-b5d81897`). Losses are against the phase
without streaming, which reproduced stage 1 within 1% (GPU 7,317 and NPU 174,098 jobs/s).

| Streaming | Bandwidth | Chase median | GPU loss | NPU loss |
|---|---|---|---|---|
| none | 0 | 184 ns | 0 | 0 |
| core 2 | 5.40 GB/s | 338 ns (1.8×) | 25.4% | 11.4% |
| core 7 | 7.64 GB/s | 416 ns (2.3×) | 32.5% | 29.3% |
| cores 2-4 | 6.95 GB/s | 854 ns (4.6×) | 51.1% | 48.1% |
| cores 2-5 and 7 | 6.26 GB/s | 1,135 ns (6.2×) | 51.1% | 53.0% |

Every output matched its reference, PLL_NPU kept its word and lock, and no phase throttled. Four predictions missed. Core 2
streamed 5.4 GB/s against 3-5 GB/s predicted. Core 7 alone cost the NPU 29% against 40-65%. The chase median did not rise
with bandwidth. Each NPU job-time ratio fell 37-66% below the chase ratio, against a predicted agreement within 25%.

The decision rule chose neither cause. The cores 2-4 and stage-1 totals are within 15% of each other, as saturation
predicts, but the accelerators slow far less than the CPU chase. The arbitration criterion, an accelerator slowing more
than 1.5 times as much as the chase, holds in no phase.

The loss follows the number of concurrent CPU streams more than the bytes they move. One A76 stream moved 7.64 GB/s and
cost the NPU 29%; three A55 streams moved 6.95 GB/s and cost it 48%. Adding cores 5 and 7 lowered the total to 6.26 GB/s
and raised the NPU loss to 53%. More streams keep more requests waiting in the DRAM controller, and the extra rows they
open lower the total that DRAM delivers.

**At 1-2 GB/s of CPU traffic** the loss is estimated, not measured. The lowest measured point is one stream at 5.4 GB/s.
A straight line from no streaming through that point gives an NPU loss of 2-4% and a GPU loss of 5-9%. Queueing delay
grows faster than linearly with load, so the straight line is an upper estimate for one paced stream at that rate.

### Deeper pipelining cannot recover the loss

Stage 1 measured each device both serially and pipelined:

- **GPU.** A serial job took 102.8 µs from submission to return. Four jobs in flight gave 9,558 jobs/s, or 104.6 µs per
  job, so the GPU runs jobs one after another and the depth adds nothing. A 65-element add costs about 100 µs, almost
  all of it per-job work, and under streaming that per-job work stretched to 285 µs.
- **NPU.** A serial job took 8.4 µs; eight in flight kept the NPU busy at 5.6 µs per job. Under streaming the serial job
  took 14.8 µs and the pipelined job 12.2 µs. The device stays busy at depth 8 in both cases, so a deeper queue cannot
  raise the rate.

Both devices therefore lose time inside each job. The relu job moves 11,264 bytes in 5.6 µs (2.0 GB/s) without streaming,
far below what DRAM delivers, so the job's own memory accesses wait on latency. A larger job amortizes per-job work, and
larger transfers keep more requests in flight. Stage 1c measured larger jobs and found that they lose more.

### Hardware features that could bound CPU bandwidth

No register named here has been read on the board, and none may be written without a hypothesis that the coordinator
approves.

- **AXI2HIF bandwidth limiter**, at 0x0a030000 and 0x0a040000. +0x30 enables the limiter (bit 0) and each limit (bit
  16+p). +0x48 and +0x4c hold the bandwidth and saturation for the total, +0x50 and +0x54 for reads, +0x58 and +0x5c for
  writes. Source: the vendor kernel's bsp/drivers/bus/sunxi-nsi.h lines 410-422 and sunxi-nsi.c lines 336-491, built
  only with `CONFIG_AW_NSI_CPU_CHANNEL`, which the vendor A733 configuration leaves off. Boot0 writes +0x18 and +0x68 only.
  This is the only hardware bound on CPU bandwidth found.
- **CPU path select**, `CPU_DA_DDR_CTRL_REG` at 0x08000200 (A733 manual 5.1.6.5). Bit 0 set sends CPU traffic through
  AXI2HIF, and boot0 sets it. Cleared, CPU traffic would pass through NSI ports 19 and 20, whose limiter (port base
  0x02020000 + 0x200 × port: +0x10 mode, +0x28 bandwidth, +0x2c saturation) and priority (+0x14) would then apply.
  Whether the path may change after DRAM initialization is unknown.
- **Core prefetch controls**, `CPUECTLR_EL1` (S3_0_C15_C1_4). On the A55, L1PCTL (bits 15:13) bounds outstanding L1
  prefetches and L3PCTL (bits 12:10) sets the prefetch distance (Cortex-A55 TRM B2.30). On the A76, PF_DIS (bit 15)
  disables data prefetch and WS_THR fields (bits 25:18) set the write-streaming thresholds (Cortex-A76 TRM B2.32). These
  reduce speculative traffic; they do not cap demand traffic.
- **DSU controls.** CLUSTERBUSQOS_EL1 (S3_0_C15_C4_4) sets a quality-of-service value per scheme ID on the bus and limits
  nothing; CLUSTERPARTCR_EL1 (S3_0_C15_C4_3) partitions L3 ways (DSU TRM 100453 r3p0, B1.6 and B1.12). The A76, A55 and
  DSU implement no MPAM.

## Stage 1c: larger jobs lose more under CPU streaming

Stage 1c repeats the stage-1b streaming sets with jobs about 30 times larger, and adds two controls. Each device runs
alone without streaming, which measures how much the two devices slow each other. One core streams three interleaved
4 MiB areas instead of one, which opens more DRAM rows from the same core and separates the requesting core from row
interleaving.

### Method

The larger-job build of accel-overlap (`SUNSTONE_ACCEL_OVERLAP_LARGE`, with `apps/accel-overlap/manifest-large.json`)
runs nine one-second phases:

- **GPU job.** An add of two 262,144-element arrays: 2 MiB read and 1 MiB written per job, four jobs in flight in four
  1 MiB output slots. The GPU input lease is 2 MiB and the output lease 4 MiB, the first GPU leases on the board larger
  than one page; context creation and mapping succeeded.
- **NPU job.** A relu over a 1024 × 176 int16 tensor: 360,448 bytes read and 360,448 written per job, eight in flight.
- **Output checks.** Pipelined GPU jobs clean and invalidate only the first 64 bytes of each output. After each phase,
  every slot runs one serial job with that 64-byte range and one with the whole 1 MiB slot, and the second output is
  compared with its reference in full. The time difference between the two jobs is the cost of the 1 MiB output
  maintenance. The last NPU output of each phase is compared in full.
- **Phases.** GPU alone, NPU alone, both with no streaming, then both under the stage-1b streaming sets (core 2, core 7,
  cores 2-4, and cores 2-5 and 7), then both while core 2 or core 7 streams three interleaved areas.

The chase task on core 0 is unchanged from stage 1b.

### Results

One returning run on port 3-1 at the approved points (entry `1794711666388475314-76dd9c8e`, 34.4 s against a budget
of 55-70 s). Every compared output matched its reference, PLL_NPU kept its word and lock, no phase throttled and the top
point held at the start and end.

Without streaming, the GPU ran 225 jobs/s alone (4.48 ms per job, 0.7 GB/s) and the NPU 10,184 jobs/s alone (98 µs per
job, 7.3 GB/s). Together they ran at 64.7% (GPU) and 74.2% (NPU) of those rates. Losses below are against the phase with
both devices and no streaming, as in stage 1b.

| Streaming | CPU bandwidth | Chase median | GPU loss | NPU loss | Stage 1b GPU loss | Stage 1b NPU loss |
|---|---|---|---|---|---|---|
| none | 0 | 357 ns | 0 | 0 | 0 | 0 |
| core 2 | 3.86 GB/s | 488 ns | 35.6% | 35.1% | 25.4% | 11.4% |
| core 7 | 6.20 GB/s | 557 ns | 55.5% | 55.9% | 32.5% | 29.3% |
| cores 2-4 | 5.77 GB/s | 984 ns | 68.4% | 67.8% | 51.1% | 48.1% |
| cores 2-5 and 7 | 5.37 GB/s | 1,200 ns | 68.3% | 69.2% | 51.1% | 53.0% |
| core 2, three areas | 2.38 GB/s | 445 ns | 30.1% | 30.3% | | |
| core 7, three areas | 4.01 GB/s | 622 ns | 53.9% | 54.7% | | |

The 1 MiB output maintenance costs little next to the GPU job. The figures are the mean over the four slots of the
whole-slot job time minus the 64-byte job time.

| Phase | GPU slot job | 1 MiB maintenance |
|---|---|---|
| GPU alone | 4,484 µs | 20 µs |
| both, no streaming | 4,474 µs | 37 µs |
| core 2 | 6,373 µs | 58 µs |
| core 7 | 9,795 µs | −623 µs |
| cores 2-4 | 12,615 µs | 23 µs |
| cores 2-5 and 7 | 12,487 µs | 24 µs |
| core 2, three areas | 5,970 µs | 132 µs |
| core 7, three areas | 9,005 µs | 142 µs |

The negative value under core 7 means that the variation between serial jobs exceeds the maintenance cost.

Seventeen predictions missed:

- The GPU ran 225 jobs/s alone, against 300-2,000. Each work item loads one 4-byte word, so the add moves 0.7 GB/s.
- Together, the GPU kept 64.7% and the NPU 74.2% of their alone rates, against at least 80%. The two devices slow each
  other, so the comparison with stage 1b is confounded.
- Seven streaming losses exceeded their ranges: the NPU under core 2, and both devices under core 7, cores 2-4 and the
  stage-1 set.
- The GPU completed 48-148 pipelined jobs in each of the seven phases that run both devices, against at least 200.

### Larger jobs are not a lever

At the stage-1 set the large jobs lost 68.3% (GPU) and 69.2% (NPU), against 51.1% and 53.0% for the stage-1b jobs.
Against the alone rates the losses are 79% and 77%. The decision rule labels both devices worse.

The large relu moves 7.3 GB/s alone, so it is bandwidth-bound and shares the DRAM total with the CPU streams. CPU
streams plus accelerator traffic came to 7.2-8.8 GB/s in the single-area streaming phases and 6.5-6.7 GB/s in the
interleaved ones, against 5.9 GB/s with no streaming. The stage-1b relu moved 2.0 GB/s and waited on latency, so a
CPU stream took less from it.

### The loss follows the requesting core

Three interleaved areas on one core cost within 6 points of a single area on that core, while moving 35-38% fewer bytes:
core 2 moved 2.38 GB/s instead of 3.86 GB/s and cost 30.1% instead of 35.6% (GPU). More open rows from the same core
did not raise the loss, so bank and row conflicts from one core are not the cause. The loss follows which cores request
DRAM, which points at a limit on outstanding transactions or arbitration between the CPU path and the accelerators.

**At about 2.4 GB/s of CPU traffic**, one A55 core streaming three areas, both large-job devices lost 30%. This is the
first measured point near the 1-2 GB/s that a camera and lidar workload moves.

The next lever is CPU-side bounding. The AXI2HIF limiter words and `CPU_DA_DDR_CTRL_REG` listed under stage 1b can be
read without writes; writing either needs a hypothesis that the coordinator approves.

## Stage 2: DMA completion timing

DMA is a separate later stage. Its known work:

- **The DMA wake path takes two task hops and a 1 ms fallback poll.** The completion interrupt wakes a service task, which
  then wakes the waiting task, and a 1 ms timer polls in case the interrupt is missed. Both break the event-driven driver
  rule, and the hops add to the 250 µs budget. The waiting task must be notified from the interrupt directly.
- **The DMA0 RAM-copy service has no board record.** Its one run was refused before launch. The board-qualified DMA scope
  is the cyclic DMA0 I2S0 loopback ([memory-dma.md](../status/memory-dma.md)).
- **The grants table still refuses the NPU beside the RAM-copy service** in the returning lifetime, for the same reason the
  GPU row existed. A host check that composes the DMA and NPU retirements must come first.
