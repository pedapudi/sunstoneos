# Partitioned scheduler integration

**Status: implemented.**

The default kernel uses global ready lists and FIFO task/ISR locks. The private
fixed-priority fork assigns every task one owner and supplies owner queues,
routed wakes, migration acknowledgments and owner deadlines. Live integration
remains incomplete and disabled. Research and the design are in
[the partitioned scheduler design](../designs/scheduler-partitioned.md).

**Software evidence:** host, emulator, source.

**Board-qualified scope:** Global-kernel eight-core CPU controls complete their
bounded wake workload and return to ROM; their timing budgets remain unmet.
One recorded CPU recognition preserves the exact reference CRCs and text.
The partitioned fork, heterogeneous placement and tickless timing have no
physical qualification.

## Open work

1. Complete static-admission cleanup and critical-context control ordering;
   validate failure/reuse and nested task/ISR critical calls.
2. Connect the complete private fork and owner port hooks with one consistent
   static-task/TLS ABI throughout the application kit. Keep the option off by default.
3. Run concurrent mixed-affinity QEMU races and all-core soak; measure scheduling
   lock acquisitions and ready work stranded on busy owners. Idle pull waits until D.
4. Finish the port ordering review and configured full gate before board work;
   preserve the lock worker's port-lock hunks and probe worker's entry code.
5. When the coordinator permits queue work, compare the same CPU latency workload
   and exact external mimus control. No new board run or image is prepared this round.
6. Take topology wiring and capacity calibration from the topology worker after
   its carried commits are reviewed; object locks, tickless timing and EDF follow B.

## Evidence

Board results with a queue entry are records in `docs/evidence/scheduler.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-scheduler-20261101.md).

<!-- evidence-table:begin -->
6 records in `docs/evidence/scheduler.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793560470911857163-ce5f22bf` | 2026-11-01 | 3-3 | `478364d6e009` | result-mismatch | failed | run_total 16.523 s, task_lock_wait_max 122.917 us, loaded_wake_max 111.458 us |
| `1793560934366050732-4e45b5ea` | 2026-11-01 | 3-3 | `21c8564d781a` | result-mismatch | failed | run_total 16.408 s, task_lock_wait_max 177.875 us, loaded_wake_max 133.333 us |
| `1793561595474856811-579a99b6` | 2026-11-01 | 3-3 | `312b33297d79` | returned | passed | run_total 68.716 s, compute_time 334670 us, frames 90, samples 57440 |
| `1793566204991504422-8a47d893` | 2026-11-01 | 3-3 | `49c60251609c` | result-mismatch | failed | run_total 16.587 s, loaded_wake_c6_p50 69.292 us, loaded_wake_c6_p99 159.958 us, loaded_wake_c6_max 159.167 us, task_lock_wait_c6_p50 10.625 us, task_lock_wait_c6_p99 111.958 us, task_lock_wait_c6_max 139.333 us, masked_c7_max 115.833 us |
| `1793575947324491365-4f50a160` | 2026-11-01 | 3-1 | `5f9682c53286` | returned | passed | run_total 171.586 s, loaded_wake_c6_p50 77.292 us, loaded_wake_c6_p99 159.958 us, loaded_wake_c6_max 165.917 us, task_lock_wait_max 137.625 us, masked_max 127.667 us |
| `1793576137408460593-a3232b16` | 2026-11-01 | 3-1 | `01b015b859b8` | returned | passed | run_total 171.872 s, loaded_wake_c6_p50 47.958 us, loaded_wake_c6_p99 55.958 us, loaded_wake_c6_max 54.25 us, task_lock_wait_max 29.625 us, masked_max 33.792 us |
<!-- evidence-table:end -->
