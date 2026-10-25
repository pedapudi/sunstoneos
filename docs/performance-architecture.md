# Performance architecture

This document sets the performance rules for SunstoneOS code on the Orange Pi
Zero 3W (Allwinner A733: six Cortex-A55 cores numbered 0–5, two Cortex-A76
cores numbered 6 and 7, a GIC-600 interrupt controller and 12 GiB of LPDDR5).
It applies to every new or changed driver, service, application and platform
path, and binds the CPU, GPU, NPU and DDR completion work. [AGENTS.md](../AGENTS.md)
and [docs/goal.md](goal.md) require each change to follow it.

The budgets are design limits checked on host models and emulators. They do
not authorize benchmark suites or optimization experiments on the board during
bring-up; [docs/validation.md](validation.md) and section 8 of
[docs/goal.md](goal.md) still govern physical performance measurement. The
safety requirements listed in [AGENTS.md](../AGENTS.md) remain in force: DMA
ownership, cache coherence, shared power and clock coordination, bounded
failure and storage preservation. A check may be removed only when a design
names the check that still runs and covers the same failure.

No figure in this document has been measured on the board. Costs marked
*counted* come from reading the code. Costs marked *estimated* are
order-of-magnitude reasoning from the instruction sequence. The USB0 figures in
section 3 come from the host model `checks/usb0-service-burst.py`, run on
2026-10-21 against the tree at commit `3d576cb`.

## Terms

- **Tick.** One FreeRTOS scheduler period. `configTICK_RATE_HZ` is 1000 in
  [include/FreeRTOSConfig.h](../include/FreeRTOSConfig.h), so a tick is 1 ms.
- **Admission.** The point at which an owner validates a resource before using
  it: a session open, a model load, a DMA job table registration or the sealing
  of the page tables. State that cannot change after admission without a
  recorded event needs no further check until that event.
- **Hot path.** Code that runs once per register access, per packet, per
  inference, per frame or per DMA period. Admission paths and failure paths
  are not hot paths.
- **Host-model metric.** A count taken in an existing host model, QEMU check or
  Unicorn emulation: calls to a translation callback, bytes passed to a digest,
  bytes copied, ticks slept, queue messages, barrier instructions in a
  disassembled function. Counts are deterministic, so a before-and-after
  comparison needs one run.
- **Known device state.** After a failure, the driver can prove from hardware
  status or from the absence of any device access what the device can still
  do. Examples: a request refused before any register write, or a DMA channel
  that reads back quiet with the saved descriptor (the check in
  [drivers/dma/a733_dma_controller.c](../drivers/dma/a733_dma_controller.c)).

## 1. Mechanisms that cost time without a matching safety requirement

Each entry gives where the mechanism occurs, what it costs, the lighter design
and the conditions under which the heavier form is still required.

### 1.1 Ownership, liveness and deadline checks repeated on every register access

**Where.** [platform/a733/hdmi_native.c](../platform/a733/hdmi_native.c)
`read32` and `write32` call `allowed`, `live`, `lock` and `unlock` around each
access. `live` and `lock` each call `tokens`, which queries the clock lease
registry, the rail registry and the PCK resource manager; `live` also calls
`a733_pmic_live`. `lock` and `unlock` call `timely` four times in total, and
the platform lock reads the clock twice more.
[platform/a733/gpu_backend.c](../platform/a733/gpu_backend.c) `start_read`
calls `output`, `before` twice and `raw_read`. `output` compares the
destination with every firmware and resource buffer and translates it with
`AT`. Each `before` calls `now`, which calls `output` again.
[drivers/uart/a733_uart_platform_native.c](../drivers/uart/a733_uart_platform_native.c)
`access_allowed` checks the task context and the register-mutex holder per
access.

**Cost.** Counted from the code, one HDMI 32-bit register read performs:

- six counter-clock reads, each a load of the one shared history word in
  [platform/a733/counter_clock.c](../platform/a733/counter_clock.c) and, when
  the read advances it, a compare-and-swap;
