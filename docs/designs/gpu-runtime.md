# Design: concurrent GPU sessions, recovery and power management

Status: **design**; no code exists yet. Every item below ends at implemented
and host-modeled, with physical behavior pending until the board evidence at
the end exists. Readers are assumed to know the FreeRTOS SMP port, non-coherent
DMA and the PowerVR firmware queues described in [gpu-queues.md](../gpu-queues.md)
and [gpu-cccb.md](../gpu-cccb.md).

Today the GPU service runs one owner's session. Compute and render refuse each
other with BUSY, one job runs at a time, completion is learned by STATUS polls
on every tick, results travel 32 words per reply, and any fault retains the
device until power-off. This design adds two isolated sessions with jobs in
flight together, interrupt completion, ownership hand-off of job buffers, hang
detection with a bounded reset, firmware loaded from the FAT volume, idle
power-off and two operating points.

## Data flow and ownership

The service worker stays the only task that touches GPU registers, the
firmware queues and the client command rings. Clients on any core send it
requests; it is the single writer of every device-facing structure, so no
register sequence needs a cross-core lock.

- **Session.** At most two (`PVR_GPU_SESSIONS`), one per consumer: the display
  renderer and the mimus compute dispatcher. A session owns one firmware
  memory context with its own GPU page catalogue, one context per job kind
  with its command ring, a timeline word that the firmware advances after each
  job, and two job slots per kind. At most one session may render, because
  the free-list and render-target owner (`a733_gpu_graphics_memory`) holds one
  set of lists. Every buffer is a device-memory lease acquired at session open;
  no allocation happens per job.
- **Job slot states.** CPU (the client fills it), SUBMITTED (the worker cleans
  the declared input ranges and writes the ring), DEVICE (kicked), COMPLETE
  (the worker has invalidated the declared output ranges and set the lease to
  CPU), then CPU again when the client reuses it. The states map onto the
  lease states of [device_memory_pool.h](../../include/device_memory_pool.h).
- **Submit.** A 32-byte message (session, kind, slot, input and output lengths,
  deadline, sequence) on the existing request queue. The worker checks only the
  per-request fields, writes the ring in 256-byte steps inside one burst, then
  submits one KICK on the kernel command queue (KCCB).
- **Completion.** One message per job on the client's reply queue: sequence,
  result and error, about 48 bytes. The reply loses `compute_words[32]` and
  `render_pixels[64]`. The client reads output in place.
- **EL0 clients.** Device pools have no EL0 mapping. An EL0 client registers
  its input and output buffers at session open, where they are checked once;
  the worker then copies input in at submit and output out before completion,
  with `memcpy`.

## Completion by interrupt; polling only as a watchdog

The GPU interrupt (SPI 63) is routed to the worker's core. The existing
handler masks it and queues one coalesced wake. On each wake the worker
drains the firmware-to-host queue (FWCCB), reads the timeline of each session
with work in flight (one non-cacheable load each, then `DMB OSHLD` ordering),
completes advanced jobs and rearms. The pinned driver detects completion the
same way (`pvr_device_irq_thread_handler` calls
`pvr_device_process_active_queues`).

With no job in flight the worker blocks without a timeout. With jobs in
flight it blocks until the earliest job deadline or the watchdog period W,
whichever comes first. W is 500 ms, the pinned driver's `WATCHDOG_TIME_MS`.
Until board evidence shows SPI 63 delivery for every job, images set W to
4 ms, so a lost interrupt costs at most 4 ms. That is the heavier form of
section 1.3 of [performance-architecture.md](../performance-architecture.md);
the board evidence below removes it. Images without interrupt callbacks keep
polling once per tick, as today.

## Memory placement, cache policy and GPU MMU isolation

| Data | Placement | CPU cache policy |
| --- | --- | --- |
| Firmware queues, command rings, timelines, firmware data | `uncached` pool, as today | None; `DMB OSHST` before the doorbell once qualified |
| Job inputs, outputs, vertices, textures, render targets | `gpu` class (stage 2 of the memory architecture), write-back | Clean the declared input ranges at submit; clean and invalidate the declared output ranges at completion; two `DSB SY` per transition |
| GPU page tables | write-back, built at session open | Cleaned once after construction; never written while the session is open |
| Firmware image read from the FAT volume | EL1 memory, 131,072 bytes | Read-only to the worker after the hash check |

Each session's page catalogue maps only that session's leases, using 2 MiB
GPU pages for buffers of 2 MiB or more, 64 KiB pages from 64 KiB, otherwise
4 KiB ([gpu-vm.md](../gpu-vm.md) supports all three). Admission checks each
span once against its lease (`device_memory_describe` and
`a733_extent_within`) and keeps the existing rule that no page-table or
firmware page appears in any span. The new host check
`checks/gpu-session-isolation.c` walks every valid entry of both catalogues
and asserts that the reachable physical pages of each session are a subset of
its own leases. It also asserts that an injected cross-session span is
refused at admission. A GPU page fault reported by the firmware fails only
the faulting session.

