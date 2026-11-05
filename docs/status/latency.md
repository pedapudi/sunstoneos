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
5. Run the combined CPU and DMA image after the thermal-readiness fix and independently established DMA authority; the coordinator lands the reviewed runner branch first.
6. Measure DMA, GPU and NPU completion timing; physical DMA cleanup is unqualified.
7. Shorten the interrupt masking of a task that suspends itself. When the latency workload stops, each worker task calls `vTaskSuspend(NULL)`. The scheduler's command path (`ssKernelCommand` in `kernel/scheduler/freertos.c`) masks interrupts from its entry to the point just before the task yields. The masked span includes four steps: taking a reference on the task handle, publishing the suspend request (`ss_control_publish`), draining every control request pending for this CPU (`ssKernelControlDrain`), and recomputing the CPU's time slice (`ssKernelSliceRefresh`). On the board, at CPU-L 1794 MHz and CPU-B 2002 MHz, these spans reached 79.1 us on core 3 and 58.0 us on core 5 in one run, and 72.0 us on core 4 in another. They fall 0.2 to 0.8 ms after the end of the busy phase, when the worker tasks on cores 2 to 7 suspend at nearly the same time, so the phase maxima exclude them. A robot's interrupt waits behind such a span, so the span counts as worst-case masking. The work has two parts:
   - Measure each of the four steps separately in the latency build to find which one takes the time. Concurrent suspends on six cores may contend for the shared control structures.
   - Establish which steps need interrupts masked. The drain and the slice refresh change this CPU's run queue, which the interrupt exit path also changes. Publishing the request and taking the handle reference might run unmasked. The drain could apply only the calling task's request and leave the other pending requests to the CPU's next maintenance point, or run in bounded batches with interrupts unmasked between them.

## Evidence

Board results are records in `docs/evidence/latency.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-latency-20261101.md).

<!-- evidence-table:begin -->
2 records in `docs/evidence/latency.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793559139160654627-a1893cde` | 2026-11-01 | 3-3 | `a4e8b1ec5fc1` | watchdog-reset | failed | run_total 16.357 s |
| `1793559978973913698-7f7e36e0` | 2026-11-01 | 3-3 | `13496da8d03f` | result-mismatch | failed | run_total 16.453 s, timer_to_handler_max 6.583 µs, irq_masked_region_max 268.125 µs, isr_lock_acquire_max 9.042 µs, isr_lock_hold_max 19.792 µs, task_lock_acquire_max 238.75 µs, task_lock_hold_max 12.625 µs, ready_task_idle_core_max 66.625 µs, ready_task_busy_core_max 104.542 µs |
<!-- evidence-table:end -->