- six task-context checks, each masking and restoring DAIF;
- seven provider status queries, each a try-lock; the clock query also scans
  up to 128 slots (`a733_clock_token_status` in
  [drivers/clock/a733_clock_leases.c](../drivers/clock/a733_clock_leases.c));
- one mutex take and give;
- two `DSB SY`.

One GPU start-sequence register read performs five `AT` translations. The
estimated cost is several microseconds per register access, against roughly
0.1–0.3 µs for the access itself.

**Lighter design.** Validate tokens, leases, the PMIC lifetime and the register
window mapping once when the owner enters a phase. Take the register lock once
for a programming sequence and check the deadline once per sequence step. Keep
the per-address allowlist, which costs a few compares and stops a wrong write
to shared hardware. Detect revocation with one acquire load of a revocation
flag that the provider sets; `healthy()` in `gpu_backend.c` already reads
`external.revoked` this way.

**Heavier form required when** a provider can revoke a clock, rail or reset
asynchronously and an access after revocation can hang the bus. Then check the
revocation flag before each access, and keep the full token query at phase
entry. Shared registers, such as CCU gate and reset registers, still need the
register lock across each read-modify-write.

### 1.2 `DSB SY` before and after every register access

**Where.** [include/a733_mmio.h](../include/a733_mmio.h)
`a733_mmio_*_dsb_sy_both`, which 49 files call
([docs/shared-helpers.md](shared-helpers.md)). All MMIO is mapped as
Device-nGnRnE: MAIR attribute 0 is `0x00` (`A733_CPU_COHERENCE_MAIR` in
[include/a733_cpu_coherence.h](../include/a733_cpu_coherence.h)) and
`l1[0]` in [platform/a733/cpu_coherence.c](../platform/a733/cpu_coherence.c)
selects it for the first gigabyte.

**Cost.** A `DSB SY` stalls the core until all of its earlier memory accesses
complete in the full-system domain, including stores to normal memory still in
the store buffer. The helpers issue two per access, at an estimated tens to
hundreds of cycles each. Device-nGnRnE accesses to one peripheral already reach it in program order.
Under the no-early-acknowledgement attribute the write response comes from the
peripheral itself, so a `DSB` after a write waits for a full bus round trip.

**Lighter design.** Use plain accessors inside a sequence to one peripheral.
Place one barrier where ordering crosses memory types:

- `DMB OSHST` before a doorbell or start write that publishes descriptors or
  data the CPU wrote to normal memory (the Linux `writel` ordering);
- `DMB OSHLD` after a status read that gates CPU reads of memory a device
  wrote (the Linux `readl` ordering);
- `DSB SY` only for completion: before `WFI`, before a timed delay that assumes
  a reset or clock write reached the device, after cache or TLB maintenance,
  and after a device interrupt-clear write where one spurious re-entry of a
  level-triggered interrupt is not acceptable.

The weaker forms do not exist in `a733_mmio.h` yet. Adding them, and moving a
driver onto them, needs the board qualification that
[docs/shared-helpers.md](shared-helpers.md) requires for any barrier change.
New drivers state their barrier placement per sequence in the design so that
the migration is mechanical once qualified.

**Heavier form required when** ordering spans two peripherals, such as a device
interrupt clear followed by a GIC write, or when the following action is not a
memory access. The design names that action.

### 1.3 One step per 1 ms tick

**Where.**

- [apps/accelerators/gpu.c](../apps/accelerators/gpu.c): the GPU client polls
  `PVR_GPU_SERVICE_STATUS` with `vTaskDelay(1)` between polls while waiting for
  READY, compute completion and STOPPED.
- [drivers/gpu/pvr_gpu_service.c](../drivers/gpu/pvr_gpu_service.c) `worker`:
  `timed()` returns true while a compute job runs, so the worker waits at most
  one tick on its queue even when the GPU interrupt is bound.
- [drivers/npu/npu_service.c](../drivers/npu/npu_service.c) `worker`: a
  one-tick wait while the controller is active, and a one-tick notification
  wait when an interrupt-bound poll made no progress.
