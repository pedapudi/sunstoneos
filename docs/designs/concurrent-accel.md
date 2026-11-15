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

Stage 1c repeats the stage-1b streaming sets with larger jobs: the GPU add grows from 65 to 262,144 elements and the
relu from 11,264 to 360,448 bytes per direction. It also adds two controls. Each device runs
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

The figures below are the mean over the four slots of the whole-slot job time minus the 64-byte job time. Without
streaming the 1 MiB output maintenance costs under 1% of the GPU job. Under streaming the difference is below the
variation between serial jobs, so this run does not resolve it.

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

The negative value under core 7 shows that variation directly.

Seventeen predictions missed:

- The GPU ran 225 jobs/s alone, against 300-2,000, which moves 0.7 GB/s. One unmeasured explanation is that each work
  item loads one 4-byte word, so few requests are in flight per group.
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
CPU stream would take less from it; this run did not measure that directly.

### Interleaved areas cost about the same as one area, at fewer bytes

Three interleaved areas on one core cost within 6 points of a single area on that core, while moving 35-38% fewer bytes:
core 2 moved 2.38 GB/s instead of 3.86 GB/s and cost 30.1% instead of 35.6% (GPU). The decision rule labels this
result "requesting cores, not row interleaving". The comparison changed two things at once, more open rows and fewer
bytes, so two readings fit it:

- **Requesting core.** The loss follows which cores request DRAM, through a limit on outstanding transactions or
  arbitration between the CPU path and the accelerators. Rows and bytes both matter little.
- **Rows offset bytes.** More open rows raise the loss by about as much as fewer bytes lower it.

DRAM-controller or NSI counters of requests, latency and row misses would separate the two; a run at equal bytes would
also separate them.

**At about 2.4 GB/s of CPU traffic**, one A55 core streaming three areas, both large-job devices lost 30%. This is the
first measured point near the 1-2 GB/s that a camera and lidar workload moves.

Under the stage-1c decision rule the next lever is CPU-side bounding: the AXI2HIF limiter and `CPU_DA_DDR_CTRL_REG`
listed under stage 1b, read without writes. That limiter bounds bytes per period, while stages 1b and 1c both show a
loss that follows bytes weakly. A counter run that measures outstanding requests and latency per master would show
whether a byte limit can help before any limiter write.

## The GPU add waits on memory four times per task

Alone, the stage-1c GPU add ran 4.48 ms per job and moved 0.70 GB/s. The time comes from the program and the
dispatch, as host analysis of the program and the queue shows:

- **Dispatch.** A job runs 8,192 workgroups, each one 32-lane task. The BXM-4-64 MC1 has one cluster, 64 USC slots
  and at most 156 USC tasks (pinned Mesa `src/imagination/common/device_info/bxm-4-64.h`). Less the 103 µs per-job
  overhead that a three-task job measured in stage 1, each task takes 535 ns, 539 GPU clocks at 1008 MHz.
- **Program.** The compiled `apps/queue-add/vector_add.cl` that stages 1 to 1c ran is 28 instruction groups with 10
  temps. (The add now takes its arguments in unified registers once per task, as described in "Arguments once per
  task" below.) It loads the argument words, `a[i]` and `b[i]`, and stores the sum, each through one DMA access of 4 bytes per lane (12 for the
  arguments). The pinned compiler's scheduler (`pco_schedule.c`) puts a data-fence wait after every DMA access and an
  extra fence after the store, so a task waits on memory four times in series. Only other resident tasks overlap
  those waits. No host source determines whether the hardware merges the 32 lanes' accesses into line requests.
- **GPU mapping.** Both leases use 256 KiB pages with the SLC-bypass bit set and the cache-coherent bit clear
  (`platform/a733/gpu_queue.c`), so every load that misses the GPU's own cache and every write-through store goes to
  DRAM. The mapping is ordinary memory, not device memory.
- **Clock.** Power-on programs PLL_GPU0 to 1008 MHz and the GPU clock to that source with divider 1, and checks both
  by readback. The manifest declares no `cpu.perf`, so session admission refuses a performance grant and the GPU
  performance service, the only path that changes the rate, does not start. The GPU supply read 960 mV, the
  1008 MHz point, at return. The run recorded no per-phase clock.

