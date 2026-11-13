# Scheduler tick and wake timing remain above target

**Status: implemented.**

The cached-walk CPU latency diagnostic returns normally on port 3-3 and completes all eight cores' samples. Task-lock wait is below its budget; masking and wake timing remain above target. The instrumented CPU0 tick is the main open item. These observations establish no worst-case bound, task-wake percentile or combined-load timing. DMA, GPU and NPU completion timing is unmeasured.

**Board-qualified scope:** None.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Eight-core CPU latency diagnostic | implemented | 64 idle and 128 busy wakes per core; normal return | `1794497365941804086-44d410dc`; [measurement record](../evidence/scheduler-latency-measurement.jsonl) |
| A55 C self-selection | measured; full dispatch remains open | 3 to 15.792 µs in retained PMU windows | same record |
| One control-record body | measured; full batch remains open | 3.125 to 20.375 µs | same record |
| Instrumented CPU0 whole tick | open | 165.917 µs; a separate sample has 74.88 percent backend stalls | same record |
| Cause of CPU0 tick stalls | measured | A busy tick averages 314,510 cycles and 220 attributable L3 refills against 36,541 cycles and one refill when idle, about 1,268 cycles per refill; SGI segments take 2.2 percent of a tick; one line transfer costs 142.5 to 153.5 cycles | `1794504237145636168-9aa4ebd9`, `1794502169612799798-8aea3851`; [cause capture](../designs/scheduler-latency-measurement.md#cause-capture-separates-release-barrier-and-line-transfer-costs) |
| CPU0 busy tick after the footprint cuts | measured; still above target | 212,455 cycles and 140.1 L3 refills per busy tick, from 314,510 and 220.2; CPU0's longest busy wake 228 µs, from 305 µs; no L3 partition scheme is configured (`CLUSTERPARTCR_EL1` 0 on every core) | `1794508626388886966-ae6d2457`; [footprint budget](../../checks/tick-footprint-budget.json) |
| CPU0 busy tick with streaming confined to CPUs 4-7 | measured; still above the idle tick | 76,410 cycles and 26.5 L3 refills per busy tick, from 212,455 and 140.1 with every core streaming (idle tick 33,304); CPU0's longest busy wake 93.1 µs, from 228 µs | `1794597393456307577-46a485eb`; [isolation result](../designs/scheduler-realtime.md#real-time-cores-may-need-to-exclude-streaming-work) |
| Retained mask interval after scheduler start | open | 86.250 µs on core 4 against 25 µs | same record |
| Kernel task-lock wait | measured below budget; worst case unqualified | 9.792 µs against 25 µs | same record |
| CPU0 busy task wake | open | Maximum 210.417 µs; 10 µs 99.99th-percentile target unqualified | same record |
| DMA, GPU and NPU completion timing | open | Unmeasured (budgets 250, 1,000 and 1,000 µs) | none |

The figures come from one instrumented run at CPU-L 1794 MHz and 1000 mV, CPU-B 2002 MHz and 1050 mV, with cached table walks. Long PMU windows confirm effective clocks of 1793 and 1998 MHz. Every retained target window has zero data and instruction TLB walks. New PMU result writes are outside the counted windows; existing control and tick observers remain inside. The aggregate core 0 mask maximum begins before the scheduler running flag and cannot qualify startup masking.

## Open work

1. Shorten CPU0's instrumented tick while preserving its eight periodic latency-fixture notifications, deadline handling and equal-priority slicing.
2. Bound masking after scheduler start, including initialization, frame restoration and the final recorder boundary.
3. Reduce the cache lines CPU0 touches per tick further. The stalls follow refills of lines that the busy memory traffic evicts, at about 1,270 cycles per L3 refill. Seven footprint cuts, held by `checks/tick-footprint-budget.py`, removed 80 refills per busy tick (entry `1794508626388886966-ae6d2457`); the remaining remote-release work is the next target. Preserve task lifetime, affinity and priority order.
4. Qualify lock-wait bounds under contention independently of diagnostic wrapper overhead.
5. Run the combined CPU and DMA image after the thermal-readiness fix and independently established DMA authority.
6. Measure DMA, GPU and NPU completion timing; physical DMA cleanup is unqualified.
7. Meet the partitioned scheduler's 25 µs masking budget. Self-suspension publishes before masking and changes only the caller's local state before saving its frame and selecting a replacement; it drains no controls. Other maintenance admits interrupts during control preparation and attempts at most sixteen distinct records per entry. Masked spans still exceed the budget. With cached table walks, PMU windows measure A55 self-selection at 3 to 15.792 µs, general selection at up to 31.667 µs and control bodies at 3.125 to 20.375 µs. These component observations cannot bound a full maintenance batch. Every retained target window reports zero data and instruction TLB walks. Nearly equal selection refill counts accompany substantially different durations, leaving the cause of the stalls unresolved. [The scheduler timing records](../evidence/scheduler-latency-measurement.jsonl), entry `1794497365941804086-44d410dc`, qualify the image, counter banks and measurement boundaries.
8. Qualify task-lock wait and its cause at the actual FIFO reservation. The cached-walk PMU diagnostic observes a public maximum of 9.792 µs against 25 µs. This observation does not identify the maximum's cause or establish a worst-case bound. Startup timing after the scheduler running flag remains unqualified because the recorder omits assembly work and some mask boundaries.
9. Resolve the partitioned scheduler's wake tail and CPU0 regression. The instrumented whole tick reaches 165.917 µs and the CPU0 busy-wake maximum reaches 210.417 µs. A separate tick invocation spends 74.88 percent of its cycles stalled in the backend. New PMU result writes occur outside the counted window, while existing tick observers remain inside it. Setup and publication can delay wake outside that window. The tick directly notifies eight latency fixture tasks and publishes due work without calling owner maintenance. The 128 busy samples per core cannot establish the 10 µs 99.99th-percentile target.

10. Measure quiet-state NPU mailbox and completion delivery with the [notification and precise-deadline waits](../designs/npu-owner-events.md). The finite recognition record has zero idle timer wakes; its request notifications do not measure an unloaded owner's rate.

## Evidence

Board results are records in `docs/evidence/latency.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-latency-20261101.md).

<!-- evidence-table:begin -->
5 records in `docs/evidence/latency.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793559139160654627-a1893cde` | 2026-11-01 | 3-3 | `a4e8b1ec5fc1` | watchdog-reset | failed | run_total 16.357 s |
| `1793559978973913698-7f7e36e0` | 2026-11-01 | 3-3 | `13496da8d03f` | result-mismatch | failed | run_total 16.453 s, timer_to_handler_max 6.583 µs, irq_masked_region_max 268.125 µs, isr_lock_acquire_max 9.042 µs, isr_lock_hold_max 19.792 µs, task_lock_acquire_max 238.75 µs, task_lock_hold_max 12.625 µs, ready_task_idle_core_max 66.625 µs, ready_task_busy_core_max 104.542 µs |
| `1793920888961447818-41b92029` | 2026-11-05 | 3-1 | `76d8533f2fc6` | returned | passed | run_total 22.506 s, core0_irq_masked_max 31.2 µs, core1_irq_masked_max 51.9 µs, core1_lock1_wait_max 9.2 µs, core1_lock1_hold_max 25.8 µs, core6_lock1_wait_max 5.3 µs, irq_masked_max 79.1 µs, cpu_clock_transitions 4 |
| `1793924840132757923-b56fe551` | 2026-11-06 | 3-1 | `7a05420668c5` | returned | passed | run_total 22.624 s, core0_irq_masked_max 31.9 µs, core1_irq_masked_max 41.6 µs, core1_lock1_wait_max 3.8 µs, core1_lock1_hold_max 21.1 µs, core6_lock1_wait_max 11.5 µs, irq_masked_max 74.4 µs, cpu_clock_transitions 4 |
| `1793948978286084246-2f19b799` | 2026-11-06 | 3-1 | `b7c1f9f870a9` | returned | passed | run_total 23.575 s, run_total 23.57 s, core0_irq_masked_max 29.5 µs, core1_irq_masked_max 45 µs, core1_lock1_wait_max 12.9 µs, core1_lock1_hold_max 20.3 µs, core6_lock1_wait_max 12.6 µs, irq_masked_max 75.7 µs, cpu_clock_transitions 4 |
<!-- evidence-table:end -->
