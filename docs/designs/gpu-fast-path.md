# GPU jobs execute in the caller and complete through interrupts

Status: **implemented** in `drivers/gpu/pvr_compute_fast.c`, `drivers/gpu/gpu_queue.c` and `include/gpu_queue.h`, and **qualified on the board** for 10,000 serial and 10,000 depth-four empty jobs on port 3-3. The initial latency and throughput budgets are not met ([gpu.jsonl](../evidence/gpu.jsonl), id `1793567368102223375-02dbc4e4`).

This design assumes familiarity with FreeRTOS tasks, device drivers and shared memory. A fence is the monotonically increasing firmware timeline value written after a job finishes. A client command buffer (CCCB) carries compute and timeline commands, and the shared kernel command buffer (KCCB) carries their KICK command.

A job is submitted by the calling task, which writes the CCCB commands and publishes the KICK. The completion interrupt notifies the waiting task directly, so no service-task hop or tick poll sits between a caller and its fence. Explicit caller wait timeouts keep their public meaning, and completion never depends on those timeouts expiring.

## Firmware contract

The pinned reference is the Orange Pi kernel revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, under [`bsp/drivers/gpu/imagination`](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination). The source records and SHA-256 values are in [GPU sources](../gpu.md), and the cache is `sources/gpu/board-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/imagination/` under `~/.cache/sunstoneos/`. This code adapts the MIT-licensed host interface; the firmware runs on the GPU's MIPS processor with its retained license.

| Contract | Reference |
| --- | --- |
| Compute command followed by UPDATE, command IDs 205 and 213 | `pvr_queue.c:697–705`; `pvr_rogue_fwif.h:1532,1564–1579` |
| One unused KCCB slot and serialized command publication before the doorbell | `pvr_ccb.c:194–204,243–283` |
| Clear the MIPS interrupt before acquiring timelines, then recheck pending status | `pvr_device.c:275–283`; `pvr_fw_mips.c:176–186` |
| Firmware priorities low, normal and high, encoded 0, 1 and 2 | `pvr_context.h:23–27`; `pvr_queue.c:1071–1072` |

UPDATE has no interrupt-request field. The reference handler scans active timelines after ordinary completion interrupts, and hardware must confirm that the firmware interrupts reliably for the submitted jobs; a missing interrupt requires recovery. The sources give no bounded compute-preemption guarantee.

## Ownership and bounded work

- The context keeps its GPU mappings, program and firmware objects for its lifetime. Four fixed job slots each keep independent PDS, CDM and argument storage until the caller collects the fence.
- Submissions serialize the CCCB and the shared KCCB with the context lock that service control uses. Lock admission is nonblocking, and contention returns BUSY. No allocation or service request precedes a successful kick. READY admits the retained uncached KCCB regions and the doorbell register once, and job publication uses aligned stores into them with the command, write-offset and kick barriers. A stale firmware read cursor returns BUSY; an impossible advance latches its predicate and cursor inputs before it retains the failed job. A batch publishes at most four jobs in one caller entry, with one KICK per job, and returns the accepted prefix, including an attempted job whose doorbell result is uncertain.
- Fences stay context-bound, and collection can select a fence or take the oldest record. Context priorities normal, low and high map to firmware values 1, 0 and 2.
- Application tasks run at EL0. A fixed supervisor-call gateway enters EL1t on the same task stack, calls the GPU dispatcher and restores EL0, accepting entry and exit only at the gateway's instruction sites. IRQs and task preemption stay enabled, and the dispatcher verifies the admitted caller and the argument's EL0 permissions under the OS's trusted shared-address-space contract.
- The interrupt handler acknowledges the device before it reads the uncached timeline, publishes the completed sequence and notifies registered waiters whose fences have passed. It serves at most two clients and does no cache maintenance, event draining, ring scan, allocation or waiting. Interrupts that arrive after the timeline snapshot remain pending. Firmware events and failures wake the service task for bounded slow-path handling.
- Wait registration is publish, recheck, block: a completion between registration and blocking leaves a task notification pending, and an interrupt before registration is caught by the recheck. Output cache acquisition runs in the collecting caller after its fence. Notification index 1 belongs to GPU waits, and index 0 stays available to other task users.
- The service keeps startup, shutdown, thermal parking, recovery and cleanup. It seals direct admission before power changes and drains accepted fences before it releases device ownership. A watchdog deadline schedules failure handling only and never turns a missing fence into success; the slow watchdog checks CPU fence state once per tick and never acquires a firmware timeline to complete a job, so a missing completion interrupt faults at the job deadline and follows the isolation protocol. Failed isolation retains every slot and buffer. The GPU stays powered while accepted work exists, and idle power policy and voltage and clock transitions belong to the lifecycle owner.

## Mapping and cache

Mappings and the initial MMU invalidation happen once at context creation. Per-job maintenance covers the declared dirty input ranges and the acquired output ranges, and CPU output translations stay read-only. Large dirty ranges can exceed the submission budget independently of ring cost, so ring publication is measured separately and maintenance is included in the end-to-end CPU measurement. Payloads default to write-back.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Submission wall time, median (guarded ring, then admitted ring) | 13.458 µs, then 3.833 µs | `1793563967717509980-ac170b9a`, `1793567368102223375-02dbc4e4` in [gpu.jsonl](../evidence/gpu.jsonl) |
| Empty-job round trip, median; pipeline at depth four | about 104 µs; 9,540 jobs/s | `1793567368102223375-02dbc4e4` |
| IRQ C-entry to notification return: median, 99th percentile, maximum | 2.292 µs, 8.167 µs, 118 µs | `1793567368102223375-02dbc4e4` |
| Submission wall time maximum | 51.875 µs | `1793567368102223375-02dbc4e4` |
| Share of aggregate job time inside device scheduling and shader execution; time in 154 reference jobs | 94.6%; 339.188 ms | `1793561022387122671-06be5388`, `1793560651607761780-3cb221f5` |
| Uncached against write-back payloads | saves 0.208 µs at the median, no tail gain | `1793564171325300569-9135052d` |

These observations do not isolate a firmware lower bound. The zero-element vector shader still loads its argument block, so the benchmark is a minimal real compute dispatch with a fence. The measured recognition penalty cannot be attributed to tick polling, because each completion interrupts the blocked service task before its backup poll expires.

## Limitations and open work

- The initial budgets of 5 µs submission CPU time, 20 µs from completion interrupt to the waiting task running, 50 µs for the 99th percentile empty-job round trip and 10,000 jobs per second remain unmet. The submission maximum does not establish a 5 µs CPU bound, and the round trip stays near 104 µs.
- The fixed GPU loops have bounded operation counts, but FreeRTOS's shared notification and scheduler locks need a bounded contention proof. The CPU timing record on `work/latency-1101` (`docs/status/latency.md`) records long waits on those locks and does not identify the origin of this run's tail samples.
- Idle timeout and hysteresis remain an integration requirement for the lifecycle owner.
- Host checks model delayed firmware reads, timeline writes and interrupts (coalescing, an interrupt during waiter registration, ring wrap and full, multiple clients, timeouts, retained failures) and reject a service hop or tick poll in submission and completion. Host timings do not establish board latency or speedup.
- Open: explain the 104 µs round trip, then run queue-add and application recognition on the fast path against the 154-job reference.
