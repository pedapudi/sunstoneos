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

0c. Known limits of the unmasked task-lock wait (`runtime/lock_prewait.c`): (1) an EL0 task cannot read the interrupt mask, so it infers an open mask from a zero critical nesting count and the absence of the task lock; a bare `portSET_INTERRUPT_MASK` outside a critical section, which does not take the task lock, can therefore wait with the mask raised, for at most the 100 us limit; (2) the 100 us limit is checked only when an event wakes the core, so a wait behind a long holder can last as long as the holder (914 us once, on the fork, core 3); the wait is unmasked either way.
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

### Lock wait inside each span

Second pair of runs on the same image and load (queue entries `1793601953161313034-b1ecfb62` stock and `1793602023862614320-8f9bd37f` fork, output exact). The recorder now adds the time spent acquiring a kernel lock after the mask rose. Columns: the longest span, the lock wait inside that span, the longest lock wait in any span, the number of spans above 5 us, and lock wait as a share of the total length of the spans above 5 us.

Stock kernel:

| max us | wait in longest us | max wait us | spans>5us | wait share of spans>5us | open | close |
|---|---|---|---|---|---|---|
| 30 | 27 | 27 | 1362 | 82% | vTaskYieldWithinAPI+0x10 | FreeRTOS_Yield+0x1fc |
| 30 | 28 | 29 | 3410 | 89% | xTaskGetSchedulerState+0x1c | xTaskGetSchedulerState+0x34 |
| 29 | 28 | 28 | 1221 | 89% | xQueueReceive+0x44 | xQueueReceive+0xcc |
| 29 | 28 | 28 | 1015 | 92% | vTaskSuspendAll+0x20 | xQueueReceive+0xd0 |
| 29 | 28 | 28 | 978 | 93% | xQueueReceive+0x44 | xQueueReceive+0xa0 |
| 29 | 27 | 27 | 44 | 92% | board_service_progress+0x3c | _ZN12_GLOBAL__N_110yield_hookEv+0xc |
| 27 | 26 | 26 | 1450 | 88% | xQueueGenericSend+0x60 | xQueueGenericSend+0x160 |
| 25 | 24 | 24 | 25 | 92% | vTaskSuspendAll+0x20 | vTaskDelay+0x18 |
| 24 | 23 | 23 | 227 | 89% | xQueueReceive+0x1ac | xQueueReceive+0xa0 |
| 24 | 22 | 22 | 31 | 87% | os_thermal_task+0x544 | os_thermal_task+0x60c |
| 17 | 15 | 15 | 20 | 84% | vTaskDelay+0x4c | FreeRTOS_Yield+0x1fc |
| 17 | 15 | 15 | 34 | 88% | board_service_progress+0x3c | _ZN12_GLOBAL__N_18dispatchEmPFvPvmmiES0_b+0x1f0 |
| 16 | 15 | 15 | 1 | 94% | xQueueReceive+0x1ac | xQueueReceive+0x1ac |
| 13 | 11 | 11 | 3 | 83% | xQueueGetMutexHolder+0x18 | xQueueGetMutexHolder+0x30 |
| 11 | 10 | 10 | 99 | 87% | board_service_progress+0x3c | _ZN12_GLOBAL__N_18dispatchEmPFvPvmmiES0_b+0x1f8 |
| 8 | 7 | 7 | 2 | 87% | board_service_progress+0x3c | _ZN12_GLOBAL__N_18dispatchEmPFvPvmmiES0_b+0x64 |
| 7 | 6 | 6 | 1 | 86% | xQueueSemaphoreTake+0x3c | xQueueSemaphoreTake+0x7c |

Partitioned fork:

| max us | wait in longest us | max wait us | spans>5us | wait share of spans>5us | open | close |
|---|---|---|---|---|---|---|
| 34 | 33 | 33 | 1225 | 96% | ssKernelObjectSuspend+0x10 | xQueueReceive+0xd0 |
| 34 | 32 | 32 | 3714 | 88% | ssKernelObjectEnter+0x14 | ssKernelObjectExit+0xd8 |
| 34 | 32 | 32 | 237 | 85% | vTaskEnterCritical+0x20 | ssKernelObjectExit+0xd8 |

