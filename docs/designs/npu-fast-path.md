# Submit resident NPU commands and complete ordered fences

The trusted resident queue completes 1,000 checked jobs on port 3-3. Its small-model
latency and throughput improve against a matched control. Idle power remains unqualified. This design is for driver and application-kit developers. A fence is a queue
identity and a monotonically increasing submission number. Its completion
permits CPU acquisition of the corresponding device output. Network decoding,
model partitioning and tensor interpretation belong to the loader and application.

## Copy-based jobs repeat preparation

The copy SDK publishes one request in `runtime/npu_app.c`. The privileged owner
in `apps/os/npu-client.c` wakes on that event, loads a model when its resident
identity changes, copies inputs, submits through the runtime service, and copies
outputs. Its job path crosses the owner and runtime worker. Repeated READY
models skip LOAD; startup and local-stop waits remain slow paths.

The matched control below calls `npu_job_run` directly, so it measures the
helper-to-worker path without the SDK owner's extra dispatch. The runtime
service cleans inputs, cleans/invalidates outputs, zeros workspace, checks
command/table integrity and checksums read-only data for each job. A slot
retains its assembled network and mappings until its model changes. These
per-job copies and checks scale with the resource sizes.

The matched board comparison keeps both linked model binaries, model-0 inputs,
852 MHz and 800 mV identical. Each image checks one warmup and 1,000 measured
outputs. Both return normally, with 1,002 interrupt completions and no mismatch.

| Path and queue entry | Job p50 / p99 / max (µs) | Jobs/s | Task CPU/job (µs) | Submit-to-doorbell p50 / p99 / max (µs) |
| --- | ---: | ---: | ---: | ---: |
| Helper, `1793563838443635411-92c1fdae` | 463.88 / 472.46 / 478.92 | 2,029.97 | 491.16 | 401.29 / 410.38 / 411.92 |
| Resident, `1793564135238449408-5dbdd6ad` | 10.25 / 12.83 / 18.71 | 26,925.33 | 34.19 | 0.96 / 0.96 / 2.67 |

Job latency includes input preparation and output acquisition. Throughput and
scheduled client-plus-worker CPU time also include comparison, digest and progress
checks. CPU time excludes interrupts charged to idle and unrelated tasks; the
resident driver separately records 1.19 µs/job of IRQ elapsed time, which can
overlap task accounting. These counters do not measure total CPU utilization.
Submit-to-doorbell ends after the fetch write and device barrier; its means are
403.44 µs for the helper and 0.943 µs for the resident queue. Instrumentation
exists only in diagnostic builds.

The control image SHA-256 is
`862114046425739e74d6c2ad70da1e4c91ad2c949db82d7bae5539a68f98d5be`;
the resident image is
`2a6411ba3fcc2b1620c18ee1cbe92393b4cd26a8705d5571f821bf84e964dd3c`.
The compiled-source snapshots match Git revisions `2ba77c83` and `f43314fd`,
respectively, across all 176 recorded project inputs. The queue records retain
the runner revision, frozen host inventories, logs and output digests.
`matched-comparison.json` under the task build directory retains the numeric
summary. The throughput includes diagnostic CPU work and is not a network
compute-throughput limit.

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
Its initial client runs at EL1 on the IRQ route core. Successful return
permanently closes the old queue's admission before the worker resumes. Local IRQ masking protects
constant-size queue updates. Output acquisition and dirty-range cache maintenance
run outside that lock. Registration and lifecycle operations remain slow paths.
The application-kit bridge below extends this adapter to one trusted EL0 producer.

The CPU cleans only input ranges it modifies, and cleans/invalidates output
ranges it modifies before submission. After completion it invalidates output
ranges before reading them. Device-only intermediate buffers need no CPU cache
maintenance. CPU access to shared output waits for its last queued device user.
Immutable descriptors and workspace remain resident; the loader must establish
any required initial workspace contents before admission.

## The application kit publishes into a shared ring

