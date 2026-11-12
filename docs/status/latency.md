# CPU timing exceeds the masked-region and lock-wait budgets

**Status: implemented.**

The native CPU control returns normally on port 3-3 and completes every wake sample. Its strict timing expectation fails. One CPU-only run establishes observed maxima, not worst-case bounds or combined-load timing. DMA, GPU and NPU completion timing is unmeasured.

**Board-qualified scope:** None.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Eight-core CPU latency control (wake samples, lock and mask recording) | implemented | Returns normally on port 3-3 with every wake sample; the strict timing expectation fails | `1793559978973913698-7f7e36e0` |
| CPU0 timer assertion to C handler entry | implemented | 6.583 µs against a 50 µs budget | same record |
| IRQ-masked or kernel critical region | open | 268.125 µs against 25 µs | same record |
| Kernel ISR lock acquisition / outer hold | open | 9.042 / 19.792 µs against 25 µs each | same record |
| Kernel task lock acquisition / outer hold | open | 238.750 / 12.625 µs against 25 µs each | same record |
| Highest-priority ready task, idle / busy core | implemented | 66.625 / 104.542 µs against 100 / 250 µs | same record |
| DMA, GPU and NPU completion timing | open | Unmeasured (budgets 250, 1,000 and 1,000 µs) | none |

The maxima come from one run at CPU-L 1794 MHz and 1000 mV, CPU-B 2002 MHz and 1050 mV. The budgets, with the basis of each, are in the note of the record; the two largest overruns precede the top-point workload by 132 ms.

## Open work

1. Separate initialization from workload recording; the two largest overruns precede the top-point workload by 132 ms, and the workload itself reaches 62.750 us masked and 73.417 us task-lock wait (core 6).
2. Design a fair task-lock acquisition protocol and bound contender work; this needs a port synchronization review (`vTaskSwitchContext`, `tasks.c:5393`).
3. Reduce kernel lock contention in `ulTaskGenericNotifyTake`, keeping the scheduler invariants.
4. Make NPU mailbox dispatch event-driven in the NPU owner; the owner polls every tick.
5. Run the combined CPU and DMA image after the thermal-readiness fix and independently established DMA authority.
6. Measure DMA, GPU and NPU completion timing; physical DMA cleanup is unqualified.
7. Meet the partitioned scheduler's 25 µs masking budget. Self-suspension publishes before masking and changes only the caller's local state before saving its frame and selecting a replacement; it drains no controls. Other maintenance admits interrupts during control preparation and attempts at most sixteen distinct records per entry. Masked spans still exceed the budget. With cached table walks, PMU windows measure A55 self-selection at 3 to 15.792 µs, general selection at up to 31.667 µs and control bodies at 3.125 to 20.375 µs. These component observations cannot bound a full maintenance batch. Every retained target window reports zero data and instruction TLB walks. Nearly equal selection refill counts accompany substantially different durations, leaving the cause of the stalls unresolved. [The scheduler timing records](../evidence/scheduler-latency-measurement.jsonl), entry `1794497365941804086-44d410dc`, qualify the image, counter banks and measurement boundaries.
8. Qualify task-lock wait and its cause at the actual FIFO reservation. The cached-walk PMU diagnostic observes a public maximum of 9.792 µs against 25 µs. This observation does not identify the maximum's cause or establish a worst-case bound. Startup timing after the scheduler running flag remains unqualified because the recorder omits assembly work and some mask boundaries.
9. Resolve the partitioned scheduler's wake tail and CPU0 regression. The instrumented whole tick reaches 165.917 µs and the CPU0 busy-wake maximum reaches 210.417 µs. A separate tick invocation spends 74.88 percent of its cycles stalled in the backend. New PMU result writes occur outside the counted window, while existing tick observers remain inside it. Setup and publication can delay wake outside that window. The tick directly notifies eight latency fixture tasks and publishes due work without calling owner maintenance. The 128 busy samples per core cannot establish the 10 µs 99.99th-percentile target.

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