CPU mappings do not change after preparation, so mappings are verified once
at admission or bind. The per-access `AT` translations are removed from
`mapped()` in `platform/a733/gpu_memory.c`, `io_span()` in
`drivers/gpu/pvr_compute_service.c`, and `output()` and the per-access
`device_page()` calls in `platform/a733/gpu_backend.c`; `device_pages()` still
checks every register page at bind. Bounds checks against the admitted span
remain.
Register sequences check the deadline once per step instead of before and
after each access.

**Barriers.** A KCCB submission now issues 11 `DSB SY` (counted). Three
ordering points need them: after the ring contents and KCCB command, before
the write-offset store; after the write-offset store, before the doorbell; and
after the doorbell. The duplicates between them go. The change to `DMB OSHST`
at the first two and the removal of the third carry the board qualification
that [shared-helpers.md](../shared-helpers.md) requires.

## Synchronization across cores

- **Requests.** FreeRTOS queues, which the SMP kernel lock serializes. Each
  client has at most two submissions outstanding, so the request queue holds
  16 messages and each reply queue two.
- **Single writer.** The worker writes slot states, session records and all
  device structures. A client writes only a slot in state CPU. The lease state
  change, a compare-exchange from the memory architecture, publishes each
  hand-off with release and acquire ordering. Per-session records are
  64-byte aligned.
- **Interrupt.** The existing critical sections around mask and rearm stay;
  they run once per interrupt.
- **Placement.** The worker runs on an A55 core with SPI 63, so the handler
  wakes it without an inter-processor interrupt. Clients of mimus compute run
  on cores 6 and 7 once the multicore work admits them.

## Hang detection, request block and transaction drain, bounded reset

Every W, with work in flight, the worker applies the pinned driver's
watchdog: two checks without progress in `kccb_cmds_executed` while the KCCB
holds commands, or a missing return for a HEALTH_CHECK command sent while the
KCCB is idle, is a hang. A job past its deadline is also a hang. A firmware
CONTEXT_RESET notification is not: the firmware has already recovered the
hardware, so that session's in-flight jobs fail and the session must be
closed and reopened while the other continues.

A full reset runs these steps:

1. Close admission and fail every in-flight job; slots stay DEVICE.
2. **Request block.** If the firmware answers, send the forced-idle request
   of the existing stop sequence, bounded at 100 ms. Otherwise go to step 3.
3. **Transaction drain.** Poll SIDEKICK_IDLE (without the GARTEN, SOCIF and
   HOSTIF bits), SLC_IDLE = `0xff` and zero requests in BIF_STATUS_MMU, as the
   pinned `pvr_fw_stop` does, bounded at 10 ms. A timeout is class 3 below.
4. Assert reset with readback and switch domain 5 off through the power
   owner, which first checks the CCU gate bits.
5. Return every slot to CPU, restage the firmware data from the retained
   image, reformat the rings, recreate both sessions' firmware contexts and
   run the existing start sequence. Page tables and leases stay.

The service allows three full resets per boot. A fourth hang leaves the GPU
off and closes both sessions; drain was shown, so their leases are released.
Session close also retries firmware CLEANUP_BUSY every millisecond up to the
close deadline, instead of treating it as fatal.

## Cold initialization and firmware from the FAT volume

The storage owner task loads the firmware with `fatfs_port_load` into a
static 131,072-byte buffer, compares one SHA-256 pass with the pinned value
compiled into the image, and hands the buffer to the GPU service before the
first OPEN. The firmware reservations are sized from the pinned layout
(102,400 code bytes, 20,480 data bytes); `pvr_fw_requirements` must produce
the same plan from the loaded file, or OPEN fails before any GPU access. A
reset or wake restages from the buffer and does not read the card again.
Images without a storage owner keep their linked copy through the same span
interface. At boot, the existing initial-exclusion check (local clock off,
reset asserted, bus gate off) must hold. If it does not, steps 3 and 4 above
run before any lease reaches the GPU, and a drain timeout marks the GPU
failed.

## Idle power-off and operating points

After 50 ms without work in flight or requests, the pinned driver's
autosuspend delay, the worker parks the GPU. It sends the forced-idle and
power-off requests, polls idle, asserts reset and releases domain 5. Firmware
memory, sessions, tables and leases stay. The next submit unparks: domain on,
clock set, then the existing start sequence against retained firmware memory,
as the pinned `pvr_power_fw_enable` does.

The pinned operating-point table gives 800 mV at 400 MHz and at 600 MHz in all
23 voltage entries, and 800 mV is the DCDC4 level the domain-5 row already
checks. Both rates come from fixed PERI0 outputs (mux values 3 and 2 in CCU
`0x0B20` bits 24–26), so neither needs a rail write or PLL change. The rate is
applied at unpark, after the worker writes it into `core_clock_speed` of the
firmware runtime configuration.
A change while powered is a park and an unpark. The default is 400 MHz until
the board qualifies 600 MHz; a thermal throttle selects 400 MHz at the next
unpark.

## Reused and removed mechanisms