[The public interface](../../include/sunstone_npu_queue.h), also included by
`sunstone_app.h`, provides resident handles, tensor windows, dirty ranges,
fences and a bounded single-producer submission ring. Enable
`SUNSTONE_A733_OS_NPU_QUEUE=ON` with `SUNSTONE_A733_OS_NPU=ON`.
`SUNSTONE_OS_NPU_QUEUE_MODELS` lists the image catalogue indices to authenticate
and preload before scheduling; its first entry is `SUNSTONE_OS_NPU_CLIENT_MODEL`.
The API supports up to eight resident handles and 64 outstanding receipts.
The selected models must fit the fixed 8 MiB reservation; otherwise preparation
is refused. Receipt capacity exhaustion returns backpressure.
The lower-level caller-backed command queue has no job-count limit.

The image places the producer on core 1, the interrupt route core. Applications
requiring Cortex-A76 placement are refused in this configuration. Open and close
use the existing privileged owner for lifecycle work. Submission writes a handle,
dependency and timeout into shared memory from EL0. A supervisor-call exception, SVC 120, rings the hardware
only when publication changes the consumer from idle to active. IRQ97 completes
one private record and starts at most one successor. No service task dispatch,
allocation, command patching or tensor maintenance runs in that interrupt.

The consumer clears its active flag and rechecks publication with sequentially
consistent atomics. A racing publisher either rings the doorbell or becomes
visible to that recheck. A final-fence waiter receives a persistent direct
notification. Waiting uses no tick interval. The exception returns submission
failure directly to its running producer; it enters no task notification path.
Terminal thermal stop is checked before every successor trigger.

An application may declare immutable tensor edges between prepared entries:

```c
static const struct ss_npu_queue_edge edges[] = {
    {.producer = 0, .output = 0, .consumer = 1, .input = 0},
};
const struct ss_npu_queue_edges ss_npu_queue_edges = {edges, 1};
```

Indices refer to the prepared-entry order and each network's tensor order.
The loader validates dimensions, representation and allocation size, then
reassembles the consumer with the producer's output address before binding.
Register both handles, fill the first input, submit the producer, and submit
the consumer with the producer's fence as its dependency. The shared intermediate
requires no CPU copy or cache operation between jobs.

Connected handles share a buffer lease. The first submit maintains every dirty
range in the group before hardware starts. Repeated device-owned uses perform
no further cache work. After CPU acquisition, mark subsequent writes with
`ss_npu_queue_dirty`; maintenance rounds only those ranges to owned cache lines.
Acquisition waits for the group's last published use. It uses the EL0-enabled cache clean/invalidate instruction (`DC CIVAC`),
which is safe because the CPU cannot dirty output lines during the device lease.
The original cache-control setting is restored at close.

`ss_npu_queue_release` returns receipt capacity in FIFO order and preserves fence
status. It does not grant CPU buffer access. `ss_npu_queue_acquire_handle` remains
available after receipt release or reuse. Close requires all receipts released
and all used groups acquired. Faults retain buffers and prevent close.
Eight sanitizer cases and twelve native SVC/IRQ cases pass. The selected
integration gate builds both NPU variants and the three baseline images and
passes kernel, check-index and shared-helper checks. Diagnostic-only counters
and updates disappear from production layouts. Seven recompiled diagnostic
objects have identical executable bytes to the measured image. Physical EL0 execution
remains unqualified. The public ring currently issues one interrupt per binary;
the native prepared-stream interface below additionally removes intermediate events.

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

`a733_npu_sequence_prepare` constructs a resident sequence with one completion
fence. It preserves each native stream's flush/stall footer and emits only the
final event and END. Preparing
links and validating dependencies happens once, while the sequence is idle.
A live command must never be patched while the fetch engine can read it.
Port 3-3 entry `1793565141195481474-2e135a8c` repeats each form 1,000 times
with two distinct compatible binaries. Their 400-byte intermediate has the same
CPU and device address in both models. Every intermediate and final output
matches its reference. The image completes 4,001 hardware jobs, including
warmup and the separate 1,000-job loop, receives 4,002 IRQs and returns normally.

| Dependent path | Stages/s | Task CPU/stage (µs) | Mean chain interval (µs) | Intermediate events across 1,000 chains |
| --- | ---: | ---: | ---: | ---: |
| Two FIFO records pending together | 45,987.32 | 19.87 | 13.94 | 1,000 |
| One resident joined stream | 48,166.66 | 18.70 | 12.02 | 0 |