- [apps/npu-runtime/main.c](../apps/npu-runtime/main.c): the client waits for
  queued work with `vTaskDelay(1)`.
- [apps/accelerators/usb.c](../apps/accelerators/usb.c): eight USB polls,
  then one tick, independent of progress.
- [platform/a733-smp-observe/usb_service.c](../platform/a733-smp-observe/usb_service.c):
  one USB pass per tick.

**Cost.** Each state transition observed by a tick poll costs up to 1 ms of
latency, and every waiting task wakes 1000 times per second. A GPU compute job
in the accelerator image needs one tick-paced STATUS poll per tick
of execution plus 31 output round trips (section 1.6).

**Lighter design.** The interrupt handler, or the service worker that observes
completion, notifies the waiting task directly, and the waiter blocks on the
notification with the deadline as its timeout. Driver state machines keep
their bounded steps; the owner repeats steps while they report progress within
a time budget, as [apps/board/usb0_service.h](../apps/board/usb0_service.h)
does for USB0 (200 µs or 64 passes per burst). A service replies to a held
request when the state changes; `NPU_SERVICE_WAIT` already works this way.

**Heavier form required when** the device has no qualified interrupt delivery
(the USB0 interrupt is still outstanding), or the expected hardware wait is at
least one tick, such as a PLL lock or a rail ramp. A tick poll may also remain
as the timeout path behind an interrupt whose delivery is unqualified; the
design states its period and the condition that removes it.

### 1.4 Re-hashing immutable buffers on every request

**Where.** [drivers/npu/npu_convolution.c](../drivers/npu/npu_convolution.c)
`publish` calls `unchanged(c, 1)` for every job. It hashes the NN descriptor
buffer and the coefficient (or PPU) buffer with a portable C SHA-256, hashes
the command stream prefix and checks every later eight-byte word of the stream
allocation. The comment above `unchanged` gives the reason: the shared SMP
mappings cannot stop another privileged writer.

**Cost.** For the catalogued convolution models the immutable buffers are a
few hundred bytes, so the per-job hash is small. Larger models make it
proportional to model size: at an estimated 15–25 cycles per byte for portable
SHA-256 on the A55, each mebibyte of coefficients adds about 8–14 ms per
inference at 1.8 GHz and 30–50 ms at the 494 MHz boot clock.

**Lighter design.** Hash once at admission, then remove the writers. Map the
verified model, coefficient and command regions read-only at EL1 and EL0 after
admission, so a stray write faults at the writing instruction. The device side
uses read-only entries in the NPU MMU. With read-only mappings in place, the
per-job hash checks nothing that the mapping does not already enforce, and is
removed. Until the page-table owner provides read-only regions, a design that
keeps the per-job hash budgets its cost from section 3.

**Heavier form required when** the bytes cross a trust boundary after
admission. Per-request input from the host is verified per request, over its
own bytes only. Memory that firmware or a remote processor can write
([docs/remote-writers.md](remote-writers.md)) is verified at each point where
the CPU relies on it.

### 1.5 Per-page translation walks over memory that has not changed

**Where.**

- [platform/a733/device_memory_layout.c](../platform/a733/device_memory_layout.c)
  `a733_device_memory_prepare` translates every 4 KiB page of the extended
  pools, read and write: 196,608 write-back pages and 4,096 non-cacheable pages
  ([checks/device-memory-capacity.c](../checks/device-memory-capacity.c)
  asserts `(196608+4096)/64` batches). EL1 maps those pools with 2 MiB blocks
  ([platform/a733-smp-cached/memory.c](../platform/a733-smp-cached/memory.c)).
- [drivers/dma/dma_memory.c](../drivers/dma/dma_memory.c) `dma_memory_accept`
  translates every 4 KiB page of every mapping on every submit, for job tables
  that are fixed when the DMA service initializes.
- [drivers/npu/a733_npu_memory.c](../drivers/npu/a733_npu_memory.c)
  `live_mappings` translates every page of the MMU arena and of every buffer
  in both `a733_npu_memory_publish` and `a733_npu_memory_finish`, and
  `canonical` re-reads every page-table word, on every job.
