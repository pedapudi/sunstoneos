# Timing qualification remains open

SunstoneOS has optional per-core maximum measurements and an eight-core load
diagnostic. Physical worst-case timing remains unmeasured. Native CPU-control and combined
CPU/DMA images link and pass host admission checks. GPU and NPU jobs remain
outside this workload.

## Initial response budgets

These are engineering acceptance targets for the existing 1 kHz scheduler.
They allocate fractions of its 1 ms period; they are not vendor guarantees or
measured worst-case execution bounds. Device budgets cover software completion
delivery. Transfer size and accelerator execution time require separate bounds.

| Region | Budget | Basis | Measured maximum |
| --- | ---: | --- | --- |
| Timer IRQ assertion to C handler entry | 50 µs | 5% of one tick | Host QEMU: 54,674.128 µs |
| IRQ-masked or kernel critical region | 25 µs | Half the IRQ budget leaves entry and routing time | Host QEMU prefix: 24,453.840 µs |
| Kernel ISR lock acquisition / outer hold | 25 µs each | Each contributes to the masked-region budget | Host QEMU: 24,453.312 / 4,656.176 µs |
| Kernel task lock acquisition / outer hold | 25 µs each | Each contributes to the masked-region budget | Host QEMU: 2,886.736 / 24,453.648 µs |
| Highest-priority ready task on an idle core | 100 µs | 10% of one tick | Host QEMU: 611.456 µs |
| Highest-priority ready task on a busy core | 250 µs | 25% of one tick | Host QEMU: 609.376 µs |
| DMA IRQ notification to completion observation | 250 µs | 5% of the minimum 5 ms audio period | Host QEMU with modeled DMA: 493.424 µs |
| GPU active-job IRQ to client receipt | 1,000 µs | One quarter of the 4 ms in-flight fallback | Separate two-core host fixture: 1,012.176 µs |
| NPU mailbox publication to owner dispatch | 1,000 µs | At most one scheduler period | Uninstrumented; board unmeasured |

The IRQ row measures the architectural timer, whose programmed compare value
defines assertion. DMA and GPU lack an equivalent captured device-edge timestamp.
Their software observations do not establish device-IRQ entry latency.
Every board maximum is unmeasured. The host values exceed the targets. Instruction-counted virtual time includes emulator CPU
interleaving and startup; it cannot establish a board overrun or a hardware fix.

## Measurement boundaries and coverage

`SUNSTONE_LATENCY` selects [the recorder](../../runtime/latency.c). Disabled builds
contain no timing calls, counters, buffers or IRQ wrapper. The recorder uses
`ISB; MRS CNTVCT_EL0`; counter frequency converts ticks to microseconds.
It records count, maximum, starting counter and call site for each core and region.
The application kit admits functions declared in the latency header; recorder
variables remain private. Reading the report's frequency uses an accessor.
It retains the first over-budget sample's start, duration and call site separately.
The budget table, core, counter frequency and region identify that check's inputs.
No average replaces a maximum, and instrumentation overhead is not subtracted.

The [optional port patches](../../patches/freertos-latency-c.patch) cover priority masking and
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
  [the recursive port lock](../../patches/freertos-latency-c.patch:104).
  Symbolization identifies `vTaskEnterCritical`, `vTaskSuspendAll` and
  `vTaskSwitchContext` in the generated kernel. Measure these holders on the
  board before moving work or changing the locking algorithm.
- Host idle and busy scheduling maxima exceed their targets at
  [the wake task](../../apps/latency/workload.c:106). Keep admission against the
  actual running task and measure on the board before changing affinity or
  scheduler selection.
- Host DMA and GPU delivery maxima exceed their targets at
  [the DMA completion sweep](../../drivers/dma/dma_service.c:371) and
  [GPU client receipt](../../drivers/gpu/gpu_queue.c:159). Bound cache-maintenance
  work and measure client placement with physical completions before a fix.
- [The late-tick diagnostic](../../platform/a733-smp-cached/perf_session.c:173)
  masks interrupts for 5,500 µs. This exceeds 25 µs by construction. Keep it
  outside latency qualification and retain it as a separate catch-up test.
- [Kernel lock contention](../../patches/freertos-smp-c.patch) has no timeout or
  fairness bound. Bound holder work and measure contention before considering a
  lock redesign. A lock timeout cannot safely substitute for scheduler ownership.
