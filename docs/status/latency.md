# Timing qualification remains open

SunstoneOS has optional per-core maximum measurements and an eight-core load
diagnostic. Physical worst-case timing remains unmeasured. The native diagnostic
is a CPU control; it does not contain a returning DMA service or accelerator jobs.

## Initial response budgets

These are engineering acceptance targets for the existing 1 kHz scheduler.
They allocate fractions of its 1 ms period; they are not vendor guarantees or
measured worst-case execution bounds. Device budgets cover software completion
delivery. Transfer size and accelerator execution time require separate bounds.

| Region | Budget | Basis | Measured maximum |
| --- | ---: | --- | --- |
| Timer IRQ assertion to C handler entry | 50 µs | 5% of one tick | Host QEMU: 54,674.144 µs |
| IRQ-masked or kernel critical region | 25 µs | Half the IRQ budget leaves entry and routing time | Host QEMU prefix: 24,454.160 µs |
| Kernel ISR lock acquisition / outer hold | 25 µs each | Each contributes to the masked-region budget | Host QEMU: 0.048 / 24,453.856 µs |
| Kernel task lock acquisition / outer hold | 25 µs each | Each contributes to the masked-region budget | Host QEMU: 909.424 / 24,856.656 µs |
| Highest-priority ready task on an idle core | 100 µs | 10% of one tick | Host QEMU: 402.528 µs |
| Highest-priority ready task on a busy core | 250 µs | 25% of one tick | Host QEMU: 939.104 µs |
| DMA IRQ notification to completion observation | 250 µs | 5% of the minimum 5 ms audio period | Host QEMU, modeled DMA: 1,952.480 µs |
| GPU active-job IRQ to client receipt | 1,000 µs | One quarter of the 4 ms in-flight fallback | Separate two-core host fixture: 1,012.176 µs |
| NPU mailbox publication to owner dispatch | 1,000 µs | At most one scheduler period | Uninstrumented; board unmeasured |

The IRQ row measures the architectural timer, whose programmed compare value
defines assertion. DMA and GPU lack an equivalent captured device-edge timestamp.
Their software observations do not establish device-IRQ entry latency.
Every board maximum is unmeasured. The host values exceed the targets except
ISR lock acquisition. Instruction-counted virtual time includes emulator CPU
interleaving and startup; it cannot establish a board overrun or a hardware fix.

## Measurement boundaries and coverage

`SUNSTONE_LATENCY` selects [the recorder](../../runtime/latency.c). Disabled builds
contain no timing calls, counters, buffers or IRQ wrapper. The recorder uses
`ISB; MRS CNTVCT_EL0`; counter frequency converts ticks to microseconds.
It records count, maximum, starting counter and call site for each core and region.
It retains the first over-budget sample's start, duration and call site separately.
The budget table, core, counter frequency and region identify that check's inputs.
No average replaces a maximum, and instrumentation overhead is not subtracted.

The [port patches](../../patches/freertos-smp-c.patch) cover priority masking and
both recursive kernel locks. Context restoration closes an interval before
assembly unmasks the priority register. Nested entries retain the outer start.
Mask samples end before recorder bookkeeping, restoration barriers and exception
return. Lock-hold samples end before recorder bookkeeping and the unlock tail.
These prefixes can expose an overrun but cannot prove the complete 25 µs bounds.
Raw DAIF regions in storage state inspection, PMU sampling, boot and terminal
paths need separate boundary probes. They have no measured maximum here.

DMA claims an IRQ timestamp before its channel sweep and finishes that captured
sample after polling and buffer maintenance. An IRQ during the sweep remains
pending for the next sweep. GPU timestamps are armed only for an active job and
travel through a private per-client slot to `gpu_queue_wait`. The first service
IRQ during a job can precede its completion IRQ, so the GPU interval can include
remaining device execution. No public GPU record layout changes.

The load diagnostic pins a highest-priority wake task and a 1 MiB memory worker
to every core. Each sweep reads, verifies and rewrites the complete extent.
It admits 64 idle and 128 busy wake samples per core. The ready-list hook
timestamps after insertion under scheduler locks and checks the running task:
any kernel idle task for an idle sample, that core's memory worker for a busy
sample. CPU0's timer wakes remote cores through scheduler SGIs. Timer entry
coverage remains CPU0 only. Each core freezes its recorder before snapshot.
The traffic pattern exercises sequential memory access; it does not establish
the speech workload's measured bandwidth or cache-miss distribution.

