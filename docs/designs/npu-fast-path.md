# Submit resident NPU commands and complete ordered fences

This design is for driver and application-kit developers. A fence is a queue
identity and a monotonically increasing submission number. Its completion
permits CPU acquisition of the corresponding device output. Network decoding,
model partitioning and tensor interpretation belong to the loader and application.

## Existing jobs cross two tasks and repeat preparation

The application SDK publishes one request in `runtime/npu_app.c`.
The privileged owner in `apps/os/npu-client.c` dispatches that request, calls
LOAD, copies inputs, submits through the runtime service, and copies outputs.
The application dispatch wait is 1 ms in the accepted base; the 25 ms waits
cover startup and local stop. Both waits can wake early on a notification.

The runtime service performs input cleaning, output clean/invalidate, workspace
zeroing, command/table checks and a read-only checksum after each job.
A slot retains one assembled network and its mappings, but switching its model
repeats loading and assembly. The public SDK wrapper also requests LOAD for
repeated use of the same model. The job path therefore includes task dispatch,
copies and checks proportional to the resource sizes.

Port 3-3 queue entry `1793559627833816772-2003f2e7` measures 200 alternating
jobs at 852 MHz and 800 mV: 1,441.8 jobs/s and 2.100042 ms maximum helper
latency. Its scope includes loading, copies, comparison and hashing.
It provides neither a 1,000-job percentile sample nor CPU cost per job.

## A resident command queue separates preparation from submission

`npu_queue.h` accepts trusted resident command handles and caller-owned job
records. Registration validates a command once. Submission appends one record
and starts the engine directly when idle. There is no allocation or fixed
job-count limit; available caller-owned records bound admission. Command buffers,
page tables and device buffers remain mapped and pinned until successful drain.

The queue executes FIFO order on the single engine. A submission may depend on
an already issued fence from the same queue. FIFO order satisfies that dependency
without searching pending jobs. Future and foreign fences are refused.
Applications may retain fence values after releasing their job records.
Cross-device dependency scheduling requires a separate consumer and is outside
this interface's current scope.

The interrupt acknowledges the event once and requires full idle before
publishing a completed fence. It starts at most one successor and directly
notifies the waiting task. An invalid event, missing idle, deadline failure or
failed trigger retains the queue and every buffer. A lost interrupt is bounded
by the existing independently armed watchdog; an exact per-job deadline interrupt
is still required before this path supports independently timed production jobs.

The runtime adapter parks the ordinary worker during exclusive queue ownership.
Its initial client runs at EL1 on the IRQ route core. Local IRQ masking protects
constant-size queue updates. Output acquisition and dirty-range cache maintenance
run outside that lock. Registration and lifecycle operations remain slow paths.
The adapter does not establish EL0 SDK access or physical qualification.

The CPU cleans only input ranges it modifies, and cleans/invalidates output
ranges it modifies before submission. After completion it invalidates output
ranges before reading them. Device-only intermediate buffers need no CPU cache
maintenance. CPU access to shared output waits for its last queued device user.
Immutable descriptors and workspace remain resident; the loader must establish
any required initial workspace contents before admission.

## Vendor interfaces define command ordering and completion

The reference is Allwinner's VIPLite 2 kernel driver in
`~/.cache/sunstoneos/sources/npu/board-vip2-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/vip2/`.
The repository is [linux-orangepi at the pinned revision](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/npu/aw_nna_vip/vip2).
[The source record](../reference-backsolve-npu.md) supplies provenance;
its cached inventory records file hashes. The driver is source-available under
its retained MIT notices and runs only as a development reference here.

- `vip_drv_hardware.c:882–893` writes the command address at `+0x654`,
  completes a memory barrier, then writes the fetch word at `+0x3a4`.
- `os/linux/vip_drv_device_driver.c:382–392` reads the destructive event word
  at `+0x010` and wakes the interrupt waiter.
- `vip_drv_hardware.c:520–529` publishes a LINK destination before its opcode.
  Prepared command sequences must retain synchronization and device-cache
  flushes between dependent commands and issue a final completion event.
- `vip_lite_config.h:409–420` labels parallel asynchronous mode unsupported
  and selects mode 3. Source presence alone does not qualify live chaining.

A prepared sequence is a command resource with one completion fence. Preparing
links and validating dependencies happens once, while the sequence is idle.
A live command must never be patched while the fetch engine can read it.
Hardware sequence construction and physical chained-throughput evidence remain
required; ISR-started FIFO jobs still make one CPU interrupt per command.

## Power and measurement require separate physical evidence

Approved measurements retain 852 MHz at 800 mV while jobs are queued.
Physical domain OFF remains unsupported: ROM recovery shares NPU SRAM, and
DCDC2 also supplies system and DRAM logic. See [the power boundary](../npu.md#the-supported-npu-power-boundary-preserves-shared-sram).

The proposed idle policy suspends only the vendor's internal NPU clocks after
10 ms with no queued or active work, and keeps them running for at least 1 ms
after resume. Submission cancels an unexecuted idle request; a suspended engine
resumes on the lifecycle path before accepting work. The design requires
simplicity, performance and correctness review before implementation.

A proposed 3-3 transition run alternates 100 busy/idle cycles using the vendor
suspend/resume sequence at `vip_drv_pm.c:157–209`. It retains domain power and
the approved operating point. Record every completion, resume delay, p50/p99/max
job latency, throughput, CPU time and idle power before and after. Require no
numerical mismatch, no retained fault, normal retirement and restored DRAM.
Reject unexplained latency or power regressions. This power-state run needs
coordinator approval before submission.

The performance comparison needs a control and a queue variant with identical
model bytes and inputs, at least 1,000 jobs each, and a prepared dependent
sequence. Report submission-to-acquisition latency separately from device time,
CPU execution time separately from waiting time, and include image hashes,
Git revisions and queue entry IDs. Software fixtures establish no board timing.
