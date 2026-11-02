# Partitioned scheduler integration

**Status: implemented.**

The default kernel uses global ready lists and FIFO task/ISR locks. The opt-in
fork connects owner queues, routed wakes, migration acknowledgments and owner
deadlines to the port. Selected static APIs, the original QEMU kernel case and
the eight-core soak pass. Mixed-affinity stress and native dynamic task APIs
remain incomplete. The fork stays off by default; see
[the design](../designs/scheduler-partitioned.md).

**Software evidence:** host, emulator, source.

**Board-qualified scope:** Global-kernel eight-core CPU controls complete their
bounded wake workload and return to ROM; their timing budgets remain unmet.
One recorded CPU recognition preserves the exact reference CRCs and text.
The partitioned fork, heterogeneous placement and tickless timing have no
physical qualification.

## Open work

1. Diagnose the mixed-affinity QEMU timeout from per-core progress, control
   acknowledgments and wake state; the original eight-core soak alone does not cover it.
2. Convert ordinary dynamic task creation with allocation/admission cleanup,
   then link the native network OS and external application with the private ABI.
3. Collect scheduling lock counts and busy-home/idle-peer frequency in the
   concurrent case. Idle pull remains absent until series D.
4. Complete configured validation before queue work: compare the CPU latency
   workload and exact mimus control. No new board image or OPP approval is pending.
5. Preserve lock-worker port-lock hunks and probe entry code; take reviewed
   topology wiring from its owner. The carried-commit audit permits its unused helpers.
6. After B, finish object locks, tickless deadlines and reservations. Current
   host evidence and remaining limits are in [the history](../history/status-scheduler-20261101.md).

## Evidence

Board results with a queue entry are records in `docs/evidence/scheduler.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-scheduler-20261101.md).

<!-- evidence-table:begin -->
4 records in `docs/evidence/scheduler.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793560470911857163-ce5f22bf` | 2026-11-01 | 3-3 | `478364d6e009` | result-mismatch | failed | run_total 16.523 s, task_lock_wait_max 122.917 us, loaded_wake_max 111.458 us |
| `1793560934366050732-4e45b5ea` | 2026-11-01 | 3-3 | `21c8564d781a` | result-mismatch | failed | run_total 16.408 s, task_lock_wait_max 177.875 us, loaded_wake_max 133.333 us |
| `1793561595474856811-579a99b6` | 2026-11-01 | 3-3 | `312b33297d79` | returned | passed | run_total 68.716 s, compute_time 334670 us, frames 90, samples 57440 |
| `1793566204991504422-8a47d893` | 2026-11-01 | 3-3 | `49c60251609c` | result-mismatch | failed | run_total 16.587 s, loaded_wake_c6_p50 69.292 us, loaded_wake_c6_p99 159.958 us, loaded_wake_c6_max 159.167 us, task_lock_wait_c6_p50 10.625 us, task_lock_wait_c6_p99 111.958 us, task_lock_wait_c6_max 139.333 us, masked_c7_max 115.833 us |
<!-- evidence-table:end -->
