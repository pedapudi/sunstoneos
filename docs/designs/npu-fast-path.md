# Submit resident NPU commands and complete ordered fences

Status: **qualified on the board** for the resident command queue (1,000 checked jobs), for resident dependent sequences, and for internal clock gating of the idle NPU in the idle benchmark at 852 MHz and 800 mV. **Implemented, not qualified:** physical EL0 execution of the application-kit ring, energy measurement, shared-SRAM access while gated, and fault recovery while suspended. This design is for driver and application-kit developers.

A fence is a queue identity and a monotonically increasing submission number. Its completion permits CPU acquisition of the corresponding device output. Network decoding, model partitioning and tensor interpretation belong to the loader and application.

## Resident jobs separate preparation from submission

The copy SDK publishes one request in `runtime/npu_app.c`. The privileged owner in `apps/os/npu-client.c` wakes on that event, loads a model when its resident identity changes, copies inputs, submits through the runtime service and copies outputs. Its job path crosses the owner and the runtime worker. The runtime service cleans inputs, cleans and invalidates outputs, zeros workspace, checks command and table integrity and checksums read-only data for each job. A slot retains its assembled network and mappings until its model changes.

`npu_queue.h` accepts trusted resident command handles and caller-owned job records. Registration validates a command once. Submission appends one record and starts the engine directly when idle. There is no allocation or fixed job-count limit; available caller-owned records bound admission. Command buffers, page tables and device buffers remain mapped and pinned until successful drain.

The queue executes in FIFO order on the single engine. A submission may depend on an already issued fence from the same queue; FIFO order satisfies that dependency without searching pending jobs. Future and foreign fences are refused. Applications may retain fence values after releasing their job records. Cross-device dependency scheduling is outside this interface.

The interrupt acknowledges the event once and requires full idle before publishing a completed fence. It starts at most one successor and directly notifies the waiting task. An invalid event, missing idle, deadline failure or failed trigger retains the queue and every buffer. A lost interrupt is bounded by the independently armed watchdog; an exact per-job deadline interrupt is still required before this path supports independently timed production jobs.

The runtime adapter parks the ordinary worker during exclusive queue ownership. Its initial client runs at EL1 on the IRQ route core. Successful return permanently closes the old queue's admission before the worker resumes. Local IRQ masking protects constant-size queue updates; output acquisition and dirty-range cache maintenance run outside that lock. Registration and lifecycle operations are slow paths.

The CPU cleans only input ranges it modifies, and cleans and invalidates output ranges it modifies before submission. After completion it invalidates output ranges before reading them. Device-only intermediate buffers need no CPU cache maintenance. CPU access to shared output waits for its last queued device user. Immutable descriptors and workspace stay resident; the loader establishes any required initial workspace contents before admission.

## The application kit publishes into a shared ring

[The public interface](../../include/sunstone_npu_queue.h), also included by `sunstone_app.h`, provides resident handles, tensor windows, dirty ranges, fences and a bounded single-producer submission ring. Enable `SUNSTONE_A733_OS_NPU_QUEUE=ON` with `SUNSTONE_A733_OS_NPU=ON`. `SUNSTONE_OS_NPU_QUEUE_MODELS` lists the image catalogue indices to authenticate and preload before scheduling; its first entry is `SUNSTONE_OS_NPU_CLIENT_MODEL`. Handle and ring capacity come from the application's declaration ([NPU resources](npu-resources.md)). Receipt capacity exhaustion returns backpressure.

The image places the producer on core 1, the interrupt route core. Applications requiring Cortex-A76 placement are refused in this configuration. Open and close use the existing privileged owner for lifecycle work. Submission writes a handle, dependency and timeout into shared memory from EL0. A supervisor-call exception, SVC 120, rings the hardware only when publication changes the consumer from idle to active. IRQ97 completes one private record and starts at most one successor. No service task dispatch, allocation, command patching or tensor maintenance runs in that interrupt.