- [platform/a733/npu_native.c](../platform/a733/npu_native.c)
  `register_access` translates the register address on every register access.

**Cost.** Each translation in
[arch/aarch64/device_memory.c](../arch/aarch64/device_memory.c) masks DAIF,
saves `PAR_EL1`, issues `AT`, an `ISB` that drains the pipeline, reads and
restores `PAR_EL1`, and restores DAIF: an estimated 30–100 cycles. Boot
preparation performs about 401,000 translations (counted), which is an
estimated 25–80 ms at 494 MHz; 392 block checks cover the same memory. An NPU
job over a 4 MiB graph pool performs at least 2,048 translations (counted)
before any page-table scan.

**Lighter design.** The page tables are sealed before device admission and do
not change afterwards. Verify each mapping once at admission, at its own
granule: one translation per 2 MiB block, plus a read of the descriptor level
that proves the block size. Register windows are verified once at
initialization. The table owner publishes a mapping generation; a consumer
re-verifies only when the generation changes. Device page tables built by the
driver, such as the NPU MMU arena, are verified once after construction and
then mapped read-only to the CPU.

**Heavier form required when** a region is mapped by 4 KiB pages with mixed
attributes (the EL3 L3 table at `0x42000000`), when the tables can change after
admission, or when two translation regimes map the region; then each regime is
checked once per granule.

### 1.6 Data copied between owners that could hand over the buffer

**Where.**

- I2S cyclic receive: `a733_dma_cyclic_rx_copy` copies each 4 KiB period into
  a snapshot, then `a733_i2s_stream_copy`
  ([drivers/audio/a733_i2s_stream.c](../drivers/audio/a733_i2s_stream.c))
  copies the snapshot to the client one byte at a time.
  `dma_service_copy_period` ([drivers/dma/dma_service.c](../drivers/dma/dma_service.c))
  checks the destination against every job's descriptors and mappings on each
  call.
- GPU compute: [apps/accelerators/usb.c](../apps/accelerators/usb.c) reads the
  input into `accelerator_gpu.input`, `pvr_gpu_service_compute_input_epoch`
  copies it to `input_copy`, and the compute service copies it into the device
  buffer. The output leaves the device buffer through a volatile byte loop
  (`read_output` in [drivers/gpu/pvr_compute_service.c](../drivers/gpu/pvr_compute_service.c)),
  then travels 32 words per reply through two FreeRTOS queue copies
  ([include/pvr_gpu_service.h](../include/pvr_gpu_service.h) `compute_words[32]`).
  Render readback moves 64 pixels per reply.
- NPU: `npu_convolution_send` copies the input into the input buffer,
  `npu_convolution_output` copies the output out, and the large-graph build
  clears the whole intermediate pool on every job
  ([drivers/npu/npu_convolution.c](../drivers/npu/npu_convolution.c)).

**Cost.** Counted: a 3,968-byte GPU compute result needs 31 request-reply round
trips, each moving a reply of about 520 bytes through two queue copies, so
about 32 KB of copying and 62 context switches deliver 4 KB. I2S copies every
received byte twice, once one byte at a time.

**Lighter design.** The producer hands over the buffer itself. A ring of
device-memory slots moves through explicit states: CPU-owned, device-owned,
completed, consumer-owned and free. The device writes a slot; the completion
path invalidates only the written range, once, and publishes (slot, length,
generation) on a completion queue. The consumer reads in place and returns the
slot. Replies carry descriptors and no payload. One copy remains where the
consumer must keep data beyond the ring depth or where an EL0 task has no
mapping of the buffer; that copy uses `memcpy` rather than a byte loop.

**Heavier form required when** a cyclic DMA engine overwrites a slot before the
slowest admitted consumer can return it; then the design sizes a snapshot copy
and states the consumer latency that forces it.

### 1.7 Global try-locks and linear scans on hot paths

**Where.**

