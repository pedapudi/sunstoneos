# CPU timing exceeds the masked-region and lock-wait budgets

The native CPU control returns normally on port 3-3 and completes every wake
sample. Its strict timing expectation fails. One CPU-only run establishes
observed maxima; it does not establish worst-case bounds or combined-load timing.
DMA, GPU and NPU completion timing remains physically unmeasured.

## Budgets and board observations

These engineering targets allocate fractions of the existing 1 kHz scheduler
period. They are acceptance targets, not vendor guarantees. Device completion
budgets cover software delivery; transfer size and accelerator execution need
separate bounds. The recorder remains active during initialization and workload.

| Region | Budget | Basis | Board maximum |
| --- | ---: | --- | ---: |
| CPU0 timer assertion to C handler entry | 50 µs | 5% of one tick | 6.583 µs |
| IRQ-masked or kernel critical region | 25 µs | Half the IRQ budget leaves routing and entry time | 268.125 µs |
| Kernel ISR lock acquisition / outer hold | 25 µs each | Each contributes to masking | 9.042 / 19.792 µs |
| Kernel task lock acquisition / outer hold | 25 µs each | Each contributes to masking | 238.750 / 12.625 µs |
| Highest-priority ready task on an idle core | 100 µs | 10% of one tick | 66.625 µs |
| Highest-priority ready task on a busy core | 250 µs | 25% of one tick | 104.542 µs |
| DMA IRQ notification to completion observation | 250 µs | 5% of the minimum 5 ms audio period | Unmeasured |
| GPU active-job IRQ to client receipt | 1,000 µs | One quarter of the 4 ms fallback | Unmeasured |
| NPU mailbox publication to owner dispatch | 1,000 µs | One scheduler period | Unmeasured |

Entry `1793559978973913698-7f7e36e0` has outcome `result-mismatch`, normal ROM
return and seven secondaries off. It uses the existing CPU-L 1794 MHz/1000 mV
and CPU-B 2002 MHz/1050 mV points during the workload. Every core completes
64 idle and 128 busy wake samples, and reads and rewrites its private 1 MiB
extent between 40 and 82 times. The workload lasts 198 ticks. Its counter runs
at 24 MHz. Thermal readings remain normal during the captured workload.

The two largest overruns precede the verified top-point workload by about
132 ms. Overruns also occur within that workload: masked duration reaches
62.750 µs on core 6, and task-lock wait reaches 73.417 µs on core 6.
Those are per-core maxima whose timestamps fall inside the workload window;
other cores' larger initialization samples prevent deriving their workload-only
maxima from this report. Separating initialization and workload recording must
preserve both sets of observations.

The CPU control ELF SHA-256 is
`13496da8d03f927b56a71626f8ee7738181ddfbdd4dc9d2afb3f14ef6a6ddb49`.
Raw queue evidence is under
`~/.cache/sunstoneos/board-queue/entries/1793559978973913698-7f7e36e0/run/`.
Decoded metrics, source locations and first-over-budget inputs are under
`~/.cache/sunstoneos/build/latency-1101/followup-2/control-board-findings.json`.

## Over-budget regions and proposed fixes

- Task publication in generated FreeRTOS `tasks.c:2144` opens the longest
  initialization mask sample. Preserve the 268.125 µs observation and record
  initialization separately from the verified top-point workload.
- `vTaskSwitchContext` in generated `tasks.c:5393` acquires the global task
  lock. Waiting reaches 238.750 µs before the workload and 73.417 µs within it.
  The longest outer hold is 12.625 µs. Bound contender work and design a fair
  acquisition protocol before claiming a finite wait bound; this requires a
  port synchronization review, beyond a local work move.
- `ulTaskGenericNotifyTake` in generated `tasks.c:7953` opens the 62.750 µs
  workload mask sample. Its interval includes kernel lock contention. Reduce
  that contention while retaining the notification and scheduler invariants.
