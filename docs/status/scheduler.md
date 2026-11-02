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
its masked-time budget fails. On the recorded recognition it is exact and 5.1% slower than the global
kernel, so it stays off by default. Heterogeneous placement and tickless timing have no physical
qualification.

## Open work

0b. Step 2 of the measure-first plan: fix the spans in the sweep table longest first on both kernels, each with a
   before and after span and a model-time check against 2%; then the above-mask control path, the robot workload and
   the tickless comparison. The sweep under the mimus load is below.
0a. Robot real-time response is the main requirement (targets in the design: wake latency 10 us, control jitter
   5% of the period at 1 to 10 kHz, no deadline miss, model within 2% of alone). Not yet measured with a robot-like workload. The measured masked spans (22 to 46 us
   in the fork under load) already exceed the wake target, so shorten or split them and put the hardest control
   path above the API interrupt mask.
0. Known defect, idle pull: a task that waits behind a running task on its owner while an allowed CPU idles
   can stay unmoved. The eight-core check's stranding phase fails on the wall clock about 1 run in 50 at host
   load 30, at `scheduler-partitioned-qemu.c:386` (`withholding_completed == i + 1` after 100 ms). Forty-three
   runs on the instruction counter with different start delays (`--skew 0..41`) all pass, so no deterministic
   reproduction exists. Idle pull stays off, as the fork does, until it is fixed.
1. Masked time stays above 25 us on cores 0, 2, 3 and at end-of-run suspension: measured phase spans reach 29 us
   (core 3) and the end-of-run self-suspension 46 us. Backend stalls are 94 to 98% of their cycles with 23 to 38
   L1D refills each, so the cause is dependent line fetches under saturated DRAM. Fewer lines per entry or
   prefetch would shorten them; neither is implemented.
2. The fork stays off by default: on the current engine the recorded recognition is exact and 0.4% slower with it
   (110.3 ms against 109.8 ms; 5.1% slower on the older engine). The owner core takes 65 to 85 more interrupts and
   loses about 2.2 ms. Owner-core counters find no single cause above 1.5 ms: the wake path nets 1.8 ms faster,
   the owner share and sends are 0.9 and 0.7 ms slower with near-equal refills, and the timed serial code
   (1.4 ms) is outside the windows. The speed stream is closed with the fork off by default.
   Replacing the compatibility object locks in C is closed with the fork; idle pull is implemented and returned
   the CPU control unchanged, with no workload that needs it.
3. Tickless idle on the global kernel, as its own goal judged on power and wake latency: remove the periodic tick
   on idle cores (work in progress on `work/tickless-idle-1101`; changes the protected `patches/freertos-smp-*.patch`).
4. Preserve lock-worker port-lock hunks and probe entry code. The prepared image,
   review, commands and remaining limits are in [the history](../history/status-scheduler-20261101.md).

## Masked-span sweep under the mimus load