## Findings and proposed changes

- Host IRQ, masking, lock-hold and task-lock-wait maxima exceed their targets
  at [timer dispatch](../../runtime/latency_irq.c:6) and
  [the recursive port lock](../../patches/freertos-smp-c.patch:386).
  Symbolization identifies `vTaskEnterCritical`, `vTaskSuspendAll` and
  `vTaskSwitchContext` in the generated kernel. Measure these holders on the
  board before moving work or changing the locking algorithm.
- Host idle and busy scheduling maxima exceed their targets at
  [the wake task](../../apps/latency/workload.c:83). Keep admission against the
  actual running task and measure on the board before changing affinity or
  scheduler selection.
- Host DMA and GPU delivery maxima exceed their targets at
  [the DMA completion sweep](../../drivers/dma/dma_service.c:330) and
  [GPU client receipt](../../drivers/gpu/gpu_queue.c:159). Bound cache-maintenance
  work and measure client placement with physical completions before a fix.
- [The late-tick diagnostic](../../platform/a733-smp-cached/perf_session.c:173)
  masks interrupts for 5,500 µs. This exceeds 25 µs by construction. Keep it
  outside latency qualification and retain it as a separate catch-up test.
- [Kernel lock contention](../../patches/freertos-smp-c.patch) has no timeout or
  fairness bound. Bound holder work and measure contention before considering a
  lock redesign. A lock timeout cannot safely substitute for scheduler ownership.
- [The DMA worker](../../drivers/dma/dma_service.c:327) scans channels in order;
  earlier channels' cache maintenance delays later completion observation.
  Measure admitted buffer sizes before splitting or budgeting that work.
- [NPU mailbox publication](../../runtime/npu_app.c:43) does not notify its owner.
  [The normal owner loop](../../apps/os/npu-client.c:93) waits one tick, so dispatch
  can approach 1 ms plus scheduling. The 25 ms waits govern opening and stopping.
  Notify the owner on publication and retain bounded fallback; the NPU owner
  implements and measures that change separately.

No production region is classified as physically over budget without board
measurements. No clock, voltage, power or storage behavior changes.

## Validation and board prerequisites

[The deterministic recorder check](../../checks/latency-recorder.c) covers maxima,
nested regions, first-fault inputs, startup exclusion, a mid-sweep IRQ and freeze.
[The host check](../../checks/latency.py) checks per-core sample and traffic counts
and reports overruns independently of functional coverage. QEMU uses instruction
counting; all its durations are host-model results. GPU queue cases 1 and 40
pass with timing enabled, including client receipt and two independent callers.
The combined model completes 25 continuous 4 KiB RAM-copy jobs and 64 idle plus
128 busy samples on every core in 4,101 scheduler ticks. The synthetic DMA client
shares priority 1 with memory workers; the real DMA service remains priority 4.
Memory traffic stops after sampling, before the unchanged DMA close deadline.
The run log, ELF and exact source hashes are under
`~/.cache/sunstoneos/build/latency-1101/load/final-corrected/`; decoded maxima
and symbolized locations are under `latency-1101/host-final/` in that build root.
The combined model ELF SHA-256 is
`d8443ea4cfd309bcffa095e0aba7c26a5da41f9bb2c624c1b0082135331ddc3d`.
The separate GPU receipt fixture is under `latency-1101/gpu-latency-receipt/`.
Native OS and application objects compile with timing enabled; no native
diagnostic image is frozen while the board queue is unavailable.

Physical DMA needs an admitted DMA0 handoff and a returning service composition.
The current pool retains its parent allocation and clock token, and the returning
CPU profile excludes audio. Reusing its RAM-copy code does not establish drain,
worker retirement, IRQ restoration or clock restoration. The combined physical
image and repeated timing runs remain required after those prerequisites.

The CPU control's native selector is `SUNSTONE_APPLICATION=latency`, with
`SUNSTONE_LATENCY=ON`, `SUNSTONE_A733_OS_CPU_RETURN=ON`,
`SUNSTONE_A733_OS_CPU_CLOCK=ON` and `A733_A76_ADMITTED=ON`.
It uses the existing CPU-L 1794 MHz/1000 mV and CPU-B 2002 MHz/1050 mV points.
`apps/latency/expect.py` writes the existing runner's complete 5,440-byte word
expectation. `apps/latency/host.py` decodes the result and rejects invented device
coverage. The CPU control precedes any combined-device run on port 3-3.
