# Partitioned scheduler integration

**Status: implemented.**

The opt-in fixed-priority fork uses owner queues, routed wakes, acknowledged
migration and task-context resource cleanup. Static and dynamic task APIs pass
configured host checks, including mixed-affinity stress and eight-core soak.
Selection takes no global task, ISR or administration lock in those tests.
The default remains the global FIFO-lock control. The CPU control with the fork
on ran on the board after a pre-start yield was removed. See [the design](../designs/scheduler-partitioned.md). Hardware values these paths assert (GIC priority views, interrupt routes, CPU identity) are in [the A733 hardware facts](../hardware/a733-facts.md).

**Software evidence:** host, emulator, source.

**Board-qualified scope:** Global-kernel eight-core CPU controls complete their
bounded wake workload and return to ROM; their timing budgets remain unmet.
One recorded CPU recognition preserves the exact reference CRCs and text.
The partitioned fork runs the eight-core CPU control to a normal ROM return with
exact coverage and wake counts and lower wake tails than the global kernel, but
its masked-time budget fails; its mimus use, heterogeneous placement and tickless
timing have no physical qualification.

## Open work

1. Masked time stays above 25 us on cores 0, 2, 3 and at end-of-run suspension: measured phase spans reach 29 us
   (core 3) and the end-of-run self-suspension 46 us. Backend stalls are 94 to 98% of their cycles with 23 to 38
   L1D refills each, so the cause is dependent line fetches under saturated DRAM. Fewer lines per entry or
   prefetch would shorten them; neither is implemented.
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
9 records in `docs/evidence/scheduler.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793560470911857163-ce5f22bf` | 2026-11-01 | 3-3 | `478364d6e009` | result-mismatch | failed | run_total 16.523 s, task_lock_wait_max 122.917 us, loaded_wake_max 111.458 us |
| `1793560934366050732-4e45b5ea` | 2026-11-01 | 3-3 | `21c8564d781a` | result-mismatch | failed | run_total 16.408 s, task_lock_wait_max 177.875 us, loaded_wake_max 133.333 us |
| `1793561595474856811-579a99b6` | 2026-11-01 | 3-3 | `312b33297d79` | returned | passed | run_total 68.716 s, compute_time 334670 us, frames 90, samples 57440 |
| `1793566204991504422-8a47d893` | 2026-11-01 | 3-3 | `49c60251609c` | result-mismatch | failed | run_total 16.587 s, loaded_wake_c6_p50 69.292 us, loaded_wake_c6_p99 159.958 us, loaded_wake_c6_max 159.167 us, task_lock_wait_c6_p50 10.625 us, task_lock_wait_c6_p99 111.958 us, task_lock_wait_c6_max 139.333 us, masked_c7_max 115.833 us |
| `1793585573653883302-e2093283` | 2026-11-02 | 3-3 | `a8fcb1064226` | watchdog-reset | failed | run_total 66.115 s, launch_duration 52.5342 s |
| `1793588341441359431-9f76aac0` | 2026-11-02 | 3-3 | `f7892d6368b4` | result-mismatch | failed | run_total 171.497 s, loaded_wake_c0_p50 95.958 us, loaded_wake_c0_p99 122.625 us, loaded_wake_c0_max 127.333 us, loaded_wake_c1_p50 42.625 us, loaded_wake_c1_p99 53.292 us, loaded_wake_c1_max 56.667 us, loaded_wake_c2_p50 37.292 us, loaded_wake_c2_p99 45.292 us, loaded_wake_c2_max 48.167 us, loaded_wake_c3_p50 50.625 us, loaded_wake_c3_p99 66.625 us, loaded_wake_c3_max 66.375 us, loaded_wake_c4_p50 33.292 us, loaded_wake_c4_p99 41.292 us, loaded_wake_c4_max 42.042 us, loaded_wake_c5_p50 33.292 us, loaded_wake_c5_p99 41.292 us, loaded_wake_c5_max 45.708 us, loaded_wake_c6_p50 23.958 us, loaded_wake_c6_p99 31.958 us, loaded_wake_c6_max 32.208 us, loaded_wake_c7_p50 17.292 us, loaded_wake_c7_p99 25.292 us, loaded_wake_c7_max 26.333 us, task_lock_wait_max 4.667 us, masked_max_c2 51.917 us, masked_max_c0 38.708 us, masked_max_c3 29.25 us, workload_masked_max_c3 29.25 us, workload_global_isr_lock_acquisitions 645, workload_global_task_lock_acquisitions 443 |
| `1793589957202732056-c566197a` | 2026-11-02 | 3-3 | `21eb96d88c7b` | result-mismatch | failed | run_total 21.954 s, loaded_wake_c0_p50 101.292 us, loaded_wake_c0_p99 122.625 us, loaded_wake_c0_max 122.542 us, masked_max_c0 21.833 us, workload_masked_max_c0 21.583 us, loaded_wake_c1_p50 41.292 us, loaded_wake_c1_p99 53.292 us, loaded_wake_c1_max 52.208 us, masked_max_c1 32.083 us, workload_masked_max_c1 18.75 us, loaded_wake_c2_p50 37.292 us, loaded_wake_c2_p99 50.625 us, loaded_wake_c2_max 48.833 us, masked_max_c2 19.375 us, workload_masked_max_c2 19.375 us, loaded_wake_c3_p50 50.625 us, loaded_wake_c3_p99 63.958 us, loaded_wake_c3_max 62.958 us, masked_max_c3 45.75 us, workload_masked_max_c3 31.833 us, loaded_wake_c4_p50 33.292 us, loaded_wake_c4_p99 42.625 us, loaded_wake_c4_max 41.833 us, masked_max_c4 16.375 us, workload_masked_max_c4 16.375 us, loaded_wake_c5_p50 31.958 us, loaded_wake_c5_p99 42.625 us, loaded_wake_c5_max 42.625 us, masked_max_c5 46.583 us, workload_masked_max_c5 13.458 us, loaded_wake_c6_p50 25.292 us, loaded_wake_c6_p99 34.625 us, loaded_wake_c6_max 33.708 us, masked_max_c6 17.167 us, workload_masked_max_c6 17.167 us, loaded_wake_c7_p50 17.958 us, loaded_wake_c7_p99 26.625 us, loaded_wake_c7_max 28 us, masked_max_c7 20.125 us, workload_masked_max_c7 11.167 us, task_lock_wait_max 11.917 us |
| `1793591168258054823-33d41fe5` | 2026-11-02 | 3-3 | `b04172228b18` | returned | failed | run_total 21.981 s, busy_masked_c0 20.833 us, busy_masked_c0_instructions 777, busy_masked_c0_cycles 39345, busy_masked_c0_l1d_refills 26, busy_masked_c0_l2d_refills 31, busy_masked_c1 19.083 us, busy_masked_c1_instructions 1447, busy_masked_c1_cycles 36925, busy_masked_c1_l1d_refills 39, busy_masked_c1_l2d_refills 37, busy_masked_c2 17.375 us, busy_masked_c2_instructions 1447, busy_masked_c2_cycles 33005, busy_masked_c2_l1d_refills 32, busy_masked_c2_l2d_refills 63, busy_masked_c3 29.958 us, busy_masked_c3_instructions 1600, busy_masked_c3_cycles 53959, busy_masked_c3_l1d_refills 21, busy_masked_c3_l2d_refills 40, busy_masked_c4 14.792 us, busy_masked_c4_instructions 1383, busy_masked_c4_cycles 28479, busy_masked_c4_l1d_refills 38, busy_masked_c4_l2d_refills 57, busy_masked_c5 11.917 us, busy_masked_c5_instructions 1383, busy_masked_c5_cycles 23480, busy_masked_c5_l1d_refills 38, busy_masked_c5_l2d_refills 46, busy_masked_c6 21.917 us, busy_masked_c6_instructions 2095, busy_masked_c6_cycles 35042, busy_masked_c6_l1d_refills 37, busy_masked_c6_l2d_refills 34, busy_masked_c7 7.75 us, busy_masked_c7_instructions 1373, busy_masked_c7_cycles 16831, busy_masked_c7_l1d_refills 44, busy_masked_c7_l2d_refills 39, span_instructions_max 3559 |
| `1793593624607852116-12606140` | 2026-11-02 | 3-3 | `820266d18ee9` | returned | passed | run_total 21.008 s, busy_masked_c0 19.125 us, busy_masked_c0_instructions 780, busy_masked_c0_cycles 37108, busy_masked_c0_stall_backend 30335, busy_masked_c0_mem_access 279, busy_masked_c0_bus_access 366, busy_masked_c0_l1d_refills 25, busy_masked_c1 18.042 us, busy_masked_c1_instructions 1448, busy_masked_c1_cycles 35241, busy_masked_c1_stall_backend 34140, busy_masked_c1_mem_access 505, busy_masked_c1_bus_access 469, busy_masked_c1_l1d_refills 35, busy_masked_c2 18.375 us, busy_masked_c2_instructions 1448, busy_masked_c2_cycles 37893, busy_masked_c2_stall_backend 36704, busy_masked_c2_mem_access 507, busy_masked_c2_bus_access 477, busy_masked_c2_l1d_refills 35, busy_masked_c3 29 us, busy_masked_c3_instructions 1657, busy_masked_c3_cycles 53959, busy_masked_c3_stall_backend 36868, busy_masked_c3_mem_access 441, busy_masked_c3_bus_access 419, busy_masked_c3_l1d_refills 23, busy_masked_c4 14.625 us, busy_masked_c4_instructions 1386, busy_masked_c4_cycles 28540, busy_masked_c4_stall_backend 27461, busy_masked_c4_mem_access 489, busy_masked_c4_bus_access 397, busy_masked_c4_l1d_refills 35, busy_masked_c5 12.458 us, busy_masked_c5_instructions 1386, busy_masked_c5_cycles 25488, busy_masked_c5_stall_backend 24415, busy_masked_c5_mem_access 489, busy_masked_c5_bus_access 389, busy_masked_c5_l1d_refills 34, busy_masked_c6 14.917 us, busy_masked_c6_instructions 2234, busy_masked_c6_cycles 29675, busy_masked_c6_stall_backend 28039, busy_masked_c6_mem_access 619, busy_masked_c6_bus_access 372, busy_masked_c6_l1d_refills 29, busy_masked_c7 6.375 us, busy_masked_c7_instructions 1376, busy_masked_c7_cycles 14491, busy_masked_c7_stall_backend 13541, busy_masked_c7_mem_access 584, busy_masked_c7_bus_access 340, busy_masked_c7_l1d_refills 41 |
<!-- evidence-table:end -->