- [platform/a733/counter_clock.c](../platform/a733/counter_clock.c)
  `a733_counter_clock_now`: every time read on every core that advances the
  shared history writes one cache line with a compare-and-swap. The read takes
  no lock and no interrupt mask
  ([multicore-performance-1025.md](designs/multicore-performance-1025.md)).
  Locks, the clock registry, DMA, USB, the NPU, the PMIC and the watchdog all
  read time through it.
- [drivers/clock/a733_clock_leases.c](../drivers/clock/a733_clock_leases.c)
  `a733_clock_token_status` and
  [drivers/power/a733_pmic.c](../drivers/power/a733_pmic.c) `a733_pmic_live`
  take a try-lock and return BUSY on contention; the clock query also scans up
  to 128 slots.
- [platform/a733/shared_locks.c](../platform/a733/shared_locks.c)
  `a733_platform_lock_enter` scans the holder of every platform lock, and one
  register mutex serializes all shared-register access.

**Cost.** Eight cores contend for the history's cache line on time reads. Under
contention a status query returns BUSY, which callers turn into a failure or a
retry (section 1.8). Section 1.1 shows these calls multiplied per register
access.

**Lighter design.** Read time from `CNTPCT_EL0` directly and keep the
monotonic check per caller; `a733_monotonic_advance` already does this in
the HDMI adapter. Give each lease a generation and a revocation flag that
readers load with acquire ordering, so a status check needs no lock and no
scan. Split the register mutex by register block where owners do not share a
register.

**Heavier form required when** the operation changes state: acquiring or
releasing a lease, changing a rail voltage or a clock parent. Those transitions
keep the manager lock.

### 1.8 Retaining every resource after a failure whose device state is known

**Where.**

- [apps/accelerators/gpu.c](../apps/accelerators/gpu.c) `request` suspends the
  client forever and keeps every lease on any result other than OK. This
  includes BUSY and DENIED, which `execute` in
  [drivers/gpu/pvr_gpu_service.c](../drivers/gpu/pvr_gpu_service.c) returns
  from phase and owner checks before any GPU access.
- [platform/a733/hdmi_native.c](../platform/a733/hdmi_native.c) `read32` calls
  `fail` when `lock` does not return 1, which includes a register-lock wait
  that timed out under contention; `fail` retains the clock, the rail and any
  held register lock.
- [apps/npu-runtime/main.c](../apps/npu-runtime/main.c) retains the NPU
  session on any refused request.

**Cost.** The project retains resources when a driver cannot tell whether
hardware finished an operation ([docs/audit/README.md](audit/README.md),
"Retained resources"). Applied to refusals and contention, the policy removes a
device until power-off after one transient condition. For the GPU and NPU
work, one refused job would end accelerator use for the session.

**Lighter design.** Each failure path classifies the device state:

1. **Refused before any device access.** Return the error; the session stays
   usable.
2. **Device in a known terminal state** with a verified quiesce, such as the
   DMA channel readback, an idle NPU controller or a firmware stop
   acknowledgement. Reset the device, rerun admission, and report the job as
   failed. Bound the number of recoveries per session.
3. **State unknown,** such as a timeout with DMA possibly outstanding. Retain
   only what the device can still reach: its memory, its clock and rail,
   and the register lock only if a shared-register sequence stopped midway.

**Heavier form required when** completion is unknown, when firmware controls
the device and no quiesce is verified, and for every storage write path.

## 2. Defaults for new work

A design follows each default below or states the requirement that prevents
it.

1. **Interrupts and completion queues.** Completion reaches the waiting task
   through a task notification or a completion queue written by the interrupt
   handler or service worker. No task polls a status request on a tick to
   learn about completion. Section 1.3 lists the permitted tick waits.
2. **Zero-copy or one-copy data paths with explicit ownership hand-off.**
   Buffers come from the device-memory pool at admission and move between
   owners by state change, using the ownership states of
   [include/device_memory_pool.h](../include/device_memory_pool.h). Each
   design counts copies per byte from producer to final consumer; more than
   one needs a stated reason.