Each chain executes two stages. Throughput and task CPU include both output
checks, digest and progress checks; the mean chain interval excludes these
checks. Both paths perform zero interstage CPU copies. The joined 240-byte
stream produces only the final device completion, so no CPU completion or
submission occurs between stages. Each iteration waits before record reuse.
This qualifies these buffers, two binaries and normal retirement; other graphs,
fault recovery and EL0 execution need separate evidence. The repeated image matches Git revision `f0d52118` across all 178 recorded
project inputs. Its SHA-256 is `ae8dc27e6c16b8b362bc3a2d7ecdc640d7616d677e52e760020d9930993464fd`.

## Power and measurement require separate physical evidence

**Proposal only:** internal clock suspension is unimplemented and physically
unqualified. The control keeps the NPU at 852 MHz and 800 mV while its
lifetime is open. A power change requires review for simplicity, hardware
performance and correctness before coding; the same three reviewers inspect
the implementation. This proposal authorizes no transition run.

### The domain and rail remain owned during internal clock suspension

“PCK4” means domain 4 of the PCK-600 controller, whose register row begins at
`0x07064000`. It is an internal SoC power switch. The pinned vendor binding
`include/dt-bindings/power/sun60iw2-power.h:5–15` identifies domain 4 as NPU,
domain 5 as GPU_TOP and domain 8 as USB2. These identities do not establish
that all storage inside the NPU domain belongs exclusively to an NPU job.
The A733 User Manual V1.00 §2 places shared SRAM at `0x74000–0xf3fff` and
requires the NPU to remain powered when that SRAM is used. ROM uses this
SRAM for its stack and FEL scratch. Domain-4 OFF therefore remains unsupported.
[The power boundary](../npu.md#the-supported-npu-power-boundary-preserves-shared-sram)
records the failed domain-OFF run and the unresolved reset behavior.

The external DCDC2 regulator is a separate resource. The Orange Pi V1.2
schematic, sheets 2, 6 and 8, connects it to NPU VDD, VDD-SYS, SoC-side
VDD-DRAM, VDD-VE, VDD08-UFS, VDD-COMB0/1 and VDD08-HDMI. The schematic is
pinned as `board-schematic` in `sources.lock.json`, SHA-256
`4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0`.
The vendor board tree marks DCDC2 boot-on and always-on and binds
`npu-supply` to it (`sun60i-a733-orangepi-zero3w.dts:2123–2131,2931–2933`).
A sole software rail token cannot make this physically shared supply exclusive.

The proposed transition retains the existing shared DCDC2 token, domain-4 ON
ownership, exclusive PLL_NPU and NPU clock tokens, shared parent/fabric leases,
PCK clock lease, mappings and buffers. It preserves the SRAM route and all
CCU gates, resets, PLL settings, voltage and PCK policies. The only proposed
writes are inside the NPU register window at `0x03600000`. Vendor source
supports internal clock suspension but does not prove ROM access to shared
SRAM during that suspension. Recovery from a fault while suspended remains
an evidence gap even after a successful normal resume.

### A one-shot timer requests gating after sustained idle

The candidate policy makes suspension eligible at the later of full-idle plus
10 ms and successful resume plus 1 ms. These are initial hysteresis settings,
not measured energy break-even points. Completed records awaiting release do
not delay gating. Submission cancels an unexecuted idle request and resumes a
suspended engine before accepting work. The minimum on-time never delays a job.

The direct queue owner performs transitions while holding the acknowledged
worker handoff. A one-shot timer only notifies that owner and carries an idle
generation; stale delivery cannot gate later work. The timer needs an interrupt
deadline facility. The owner serializes timer handling, submission and interrupt
completion through the existing device lock. It verifies queue emptiness,
full idle (`+0x004 = 0x7fffffff`) and completion-event retirement before the
first suspend write. The event word at `+0x010` is destructive on read, so
an independent timer callback must never consume it outside the completion path.

The transition states are RUNNING, IDLE_ON, SUSPENDING, SUSPENDED, RESUMING and
RETAINED. Admission accepts jobs only in RUNNING or IDLE_ON. Arrival before
the first suspend write cancels the idle request. Arrival afterward receives
backpressure until resume completes. A partial write, unexpected readback or
expired transition deadline enters RETAINED, preserving resources without an
automatic retry. A suspended queue must resume successfully before CLOSE.

### The internal gate preserves the top operating point

The sequence comes from `vip_drv_pm.c:142–220` in the pinned VIPLite 2 source
identified above, SHA-256
`f92cc17a7bf5b9fa40a2991dbf9e511eb4b0cddcf09829f2b047bfe4cf141d35`.
Its silicon branch uses a 1 µs delay; `inc/vip_lite_config.h:161–163,470–475`
selects silicon and enables internal clock suspension by default. The enclosing
`power_clock_state_in/out` routines also change frequency scaling
(`vip_drv_pm.c:438–465`). The proposal uses only their gate helpers and preserves
the full-scale fields established at OPEN.

| Phase | Proposed ordered operations |
| --- | --- |
| Suspend | Read `+0x000`; preserve unrelated fields and scaler bits 8:2. Set bit 0, write with bit 9 high, then write with bit 9 low. Wait at least 1 µs. |
| Resume | Read `+0x000`; preserve unrelated fields and scaler bits 8:2. Clear bit 0, write with bit 9 high, then write with bit 9 low. Wait at least 1 µs. Read `+0x10c`, preserve scaler bits 7:1, then pulse bit 0 high and low. |
| Publish readiness | Verify gate/load bits, both full-scale fields, unchanged top-point clock configuration, full idle and no unretired completion. Publish IDLE_ON only after these checks pass. |

Dependent writes complete with DSB in the native adapter. The vendor helpers
do not verify gated readback or bound failures. The implementation must establish
which control registers remain readable while suspended before requiring their
readback. It must preserve the full-idle proof from before suspension and
recheck full idle after resume. A readback failure retains ownership; it cannot
justify another power operation.

The expected gate-only resume cost is the mandated 1 µs settle plus MMIO,
barriers, verification and owner scheduling. That expression is a source-based
lower bound, not a measured latency or upper bound. The candidate budget is
100 µs from resume request to readiness, with at most 100 µs added to the
first resumed job's p99 and maximum latency. Neither budget has physical evidence.
The 100 µs target includes owner scheduling; an implementation that wakes only
on a 1 ms tick cannot satisfy it.

Full startup has different costs: the vendor platform waits 2 ms after its
power/clock hook (`vip_drv_device_platform.c:1385–1387`), and the board tree
specifies a 1,000 µs regulator-enable ramp. These values cannot support a
100 µs rail/domain power-up claim. The proposed gate retains the settled rail,
ON domain and locked PLL, so it must avoid the full startup path. The host model
reports 4,615 µs for native startup with domain 4 already ON
([startup measurements](npu-board-measurements.md#startup-latency-change-by-change)).
That model establishes neither physical startup time nor gate-only resume latency.

### A proposed run measures latency and energy against the always-on control

Before a transition run, complete the three-person design and code review,
exercise cancellation and partial failures in host checks, and establish the
readback policy. The coordinator must approve a frozen run separately. Its
prediction is that internal gating preserves job output and recovery resources
while reducing energy in idle gaps longer than the measured break-even duration.

The candidate port 3-3 run changes only internal gate policy from the always-on
852 MHz, 800 mV control. Alternate 100 busy/idle cycles and compare 9, 11 and
100 ms idle gaps; the 9 ms case must never gate. Record each transition and
completion, request-to-ready resume delay, first-job delay, p50/p99/maximum job
latency, throughput, CPU time, idle power and cycle energy. Bind logs to the
image hash, Git revision and queue entry. Require no numerical mismatch,
retained fault or cleanup failure, with normal retirement and restored DRAM.

Continuously queued throughput must remain at least 98% of control. P99,
maximum latency and CPU time per job must remain within 105%, 110% and 105%
of control respectively, as well as meeting the absolute resume budgets.
Unexplained regressions block acceptance even within those limits. Idle savings
must exceed meter uncertainty at 95% confidence, and total cycle energy must
not increase beyond measurement uncertainty. Measure transition energy and the
break-even idle duration before retaining or changing the 10 ms threshold.
No available measurement establishes savings, resume latency, safe shared-SRAM
access while gated, or watchdog recovery from a suspended-state fault.

The performance comparison needs a control and a queue variant with identical
model bytes and inputs, at least 1,000 jobs each, and a prepared dependent
sequence. Report submission-to-acquisition latency separately from device time,
CPU execution time separately from waiting time, and include image hashes,
Git revisions and queue entry IDs. Software fixtures establish no board timing.