Result. The hypothesis holds on both kernels: lock wait is 82 to 96% of every span above 5 us, and in the longest span of each pair all but 1 to 3 us is wait. The held part is 1 to 3 us for the longest spans. The prediction's exception was wrong: the core 6 service and dispatch pairs wait for the lock as much as the rest (87 to 92%). The memory-work account of the earlier rounds explains the length of the holder's own span, which each waiter then adds to its wait, so shortening held work still shortens waits, but the first fix is to stop waiting with the mask raised. The fix goes into the lock acquisition in the port's task lock and interrupt lock, which sits in the protected `patches/freertos-smp-*.patch` and the fork's object enter; the protected diff goes to the coordinator.

### Unmasked wait for the task lock

`runtime/lock_prewait.c` (`SUNSTONE_LOCK_PREWAIT`, design: [waiting for the task lock with the mask open](../designs/scheduler-partitioned.md#waiting-for-the-task-lock-with-the-mask-open)) makes kernel entry points wait, mask open, until at most one core holds or waits for the task lock, then mask and queue as before. It needs no change to a protected patch. Same image and load as the sweep above; queue entries `1793603800991531488-e4485ccd` (stock) and `1793603872067351594-076593c9` (fork), output exact.

Three board attempts preceded it. The first never waited because the open mask reads back as 0xf0, not the 0xff written. The second waited for under 1% of entries because nearly all kernel callers are EL0 tasks, which cannot read the mask. A third image deadlocked on the stock kernel (watchdog reset): a task that had suspended the scheduler held the task lock with the mask open and waited for its own hold, which the guard now skips.

| | stock before | stock after | fork before | fork after |
| --- | --- | --- | --- | --- |
| Spans above 5 us | 9923 | 5206 | 5176 | 3291 |
| Lock wait inside those spans, all cores | 99.7 ms | 48.8 ms | 57.4 ms | 25.6 ms |
| Longest span | 30 us | 30 us | 35 us | 30 us |
| Longest masked lock wait per core 0 to 7 (us) | 22 21 22 28 28 29 29 12 | 26 27 25 27 28 29 29 11 | 25 15 27 26 33 32 32 6 | 15 18 12 19 20 19 19 7 |
| Longest unmasked wait (us) | 0 | 82 | 0 | 914 on core 3, 77 on core 0, at most 19 elsewhere |
| Recognition time (ms) | 136.388 | 136.574 (+0.14%) | 127.188 | 128.427 (+0.97%) |

The wait is no more than 100 us: it ended by the limit 0 times on the fork and 4 times on the stock kernel. The 914 us on the fork is one wait that slept in `WFE` while another core held the lock through a long section; it waited unmasked and no later than the holder, but the limit is checked only when an event wakes the core, so it is not enforced there.

Result. The wait halves the time spent in long masked spans on both kernels and keeps the recognition within 2% of the image without it, but it does not reach the 1 to 3 us goal: the longest spans and the longest masked waits are unchanged on the stock kernel and 13 to 14 us lower on the fork. Cores that see the lock nearly free queue together, so a burst of eight still queues seven holds. A waiter cannot take a ticket with the mask open: an interrupt that switches tasks would leave the new task queued behind a ticket held by a descheduled one.

Convoy. The lock is the task lock (`xPortLocks[1]`), the same one every kernel call takes. Lock wait in the long spans by caller before and after (ms, all cores): stock `xTaskGetSchedulerState` 33.5 to 14.3, `xQueueReceive` 26.4 to 10.9, `xQueueGenericSend` 14.2 to 5.4, `vTaskYieldWithinAPI` 12.4 to 10.2, `vTaskSuspendAll` 10.9 to 6.5. Fork `ssKernelObjectEnter` 42.2 to 18.2, `ssKernelObjectSuspend` 13.2 to 6.0, `vTaskEnterCritical` 2.0 to 1.4. On the fork the contended object is the global task lock taken by every object enter and suspend, not a per-owner object. The next fixes are shorter holds (each hold is about 3.5 us of dependent DRAM refills) or less sharing: per-queue or per-owner locking in place of the task lock, and an abortable queue lock so a waiter can take its place in line with the mask open.

## Robot table: control path and task path, same image and load

One board run per kernel on port 3-3 (queue entries `1793605058169799359-f2420456` stock and `1793605129875013399-cf0da02e` fork), the mimus OS image with the control interrupts (`SUNSTONE_CONTROL_IRQ`), engine bcaf39ff5, eight workers under the recognition, output exact. On cores 1 to 5 the two paths run at the same time at 1, 2, 4, 8 and 10 kHz, one rate per core. Design: [control interrupts above the API mask](../designs/scheduler-partitioned.md#control-interrupts-above-the-api-mask-do-not-wait-for-kernel-spans). Latency is the handler or task start minus the programmed deadline; jitter is the error of the interval between starts against the periods that passed. Each cell is the maximum in microseconds; the 99.99th percentile equals the maximum because a core sees 200 to 2000 periods in the 0.2 s recognition, so it supports no tighter tail than that (the 10^6-period runs were not made). No sample was dropped, no sequence gap, no missed period in either path on either kernel.

| Rate | Jitter bound (5%) | Control path latency stock / fork | Control path jitter stock / fork | Task path latency stock / fork | Task path jitter stock / fork |
| --- | --- | --- | --- | --- | --- |
| 1 kHz | 50 | 1.5 / 0.5 | 1.2 / 0.2 | 15.9 / 8.2 | 12.5 / 4.2 |
| 2 kHz | 25 | 1.5 / 0.7 | 1.3 / 0.3 | 28.6 / 11.4 | 25.8 / 7.6 |
| 4 kHz | 12.5 | 6.1 / 3.0 | 5.8 / 2.7 | 23.8 / 30.9 | 19.3 / 27.2 |
| 8 kHz | 6.25 | 5.3 / 2.5 | 5.0 / 2.2 | 28.9 / 17.2 | 25.5 / 13.7 |
| 10 kHz | 5 | 3.5 / 2.5 | 3.2 / 2.2 | 32.0 / 12.0 | 26.5 / 8.4 |

| | Stock | Fork |
| --- | --- | --- |
| Recognition time, image with control interrupts | 114.838 ms | 113.398 ms |
| Recognition time, plain image (no recorder, no interrupts) | 109.629 ms | 110.178 ms (+0.50%) |
| Longest masked span (sweep image, with the recorder) | 30 us | 35 us (30 us with the unmasked task-lock wait) |

Result. The control path meets the 10 us wake target and the 5% jitter bound at every rate on both kernels, including 10 kHz (3.2 us stock, 2.2 us fork against 5 us). The task path meets neither on either kernel: wake latency 8 to 32 us, and jitter 4 to 27 us against the bound, which at 4 kHz (12.5 us) is missed by both (19.3 and 27.2 us). The task path's latency tracks the longest masked span and the task-lock convoy; the control path does not. The fork's task path is better at four of five rates and worse at 4 kHz, and neither is near the target, so the fork does not change which path serves a 4 kHz loop: the control path does.

Mimus. The instrumented sweep images showed the fork 6.8% faster (127.2 against 136.4 ms); the plain images show the fork 0.50% slower (110.178 against 109.629 ms), as in the speed stream. The span recorder explains the difference: it adds 26 ms to the stock recognition and 17 ms to the fork's. The control interrupts add 4.7 ms (4.4%) to stock and 3.2 ms (2.9%) to the fork while five handlers run at up to 10 kHz.

## Evidence

Board results with a queue entry are records in `docs/evidence/scheduler.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-scheduler-20261101.md).

<!-- evidence-table:begin -->
35 records in `docs/evidence/scheduler.jsonl`; run `tools/evidence.py sync` to regenerate this table.

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
| `1793601953161313034-b1ecfb62` | 2026-11-02 | 3-3 | `81829cfe943b` | returned | passed | run_total 70.104 s, masked_span_max 30 us, lock_wait_share_of_long_spans 82 -96% |
| `1793602023862614320-8f9bd37f` | 2026-11-02 | 3-3 | `06a4747c4deb` | returned | passed | run_total 70.245 s, masked_span_max 34 us, lock_wait_share_of_long_spans 82 -96% |
| `1793603800991531488-e4485ccd` | 2026-11-02 | 3-3 | `09bbd24cff47` | returned | passed | run_total 69.982 s, masked_span_max 30 us |
| `1793603872067351594-076593c9` | 2026-11-02 | 3-3 | `6340760b1c30` | returned | passed | run_total 69.989 s, masked_span_max 30 us |
| `1793605058169799359-f2420456` | 2026-11-02 | 3-3 | `cb1356333b6e` | returned | passed | run_total 71.133 s, control_latency_max_4khz 6.1 us |
| `1793605129875013399-cf0da02e` | 2026-11-02 | 3-3 | `5863e0cde9bb` | returned | passed | run_total 86.704 s, control_latency_max_4khz 3 us |
| `1793605217849580518-3247bc18` | 2026-11-02 | 3-3 | `1f8c64639e22` | returned | passed | run_total 106.288 s, recognition_time 109.629 ms |
| `1793605330258102733-1624632e` | 2026-11-02 | 3-3 | `e6995c5f33fc` | returned | passed | run_total 79.197 s, recognition_time 110.178 ms |
<!-- evidence-table:end -->
