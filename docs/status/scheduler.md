# Partitioned scheduler integration

**Status: complete for this stream. The partitioned fork is the default kernel in SMP images.** `-DSUNSTONE_PARTITIONED_SCHEDULER=OFF` builds the global FIFO-lock kernel as a control. The design is [the scheduler design](../designs/scheduler-partitioned.md); hardware values the interrupt and mask paths assert (GIC priority views, routes, CPU identity) are in [the A733 hardware facts](../hardware/a733-facts.md). Earlier status is in [the history](../history/status-scheduler-20261101.md).

**Software evidence:** host, emulator, source. **Board evidence:** CPU-return images on port 3-3 running the recorded mimus recognition with the OS services and drivers linked; every run below returned with exact output.

## Result against the robot targets

The robot's hard real-time work runs in interrupt handlers above the kernel API mask (the control path); its fusion and logic run as tasks woken by interrupts (the task path). Targets: wake latency at most 10 us, jitter at most 5% of the period, no missed period, model within 2% of its run alone. All figures are maxima over the periods named, in microseconds, from board soaks of 25 s under the full mimus load; the 99.99th percentile is a histogram bin's upper edge.

| Target | Control path (above the API mask) | Task path (woken task) |
| --- | --- | --- |
| Wake latency at most 10 us | Met: maximum 3.6, 99.99th percentile 3.0 at 10 kHz over 1 254 587 periods (fork, five soaks after the exit change) | Not met: maximum 23.5 to 31.5, 99.99th percentile 19.7 to 24.5 at 1, 2 and 4 kHz over 125 000 to 504 000 periods per rate; the same with the global locks and with per-object locks (next section) |
| Jitter at most 5% of the period | Met at 10 kHz: maximum 3.4 against 5 | Met at 1 kHz (maximum 18.5 against 50) and 2 kHz (20.9 against 25); not met at 4 kHz (28.3 maximum, 21.8 at the 99.99th percentile, against 12.5) |
| No missed period | Met: none in any soak | Met: none in any soak |
| Model within 2% | Met: the interrupts cost 0.02 to 0.06 percent of recognition time per kHz | Same |

The control path has been measured with long runs at 10 kHz only; at 1 to 8 kHz the short runs (200 to 2000 periods) show 0.5 to 6.1 us. The task path is the open target (backlog item 1).

Kernel choice. Pooled over five soaks per kernel (fork before the exit change, stock unchanged), the stock kernel's task path reached 40.8 us at 1 kHz and 41.8 us at 2 kHz against the fork's 23.5 and 25.3, and met the jitter bound at 1 kHz only; its control path reached 15.1 us (jitter 14.8) at 10 kHz against 12.8 (12.5) for the fork then, and 3.6 (3.4) after the exit change. Recognition time: stock 110.853 ms, fork 110.394 ms over the soak; without interrupts 109.975 and 110.023 ms. The fork is the default.

Model cost of the interrupts, as a share of recognition time per kHz of interrupt rate: fork 0.020 (17 kHz of control and task interrupts cost 0.34%), stock 0.047 (0.80%); with 25 kHz of each path on five cores, +2.73% on the fork and +4.55% on stock, 0.055 and 0.091 per kHz, about 0.5 and 0.9 us of recognition time per interrupt. A robot should budget 0.02 to 0.1 percent of model throughput per kHz, spread over the cores that take the interrupts.

Idle wake cost on the control core. The idle task is `dsb sy; wfi` and nothing else (`a733_cpu_idle`); there is no retention or clock-gating step. On the fork, 170 000 wake-ups per soak on core 4 took a median of 0.33 us from interrupt to handler, 0.5 to 0.7 us at the 99th percentile, 3.2 us at the 99.99th and at most 3.6 us, so the control core can stay in plain `wfi`. The stock kernel's `wfi` wake tail reached 10.9 us, because its interrupts waited behind `PSTATE.I` held by its yield handler.

## The task path's time is in the wake path, not in lock waits