3. **Weakest correct barrier and range cache maintenance.** CPU-to-CPU
   synchronization uses inner-shareable ordering: acquire and release accesses,
   `DMB ISH`, and `DSB ISHST` before `SEV`. CPU-to-device ordering uses the
   outer-shareable `DMB` forms of section 1.2. Cache maintenance uses
   `aarch64_device_cache` by virtual address over the exact bytes the other
   agent reads or wrote. Set-and-way maintenance is not used, and a whole
   buffer is maintained only when the whole buffer changed. `DSB SY` appears
   only with a written reason. The leading `DSB SY` in `aarch64_device_cache` is redundant for data the calling
   core wrote, because data-cache maintenance by address is ordered after the
   same core's earlier stores to that cache line; removing it is a change to
   [arch/aarch64/device_memory.c](../arch/aarch64/device_memory.c), which the
   primary agent owns.
4. **Verify once at admission.** Mappings, model and firmware integrity, job
   tables, FIFO endpoints and lease ownership are checked when admitted and
   trusted until a recorded event invalidates them: a mapping generation
   change, a lease revocation or a session reset. Hot paths keep only checks
   whose inputs change per operation, such as an index or length supplied with
   the request.
5. **Batch MMIO and DMA.** A register sequence runs under one lock acquisition
   and one deadline check per step. DMA transfers use the largest segment the
   controller and the policy allow (1 MiB today, `A733_DMA_MAX_SEGMENT_BYTES`
   in [include/a733_dma.h](../include/a733_dma.h)) and raise one interrupt per
   transfer, or one per period for cyclic audio.
6. **Per-core data on separate cache lines.** State that several cores write
   is either per core in a 64-byte-aligned structure or written by one owner.
   [platform/a733/counter_clock.c](../platform/a733/counter_clock.c) asserts
   this separation for its own fields. The FreeRTOS port's per-core arrays
   (`ullCriticalNestings`, `ullPortInterruptNestings` and three more in
   [patches/freertos-smp-asm.patch](../patches/freertos-smp-asm.patch)) share
   lines today; that change belongs to the primary agent.
7. **Hot work on the A76 cores once admitted.** Recognition kernels and the
   CPU side of GPU and NPU jobs set their affinity to cores 6 and 7 after the
   multicore work admits those cores for the image. Service, USB0 and
   housekeeping tasks stay on A55 cores, normally core 0, so they do not
   preempt hot work. Affinity is set with `xTaskCreateStaticAffinitySet`.
8. **Large mappings for large buffers.** CPU mappings of buffers of 64 KiB or
   more use 2 MiB blocks, or 64 KiB contiguous runs of 4 KiB pages where a
   block does not fit. The DDR work maps the full 12 GiB with 1 GiB and 2 MiB
   blocks. Device MMUs (NPU, GPU, IOMMU) use their largest supported page size
   for large buffers, as stated by the hardware documentation or the pinned
   reference driver; a design that keeps 4 KiB device pages records why. With
   4 KiB pages, a TLB of about a thousand entries reaches only about 4 MiB.
9. **Bounded failure without global retention.** Failure paths follow the
   three classes of section 1.8. A design lists each failure, its class and
   the resources it retains.

## 3. Budgets for the CPU, GPU, NPU and DDR work

Each budget is a host-model metric. A change that exceeds a budget needs a
design note explaining why; a change that worsens a metric without an
explanation is a regression and blocks the merge. A baseline marked *counted*
was derived from code and becomes a measured baseline when the owning host
model gains the counter.

### Memory allocator

[runtime/device_memory_pool.c](../runtime/device_memory_pool.c) is first fit
over at most 32 allocations (`DEVICE_MEMORY_MAX_ALLOCATIONS`) in a pool of at
most 768 MiB.

| Metric | Baseline | Budget |
| --- | --- | --- |
| Slot comparisons per acquire | At most 33 × 32 = 1,056 (counted) | Unchanged at admission; zero allocations on any hot path |
| Allocations per inference, frame or DMA period | Zero in current GPU, NPU and DMA paths | Zero |
| Largest free extent after the image's admission sequence plus one retained allocation of each kind | Not modeled | At least the largest buffer the image allocates after admission; checked by replaying the admission order in `checks/device-memory-capacity.c` |
| Free extents after every session of the image opens and closes once | Not modeled | Identical to the free extents before the cycle, checked in the same replay |