If the waits bound the task, 535 ns equals four memory round trips divided by the resident tasks, so the round trip
is 134 ns times the resident tasks. The unified-store sizing of [the GPU recognizer design](gpu-mimus-compute.md)
allows 89 tasks at 10 temps, so the 64 USC slots would bound residency, and the round trip would be 8.6 µs, against a
205 ns CPU chase in the same phase. The waits therefore do not explain the time at full residency. Either few tasks
are resident (2-4 tasks give a 270-540 ns round trip), or a per-request limit binds: one request per lane access
gives 1,048,576 requests per job, 4.2 ns each. A wider program cannot separate the two, because both predict the
same gain. The request count of the GPU's NSI port can, if those counters count live on the board, which is not yet
shown. No vendor figure for this GPU's buffer bandwidth exists in the datasheet, the manual or the pinned GPU sources.

### Four elements per lane triple the GPU add

The width build of accel-overlap (`SUNSTONE_ACCEL_OVERLAP_WIDTH`) runs the GPU-alone phase with
[add4/vector_add4.cl](../../apps/accel-overlap/add4/vector_add4.cl). Each lane loads four consecutive words of each
vector with one access and stores the four sums with two 2-word stores, so a job runs 2,048 workgroups. The program
that this run used loaded its arguments in every lane; it had 18 temps and five serial memory waits per task. The
add now takes its arguments in unified registers once per task, and the counter build's per-lane mode reproduces the
earlier form.

One returning run on port 3-1 (entry `1794714617637336813-d6d9ac0d`) gave:

| Measurement | One element per lane (stage 1c) | Four elements per lane | Predicted |
|---|---|---|---|
| Pipelined rate | 225 jobs/s (0.70 GB/s) | 676 jobs/s (2.13 GB/s) | 550-850 jobs/s |
| Serial slot job | 4,484 µs | 1,495 µs | 1.2-1.8 ms |
| 1 MiB output maintenance | 20 µs | 104 µs | |

Every compared output matched its reference, the run returned with no GPU fault, PLL_NPU kept its word and lock, and
the top point held. The gain of 3.0 times matches five waits carrying four times the bytes of four waits, at unchanged
residency. Few resident tasks and a per-request limit both predicted this gain, so the run does not separate them;
the request count per job of the GPU's NSI port can.

Mapping the leases without SLC bypass, so that GPU misses can use the system-level cache, is a separate candidate. It
needs its own hypothesis after the NSI counters are shown to count.

### NSI counters

The request count per job on the GPU's interconnect port separates the two explanations. The A733 interconnect (NSI)
gives each initiator port a bank of traffic counters at 0x02020000 + 0x200 × port + 0xc0: an enable word, a clear
word, a period word, and read-only counts of read and write requests, data (16-byte units) and latency. Port 6 is the
GPU and port 18 the NPU (vendor sunxi-nsi.h lines 377-385; sun60iw2p1.dtsi ia_pmu_data_unit).

The counter owner ([drivers/dram/a733_nsi_counters.c](../../drivers/dram/a733_nsi_counters.c), OS option
`SUNSTONE_ACCEL_NSI_COUNTERS`, off by default) holds only these two banks. It reads all eighteen words before any
write and keeps them as reset values, and refuses a bank whose enable bit is already set. A phase follows the vendor's
sequence (`__nsi_pmu_timer_store_v2`): period, disable, clear, enable, then 10 µs. Each written word is read back. The
end of a phase disables the bank, reads the counts twice 10 µs apart, and restores the period, clear and enable words
to their reset values. A failed readback restores every programmed bank and refuses later writes. A failed session's
GPU quiesce restores any bank left programmed. The owner writes six words in all, three per bank, and no QoS,
priority, mode or limit word. [checks/nsi-counters.c](../../checks/nsi-counters.c) runs the sequence against a
register model, with a failing or corrupt access injected at every read, write and readback.