Split of each task-path wake at the handler (`SUNSTONE_CONTROL_IRQ` images; entry delay is the handler start minus the deadline, gap is the task start minus the handler start). Two unmodified-fork soaks pooled, microseconds: entry delay p99.99 6.3 / 8.3 / 9.2 and maximum 10.0 / 8.8 / 11.3 at 1 / 2 / 4 kHz; gap median 4.8 / 4.2 / 3.7, p99.99 21.3 / 19.0 / 24.2, maximum 22.7 / 24.7 / 25.2. The gap carries the tail. Phase timers show the wake path (notification 1 us, interrupt-exit owner work 1.6 to 2 us, task switch 1.4 us on average) and its tail events (control drains and idle-pull transfers of 5 to 17 us, task switches to 11 us); none of them waits for a global lock. Per-object kernel locks (design: [object locks](../designs/scheduler-partitioned.md#object-locks-queues-event-groups-and-stream-buffers-lock-their-own-object)) leave the figures unchanged within the run-to-run spread, measured against a build of the same image in which queues, event groups and stream buffers still took the two global locks: entry delay maxima 5.3 / 4.8 / 5.5 against 5.7 / 5.0 / 5.7, gap maxima 15.7 / 25.0 / 19.8 against 16.0 / 25.8 / 16.0. A prefetch of the woken task's lines with idle pull deferred past a pending switch changed nothing and was removed. The 10 us target cannot be reached by locking changes; the wake path itself must shorten. Queues, event groups and stream buffers lock their own object and no build option restores the global locks for them. Operations on independent objects do not wait for each other, which the `objects` emulator case checks (it holds one queue's lock while six other cores operate on their own queues, and cannot finish if the queues share a lock), and the event-list insertion race is closed. The sections that still take the global locks, and why, are listed in the design under "Sections that keep the global locks".

Present result of the task-path work, in five points.

1. Notification change (kept). `ssKernelNotify` no longer raises and lowers the priority mask when it runs in an exception with `PSTATE.I` already set, which removes about four data and four instruction barriers per wake. Five soaks against three unmodified-fork soaks, pooled, task latency at 4 kHz: 99th percentile 11.5 to 7.5 us, 99.9th 19.8 to 11.5 us, wakes of 10 us or more 1.43% to 0.17% (989 to 2071 per soak before, 116 to 246 after). The 99.99th percentile (26.3 to 22.8 us) and the maximum (31.0 to 26.5 us) barely move, and the 1 and 2 kHz tails are unchanged. The five "after" soaks ran images with the per-object locks, which this kernel uses, and the three "before" soaks ran images with the global locks. Two phase-timed single soaks showed no difference between the two lock schemes, so the improvement is attributed to the notification change, but no soak set isolates it.
2. Per-object locks (kept; no effect on the task path). Queues, event groups and stream buffers lock their own hashed slot instead of the two global locks. The `objects` emulator case shows that unrelated objects do not wait for each other. On the board the global locks serialized independent queues heavily (99% of waits over 5 us were charged to a holder working on a different object), but removing that serialization did not change the task path's entry delay or gap. The kernel keeps them for the lower contention among recognition workers and for the closed race in point 3.
3. Event-list race (fixed). A blocking task inserted itself into an event list under the task lock alone, while the owner timing out another waiter of the same list removed its item under the interrupt lock alone. The insertion now holds the interrupt-side lock of the object's slot as well, and a linked-kernel host case fails without it.
4. The 10 us task-path target is not met: the 99.99th percentile is 19 to 26 us at 1, 2 and 4 kHz after the change, and the maximum 25 to 27 us. The measured cause is not lock waits. Entry delay is 5 to 11 us at most. The gap from handler to task start carries the tail: control drains and idle-pull transfers of 5 to 17 us (about 600 drains over 5 us per soak on cores 1 and 2), and task switches of up to 11 us. A cap of one idle-pull transfer per pass shortened the pull phase but not the drains, and is an off-by-default option. Locking changes alone cannot meet the target. Not done: a breakdown of one long drain into its records, which was the next step planned.
5. Task path on A76 cores (a single soak, entry `8d56d9aa`; not repeated). Core 7 at 4 kHz reached 8.8 us at most and core 6 at 1 kHz 12.3 us, against 14.8 to 20.6 us on the A55 cores 1 to 3. Slow wakes on the A55 cores refill two to three times as many L2 lines as fast ones. This suggests that running the real-time tasks on the A76 cores may help, but one soak does not establish it, and the recognition time with the task path on those cores (the 2% rule) was not measured.

## Intervals with PSTATE.I set on the control core

Only `PSTATE.I` holds off an interrupt above the API mask; kernel spans do not. Inventory of the fork's port, in instructions:

| Interval | Length | Status |
| --- | --- | --- |
| Interrupt entry to the C handler | 64 (33 register stores) | Needed: saves the frame |
| Interrupt exit after the handler (`DAIFSET`, `DSB SY`, EOI, restore, `ERET`) | about 70, one `DSB SY` | Needed |
| Owner work at interrupt exit (`ssKernelService`) | C, up to 11 us on core 4, 300 to 670 exits per soak over 5 us | Fixed: runs under the API mask with `PSTATE.I` clear (design: the exit work runs under the API mask) |
| Yield supervisor call: save, `ssKernelOwnerYieldTail`, `vTaskSwitchContext`, restore | 187 in the handler; the tail takes at most 3.7 us (5.0 us once) | Open (backlog) |
| Mask and unmask supervisor calls | 9 to 25 each, two `DSB SY` and two `ISB` | Open (backlog) |
| `uxPortSetInterruptMaskFromISR` and `vPortClearInterruptMaskFromISR` | seven instructions each, two `DSB SY`; `PSTATE.I` is set between the first `DAIFSET` and the `DAIFCLR` | Open (backlog) |

After the exit change the largest control latency per soak is 3.3 to 3.7 us, from the compute kernel's own memory stalls, so the open intervals do not reach the target on this load.

## Open defects and limits

- **Idle pull is on in the default fork** and every fork number here was measured with it on; only the linked test fixtures turn it off (`SS_KERNEL_TEST_NO_IDLE_PULL`). Its defect is a missed pull, a performance defect: a ready migratable task can wait behind a running task on its own core while an allowed core idles. It cannot lose or duplicate a task (a move is the acknowledged transfer of set-affinity, covered by the mixed-affinity stress and eight-core soak), cannot move a pinned task or break an affinity mask (a candidate is taken only when its mask holds the idle target) and cannot invert priority (only waiting tasks are candidates). The eight-core check's stranding phase fails on the wall clock about 1 run in 50 at host load 30 (`scheduler-partitioned-qemu.c:386`) and in none of 43 runs on the instruction counter. On the board three soaks show 3939 to 6722 pulls per 25 s (`1793617020998598953-a6ecdfc1`, `1793617131735748473-bef01b84`, `1793617277487733781-666ca5a4`); a miss cannot be seen there, and recognition time is unchanged. The cost of a miss is unmeasured.
- **Panic fixed.** One fork soak panicked before the fix (`1793608348858278119-8f8e4282`): the patched interrupt exit ran owner work inside masked kernel sections after a control interrupt. Reproduced in QEMU, fixed, and sixteen clean soaks follow (item 0d below).
- **Known limits of the unmasked task-lock wait** (`runtime/lock_prewait.c`, off by default): an EL0 task cannot read the mask, so it infers an open mask from a zero critical nesting count; a bare `portSET_INTERRUPT_MASK` outside a critical section can wait with the mask raised for at most 100 us; the limit is checked only when an event wakes the core, so a wait behind a long holder can last as long as the holder (914 us once).
- Heterogeneous placement and tickless timing have no physical qualification.

0d. Panic confirmation (entries). Cause and design: [the design](../designs/scheduler-partitioned.md#the-owner-exit-work-must-not-run-after-an-interrupt-above-the-api-mask). A QEMU case reproduces it (4 of 4 failing before, 18 of 18 passing after). Board runs on the fixed image, all returned with exact output and no stop: five soaks (`1793612412507235041-e4633398`, `1793612556768525405-1c62fd5e`, `1793612765449252497-9a99b817`, `1793612995483833917-501eefc5`, `1793613102942277574-dfc0978e`), two with the list integrity bytes (`1793613285642527139-50993bfe`, `1793613482426622002-7cef3e33`; `1793613226184327517-be4330b8` was a host error when the board's USB node vanished), four after the exit change (`1793615594715919660-cd3ee23f`, `1793615747063569155-173a0d54`, `1793615873648308943-3eabd2f7`, `1793616048963593965-e8fc961d`; `1793616227806453788-839592bb` was a host error of the same kind) and five with the switch-request counters (`1793616388485243353-0fb2da02`, `1793616500737293739-0207eacf`, `1793616666248085091-827a73a9`, `1793616794846609508-07db9f51`, `1793616907929869657-01e648b2`).

## Backlog

1. Task-path wake latency to the 10 us target (about 20 to 26 us at the 99.99th percentile after the notification change). The gap from handler to task start carries the tail; shorten the wake path (fewer supervisor calls and barriers, no owner pass before the switch, bounded idle-pull and control-drain work per exit).
2. Per-object locking: done for queues, event groups, stream buffers, the timer's per-timer fields and the task tag, task number and thread-local-storage fields; no effect on the task path. The public critical sections, the timer service and the scheduler's own state keep global locks or owner-private state (design: "Sections that keep the global locks").
3. Shorter lock holds and prefetch: the prefetch of the wake path's lines was tried and gave nothing.
4. The abortable queue lock: a waiter that can take its place in line with the mask open.
5. Tickless idle on the stock-derived kernel (work on `work/tickless-idle-1101`; changes the protected `patches/freertos-smp-*.patch`): measure control-period jitter with and without it.
6. The 11 us owner-work time at interrupt exit on the control core. It no longer holds off the control interrupt, but it still delays ordinary interrupts and the task-path wake; bound the work per exit.
7. The stock kernel's yield handler holds `PSTATE.I` through its context switch (the source of its 13 to 16 us control tail); and the fork's remaining intervals in the inventory above (yield supervisor call, mask supervisor calls, the FromISR mask pair).
8. The soak sweep that includes the earlier 95 to 206 us stock spans.
9. Idle pull: fix the missed pull or measure it on against off on the board.
10. Control-path long runs at 1 to 8 kHz (10^6 periods each); only 10 kHz has them.

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

## Long runs, verdict inputs

Five boots per kernel on port 3-3, each a 25 s soak of the recognition (225 to 226 recognitions) with the interrupts running throughout: the task path alone at 1, 2 and 4 kHz on cores 1 to 3 and the control path alone at 10 kHz on core 4. Stock is `sw-off` images of the second long set; the fork is the fixed image (the interrupt-exit guard of [the design](../designs/scheduler-partitioned.md#the-owner-exit-work-must-not-run-after-an-interrupt-above-the-api-mask)); runs on the earlier fork image are not pooled. Every run returned with exact output and no sample dropped, no sequence gap and no missed period. Pooled from the printed histograms (0.167 us bins; the p99.99 is a bin's upper edge); maxima in microseconds. One run on a fixed fork image with the list integrity bytes was a host error (the USB node of the board vanished), not a scheduler failure; the other two ran clean (`1793613285642527139-50993bfe`, `1793613482426622002-7cef3e33`).

| Kernel | Path | Rate | Runs | Periods | Latency max | Latency p99.99 | Jitter max | Jitter p99.99 | Jitter bound (5%) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| stock | task | 1 kHz | 5 | 125 951 | 40.8 | 26.7 | 37.8 | 24.7 | 50 |
| fork | task | 1 kHz | 5 | 125 550 | 23.5 | 22.0 | 18.5 | 16.8 | 50 |
| stock | task | 2 kHz | 5 | 251 723 | 41.8 | 25.8 | 38.4 | 23.7 | 25 |
| fork | task | 2 kHz | 5 | 251 855 | 25.3 | 19.7 | 20.9 | 16.8 | 25 |
| stock | task | 4 kHz | 5 | 503 452 | 38.4 | 24.3 | 34.2 | 22.8 | 12.5 |
| fork | task | 4 kHz | 5 | 503 711 | 31.5 | 24.5 | 28.3 | 21.8 | 12.5 |
| stock | control | 10 kHz | 5 | 1 259 472 | 15.1 | 9.3 | 14.8 | 10.0 | 5 |
| fork | control | 10 kHz | 5 | 1 254 602 | 12.8 | 6.2 | 12.5 | 7.2 | 5 |

The pooled counts exceed 100 000 periods per task-path rate and 1.25 million at 10 kHz, so the 3 MiB heap dropped no sample (the heap peak in the output stays near 107 KB). The control path was measured at 10 kHz only in the long runs; at 1 to 8 kHz the short runs above (200 to 2000 periods) show 0.5 to 6.1 us.

Recognition time over the soak: stock 110.853 ms, fork 110.394 ms (0.4% faster); the plain soak images without interrupts: stock 109.975 ms, fork 110.023 ms. The control and task interrupts of the long set (17 kHz in all) therefore cost 0.80% on stock and 0.34% on the fork, which is 0.047 and 0.020 percent of recognition time per kHz of interrupts. The both-paths image (25 kHz of each, five cores) costs +4.55% on stock (114.979 ms) and +2.73% on the fork (113.024 ms, run on the earlier fork image): 0.091 and 0.055 percent per kHz, or about 0.9 and 0.5 us of recognition time per interrupt. A robot budgets 0.05 to 0.1 percent of model throughput per kHz of interrupt rate, spread over the cores that take them.

Targets. The wake latency of 10 us is met by neither task path on either kernel (p99.99 20 to 27 us). The jitter bound is met on the task path at 1 kHz by both and at 2 kHz by the fork alone (maximum 20.9 us against 25), and at 4 kHz by neither (21.8 against 12.5 us at p99.99). The control path meets the wake target at the 99.99th percentile on both (6.2 and 9.3 us) and misses it at the maximum on both (12.8 and 15.1 us over 1.25 million periods); at 10 kHz neither meets the 5 us jitter bound, the fork's p99.99 is 7.2 us and stock's 10.0 us.

### The panic, after the fix

The fixed fork image ran five clean soaks and two clean soaks with `configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES` on (stack pattern checks were on). Earlier fork images ran 14 soaks with one panic. Seven clean board runs after a fix that a QEMU case reproduces deterministically (4 of 4 failing before, 18 of 18 passing after) is evidence, not proof; the fork's board hours with control interrupts stay a watched item.

Owner-work counters on the board (fixed fork, two runs): drains from interrupt exit are 77 600 to 140 200 on cores 1, 2, 3, 5, 6 and 7, 119 700 on core 0 with 25 300 from the tick, and 317 700 on core 4, where 11 800 to 12 300 exits (about 4%) were skipped by the guard. Cores without a control interrupt skip none, so the guard leaves the owner work running after ordinary interrupts. The same holds in QEMU (232 to 444 drains per CPU from interrupt exit, none skipped, without the control interrupt).

### Why the stock control path has a 15 us tail

The recorder keeps the four largest latencies of the control core with the interrupted context (`ICC_PMR_EL1` open at 0xf0, running priority 0x40 in every case, critical nesting zero). Three stock runs, core 4: the largest, 15.8 us at 66.425 s, was taken at `vTaskDelay+0x80`, the instruction after the yield supervisor call (`svc #0x69`), with the task at EL0; 13.7, 12.8 and 12.6 us were taken at the same address, and 14.3, 13.6, 12.3 and 12.0 us in the second run likewise. The third run's four largest (13.7, 13.4, 12.4, 11.9 us) were taken at `prvPassiveIdleTask+0x14`, the instruction after `wfi`. So the tail is not a kernel span, which the control interrupt ignores: it is the time `DAIF.I` stayed set while the stock yield handler ran its context switch under the task and ISR locks (the convoy of the earlier sweep), and, on a core that was idle, the wake-up from `wfi`. The fork shows no such instance: its four largest per run are 3.5 to 5.1 us, all inside the user-mode compute kernel. The recorder cannot show what the supervisor handler waited for, so the lock wait inside the yield handler is inferred from the address and from the sweep, not observed.

### Verdict by the rule

1. Control path on both kernels: neither meets the maximum (12.8 and 15.1 us) nor the 10 kHz jitter bound; both meet the 99.99th-percentile wake target. Equal on this rule.
2. Task path: the fork meets the jitter bound at 2 kHz and stock at 1 kHz only; neither meets the 10 us wake target. The fork is ahead.
3. Model time: within 0.5% in both directions (fork 0.4% faster over the soak, 0.04% slower plain).

The rule chose the fork, and it is the default.

## Evidence

Board results with a queue entry are records in `docs/evidence/scheduler.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-scheduler-20261101.md).

<!-- evidence-table:begin -->
89 records in `docs/evidence/scheduler.jsonl`; run `tools/evidence.py sync` to regenerate this table.

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
| `1793608348858278119-8f8e4282` | 2026-11-02 | 3-3 | `0e7b99d86e01` | watchdog-reset | failed | run_total 94.975 s, panic_line 1846 |
| `1793609473031303913-0abdcc80` | 2026-11-02 | 3-3 | `06d8e9cdee14` | returned | passed | run_total 102.579 s, recognition_mean 110.785 ms, soak_iterations 225 |
| `1793609576370956216-79c0b681` | 2026-11-02 | 3-3 | `826dadd7a24f` | returned | passed | run_total 147.398 s, recognition_mean 110.44 ms, soak_iterations 226 |
| `1793609727856926898-b80b05a5` | 2026-11-02 | 3-3 | `06d8e9cdee14` | returned | passed | run_total 147.204 s, recognition_mean 110.847 ms, soak_iterations 225 |
| `1793609880597303136-c36b66f2` | 2026-11-02 | 3-3 | `826dadd7a24f` | returned | passed | run_total 139.33 s, recognition_mean 110.355 ms, soak_iterations 226 |
| `1793610025109272345-3c2e8775` | 2026-11-02 | 3-3 | `06d8e9cdee14` | returned | passed | run_total 95.343 s, recognition_mean 110.868 ms, soak_iterations 225 |
| `1793610121291518039-7045ddd1` | 2026-11-02 | 3-3 | `826dadd7a24f` | returned | passed | run_total 95.789 s, recognition_mean 110.363 ms, soak_iterations 226 |
| `1793610217673724777-5096d30a` | 2026-11-02 | 3-3 | `06d8e9cdee14` | returned | passed | run_total 113.393 s, recognition_mean 110.921 ms, soak_iterations 225 |
| `1793610331962921349-cc0e3767` | 2026-11-02 | 3-3 | `826dadd7a24f` | returned | passed | run_total 135.508 s, recognition_mean 110.607 ms, soak_iterations 225 |
| `1793610472504325386-8b909439` | 2026-11-02 | 3-3 | `06d8e9cdee14` | returned | passed | run_total 155.065 s, recognition_mean 110.844 ms, soak_iterations 225 |
| `1793610631997622531-eedace9e` | 2026-11-02 | 3-3 | `826dadd7a24f` | returned | passed | run_total 146.918 s, recognition_mean 110.633 ms, soak_iterations 225 |
| `1793610783324757258-43d1d45f` | 2026-11-02 | 3-3 | `bec3e8fd186b` | returned | passed | run_total 99.579 s, recognition_mean 109.975 ms, soak_iterations 227 |
| `1793610884370946758-d311b355` | 2026-11-02 | 3-3 | `75faeecc303b` | returned | passed | run_total 147.71 s, recognition_mean 110.023 ms, soak_iterations 227 |
| `1793612065441570109-89e82b22` | 2026-11-02 | 3-3 | `742f57551e83` | returned | passed | run_total 104.262 s, recognition_mean 113.024 ms, soak_iterations 220 |
| `1793612127118715138-3a0a58c0` | 2026-11-02 | 3-3 | `03ec2eef2c9b` | returned | passed | run_total 95.475 s, recognition_mean 110.435 ms, soak_iterations 226 |
| `1793612267894668436-a0370422` | 2026-11-02 | 3-3 | `c62eea8ee8bc` | returned | passed | run_total 95.58 s, recognition_mean 110.878 ms, soak_iterations 225 |
| `1793612364141383439-45ae7861` | 2026-11-02 | 3-3 | `c62eea8ee8bc` | returned | passed | run_total 95.47 s, recognition_mean 110.889 ms, soak_iterations 225 |
| `1793612412507235041-e4633398` | 2026-11-02 | 3-3 | `da6d553b7bd7` | returned | passed | run_total 95.599 s, recognition_mean 110.431 ms, soak_iterations 226 |
| `1793612460432955799-63c98c79` | 2026-11-02 | 3-3 | `c62eea8ee8bc` | returned | passed | run_total 95.373 s, recognition_mean 110.875 ms, soak_iterations 225 |
| `1793612556768525405-1c62fd5e` | 2026-11-02 | 3-3 | `da6d553b7bd7` | returned | passed | run_total 111.46 s, recognition_mean 110.375 ms, soak_iterations 226 |
| `1793612653214000062-11b41595` | 2026-11-02 | 3-3 | `03ec2eef2c9b` | returned | passed | run_total 121.246 s, recognition_mean 110.439 ms, soak_iterations 226 |
| `1793612765449252497-9a99b817` | 2026-11-02 | 3-3 | `da6d553b7bd7` | returned | passed | run_total 102.966 s, recognition_mean 110.375 ms, soak_iterations 226 |
| `1793612995483833917-501eefc5` | 2026-11-02 | 3-3 | `da6d553b7bd7` | returned | passed | run_total 106.384 s, recognition_mean 110.382 ms, soak_iterations 226 |
| `1793613102942277574-dfc0978e` | 2026-11-02 | 3-3 | `da6d553b7bd7` | returned | passed | run_total 118.47 s, recognition_mean 110.408 ms, soak_iterations 226 |
| `1793613226184327517-be4330b8` | 2026-11-02 | 3-3 | `54471f8407d6` | host-error | inconclusive | run_total 52.416 s |
| `1793613285642527139-50993bfe` | 2026-11-02 | 3-3 | `54471f8407d6` | returned | passed | run_total 192.824 s, recognition_mean 110.765 ms, soak_iterations 225 |
| `1793613482426622002-7cef3e33` | 2026-11-02 | 3-3 | `54471f8407d6` | returned | passed | run_total 183.378 s, recognition_mean 110.65 ms, soak_iterations 225 |
| `1793615594715919660-cd3ee23f` | 2026-11-02 | 3-3 | `96b1d71741f7` | returned | passed | run_total 149.154 s, control_latency_max 3.38 us, control_jitter_max 3.12 us, recognition_mean 110.594 ms |
| `1793615747063569155-173a0d54` | 2026-11-02 | 3-3 | `96b1d71741f7` | returned | passed | run_total 125.623 s, control_latency_max 3.54 us, control_jitter_max 3.25 us, recognition_mean 110.594 ms |
| `1793615873648308943-3eabd2f7` | 2026-11-02 | 3-3 | `96b1d71741f7` | returned | passed | run_total 170.263 s, control_latency_max 3.58 us, control_jitter_max 3.29 us, recognition_mean 110.584 ms |
| `1793616048963593965-e8fc961d` | 2026-11-02 | 3-3 | `96b1d71741f7` | returned | passed | run_total 173.379 s, control_latency_max 3.29 us, control_jitter_max 3 us, recognition_mean 110.826 ms |
| `1793616227806453788-839592bb` | 2026-11-02 | 3-3 | `96b1d71741f7` | host-error | inconclusive | run_total 38.333 s |
| `1793616388485243353-0fb2da02` | 2026-11-02 | 3-3 | `591ef3007a46` | returned | passed | run_total 110.751 s, control_latency_max 3.33 us, control_jitter_max 3.04 us, recognition_mean 110.777 ms |
| `1793616500737293739-0207eacf` | 2026-11-02 | 3-3 | `591ef3007a46` | returned | passed | run_total 156.797 s, control_latency_max 3.42 us, control_jitter_max 3.12 us, recognition_mean 110.866 ms |
| `1793616666248085091-827a73a9` | 2026-11-02 | 3-3 | `591ef3007a46` | returned | passed | run_total 126.483 s, control_latency_max 3.62 us, control_jitter_max 3.38 us, recognition_mean 110.757 ms |
| `1793616794846609508-07db9f51` | 2026-11-02 | 3-3 | `591ef3007a46` | returned | passed | run_total 109.47 s, control_latency_max 3.58 us, control_jitter_max 3.33 us, recognition_mean 110.898 ms |
| `1793616907929869657-01e648b2` | 2026-11-02 | 3-3 | `591ef3007a46` | returned | passed | run_total 95.776 s, control_latency_max 3.38 us, control_jitter_max 3.08 us, recognition_mean 110.965 ms |
| `1793617020998598953-a6ecdfc1` | 2026-11-02 | 3-3 | `8164782bdec2` | returned | passed | run_total 109.557 s, idle_pulls 6722, idle_entries_with_work_waiting 4632, recognition_mean 111.033 ms |
| `1793617131735748473-bef01b84` | 2026-11-02 | 3-3 | `8164782bdec2` | returned | passed | run_total 140.514 s, idle_pulls 3939, idle_entries_with_work_waiting 2496, recognition_mean 110.943 ms |
| `1793617277487733781-666ca5a4` | 2026-11-02 | 3-3 | `8164782bdec2` | returned | passed | run_total 141.26 s, idle_pulls 6266, idle_entries_with_work_waiting 7600, recognition_mean 111.031 ms |
| `1793668412009966237-1c10f02f` | 2026-11-03 | 3-3 | `e7a4f58229a1` | returned | passed | run_total 273.108 s, task_latency_max_4khz 31.1 us, entry_max_4khz 11.3 us, gap_max_4khz 25.2 us, recognition_mean 110.683 ms |
| `1793668868450145397-9ef28fda` | 2026-11-03 | 3-3 | `89fb9417c2c6` | returned | passed | run_total 275.648 s, gap_max_4khz 16 us, recognition_mean 109.411 ms |
| `1793668892195242489-3b585c56` | 2026-11-03 | 3-3 | `89fb9417c2c6` | returned | passed | run_total 311.477 s, gap_max_4khz 19.8 us, recognition_mean 109.434 ms |
| `1793669877242530330-39c2dcac` | 2026-11-03 | 3-3 | `422d047168aa` | returned | failed | run_total 280.539 s, gap_p50_1khz 6.5 us |
| `1793669888246106384-74781dfd` | 2026-11-03 | 3-3 | `422d047168aa` | returned | failed | run_total 278.983 s, gap_p50_1khz 6.5 us |
| `1793671006348692345-8b3ce66b` | 2026-11-03 | 3-3 | `e7a4f58229a1` | returned | passed | run_total 260.588 s, task_latency_max_4khz 31.1 us, recognition_mean 110.551 ms |
| `1793672779737331795-9c485d61` | 2026-11-03 | 3-3 | `72b12109513e` | returned | passed | run_total 138.924 s, p99_4khz 7.67 us, wakes_ge10us_4khz 145 |
| `1793672791833556437-3d0beaf0` | 2026-11-03 | 3-3 | `72b12109513e` | returned | passed | run_total 416.477 s, p99_4khz 7.67 us, wakes_ge10us_4khz 246 |
| `1793674399585324316-78758903` | 2026-11-03 | 3-3 | `72b12109513e` | returned | passed | run_total 304.397 s, p99_4khz 7.67 us, wakes_ge10us_4khz 121 |
| `1793674413669633557-95819732` | 2026-11-03 | 3-3 | `72b12109513e` | returned | passed | run_total 219.156 s, p99_4khz 7.17 us, wakes_ge10us_4khz 236 |
| `1793674429022164361-d3113ba6` | 2026-11-03 | 3-3 | `72b12109513e` | returned | passed | run_total 98.588 s, p99_4khz 7.33 us, wakes_ge10us_4khz 116 |
| `1793676328926858188-8d56d9aa` | 2026-11-03 | 3-3 | `0d5bdbfd73f1` | returned | inconclusive | run_total 96.879 s, core7_max_4khz 8.83 us, core6_max_1khz 12.29 us |
| `1793676331839315084-eb658d1e` | 2026-11-03 | 3-3 | `5a3dfb486054` | returned | inconclusive | run_total 97.391 s, pull_over5us_core1 676, drains_over5us_core1 622 |
| `1793676334897453864-1c03391a` | 2026-11-03 | 3-3 | `b172a440f124` | returned | failed | run_total 96.658 s, pull_over5us_core1 428, drains_over5us_core1 688 |
<!-- evidence-table:end -->
