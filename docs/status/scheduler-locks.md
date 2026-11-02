# FIFO kernel locks and interrupt-mask attribution

**Status: implemented.**

Both global FreeRTOS kernel locks are FIFO ticket locks, and the latency recorder attributes each retained mask and lock hold to its entry and exit sites.

**Board-qualified scope:** Three eight-core CPU latency images on port 3-1
return normally with CPU rails restored: test-and-set, tickets, and tickets
with the thermal fast path. The matched lock pair reduces worst loaded wait
from 137.625 to 29.625 microseconds and masking from 127.667 to 33.792.
The ticket locks are integrated. The thermal fast path is not: its single run
records 32.500 microseconds wait and 38.458 masking, so it is reverted.
The 25 microsecond budget remains unmet. These finite runs do not qualify
worst-case timing, sustained operation or power behavior.

## Open work

1. Meet the 25 microsecond budget for task-lock wait and interrupt masking. The ticket locks reach 29.625 microseconds of wait and 33.792 of masking.
2. Shorten the dominant masked path, `ulTaskGenericNotifyTake` to `FreeRTOS_Yield` (33.792 microseconds with tickets). Notification and suspension changes belong to the partitioned scheduler.
3. Shorten the dominant task-lock holds, `vTaskSuspendAll` to `vTaskExitCritical` (18.125 microseconds) and `vTaskSwitchContext` (9.625 and 8.792). The partitioned scheduler owns them; `freertos-smp-tasks.patch` and the owner run queues are unchanged here.
4. Explain why the thermal rearm fast path raised wait and masking (32.500 and 38.458 microseconds) before attempting it again; the cause of the single-run regression is unknown.
5. Repeat each lock condition on the board. One run per condition does not establish repeatability, and the ticket locks raise the task-lock wait median on every core.
6. Reduce the initialization masking maximum of 266.333 microseconds at `prvAddNewTaskToReadyList` to `vPortRestoreTaskContext`.
7. Qualify worst-case timing, sustained operation and power behavior.

## Evidence

Board results with a queue entry are records in `docs/evidence/scheduler-locks.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-scheduler-locks-20261101.md).

<!-- evidence-table:begin -->
3 records in `docs/evidence/scheduler-locks.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793575947324491365-4f50a160` | 2026-11-01 | 3-1 | `5f9682c53286` | returned | passed | run_total 171.586 s, task_lock_wait_max 137.625 µs, masking_max 127.667 µs, loaded_wake_max 180.167 µs, loaded_wake_p99_max 159.958 µs |
| `1793576137408460593-a3232b16` | 2026-11-01 | 3-1 | `01b015b859b8` | returned | passed | run_total 171.872 s, task_lock_wait_max 29.625 µs, masking_max 33.792 µs, loaded_wake_max 126.083 µs, loaded_wake_p99_max 117.292 µs |
| `1793576768527805851-5709adc9` | 2026-11-01 | 3-1 | `2c928eebe120` | returned | passed | run_total 173.877 s, task_lock_wait_max 32.5 µs, masking_max 38.458 µs, loaded_wake_max 116.667 µs, loaded_wake_p99_max 117.292 µs |
<!-- evidence-table:end -->