The consumer clears its active flag and rechecks publication with sequentially consistent atomics. A racing publisher either rings the doorbell or becomes visible to that recheck. A final-fence waiter receives a persistent direct notification. Waiting uses no tick interval. The exception returns submission failure directly to its running producer; it enters no task notification path. Terminal thermal stop is checked before every successor trigger.

An application may declare immutable tensor edges between prepared entries:

```c
static const struct ss_npu_queue_edge edges[] = {
    {.producer = 0, .output = 0, .consumer = 1, .input = 0},
};
const struct ss_npu_queue_edges ss_npu_queue_edges = {edges, 1};
```

Indices refer to the prepared-entry order and each network's tensor order. The loader validates dimensions, representation and allocation size, then reassembles the consumer with the producer's output address before binding. The application registers both handles, fills the first input, submits the producer, and submits the consumer with the producer's fence as its dependency. The shared intermediate needs no CPU copy or cache operation between jobs.

Connected handles share a buffer lease. The first submit maintains every dirty range in the group before hardware starts. Repeated device-owned uses perform no further cache work. After CPU acquisition, `ss_npu_queue_dirty` marks subsequent writes; maintenance rounds only those ranges to owned cache lines. Acquisition waits for the group's last published use and uses the EL0-enabled cache clean/invalidate instruction (`DC CIVAC`), which is safe because the CPU cannot dirty output lines during the device lease. The original cache-control setting is restored at close.

`ss_npu_queue_release` returns receipt capacity in FIFO order and preserves fence status. It does not grant CPU buffer access. `ss_npu_queue_acquire_handle` remains available after receipt release or reuse. Close requires all receipts released and all used groups acquired. Faults retain buffers and prevent close. Eight sanitizer cases and twelve native SVC/IRQ cases pass. Diagnostic-only counters and updates are absent from production layouts. The public ring issues one interrupt per binary; the prepared-stream interface below removes intermediate events.

## Vendor interfaces define command ordering and completion

The reference is Allwinner's VIPLite 2 kernel driver (pinned at the [linux-orangepi revision](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/npu/aw_nna_vip/vip2); provenance in [the source record](../reference-backsolve-npu.md)). It is source-available under its retained MIT notices and is a development reference only.

- `vip_drv_hardware.c:882-893` writes the command address at `+0x654`, completes a memory barrier, then writes the fetch word at `+0x3a4`.
- `os/linux/vip_drv_device_driver.c:382-392` reads the destructive event word at `+0x010` and wakes the interrupt waiter.
- `vip_drv_hardware.c:520-529` publishes a LINK destination before its opcode. Prepared command sequences retain synchronization and device-cache flushes between dependent commands and issue a final completion event.
- `vip_lite_config.h:409-420` labels parallel asynchronous mode unsupported and selects mode 3. Source presence alone does not qualify live chaining.

`a733_npu_sequence_prepare` constructs a resident sequence with one completion fence. It preserves each native stream's flush/stall footer and emits only the final event and END. Linking and dependency validation happen once, while the sequence is idle. A live command is never patched while the fetch engine can read it. A joined 240-byte stream of two stages produces only the final device completion, so no CPU completion or submission occurs between stages.

## Qualified figures

All rows are 1,000-job runs on port 3-3 at 852 MHz and 800 mV with normal return; the records are in [npu.jsonl](../evidence/npu.jsonl).

| Path | Job p50 / p99 (µs) | Jobs/s | Task CPU per job (µs) | Record |
| --- | --- | --- | --- | --- |
| Copy helper control | 463.88 / 472.46 | 2,029.97 | 491.16 | `1793563838443635411-92c1fdae` |
| Resident queue | 10.25 / 12.83 | 26,925.33 | 34.19 | `1793564135238449408-5dbdd6ad` |
| Two dependent FIFO records | chain interval 13.94 µs | 45,987.32 stages/s | 19.87 per stage | `1793565141195481474-2e135a8c` |
| One joined stream | chain interval 12.02 µs | 48,166.66 stages/s | 18.70 per stage | same |