- [The DMA worker](../../drivers/dma/dma_service.c:371) scans channels in order;
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
The combined model completes 28 continuous 4 KiB RAM-copy jobs and retires its
worker after collecting the retirement reply. Every core completes 64 idle and
128 busy wake samples. Its 5,500-tick run uses modeled DMA registers and movement;
QEMU durations establish instrumentation coverage only. Raw evidence is under
`~/.cache/sunstoneos/build/latency-1101/return-1101/qemu-retirement/`.
The model ELF SHA-256 is
`40d05d64d7e9100fec000fa7d6bbc3fc8e915179aa97ece6778c1af9df26ab2d`.
The separate GPU receipt fixture remains under
`~/.cache/sunstoneos/build/latency-1101/gpu-latency-receipt/`.

The native images use application work regions for eight private 1 MiB extents.
Their initialized data includes the global-offset table inside image RAM.
The link check and native session-reservation checks pass. The combined image
uses the OS RAM-copy API and DMA0 channel 0; it starts no GPU or NPU job.
Memory traffic stops after loaded samples, before the final DMA drain.

Normal DMA return closes the channel, restores its descriptor, collects the
retirement reply and waits for the service worker's last-access acknowledgment.
It masks the source interrupt and waits at most 100 µs for GIC distributor
propagation before verifying that the interrupt is disabled. The original
bootstrap task then verifies the adopted controller, clocks, routes and idle
channels within a fixed 50 ms window. No clock, gate, reset or route write occurs
on that verification path. The parent allocation and clock token remain retained.
The CPU return path waits for this final acknowledgment before terminal cleanup.
Failed checks retain their phase, register read, token result and deadline inputs.

[The native mask check](../../checks/dma-return-mask.py) executes the linked code
with delayed and stuck propagation. Removing its retry branch fails the delayed
case. [The pool return check](../../checks/dma-pool-return.py) exercises actual
FreeRTOS tasks through copy, close, reply collection and both acknowledgments.
Changed routing and permanent registry contention prevent the final acknowledgment;
transient contention completes. These models establish no physical drain bound.

The supervised, resident and resident-audio baseline builds and kernel check pass.
The multithreaded QEMU soak fails the notification-count assertion at
`checks/smp8-soak.c:104`; the observed count is unavailable. The same ELF passes
with single-thread TCG and instruction-counted time. That control preserves
eight guest cores, interrupts, migration and floating-point context checks;
it does not explain the original failure or qualify concurrent host threads.
The check index passes. Gate records and the preserved failure are under
`return-1101/integration/`, and the control is under `return-1101/soak8-single-tcg/`
beneath the latency build directory. The gate caps builds at four jobs.

## Disabled production and queued runs

Disabled timing uses the unchanged base FreeRTOS patches. Enabled targets apply
separate patches to private port copies. [The production comparison](../../checks/latency-production.py)
compares a normal OS image against `00e9f775`, with timing disabled and no external
application. Executable bytes and addresses, writable sections and allocated
object layouts are identical; disassembly has no differences or latency symbols.
The executable section contains 548,844 bytes, with SHA-256
`e1b004427ae69c47f86c228dd020390698fea1cb95ff91cbec68903d75503a51`.
Only build identity and debug information differ. Evidence is under
`~/.cache/sunstoneos/build/latency-1101/return-1101/production-comparison-final/`.

The CPU selector is `SUNSTONE_APPLICATION=latency`; the combined selector is
`latency-dma` with `SUNSTONE_A733_OS_DMA=ON`. Both enable timing, returning CPU
support and the admitted A76 cores. They use CPU-L 1794 MHz/1000 mV and CPU-B
2002 MHz/1050 mV. No additional operating point is introduced.
`apps/latency/expect.py` produces the complete 5,440-byte expectation; `--dma`
requires real DMA job and completion counts. The decoder rejects device samples
that the selected workload does not exercise.

Built images, hashes, expectations and short submission instructions are under
`~/.cache/sunstoneos/build/latency-1101/return-1101/images/`.
The CPU control precedes the combined run on port 3-3. Both boards are paused
for physical recovery, and no run is submitted. The combined run also requires
independent DMA access, inherited-state, drain and recovery evidence. The runner
binds these facts to the original USB instance and immutable session bytes before
reset. The supplied combined template refuses admission until those physical
facts exist; its flags do not establish them. Board timing remains unmeasured.