### DMA

| Metric | Baseline | Budget |
| --- | --- | --- |
| Bytes per descriptor, memory-to-memory and bulk device transfers | 4 KiB in the DMA pool session (`exact_mapping` in [platform/a733/dma_pool_session.c](../platform/a733/dma_pool_session.c)); policy limit 1 MiB | At least 64 KiB when the transfer is that large |
| Bytes per descriptor, cyclic audio | 4 KiB per period | Period of at least 5 ms of audio |
| Interrupts per transfer | One when the interrupt is bound; tick polling otherwise | One per transfer or period |
| Address translations per submit | One per 4 KiB of every mapping (counted) | Zero for an admitted job table |
| Copies per received audio byte | Two, one of them byte by byte (counted) | One at most, by `memcpy` |

### GPU

Baselines are counted for the compute path of the accelerator image.

| Metric | Baseline | Budget |
| --- | --- | --- |
| Submission latency: client send to GPU kick, with the worker idle | One queue hand-off and one worker step; not measured | No tick wait or status poll between send and kick; at most one context switch |
| Client wake-ups per compute job | One per tick of execution plus 32 reply wake-ups | At most three: submit reply, completion, result |
| Worker wake-ups per job beyond interrupts | One per tick of execution | At most two |
| Queue round trips to deliver a 3,968-byte result | 31 | One, carrying a buffer reference |
| Copies per input byte, after the transport | Two | One at most |
| Copies per result or rendered frame byte | At least five for compute results; render readback 64 pixels per reply | Zero for scanout (ownership hand-off); one for evidence readback |
| `AT` translations per GPU register read | Five | Zero after initialization |

### NPU

Baselines are counted for `npu_convolution` in the large-graph build.

| Metric | Baseline | Budget |
| --- | --- | --- |
| CPU work per inference outside cache maintenance | Digest of NN and coefficient or PPU buffers and the command stream; translation of the MMU arena and every buffer, including the 4 MiB pool, twice; scan of every MMU table word; input copy; pool clear; output copy | No digest, no translation and no table scan after admission; at most one input copy and no output copy when the consumer reads the output buffer in place |
| Cache maintenance per inference | Whole READ and WRITE buffers | Bytes the CPU wrote (clean) and bytes the NPU wrote (invalidate) |
| Queue round trips per inference | Up to three (SUBMIT, WAIT, REAP) | Two (SUBMIT, completion) |
| Model verification | One SHA-256 pass over the model at preparation, plus the per-job digests above | One pass over the model and its coefficients at load; none per inference |

### CPU scheduler

The kernel port and its patches belong to the primary agent; these budgets
state what the multicore work must reach.

| Metric | Baseline | Budget |
| --- | --- | --- |
| Critical-section enter and exit pair, EL1 task, cached eight-core image | Not measured; the port takes task and ISR spinlocks with `DSB SY` and `DMB SY` around each access ([docs/audit/performance.md](audit/performance.md), "Lighten the SMP spinlock and separate per-core state") | At most 100 instructions in the QEMU SMP model, no `DSB SY`, no `SVC` |
| Context switch, task notification to the woken task on the same core | Not measured | At most 800 instructions including FPU state, in the QEMU SMP model |
| Counter-clock read | A global ticket lock with interrupts masked (counted) | One `ISB`, one `MRS` and a per-caller compare |
| Tick-driven wake-ups per idle service task | One per tick for most board services ([docs/audit/performance.md](audit/performance.md), "Keep service tasks off recognition cores") | Zero, except watchdog renewal at its required period |
| Cache lines written by more than one core in the scheduler fast path | Five arrays sharing lines | Zero |

### USB0 evidence channel

Baselines come from `checks/usb0-service-burst.py`, which models the shared
loop in [apps/board/usb0_service.h](../apps/board/usb0_service.h) with a host
that services a packet 20 µs after it is written.