One board run per kernel on port 3-3 (queue entries `1793601405518603317-ea30c713` stock and `1793601476379926752-0c3dd3cd` fork), the mimus OS image with the OS services and drivers linked, engine bcaf39ff5, eight workers, the three recorded inputs, output exact. The recorder lists every distinct (open call, close call) pair of masked spans on each core; the tables keep pairs whose longest span exceeds 5 microseconds. Sites are return addresses shown as function and offset. The count is spans over all cores in the recognition, and the p99.99 is the upper edge of the histogram bin that holds that rank (the maximum when a pair has fewer than 10^4 spans on a core, as most do). No table overflowed. The design section is [the span sweep](../designs/scheduler-partitioned.md#the-span-sweep-lists-every-masked-pair-before-any-span-is-fixed).

Stock kernel, 17 pairs above 5 microseconds (68 distinct pairs in all):

| max us | p99.99 us | count | open site | close site | max by core |
|---|---|---|---|---|---|
| 31 | 31 | 18148 | xTaskGetSchedulerState+0x1c | xTaskGetSchedulerState+0x34 | 0:17 1:18 2:22 3:31 4:30 5:25 6:28 7:9 |
| 30 | 30 | 2823 | vTaskYieldWithinAPI+0x10 | FreeRTOS_Yield+0x1fc | 0:24 1:30 2:24 3:23 4:14 5:9 6:9 7:11 |
| 30 | 30 | 7195 | xQueueGenericSend+0x60 | xQueueGenericSend+0x160 | 0:14 1:15 2:17 3:23 4:23 5:30 6:18 7:8 |
| 29 | 29 | 2415 | xQueueReceive+0x44 | xQueueReceive+0xcc | 0:13 1:18 2:24 3:29 4:28 5:19 6:11 |
| 29 | 29 | 2885 | vTaskSuspendAll+0x20 | xQueueReceive+0xd0 | 0:20 1:18 2:22 3:21 4:29 5:22 6:12 7:7 |
| 28 | 28 | 2431 | xQueueReceive+0x44 | xQueueReceive+0xa0 | 6:28 |
| 28 | 28 | 1392 | board_service_progress+0x3c | _ZN12_GLOBAL__N_110yield_hookEv+0xc | 6:28 |
| 26 | 26 | 250 | vTaskSuspendAll+0x20 | vTaskDelay+0x18 | 0:26 2:9 3:14 4:22 5:15 |
| 26 | 26 | 250 | vTaskDelay+0x4c | FreeRTOS_Yield+0x1fc | 0:16 2:8 3:19 4:17 5:26 |
| 23 | 23 | 1 | xTaskGetSchedulerState+0x1c | xTaskGetSchedulerState+0x1c | 4:23 |
| 20 | 20 | 1591 | xQueueReceive+0x1ac | xQueueReceive+0xa0 | 2:7 3:14 4:18 5:20 6:12 |
| 20 | 20 | 238 | os_thermal_task+0x544 | os_thermal_task+0x60c | 0:13 2:15 3:16 4:9 5:20 |
| 19 | 19 | 20 | xQueueGetMutexHolder+0x18 | xQueueGetMutexHolder+0x30 | 4:13 5:19 |
| 16 | 16 | 470 | board_service_progress+0x3c | _ZN12_GLOBAL__N_18dispatchEmPFvPvmmiES0_b+0x1f0 | 6:16 |
| 13 | 13 | 2 | xQueueSemaphoreTake+0x3c | xQueueSemaphoreTake+0x7c | 5:13 |
| 10 | 10 | 470 | board_service_progress+0x3c | _ZN12_GLOBAL__N_18dispatchEmPFvPvmmiES0_b+0x1f8 | 6:10 |
| 6 | 6 | 470 | board_service_progress+0x3c | _ZN12_GLOBAL__N_18dispatchEmPFvPvmmiES0_b+0x64 | 6:6 |

Partitioned fork, 4 pairs above 5 microseconds:

| max us | p99.99 us | count | open site | close site | max by core |
|---|---|---|---|---|---|
| 33 | 33 | 17362 | ssKernelObjectEnter+0x14 | ssKernelObjectExit+0xd8 | 0:17 1:15 2:22 3:27 4:33 5:30 6:32 7:11 |
| 33 | 33 | 3077 | vTaskEnterCritical+0x20 | ssKernelObjectExit+0xd8 | 3:17 6:33 |
| 27 | 27 | 2459 | ssKernelObjectSuspend+0x10 | xQueueReceive+0xd0 | 0:18 1:15 2:20 3:27 4:26 5:23 6:8 |
| 6 | 6 | 826 | xTaskGetSchedulerState+0x24 | xTaskGetSchedulerState+0x58 | 0:6 |

Findings. Every pair on both kernels exceeds the 10 microsecond wake target and the 5 microsecond defect line, and the longest are 31 microseconds (stock) and 33 microseconds (fork). The earlier 95 to 206 microsecond stock spans did not recur under the mimus load; they came from the eight-core soak and are not in this table, so the soak needs its own sweep before those spans are called fixed. The stock kernel's longest span is the scheduler-state critical section, spread over every core, and the queue send and receive paths; the fork's are the object enter and exit pair and the critical section that ends in the object exit. A span is not a function: the same function appears as open and close sites on several rows, so a fix must change the memory touched inside the masked region, not one function.

## Evidence

Board results with a queue entry are records in `docs/evidence/scheduler.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-scheduler-20261101.md).

<!-- evidence-table:begin -->
27 records in `docs/evidence/scheduler.jsonl`; run `tools/evidence.py sync` to regenerate this table.

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
| `1793594897836719012-99dd8f9a` | 2026-11-02 | 3-3 | `c43a895e88de` | returned | passed | run_total 19.024 s, loaded_wake_c0_p50 101.292 us, loaded_wake_c0_p99 133.292 us, loaded_wake_c0_max 135.5 us, masked_max_c0 21.875 us, loaded_wake_c1_p50 41.292 us, loaded_wake_c1_p99 53.292 us, loaded_wake_c1_max 52.667 us, masked_max_c1 41.208 us, loaded_wake_c2_p50 37.292 us, loaded_wake_c2_p99 47.958 us, loaded_wake_c2_max 52.75 us, masked_max_c2 21.792 us, loaded_wake_c3_p50 50.625 us, loaded_wake_c3_p99 66.625 us, loaded_wake_c3_max 66 us, masked_max_c3 34.208 us, loaded_wake_c4_p50 31.958 us, loaded_wake_c4_p99 42.625 us, loaded_wake_c4_max 54.833 us, masked_max_c4 20.417 us, loaded_wake_c5_p50 31.958 us, loaded_wake_c5_p99 42.625 us, loaded_wake_c5_max 46.208 us, masked_max_c5 28.458 us, loaded_wake_c6_p50 23.958 us, loaded_wake_c6_p99 31.958 us, loaded_wake_c6_max 34.375 us, masked_max_c6 23.208 us, loaded_wake_c7_p50 17.292 us, loaded_wake_c7_p99 23.958 us, loaded_wake_c7_max 24.875 us, masked_max_c7 25.333 us |
| `1793595172555717085-53db73e3` | 2026-11-02 | 3-3 | `8130d5f6f184` | result-mismatch | passed | run_total 70.215 s, compute_time 348685 us, real_time_factor 0.09713, frames 90, samples 57440, yields 333 |
| `1793595243567121086-cf21e7ef` | 2026-11-02 | 3-3 | `9d07d2461f33` | result-mismatch | failed | run_total 71.04 s, compute_time 366355 us, real_time_factor 0.10205, frames 90, samples 57440, yields 342 |
| `1793595339857900481-2081a626` | 2026-11-02 | 3-3 | `8130d5f6f184` | result-mismatch | passed | run_total 70.103 s, compute_time 348752 us, real_time_factor 0.09715, frames 90, samples 57440, yields 331 |
| `1793595434552850928-da60cec8` | 2026-11-02 | 3-3 | `9d07d2461f33` | result-mismatch | failed | run_total 70.348 s, compute_time 366490 us, real_time_factor 0.10209, frames 90, samples 57440, yields 342 |
| `1793596277164206286-d6abcfc2` | 2026-11-02 | 3-3 | `726c83b62df3` | returned | passed | run_total 70.147 s, compute_time 109585 us, real_time_factor 0.03053, call_total 80564 us, dispatch_kernel 5381 us, owner_compute 46358 us, last_finisher_wait 21961 us, completion_path 6862 us, wake_to_run_sum 7375 us, wake_to_run_max_sum 1791 us, worker_compute 223596 us, worker_done_kernel 15012 us, calls 470, placement_errors 0, core_changes 0 |
| `1793596348014754310-98c4819c` | 2026-11-02 | 3-3 | `9dd9696975c2` | returned | passed | run_total 70.054 s, compute_time 110111 us, real_time_factor 0.03067, call_total 79672 us, dispatch_kernel 5971 us, owner_compute 47199 us, last_finisher_wait 21264 us, completion_path 5237 us, wake_to_run_sum 9625 us, wake_to_run_max_sum 2092 us, worker_compute 224325 us, worker_done_kernel 9491 us, calls 470, placement_errors 0, core_changes 0 |
| `1793597031558077290-ec888c56` | 2026-11-02 | 3-3 | `bca9feb836ec` | returned | passed | run_total 71.282 s, compute_time 109831 us, real_time_factor 0.03059, call_total 80800 us, dispatch_kernel 5403 us, owner_compute 46418 us, last_finisher_wait 22044 us, completion_path 6934 us, owner_core_sgis 293 |
| `1793597103749976112-6a36d3ff` | 2026-11-02 | 3-3 | `80ed05ccaefd` | returned | passed | run_total 73.381 s, compute_time 110067 us, real_time_factor 0.03066, call_total 79647 us, dispatch_kernel 5988 us, owner_compute 47232 us, last_finisher_wait 21205 us, completion_path 5220 us, owner_core_sgis 358 |
| `1793597263633780613-1139f35a` | 2026-11-02 | 3-3 | `45846dc37f34` | returned | passed | run_total 73.225 s, compute_time 110239 us, real_time_factor 0.03071, call_total 79789 us, dispatch_kernel 6033 us, owner_compute 47236 us, last_finisher_wait 21192 us, completion_path 5327 us, owner_core_sgis 359 |
| `1793597490409282453-8e76d47a` | 2026-11-02 | 3-3 | `bca9feb836ec` | returned | passed | run_total 71.277 s, compute_time 109939 us, real_time_factor 0.03062, call_total 80814 us, dispatch_kernel 5393 us, owner_compute 46361 us, last_finisher_wait 22172 us, completion_path 6887 us, owner_core_sgis 295 |
| `1793597562539071615-6699c1cf` | 2026-11-02 | 3-3 | `f679a60ee591` | returned | passed | run_total 70.833 s, compute_time 110240 us, real_time_factor 0.03071, call_total 79764 us, dispatch_kernel 6058 us, owner_compute 47282 us, last_finisher_wait 21249 us, completion_path 5173 us, owner_core_sgis 368 |
| `1793597633935865983-352fe050` | 2026-11-02 | 3-3 | `bca9feb836ec` | returned | passed | run_total 70.552 s, compute_time 109729 us, real_time_factor 0.03057, call_total 80666 us, dispatch_kernel 5377 us, owner_compute 46366 us, last_finisher_wait 21890 us, completion_path 7032 us, owner_core_sgis 296 |
| `1793597705209176004-1a689849` | 2026-11-02 | 3-3 | `f679a60ee591` | returned | passed | run_total 71.208 s, compute_time 110311 us, real_time_factor 0.03073, call_total 79833 us, dispatch_kernel 6065 us, owner_compute 47262 us, last_finisher_wait 21221 us, completion_path 5284 us, owner_core_sgis 380 |
| `1793599717707582059-be657013` | 2026-11-02 | 3-3 | `3ddd2272d06b` | returned | passed | run_total 69.836 s, compute_time 110037 us, real_time_factor 0.03065, core6_total_cycles 3.76359e+08, core6_total_l1d_refills 1.40102e+06, core6_total_l2d_refills 523400, core6_total_stall_backend 1.38231e+08, core6_serial_cycles 2.41434e+08, core6_serial_l1d_refills 684543, core6_serial_l2d_refills 155702, core6_serial_stall_backend 8.80371e+07, core6_send_cycles 1.05677e+07, core6_send_l1d_refills 87466, core6_send_l2d_refills 75370, core6_send_stall_backend 5.11003e+06, core6_owner_cycles 9.2856e+07, core6_owner_l1d_refills 489574, core6_owner_l2d_refills 172442, core6_owner_stall_backend 3.12611e+07, core6_tail_cycles 3.15015e+07, core6_tail_l1d_refills 139441, core6_tail_l2d_refills 119886, core6_tail_stall_backend 1.38231e+07, core6_kernel_publish_entries 0, core6_kernel_publish_cycles 0, core6_kernel_publish_l1d_refills 0, core6_kernel_publish_l2d_refills 0, core6_kernel_publish_stall_backend 0, core6_kernel_service_entries 0, core6_kernel_service_cycles 0, core6_kernel_service_l1d_refills 0, core6_kernel_service_l2d_refills 0, core6_kernel_service_stall_backend 0 |
| `1793599788007346215-eb5bcd80` | 2026-11-02 | 3-3 | `3955831c4a46` | returned | passed | run_total 70.453 s, compute_time 110478 us, real_time_factor 0.03077, core6_total_cycles 3.77264e+08, core6_total_l1d_refills 1.31643e+06, core6_total_l2d_refills 436890, core6_total_stall_backend 1.40047e+08, core6_serial_cycles 2.49382e+08, core6_serial_l1d_refills 683751, core6_serial_l2d_refills 154823, core6_serial_stall_backend 9.19342e+07, core6_send_cycles 1.21946e+07, core6_send_l1d_refills 62127, core6_send_l2d_refills 51091, core6_send_stall_backend 6.77505e+06, core6_owner_cycles 9.46081e+07, core6_owner_l1d_refills 496821, core6_owner_l2d_refills 176483, core6_owner_stall_backend 3.24708e+07, core6_tail_cycles 2.10798e+07, core6_tail_l1d_refills 73731, core6_tail_l2d_refills 54493, core6_tail_stall_backend 8.86749e+06, core6_kernel_publish_entries 2658, core6_kernel_publish_cycles 2.04888e+06, core6_kernel_publish_l1d_refills 16157, core6_kernel_publish_l2d_refills 13420, core6_kernel_publish_stall_backend 1.22944e+06, core6_kernel_service_entries 1596, core6_kernel_service_cycles 810667, core6_kernel_service_l1d_refills 10580, core6_kernel_service_l2d_refills 6582, core6_kernel_service_stall_backend 313774 |
| `1793601405518603317-ea30c713` | 2026-11-02 | 3-3 | `898461eb02bb` | returned | passed | run_total 70.242 s, masked_span_max 31 us |
| `1793601476379926752-0c3dd3cd` | 2026-11-02 | 3-3 | `6e3aa0c5f181` | returned | passed | run_total 70.029 s, masked_span_max 33 us |
<!-- evidence-table:end -->