The vendor device tree places port 6 in the GPU_TOP power domain and port 18 in the NPU domain. A write to a port
in a powered-off domain can hang the interconnect, so the OS owner
([drivers/dram/a733_nsi_counters_guard.c](../../drivers/dram/a733_nsi_counters_guard.c)) refuses every access to
the port-6 bank while GPU_TOP is not on. The GPU power controller calls it when a power-off begins, before any OFF
step, and the owner then restores the bank while GPU_TOP is still on. From then until the controller finishes the
next power-on, a call that would reach the bank is refused, and the bank starts refused until the GPU's first
power-on. Each access sets an in-flight bit before it tests the refusal, with IRQs masked, so a power-off that
arrives during a call refuses the rest of that call and waits for at most the one access already under way. Such a
bank, and any bank whose restore missed, may hold programmed words while it is refused. The next power-on restores
its CYCLE, CLR and ENABLE words to the values OPEN read, each read back, while GPU_TOP is on; if a call holds the owner
at that moment, the call restores the bank before it returns. The guard therefore does not depend on whether
GPU_TOP's power-on resets the NSI words.
[checks/nsi-counters-gpu-power.py](../../checks/nsi-counters-gpu-power.py) runs the owner with the GPU platform and
power controller against a modeled GPU_TOP: no access when the counters are unused, the restore before the TOP5 OFF
command, refusal with no access while GPU_TOP is off, a power-off in the middle of a call, use after the next
power-on, and the restore at a power-on that keeps the words, from the power-on and from a call that holds the owner.
Moving the restore after the OFF command, dropping the refusal, or dropping either power-on restore makes it fail.
The NPU domain stays on once the NPU has started: its local stop leaves domain 4 on (drivers/npu/a733_npu_startup.c).

The application reaches the owner through the GPU call gateway, from the GPU owner task only. An earlier counter build
of accel-overlap ran four counted phases: idle for 100 ms, then the GPU alone, the NPU alone and the core-2 stream
alone, one second each. Those phases set the period word to its largest value, 7.16 s at the 600 MHz NSI clock, and
sampled at half of each phase. Two window probes followed: the GPU alone on port 6 and the NPU alone on port 18, 600 ms each, with
a 100 ms period and a sample every 5 ms. The vendor's readers print each count times its unit with no division by
time, and the driver calls the period a sample timer, so the probes tested whether a count only becomes visible when
a period ends.

One returning run on port 3-1 (entry `1794721288049775573-7d82471a`) showed that the counts latch:

- **Counted phases.** Every count read 0 in all four counted phases, before and after the disable, because no
  period ended inside them.
- **Window probes.** Each count stayed 0 until the first period end, 100.1 ms after enable on port 6 and 101.1 ms on
  port 18. It then stepped once per period, 100.2 ms and 100.0 ms, to that period's total, and held that value
  until the next period end.
- **The period clock.** The NSI clock runs at 600 MHz, as the vendor device tree assigns it to both ports
  (`master_clks`).
- **Readbacks and rates.** Every readback matched, open and close returned OK, and the GPU and NPU rates fell in
  their bands.

A count therefore reports a completed period only. **Every phase that reads the counters needs a period that ends
inside it, and the owner must sample at the period ends:** each sample comes just after an end and is that period's
total. A phase shorter than one period reads 0.

The probe periods gave these counts per job:

| Port and direction | Data units (16 B) | Ratio to the bytes moved / 16 | Requests | Mean latency |
|---|---|---|---|---|
| GPU, read | 201,867 | 1.54 | 102,859 | 421 ns, 29.1 requests outstanding |
| GPU, write | 131,235 | 2.00 | 32,833 | |
| NPU, read | 36,172 | 1.61 | 3,230 | |
| NPU, write | 32,790 | 1.46 | 2,690 | |

The GPU counts fit a model of the four-elements-per-lane add in which a data unit is one 16-byte beat:

- **Vector loads:** each lane's two 16-byte loads merge into 32,768 four-beat line requests, 131,072 beats.
- **Argument loads:** each of the 65,536 lanes loads the 12-byte argument block in its own one-beat request. These are
  48% of the port's requests.
- **Stores:** each 8-byte store fills a beat, so the two stores per lane give 131,072 beats in 32,768 requests.
- **Residual:** 4,555 read requests and 5,259 read beats remain, with 65 write requests and 163 write beats.

The NPU relu (1,024 rows of 176 16-bit values) writes 32,768 units, 512 bytes per 352-byte row, within 0.07%. Its
reads are those units plus a residual of 3,404 units in 1,182 requests. Its write requests (2,690) fit neither 16-beat
bursts (2,048) nor splits at 256-byte boundaries (2,944).

The two CPU bridges (AXI2HIF, 0x0a030000 and 0x0a040000) have counters too. The vendor driver accesses them behind
a clock (`cpu_direct`, CCU 0x0c00 and 0x0c0c) and a selector bit (0x08000200) that no capture from port 3-1 has read.
They are left to a later stage.

### Arguments once per task

Two explanations fit the GPU counts of the four-elements-per-lane add:

- **A request-rate limit.** The GPU's path to DRAM serves a fixed number of requests per second. The add sustained
  69.2 M read requests/s (673.2 jobs/s times 102,859 requests).
- **Low residency.** About two tasks are resident at a time, each waiting on its memory accesses in series. Two
  resident tasks at the measured 421 ns read latency predict 28.5 reads outstanding, against 29.1 measured.

A program that loads its arguments once per task separates them. It removes the 65,536 per-lane argument requests, 64%
of the read requests, and one of the five serial waits. A request-rate limit then gives at least 1.5 times the job
rate; residency gives at most 1.25 times, since four waits replace five.

**The program.** [vector_add4_inline.cl](../../apps/accel-overlap/add4inline/vector_add4_inline.cl) uses the inline
argument ABI of [the compute queue](gpu-compute-queue.md#programs-keep-the-compilers-two-pointer-register-layout), in
which the PDS writes the argument block into unified registers once per task. A job's block is binding 1's address,
a count and a mode word. The queue creates one GPU context per boot, and a context holds one program, so both GPU
phases run this program:

- **Mode 1, arguments per lane:** binding 1 is a 12-byte record of the input address and count, which every lane
  loads, as the four-elements-per-lane add loads its argument block. This phase repeats the earlier measurement.
- **Mode 0, arguments once:** binding 1 is the input itself, and no lane loads its arguments.

The mode is the same for every lane of a job, and both modes compile into one program with 18 temps, as the
four-elements-per-lane add has. [checks/gpu-queue-inline.py](../../checks/gpu-queue-inline.py) checks that the queue's
PDS data and program write the unified registers this program's compiler record reads.

**The counter build.** The counter build of accel-overlap runs eight phases in order: idle, GPU mode 1, GPU mode 0,
the relu at 176, 128, 192 and 256 values per row (1,024 rows each), and the core-2 stream. Each phase starts its
devices, waits 20 ms, then counts both ports over five 100 ms periods. It samples both banks 3 ms before and 2 ms
after each period end, so each pre sample shows the previous period's total and each post sample shows the period just
ended. Every job count is sampled at the same times, and the decoder interpolates the jobs of each period between its
two samples. The core-2 stream and idle are controls: neither moves data through ports 6 or 18. The result is
9,496 bytes, and [host.py](../../apps/accel-overlap/host.py) decodes it with the predictions below
([checks/accel-overlap-counters.py](../../checks/accel-overlap-counters.py) decodes modeled records of each outcome).

**GPU predictions, per job.** Mode 1 must repeat the earlier counts within 2% and its rate within 10%; a miss voids
the comparison. Mode 0 drops the argument requests and their beats and keeps the writes.

| Phase | Read requests | Read units | Write requests | Write units | Jobs/s |
|---|---|---|---|---|---|
| GPU mode 1 | 102,859 | 201,867 | 32,833 | 131,235 | 606-741 |
| GPU mode 0 | 35,000-40,000 (37,323) | 136,331 | 32,833 | 131,235 | Request rate: 1,302-1,855; residency: at most 841 |

If reads alone are limited, mode 0 runs at mode 1's read-request rate: 69.2 M/s, 1,855 jobs/s, 2.76 times mode 1. If
reads and writes share the limit, it runs at mode 1's combined rate: 91.4 M/s, 1,302 jobs/s, 1.93 times. A ratio of at least 1.5 decides for the request-rate model, at most 1.25 for residency, and a ratio between
them decides neither. At 1,855 jobs/s the add would move 5.8 GB/s of vector data, so DRAM bandwidth may bound the
gain first.

**NPU predictions, per job.** Two rules fit the relu's 32,768 write units at 176 values per row:

- **Rows:** every row of at most 512 bytes writes one 512-byte unit.
- **Bursts:** each row rounds up to a whole number of 256-byte bursts.

| Width | Bytes per row | Known units | Rows rule | Bursts rule |
|---|---|---|---|---|
| 176 | 352 | 22,528 | 32,768 | 32,768 |
| 128 | 256 | 16,384 | 32,768 | 16,384 |
| 192 | 384 | 24,576 | 32,768 | 32,768 |
| 256 | 512 | 32,768 | 32,768 | 32,768 |

A rule holds when its write units fit every width within 1%. Width 128 separates the two rules, and width 192 tests
both against the known bytes. Each width's read units are the rule's units plus the 3,404-unit residual, within 3%.
No rate model exists for the other widths, so their rates are checked only at 1,000 jobs/s or more. The controls must
move fewer than 10,000 units per period on each port, 0.05% of a GPU period.

**What a rule means for tile graphs.** The tile-graph planner of
[npu-tiled-execution.md](npu-tiled-execution.md) chooses tensor shapes without regard to row length. Under the rows
rule, a tile whose rows hold fewer than 256 16-bit values moves 512 bytes per row: a 176-value row costs 1.45 times
its bytes, and a 128-value row twice. The planner would then prefer innermost dimensions of 256 values. Under the
bursts rule, rows round up to multiples of 128 values: 128 costs nothing extra and 176 costs as much as 256. If
neither rule holds, the counts give no shape rule. The planner stays unchanged until a run decides.

**Results.** One returning run on port 3-1 (entry `1794731628675614053-3bc9fbc0`, image built from 3c1d021c7) gave
these counts per job:

| Phase | Read requests | Read units | Write requests | Write units | Jobs/s |
|---|---|---|---|---|---|
| GPU mode 1 | 102,767 | 201,687 | 32,800 | 131,107 | 699.9 |
| GPU mode 0 | 37,219 | 136,118 | 32,797 | 131,096 | 1,080.2 |
| NPU, 176 values per row | 3,228 | 36,145 | 2,688 | 32,765 | 10,189 |
| NPU, 128 | 3,100 | 24,884 | 2,048 | 20,480 | 14,654 |
| NPU, 192 | 3,612 | 37,169 | 3,072 | 30,718 | 10,257 |
| NPU, 256 | 5,149 | 45,373 | 4,097 | 40,968 | 8,189 |

Every sample fell on the predicted side of its period end, every pre sample showed the previous period, idle and the
core-2 stream counted 0 on both ports, every output matched its reference, and PLL_NPU and the top point held.

- **GPU: both models missed.** Mode 1 repeated the four-elements-per-lane counts within 0.2% and ran 4% faster.
  Mode 0 dropped 65,548 read requests per job, the 65,536 per-lane argument loads, and kept the writes within 0.05%.
  The rate rose 1.54 times. The decoder's threshold of 1.5 classed this as the request-rate model, but that model
  predicted 1,302 to 1,855 jobs/s and the run measured 1,080, below its range; residency predicted at most 1.25
  times, which the run exceeded. The read-request rate fell from
  71.9 M/s to 40.2 M/s, and the mean read latency from 245.6 to 173.1 NSI cycles. Reads outstanding, rate times
  latency, fell from about 29 to about 12. The port therefore did not run at a fixed request rate in mode 0.
- **NPU: the row-rule prediction missed.** Neither rule fits the four widths, and the read units missed their
  range at 128 and 256 values per row. At 128, 192 and 256 values per row, each write request carries 10 units and
  there is one request per 64 values, so the write units are 1.25 times the bytes written divided by 16. At 176
  values per row the write units equal 512 bytes per row, 1.45 times the bytes. The rows of 192 values moved fewer
  write units than the rows of 176 values, at the same job rate.

| Width | Write units, rows rule | Bursts rule | Measured | Read units predicted (rows rule) | Measured |
|---|---|---|---|---|---|
| 176 | 32,768 | 32,768 | 32,765 | 35,087-37,257 | 36,145 |
| 128 | 32,768 | 16,384 | 20,480 | 35,087-37,257 | 24,884 (miss) |
| 192 | 32,768 | 32,768 | 30,718 | 35,087-37,257 | 37,169 |
| 256 | 32,768 | 32,768 | 40,968 | 35,087-37,257 | 45,373 (miss) |

The arguments-once gain is measured; the GPU throughput model and the NPU row rule remain open. The run returned in
4.50 s from arming, with 4.23 s of phases, against a 55 s session bound.


### Width 144: per-row writes, contiguous chunks or whole units for misaligned rows

At 128, 192 and 256 values per row, the relu writes one request of 10 units per 64 values. Two forms fit those widths:

- **A, per row:** each row writes ceil(width / 64) requests of 10 units.
- **B, contiguous:** the output is written in 64-value chunks across row ends, 10 units each.

Both forms miss the measured counts at 176 values per row, where each 8 rows took 21 write requests and 256 units: A gives
24 and 240, B gives 22 and 220. A 176-value row holds 352 bytes, 32 bytes off a multiple of 64, and the aligned widths have
no such offset. A third form takes its basis from the 176 measurement:

- **C, whole units for misaligned rows:** a row whose length in bytes is not a multiple of 64 writes one 512-byte unit
  (32 units). C predicts no request count.

A 144-value row holds 288 bytes, also 32 bytes off a multiple of 64, so 144 separates the three forms.

**The run.** The counter build runs width 144 in place of 192, as phase 5 of the eight, with the graph `relu-w144` of
`tools/npu-fixtures.json` (1,024 rows of 16-bit values). The other phases repeat run `1794731628675614053-3bc9fbc0`
and serve as controls.

**The job-time fit.** That run's four widths give a relu job time of 14.3 µs plus 2.63 ns per write unit. Its residuals
are within 2.5%:

| Width | Write units | Fit (jobs/s) | Measured (jobs/s) |
| --- | --- | --- | --- |
| 128 | 20,480 | 14,671 | 14,654 |
| 176 | 32,765 | 9,953 | 10,189 |
| 192 | 30,718 | 10,517 | 10,257 |
| 256 | 40,968 | 8,194 | 8,189 |

**Predictions per job at 144 values per row.** Jobs/s comes from the fit, within 5%:

| Form | Write requests | Write units | Jobs/s |
| --- | --- | --- | --- |
| A | 3,072 (24 per 8 rows) | 30,720 | 10,516 (9,990-11,042) |
| B | 2,304 (18 per 8 rows) | 23,040 | 13,352 (12,684-14,020) |
| C | not predicted | 32,768 | 9,952 (9,455-10,450) |

The read requests are the same in every form, so they do not decide among them. The output tiles across a row (two at
144 values, each 127 values wide) plus floor(width / 32) / 4 give 3 per row, plus 28 per job: 3,100 within 5%. This
rule fits 128, 192 and 256 and missed 176 by 4%. No form predicts the read units; they are reported against the range
from 128 to 176 values per row, 24,884 to 36,145.

**Decision order.** [host.py](../../apps/accel-overlap/host.py) applies it, and
[checks/accel-overlap-counters.py](../../checks/accel-overlap-counters.py) decodes a modeled record of each outcome:

1. A control that misses voids the run. The controls are both GPU modes, which repeat their counts per job within 2%,
   and the relu at 176, 128 and 256 values per row, which repeats its write requests and units within 1% and its rate
   within 5%. Idle and the core-2 stream move under 10,000 units per period on each port.
2. The write units at 144 values per row pick A, B or C within 1%. The forms lie at least 6.7% apart.
3. The write requests confirm A or B within 1%. Counts that fit no form decide nothing.

**What each outcome changes in the tile planner** ([npu-tiled-execution.md](npu-tiled-execution.md)):

- **A:** a row costs ceil(width / 64) times 10 units, so the planner picks innermost dimensions in multiples of 64
  values. A 144-value row then costs as much as a 192-value row.
- **B:** the cost follows the bytes written (1.25 units per 16 bytes) with no rounding per row. The planner needs no
  row-length rule.
- **C:** a row that is not a multiple of 64 bytes costs 512 bytes. The planner sets the output row stride (descriptor
  word 17, [npu-command-formats.md](../hardware/npu-command-formats.md)) to a multiple of 64 bytes, or picks widths
  that are.
- **No form:** the planner stays unchanged, and 144 becomes a fourth constraint for host analysis.

The planner changes only after a second width, predicted before its run, also fits:

- **After A or B:** 160 values per row. A predicts 24 requests and 240 units per 8 rows; B predicts 20 and 200.
- **After C:** 240 values per row, a 480-byte row 32 bytes off a multiple of 64. C predicts 32,768 units per job, A
  40,960 and B 38,400.

## Stage 2: DMA completion timing

DMA is a separate later stage. Its known work:

- **The DMA wake path takes two task hops and a 1 ms fallback poll.** The completion interrupt wakes a service task, which
  then wakes the waiting task, and a 1 ms timer polls in case the interrupt is missed. Both break the event-driven driver
  rule, and the hops add to the 250 µs budget. The waiting task must be notified from the interrupt directly.
- **The DMA0 RAM-copy service has no board record.** Its one run was refused before launch. The board-qualified DMA scope
  is the cyclic DMA0 I2S0 loopback ([memory-dma.md](../status/memory-dma.md)).
- **The grants table still refuses the NPU beside the RAM-copy service** in the returning lifetime, for the same reason the
  GPU row existed. A host check that composes the DMA and NPU retirements must come first.