- [The deliberate late-tick test](../../platform/a733-smp-cached/perf_session.c#L173)
  masks interrupts for 5,500 µs by construction. Keep it outside timing
  qualification and retain it as a separate catch-up test.
- [The NPU mailbox publisher](../../runtime/npu_app.c#L43) does not notify its
  owner. [The owner loop](../../apps/os/npu-client.c#L93) polls every tick;
  dispatch can approach 1 ms plus scheduling. Opening and stopping use the
  25 ms waits. Event-driven dispatch belongs to the NPU owner.

The generated kernel is pinned to revision
`97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2` with the SMP task patch. The measured
ELF and generated source remain in `return-1101/control-v3-build/` beneath the
latency build directory. No larger kernel-lock redesign is implemented here.

## Recorder boundaries and production cost

`SUNSTONE_LATENCY` selects [the recorder](../../runtime/latency.c). It uses
`ISB; MRS CNTVCT_EL0` and records each core's count, maximum, start counter and
call site. The first over-budget sample retains its own duration and site.
Instrumentation overhead is included. The timer compare defines assertion for
CPU0 PPI30; no equivalent device-edge timestamp exists for the DMA or GPU rows.

[The optional port patches](../../patches/freertos-latency-c.patch) cover priority
masking and both recursive kernel locks. Context restoration closes a mask
interval before assembly unmasks priority. Nested entries retain the outer
start. Samples exclude recorder bookkeeping and the restoration or unlock tail;
these prefixes expose overruns but cannot prove complete duration bounds.
Raw DAIF regions in boot, terminal, storage inspection and PMU sampling need
separate probes. Every core freezes recording before the report is copied.

The disabled normal OS has identical executable bytes and addresses, writable
storage and allocated-object layout against `00e9f775`. The disabled image contains no
latency calls, counters or buffers. The executable section has 548,844 bytes and SHA-256
`e1b004427ae69c47f86c228dd020390698fea1cb95ff91cbec68903d75503a51`.
Only build identity and debug metadata differ. Current evidence is under
`followup-2/production-comparison/` beneath the latency build directory.

## Host checks and queued DMA prerequisites

[The native startup check](../../checks/latency-startup.py) executes the linked
readiness logic. The CPU control awaits fresh thermal readiness before measuring;
measurement still refuses thermal throttling. Live and thermal failures have
separate details. Seven modeled startup cases pass. The original control entry
`1793559139160654627-a1893cde` has `watchdog-reset` and application detail `0xb01`;
its failure address matches the startup model's premature thermal refusal.

The multithreaded soak uses the same task patch as the native SMP image.
[Its CMake selection](../../cmake/smp.cmake#L15) includes affinity-aware time slicing.
Upstream affinity-blind time slicing requests redundant cross-core yields;
with four host CPUs, the ready mover starves while a ring task repeats the
run-state/yield-lock loop. A controlled application of only the existing
slicing hunk resolves the captured notification timeout. Twenty multithreaded
soak trials pass with four host CPUs, unchanged priorities and unchanged timeout.
Two-core queue and migration checks pass. The checker records CPU affinity and
explicitly selects multithreaded TCG. QEMU establishes software behavior only.

The supervised, resident and resident-audio baseline builds, kernel check,
eight-core soak and check index pass through `followup-2/integration/` beneath
the latency build directory. The soak diagnosis and isolated correction are
under `followup-2/soak/`. Single-thread instruction counting is not the resolution.

Normal DMA retirement closes channels, restores descriptors, collects the
retirement reply and waits for worker and bootstrap acknowledgments. It waits
for GIC propagation before mask readback and verifies the adopted platform
within a fixed deadline. Tokens and parent allocations remain retained.
Delayed/stuck mask, platform mismatch and registry-contention host cases pass;
physical DMA cleanup remains unqualified.

The separate `work/latency-runner-1101` branch starts at `bringup` `6ebe687a`.
Commit `4372ff80` contains Python runner, record-codec and check changes only.
Independent review passes, with 37 focused cases, 54 existing runner cases and
23 independent mutation checks. The coordinator creates DMA authority from
independent physical access, drain, exclusion and recovery evidence. The runner
binds its declarations to immutable inputs and the original USB instance before
reset; hashes and booleans do not establish those physical facts.

The combined image requires the thermal-readiness fix before execution and
remains unsubmitted. The functional CPU control completes; its timing findings
remain open. Pending prerequisites are coordinator landing of the reviewed
runner branch and independently established DMA authority. The provisional DMA
template refuses admission. No device completion maximum or combined-load bound
is qualified.
