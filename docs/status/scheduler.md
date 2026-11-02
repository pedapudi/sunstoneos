# Partitioned scheduler integration

**Status: implemented.**

The opt-in fixed-priority fork uses owner queues, routed wakes, acknowledged
migration and task-context resource cleanup. Static and dynamic task APIs pass
configured host checks, including mixed-affinity stress and eight-core soak.
Selection takes no global task, ISR or administration lock in those tests.
The default remains the global FIFO-lock control; a CPU comparison image is
prepared for the coordinator. See [the design](../designs/scheduler-partitioned.md).

**Software evidence:** host, emulator, source.

**Board-qualified scope:** Global-kernel eight-core CPU controls complete their
bounded wake workload and return to ROM; their timing budgets remain unmet.
One recorded CPU recognition preserves the exact reference CRCs and text.
The partitioned fork, heterogeneous placement and tickless timing have no
physical qualification.

## Open work

1. Run the prepared CPU control through the coordinator's queue; compare per-core
   wake and lock distributions with the same-board baseline, then record the result.
2. Qualify the fork with the exact mimus reference and board soak before enabling
   it by default. Current passing evidence is host-only.
3. Add idle pull in series D: controlled tests still strand ready work on a busy
   home while an eligible peer idles. Topology tables are available for placement.
4. Replace compatibility object locks in C, then finish tickless deadlines and
   reservations. The periodic CPU0 tick remains during B.
5. Preserve lock-worker port-lock hunks and probe entry code. The prepared image,
   review, commands and remaining limits are in [the history](../history/status-scheduler-20261101.md).

## Evidence

Board results with a queue entry are records in `docs/evidence/scheduler.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-scheduler-20261101.md).

<!-- evidence-table:begin -->
5 records in `docs/evidence/scheduler.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793560470911857163-ce5f22bf` | 2026-11-01 | 3-3 | `478364d6e009` | result-mismatch | failed | run_total 16.523 s, task_lock_wait_max 122.917 us, loaded_wake_max 111.458 us |
| `1793560934366050732-4e45b5ea` | 2026-11-01 | 3-3 | `21c8564d781a` | result-mismatch | failed | run_total 16.408 s, task_lock_wait_max 177.875 us, loaded_wake_max 133.333 us |
| `1793561595474856811-579a99b6` | 2026-11-01 | 3-3 | `312b33297d79` | returned | passed | run_total 68.716 s, compute_time 334670 us, frames 90, samples 57440 |
| `1793566204991504422-8a47d893` | 2026-11-01 | 3-3 | `49c60251609c` | result-mismatch | failed | run_total 16.587 s, loaded_wake_c6_p50 69.292 us, loaded_wake_c6_p99 159.958 us, loaded_wake_c6_max 159.167 us, task_lock_wait_c6_p50 10.625 us, task_lock_wait_c6_p99 111.958 us, task_lock_wait_c6_max 139.333 us, masked_c7_max 115.833 us |
| `1793585573653883302-e2093283` | 2026-11-02 | 3-3 | `a8fcb1064226` | watchdog-reset | failed | run_total 66.115 s, launch_duration 52.5342 s |
<!-- evidence-table:end -->