Submit to doorbell has a mean of 403.44 µs for the control and 0.943 µs for the resident queue. The throughput includes diagnostic CPU work and is not a network compute-throughput limit. Output equality holds for every checked job and intermediate; the dependent runs qualify these buffers, two binaries and normal retirement. Other graphs, fault recovery and EL0 execution need separate evidence.

## Idle gating of the NPU clock

Opt-in internal clock suspension (`drivers/npu/npu_idle.c`) and its power owner (`drivers/npu/npu_runtime_queue.c`) gate the idle NPU. A queue that does not call `npu_runtime_queue_power_start()` creates no owner and stays powered; the always-on control keeps the NPU at 852 MHz and 800 mV while its lifetime is open. The scope is the NPU register window and the vendor VIPLite gate helpers only: no change to the rail, PCK4, the PLL, voltages or top-point clocks.

### The domain and rail stay owned during suspension

PCK4 is domain 4 of the PCK-600 controller, whose register row begins at `0x07064000`, an internal SoC power switch. The pinned vendor binding (`include/dt-bindings/power/sun60iw2-power.h:5-15`) identifies domain 4 as NPU, domain 5 as GPU_TOP and domain 8 as USB2. The A733 User Manual V1.00 section 2 places shared SRAM at `0x74000-0xf3fff` and requires the NPU to remain powered when that SRAM is used; the ROM uses it for its stack and FEL scratch. Domain-4 OFF is therefore unsupported ([power boundary](../npu.md#the-supported-npu-power-boundary-preserves-shared-sram)).

The external DCDC2 regulator is a separate resource. The Orange Pi V1.2 schematic (sheets 2, 6 and 8; `board-schematic` in `sources.lock.json`, SHA-256 `4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0`) connects it to NPU VDD, VDD-SYS, SoC-side VDD-DRAM, VDD-VE, VDD08-UFS, VDD-COMB0/1 and VDD08-HDMI. The vendor board tree marks DCDC2 boot-on and always-on and binds `npu-supply` to it (`sun60i-a733-orangepi-zero3w.dts:2123-2131,2931-2933`). A sole software rail token cannot make this shared supply exclusive.

Suspension retains the shared DCDC2 token, domain-4 ON ownership, exclusive PLL_NPU and NPU clock tokens, shared parent and fabric leases, the PCK clock lease, mappings and buffers. It preserves the SRAM route and all CCU gates, resets, PLL settings, voltage and PCK policies. Its only writes are inside the NPU register window at `0x03600000`. Vendor source supports internal clock suspension but does not prove ROM access to shared SRAM during it, and recovery from a fault while suspended is an evidence gap.

### A one-shot alarm requests gating after sustained idle

Suspension is eligible once the queue has been idle for 10 ms. A successful resume restarts that interval, so the same 10 ms is the minimum on-time. These are initial hysteresis settings, not measured energy break-even points. Completed records awaiting release do not delay gating. The EL0 public ring needs a predicate that tells the owner a published successor exists, with a bounded alarm or a kick when the ring drains; this is open work.

A dedicated static EL1 power owner task runs on the interrupt core at a priority above the producer; the caller of `npu_runtime_queue_power_start()` guarantees that ordering, because the kernel build does not query priorities. The call happens after claim and registration, and the owner remains parked after the queue returns, because the shared deadline registry retains its task identity. The owner waits on the shared deadline facility (`sunstone_deadline_wait_until()`), not on a tick and not on a device timer of its own. Submission and completion never wake it. Every alarm is a bounded look at the state:

- **Busy queue.** The owner arms the next alarm one idle delay (10 ms) ahead, so under sustained load it wakes about 100 times per second, and the completion interrupt wakes no task.
- **Idle but before the delay.** The alarm is the idle time plus the delay. A completion refreshes the idle time and a generation counter from the completion timestamp, so a stale alarm finds a recent completion and rearms.
- **Idle for the delay.** The owner asks the interrupt controller whether it is quiet. Quiet gates. "Not now" (a distributor write in progress on another core, or an NPU interrupt pending or active) is not a fault; the owner counts it and rearms 1 ms ahead. Only an invalid binding is retained.

The part descriptor (`struct npu_idle_part`; `include/a733_npu_idle.h` holds the A733 values) carries every register offset, bit pattern, the full-idle word, the idle delay and the resume budget. The state machine in `npu_idle.c` and the register whitelist in the queue adapter read them from it and contain no A733 constant.

The owner makes the gate decision and performs the suspend sequence under the queue's raw local interrupt lock. It gates only when the queue is empty, completions equal submissions, the last completion left full idle, the lifetime window is open and the interrupt controller is quiet; no state can change inside the lock. The event word at `+0x010` is destructive on read, so no power path reads it. The lock bounds the suspend sequence by the 1 µs settle plus MMIO latency; a settle wait is capped by the resume budget in strict mode and by the part's 1 ms hard bound otherwise.

The phases are ON, SUSPENDED and RETAINED. `ON` covers both running and idle-on-hysteresis; the transition in progress is recorded as the step of the failure record. Admission under the same lock (`npu_runtime_queue_admit()`) returns ready only in ON. In SUSPENDED it records one resume request, returns pending, changes no job, command or fence, and the caller wakes the owner and waits for its single readiness notification (`npu_runtime_queue_wait_ready()`; the EL0 doorbell uses `npu_runtime_queue_power_kick()` after its supervisor call returns). `npu_runtime_queue_submit()` admits and submits in one lock span and returns pending (0) without failing, because a producer that admitted and submitted in separate spans could find the device gated between them. The owner resumes, calls the interrupt quiet query once more before it unlocks, publishes readiness once and rearms the idle alarm.

A partial write, unexpected readback, failed lifetime check, invalid interrupt binding or expired settle enters RETAINED with the first failing site, phase, step, offset, value and expectation preserved. It sets the queue and runtime failure flags and the service-recovery bit, and never retries. A resume that completes but exceeds the budget records its elapsed time in both modes; only a strict (benchmark) queue retains it. An interrupt delivered while the clocks are suspended, or with no active job, is a fault retained without any device access. If the post-resume quiet query found an interrupt pending, the failure site is `RESUME_IRQ`, which separates a glitch caused by the ungate from a later spurious interrupt. A suspended queue resumes before CLOSE: the return path stops the owner, which resumes first, and returns failure if the resume does not leave the clocks running.

### The gate keeps the top operating point

The sequence comes from `vip_drv_pm.c:142-220` in the pinned VIPLite 2 source (SHA-256 `f92cc17a7bf5b9fa40a2991dbf9e511eb4b0cddcf09829f2b047bfe4cf141d35`). Its silicon branch uses a 1 µs delay; `inc/vip_lite_config.h:161-163,470-475` selects silicon and enables internal clock suspension by default. The enclosing `power_clock_state_in/out` routines also change frequency scaling (`vip_drv_pm.c:438-465`); this driver uses only their gate helpers and preserves the full-scale fields established at OPEN.

| Phase | Ordered operations |
| --- | --- |
| Suspend | Read `+0x000`; preserve unrelated fields and scaler bits 8:2. Set bit 0, write with bit 9 high, then write with bit 9 low. Wait at least 1 µs. |
| Resume | Read `+0x000`; preserve unrelated fields and scaler bits 8:2. Clear bit 0, write with bit 9 high, then write with bit 9 low. Wait at least 1 µs. Read `+0x10c`, preserve scaler bits 7:1, then pulse bit 0 high and low. |
| Publish readiness | Read `+0x000`, `+0x10c` and the idle word, record them, and publish IDLE_ON. Only a gate bit still set in `+0x000` after the ungate pulse refuses. |

Dependent writes complete with DSB in the native adapter. The vendor helpers do not verify gated readback or bound failures, and neither does this driver. Register values are read and written back as the vendor does, and values other than the ones established at OPEN are recorded, not refused. No register read follows the final suspend write. While the clocks are suspended the only permitted read is `+0x000` at resume entry; its value is recorded and its gate and pulse bits are cleared for the ungate writes, so a lost suspend write is harmless. After the ungate pulse and the 1 µs settle, `+0x000` is read again: a gate bit still set means the clocks are off and the queue is retained. The owner then reads `+0x10c`, pulses bit 0 high and low on the value it read, and records `+0x000`, `+0x10c` and the idle word. The idle word is compared as the vendor does (`vipdrv_hw_wait_idle`) under the mask `0x7fffffff`; the gate requires the idle word under that mask, and a unit that is not idle refuses it. A report carries the register values of the first cycle that differed from the expected values, or of the latest cycle when none differed, and the number of such cycles. The adapter refuses all other offsets, and the host fixtures fail any read of an unlisted or destructive register and any read other than `+0x000` while suspended. A readback failure retains ownership and cannot justify another power operation.

The resume budget of 100 µs (request to readiness, at most 100 µs added to the first resumed job's p99 and maximum) is a limit that the benchmark images measure and report; they do not retain the queue when a resume exceeds it. An implementation that wakes only on a 1 ms tick cannot satisfy it. Full startup differs: the vendor platform waits 2 ms after its power and clock hook (`vip_drv_device_platform.c:1385-1387`) and the board tree specifies a 1,000 µs regulator-enable ramp, so the gate must avoid the full startup path.

### Benchmark and acceptance limits

Three images share every source and differ only in policy: a baseline that creates no power owner, the control (owner without register writes) and the gating image, all at 852 MHz and 800 mV. Each runs five windows of 12,000 back-to-back jobs and then 100, 100 and 30 cold jobs after 9, 11 and 100 ms idle gaps; the 9 ms case must never gate. Comparisons use the median over the five windows of a boot, then the median over boots. A busy window never lets the gating image gate, so gating against control in those windows measures noise only; the gating cost is judged by the cold groups.

Continuously queued throughput must stay at least 98% of control; p99, maximum latency and CPU time per job within 105%, 110% and 105% of control, and every resume within the absolute budget. CPU time charges the client, the runtime worker and the power owner task; interrupt time landing while the idle task runs is charged to the idle task and appears in neither figure. Cold-job latency after each idle gap is reported separately. Unexplained regressions block acceptance. The run requires no numerical mismatch, retained fault or cleanup failure, with normal retirement and restored DRAM.

Host evidence: `checks/npu-idle.py` (transition groups, access-limited suspended reads, over-budget resume, every injected failure), `checks/npu-queue-handoff.py` (production owner, queue, idle and service code with modeled scheduler, deadline facility, registers and interrupt controller, and regression mutations of the owner) and `checks/npu-idle-report.py` (benchmark report decoder and acceptance comparison). The board rounds are in [npu.jsonl](../evidence/npu.jsonl) and the plan with its prediction is in [npu-fast-path.jsonl](../evidence/npu-fast-path.jsonl), `npu-fast-path-idle-gating-plan`.

## Limits and open work

1. No energy evidence exists. The repository's PMIC provider reads only the AXP8191 revision, enable and voltage-selector bytes and implements no DCDC2 current or power readback. Idle savings, cycle energy and the break-even idle duration are unmeasured.
2. Safe shared-SRAM access while gated and watchdog recovery from a suspended-state fault are unmeasured.
3. The EL0 public ring needs the published-successor predicate and a bounded alarm or kick when the ring drains before gating applies to it.
4. Physical EL0 execution of the ring is unqualified.
5. A comparison between a control and a queue variant with identical model bytes, at least 1,000 jobs each and a prepared dependent sequence, reports submission-to-acquisition latency separately from device time and CPU execution time separately from waiting time; the figures above are its first instance.