- **Reused:** the KCCB and FWCCB transport, client rings, start, control and
  stop controllers, translation invalidation for session close, workload page
  tables and their publication, free lists, child disposal, the native
  backend's register allowlist, interrupt mask and rearm, GPU power and clock
  parent, firmware staging, `fatfs_port_load`, device-memory leases.
- **Removed:** STATUS polling for completion (STATUS stays for diagnostics);
  the 32-word and 64-pixel reply payloads; `input_copy` and its `input_state`
  hand-off; per-access `AT`; two deadline checks per register access; the tick
  poll while a job runs; mutual BUSY between compute and render; retention
  after a refusal.

## Performance budgets

Counts come from the QEMU FreeRTOS compositions
`checks/pvr-gpu-service.{c,py}` and `checks/pvr-compute-service.{c,py}`
(queue receives, worker loop passes, timed-out waits, `memcpy` bytes, and
calls to `aarch64_device_translate` and `aarch64_device_cache` counted with
`--wrap`), the Unicorn backend model `checks/a733-gpu-backend-emulate.py`,
`checks/pvr-gpu-vm.c`, `checks/device-memory-capacity.c` and
`checks/gpu-software-failure.py`. `DSB SY` counts come from disassembly of the
compiled submission path. The first implementation commit records each
baseline with its inputs.

| Metric | Baseline | Target |
| --- | --- | --- |
| Jobs in flight: sessions × per session | 1 × 1 (counted) | 2 × 2, compute and render together |
| Client wake-ups per compute job | One per tick of execution plus 32 | 1 |
| Worker wake-ups per job beyond the interrupt | One per tick of execution | 1, the submit message |
| Worker timed wake-ups per second while a job runs | 1,000 | 2 (W = 500 ms); 250 until delivery is qualified |
| Ticks between client send and kick, worker idle | Not measured | 0, at most one context switch |
| Queue round trips to deliver a 3,968-byte result | 31 | 0 beyond the completion message |
| Copies per input byte after transport | 2 | 0 for EL1 clients, 1 for EL0 |
| Copies per result byte | At least 5 | 0 for EL1 clients, 1 for EL0 |
| `AT` per GPU register read | 5 | 0 |
| `AT` per KCCB submission | 4 (counted) | 0 |
| `AT` per ring write step or timeline read | 1 (counted) | 0 |
| `DSB SY` per KCCB submission | 11 (counted) | 3; 0 plus 2 `DMB OSHST` after qualification |
| Allocations per job | 0 | 0 |
| GPU MMU leaf entries for a 4 MiB buffer | 1,024 | 2 |
| Free extents after both sessions open and close | Not modeled | Identical to before |
| Time from last progress to reset start | None; faults retain | At most 1.5 s, or the job deadline |
| Recoveries per boot | 0 | 3 full resets |
| Idle time before domain 5 turns off | Never before CLOSE | 50 ms |
| Card reads and hashes of the firmware | Linked into the image | One each per boot; none per reset or wake |

## Failure classes

| Failure | Class | Retained |
| --- | --- | --- |
| Bad session, slot, length or state; queue full; reset in progress | 1, refused before access | Nothing |
| CONTEXT_RESET or GPU page fault in one session | 2, firmware reported recovery | That session's slots until it closes |
| Hang with drain shown; firmware hash or plan mismatch | 2 | Nothing after reset or refusal |
| Drain timeout; reset readback mismatch; domain 5 not off; missing cleanup return at close | 3, unknown | All GPU leases, firmware memory, tables, clock, DCDC4 token, domain 5 |

## Left out, with reasons

- **800 MHz and 1,008 MHz.** They need DCDC4 above 800 mV for some speed bins
  (up to 840 mV and 960 mV), a speed-bin read and rail writes, which the power
  owner does not provide.
- **Dependencies between sessions,** such as rendering from a compute result.
  No consumer needs them.
- **More than two sessions, priorities and preemption.** There are two
  consumers.
- **Load-based frequency changes.** A change costs a park and an unpark.
- **Multi-page compute and new render features.** Other packages own them.

## Changes outside this package

- `apps/accelerators/gpu.c` and the render applications move to completion
  messages; the removed reply payloads go when they do.
- A shared portable SHA-256 helper, extracted from
  `drivers/npu/npu_convolution.c`.
- The power owner's domain-5 release and DCDC4 token, and the memory
  architecture's `gpu` class with 2 MiB-aligned leases.
- No reserved CPU or scheduler code changes.

## Board evidence that would confirm the design

- SPI 63 delivered for every completed job over a run of at least 1,000 jobs;
  this sets W to 500 ms.
- Compute and render completing together with correct outputs and no GPU page
  fault, and an injected out-of-session address faulting only its session.
- SIDEKICK_IDLE, SLC_IDLE and BIF_STATUS_MMU reaching idle after an injected
  shader hang, followed by a successful restart and job.
- Domain 5 off and on through park and unpark, with the firmware booting each
  time, and a correct job at 600 MHz.
- The firmware loaded from the card matching the pinned hash on a cold boot.
