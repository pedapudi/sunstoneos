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

## Stage 2: DMA completion timing

DMA is a separate later stage. Its known work:

- **The DMA wake path takes two task hops and a 1 ms fallback poll.** The completion interrupt wakes a service task, which
  then wakes the waiting task, and a 1 ms timer polls in case the interrupt is missed. Both break the event-driven driver
  rule, and the hops add to the 250 µs budget. The waiting task must be notified from the interrupt directly.
- **The DMA0 RAM-copy service has no board record.** Its one run was refused before launch. The board-qualified DMA scope
  is the cyclic DMA0 I2S0 loopback ([memory-dma.md](../status/memory-dma.md)).
- **The grants table still refuses the NPU beside the RAM-copy service** in the returning lifetime, for the same reason the
  GPU row existed. A host check that composes the DMA and NPU retirements must come first.