| Metric | Baseline | Budget |
| --- | --- | --- |
| 64 KiB response | 13 ticks, about 5.0 MB/s; at most 10 packets and 189 µs busy per tick | Every image, including the accelerator, NPU runtime and observation images, uses the shared loop and meets the same figure |
| 2,064-byte request | 2 ticks | 2 ticks |
| 1,160-byte response | 1 tick | 1 tick |
| Idle cost | 1 pass and 2 µs per tick | At most 1 pass and 5 µs per tick |
| Accelerator image (fixed 8 polls per tick) | Not modeled; at most 8 packets per tick, about 4 MB/s | Shared loop, as above |
| With qualified USB0 interrupt delivery | Not available | At least 16 MB/s for a 1 MiB response in the model |

### DDR

| Metric | Baseline | Budget |
| --- | --- | --- |
| Translations to admit the extended device pools | About 401,000 (counted) | One per 2 MiB block: 392 for the same 784 MiB |
| Memory mapped at EL1 | 128 MiB of DDR in 2 MiB blocks ([platform/a733/cpu_coherence.c](../platform/a733/cpu_coherence.c)), plus the extended pools | Full 12 GiB in 1 GiB and 2 MiB blocks, with page-table memory under 64 KiB |
| Per-page work in any DDR admission or test path | Per page | Per block, except a memory test that writes every byte by design |

## 4. How designs and reviews apply this document

### Design note

Before implementation, the owner writes a short design in the component
document or the change description. It contains:

1. **Hot paths and their rates.** For example: one register access per DMA
   period at 50 Hz; one inference per audio window.
2. **Per-operation cost list.** For each hot path: checks, locks, barriers,
   translations, digests, copies, wake-ups and tick waits per operation.
3. **Budgets.** The applicable rows of section 3, the baseline and the target.
   A path that section 3 does not cover gets its own metric in the same form.
4. **Heavier forms used.** Each use of a mechanism from section 1, with the
   condition from its "heavier form required when" paragraph that applies.
5. **Failure classes.** Each failure, its class from section 1.8 and the
   resources it retains.
6. **Measurement.** The host model or emulator and the counter that measures
   each budget, before and after the change.

The design stays proportional to the change: a one-file fix states the hot
path it touches and its count before and after.

### Review

A reviewer checks the design before code is written, and the change before it
merges, for three things:

- **Over-engineering.** Every check on a hot path has an input that changes per
  operation, or the design names the event that makes the admission result
  stale. Every lock is taken once per sequence rather than once per access.
  No abstraction exists without a current consumer.
- **Hardware use.** Interrupts are used where delivery is qualified; DMA
  segments, mapping granules and core placement follow section 2.
- **Correctness.** Removing a check names the check that still covers the same
  failure. Barrier changes cite the ordering rule they rely on and carry the
  board qualification that [docs/shared-helpers.md](shared-helpers.md)
  requires. DMA ownership and cache-maintenance states remain explicit.

Review findings name the section of this document they apply.

### Measurement and records

- Add counters to the existing host model of the component; do not add a
  separate reporting system. Record the before and after counts in the
  component document's evidence with the input identities, as
  [docs/validation.md](validation.md) requires.
- An unexplained regression in a section 3 metric blocks the merge. An
  explained regression records the requirement that causes it.
- Physical timing runs only in scheduled board sessions after functional
  coverage, under the measurement rules of [docs/validation.md](validation.md).

### Changes that need the primary agent

Several defaults depend on code the primary agent owns: the FreeRTOS SMP
patches (critical sections, per-core arrays, idle `WFI`), the page tables in
[platform/a733/cpu_coherence.c](../platform/a733/cpu_coherence.c) and
[platform/a733-smp-cached/memory.c](../platform/a733-smp-cached/memory.c)
(read-only regions, DDR mapping, a mapping generation), and
[arch/aarch64/device_memory.c](../arch/aarch64/device_memory.c) (the leading
`DSB SY`). A design that needs one of these states the change and its
consumer, and the primary agent schedules it.
