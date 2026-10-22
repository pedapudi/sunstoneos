# Design: concurrent GPU sessions, recovery and power management

Status: **partly implemented**. Worker pacing (the "Interrupts and worker
pacing" section) is implemented and host-modeled, including the power
controller's back-to-back steps; [gpu-service.md](../gpu-service.md) records its
measurements. The other sections remain design. Each item ends at implemented
and host-modeled; physical behavior stays pending until the board evidence at
the end exists. Firmware events and failure classes are in
[gpu-recovery.md](../gpu-recovery.md).

Today the GPU service runs one job at a time, polls STATUS every tick and
retains the device after any fault. This design adds two isolated sessions
with jobs in flight together, interrupt completion, buffer hand-off by
ownership state, a reset bounded by a job deadline, firmware loaded from the
FAT volume and idle power-off. It depends on the unmerged memory architecture
design (`docs/designs/memory-architecture.md`) for
`device_memory_set_state()`, the holder rule and the 2 MiB-aligned `gpu`
class.

## Data flow and ownership

The service worker alone touches GPU registers, firmware queues and client
rings, so no register sequence needs a cross-core lock. It is the only lease
holder: it alone calls `device_memory_set_state()` and maintains cache lines.

- **Sessions** are fixed by the image configuration. The display renderer's
  session has one render job in flight; the mimus session has one compute
  context and two job slots. A static assertion allows one render session,
  because the free-list owner holds one set of lists. Each session has its
  own GPU page catalogue, a firmware timeline word and a job deadline
  `max_job_us` (100 ms for display; for mimus, set from its largest measured
  layer) that applies to every job. Every buffer is a lease acquired at open.
- **Slots** are CPU (lent to the client) or DEVICE. The submit message orders
  the client's writes before the worker's clean; the completion message
  orders the worker's invalidate before the client's reads.
- **Submit** is a 32-byte message: session, slot, written input bytes, a flag
  for an output range written while CPU, sequence. The worker checks it,
  cleans, writes the ring, submits one KICK and replies ACCEPTED or a refusal.
- **Completion** is one message per job: sequence, result, error. OK or RESET
  returns the slot to CPU; LOST leaves it DEVICE for the boot. The reply
  loses `compute_words[32]` and `render_pixels[64]`; EL1 clients read output
  in place.
- **EL0 clients** register buffers at open, because device pools have no EL0
  mapping, and the worker copies each buffer with one `memcpy`. A compute
  buffer holds at most 64 pages (the `pvr_compute_service.c` limit); at an
  assumed 1 GB/s, 256 KiB delays the render session by about 0.26 ms, 1.6% of
  a 60 Hz frame.
- **Display hand-off.** After completion, a device-only render target (class
  (c) below) in a scanout format passes to the display owner as a physical
  span, with no maintenance or copy, and its lease enters a DISPLAY state.
  The display owner returns it after the next frame latches at vblank. While
  it is DISPLAY, the worker refuses to render into it, close and reset leave
  it out of PENDING_RELEASE, and release waits for both an acknowledged stop
  and the return.
- **Close** is refused while the session has jobs in flight. It cleans up the
  session's firmware contexts (retrying CLEANUP_BUSY up to the close
  deadline), invalidates its translations with the existing MMU cache command
  and marks its leases PENDING_RELEASE. They are released when a later park
  or reset reaches STOPPED through the acknowledged handshake.

## Interrupts and worker pacing

The GPU interrupt (SPI 63, INTID 95) is routed to the worker's core. The
existing handler masks it and queues one coalesced wake. The worker drains
the firmware-to-host queue (FWCCB), reads each busy session's timeline,
completes advanced jobs and rearms.

The worker has one wait. It blocks until the earliest job deadline, a request
or interrupt wake, or a 4 ms bound while a job is in flight. With nothing in
flight it blocks 50 ms and then parks; parked, it blocks without a timeout.
The 4 ms bound covers a lost interrupt until SPI 63 delivery is qualified, and
completes jobs in an image without `platform/a733-smp-cached/irq_table.c`. The
per-tick mode and the `!config.irq.mask` branch of `timed()` in
`pvr_gpu_service.c` are removed.

Start, stop and job preparation are paced by the hardware:

- A 3 µs `DELAY` step in `pvr_gpu_start.c` waits on the counter inside the
  step; register writes and fences run back to back.
- A register `POLL` in `pvr_gpu_stop.c`, the readiness-byte read and a
  firmware acknowledgement re-read for up to 50 µs, then return AGAIN; the
  worker then waits one tick, as section 1.3 of performance-architecture.md
  allows for a hardware wait of a tick or more.
- The worker repeats progressing steps for up to 200 µs per burst (the USB0
  bound) in place of 16 steps. With CPU work left, it checks the request queue
  without waiting and calls `taskYIELD()`.

## Memory placement and cache policy

Each session range is classed at open by CPU access:

| Range | Placement | CPU maintenance |
| --- | --- | --- |
| (a) CPU-written input: vertices, indices, texture uploads, compute input | `gpu` class, write-back | Clean the written bytes at submit |
| (b) CPU-read output: compute results, evidence readback | `gpu` class, write-back | Invalidate the bytes the client reads at completion; clean and invalidate at submit only when the submit flag says the client wrote it |
| (c) Device-only: depth and stencil, parameter buffers, a scanout render target | `gpu` class, write-back | Clean and invalidate once at session open |
| Firmware queues, rings, timelines, runtime configuration, system-init page | `uncached` pool (attribute `0x44`) | None; barriers only |
| Firmware code, data, MIPS page table (buffers 0–2, 139,264 bytes) | write-back (`0xff`) | Invalidate at each stop; clean and invalidate at start only after a CPU write |
| GPU page tables | write-back | Cleaned once after construction |

Class (b) assumes the firmware advances a timeline only after the job's
writes, including system-level-cache contents, reach the point of coherency.
The pinned board driver documents per-job device cache flushing and orders
each task before its timeline UPDATE
([gpu-compiled-programs.md](../gpu-compiled-programs.md) cites both).

**Compute output guard.** Each compute job today fills its output region with
guard and poison words, then cleans and invalidates the output page. Only the
fixed evidence programs compare results with known values, so only they
detect a job that wrote no output. The fill becomes a session option set by
the evidence images (`apps/gpu-finite`, `apps/gpu-program`, the accelerator
evidence mode), where the result check in `pvr_compute_job.c` still runs.
Production sessions such as mimus skip it; their validation image compares
output with a reference.

**GPU MMU isolation.** Each session's catalogue maps only its own leases, with
2 MiB GPU pages where the aligned page lies inside the lease
([gpu-vm.md](../gpu-vm.md)). Admission checks each span once
(`device_memory_describe`, `a733_extent_within`). The per-access `AT` in
`mapped()`, `output_span()`, `device_page()` and `io_span()` is redundant
because an admitted span's CPU mapping changes only through
`device_memory_set_state()`, which only the worker calls and which reruns the
mapping check for that lease; the platform pins every other mapping. That
removal waits for the memory architecture design. Each free-list grow or
reconstruction batch gets one `a733_extent_within` against the render
session's pool. A new host check, `checks/gpu-session-isolation.c`, asserts
each catalogue entry, free-list entry and command address against its
session.

## Synchronization across cores

- **Requests** use FreeRTOS queues. A client waits for each reply before its
  next request and has at most two jobs in flight, so the worker owes it at
  most two completions and one reply; each reply queue holds three. A post
  that still fails makes that session LOST.
- **Single writer.** A client writes only bytes of its CPU slots; the worker
  writes everything else.
- **Placement.** The worker runs on the A55 core named in the INTID 95 route,
  so the handler wakes it without an inter-processor interrupt.

## Hang detection and bounded reset

A missed `max_job_us` detects a hang within `max_job_us` plus 4 ms. The pinned
driver's periodic KCCB-progress watchdog is dropped; the start-up
HEALTH_CHECK stays. Event 105 (CONTEXT_RESET) starts a full reset.

A full reset closes admission and runs `pvr_gpu_stop` from its first step:
forced idle (state 2) and power off (state 0) through the firmware handshake,
retire, the drain list of `pvr_gpu_stop_init`, reset with readback, reclaim
and domain 5 off. These acknowledgements are the same evidence as a park, so
a missed deadline followed by them is class 2 (section 1.8 of
performance-architecture.md). The comments in `include/pvr_gpu_stop.h`,
`include/a733_gpu_memory.h` and `docs/gpu-isolation.md` change to say so.

- **STOPPED reached.** The worker invalidates the failed jobs' class (b)
  ranges, posts RESET completions and releases PENDING_RELEASE leases. It
  restages firmware data, rebuilds the render session's free-list, HWRT-data
  and render-target objects, reformats the rings, recreates contexts and
  starts.
- **Handshake denied, a step timed out, or no firmware answer.**
  `pvr_gpu_stop` latches FAILED. The worker masks SPI 63, makes no further GPU
  or power I/O, posts LOST completions and keeps domain 5, the clock, the
  DCDC4 token, every lease, firmware memory and page table (class 3).

Three acknowledged resets are allowed per boot; after a fourth deadline miss
the GPU stays off.

## Idle power-off

**Park.** After 50 ms with nothing in flight (the pinned driver's autosuspend
delay), the worker masks SPI 63 and runs `pvr_gpu_stop`; its isolate callback
reclaims buffers 0–2. Sessions, page tables and leases stay.

**Unpark** powers domain 5 on, enables the clock and runs the existing start
sequence. Its first step, `a733_gpu_memory_publish`, runs only in the OFFLINE
phase before reset is released and ends with `DSB SY`. Before that barrier it
now clears the readiness byte (system-init offset 352, uncached), so every
start waits for a fresh boot; the pinned driver never clears it after the
first boot. The publish cleans and invalidates buffers 0–2 only when the CPU
wrote them since the last reclaim (first start, restage, reset). Otherwise
the CPU holds at most clean lines of them, and the next reclaim invalidates
those before any CPU read. `CHECK_CLEAR` still checks that nothing set the
byte before boot.

The unpark target, power-on to first kick, is 5 ms; the park delay is
max(50 ms, 10 × measured unpark time). A 60 Hz display never parks.

**Rate.** `A733_GPU_INITIAL_HZ` (400 MHz) stays an image constant. The pinned
table gives 800 mV for 400 and 600 MHz, both from fixed PERI0 outputs. 600 MHz
becomes a configuration value, with a thermal fallback to 400 MHz, after its
board item below.

## Firmware from the FAT volume

The storage owner loads the firmware with `fatfs_port_load` into a static
131,072-byte buffer and compares `storage_digest_bytes()` with the pinned
hash. On a match, service initialization decodes the plan once with
`pvr_fw_requirements`; reservation, MIPS setup, start and stop take it. The
writable buffer is re-hashed at each reset: 4,593,001 AArch64 instructions
(Unicorn count with the image's flags), about 2.6 ms at 1.8 GHz. That hash is
removed when the page-table owner maps the buffer read-only after admission;
nothing else writes it and the GPU cannot reach it. `ID_AA64ISAR0_EL1` is not
yet recorded ([handoff.md](../handoff.md)); if its SHA2 field is set, SHA-256
instructions are a follow-up for the storage owner. A failed boot-time
exclusion check refuses the GPU ([remote-writers.md](../remote-writers.md)).

## Performance budgets

Counts come from `checks/pvr-gpu-service.{c,py}`,
`checks/pvr-compute-service.{c,py}`, `checks/a733-gpu-backend-emulate.py`,
`checks/pvr-gpu-vm.c`, `checks/device-memory-capacity.c` and disassembly. The
change adds counters to the first two: worker ticks and wake-ups per phase,
timed waits, `memcpy` bytes, and `--wrap` counters on `aarch64_device_cache`
(lines = bytes / 64) and `aarch64_device_translate`. Baselines marked "code"
are counted from the step lists and cache calls; the counters confirm them
before the change.

| Metric | Baseline | Target |
| --- | --- | --- |
| Jobs in flight | 1 | Render 1 + compute 2 |
| Client wake-ups per compute job | 1 per tick + 32 | 2 |
| Worker timed wake-ups per second, job running | 1,000 | 250 without qualified SPI 63; 0 after |
| Worker ticks, unpark to first kick | Code: at least 4 (two delays, one burst limit in 27–35 steps, one boot read) + 1 per further boot read | 0 when every wait ends within 50 µs; 1 wake-up |
| Worker ticks, park to STOPPED | Code: at least 2 (two acknowledgements) + 1 per unsatisfied poll | 0 when every wait ends within 50 µs; 1 wake-up |
| Worker ticks, full reset | No reset exists | Park + unpark; 0 for CPU rebuild |
| Ticks from send to kick | Code: 0 within 16 steps; +1 per further 16 render steps | 0; at most one context switch |
| Unpark wall clock | Not measured | 5 ms (board) |
| `DC` lines per park and unpark | 2,176 + 2,176 | 2,176 + 0 |
| Queue round trips, 3,968-byte result | 31 | 0 beyond completion |
| Copies per input byte; per result byte | 2; at least 5 | 0 for EL1 clients, 1 for EL0 |
| Copies per rendered frame byte | 2 (slot copy, RGB565) | 0 for scanout |
| `DC` lines per display frame, 720p | 329,152 (profile 2); 591,552 (profile 4) | Written input bytes / 64 |
| Compute job cache work | Code: 132 lines (fixed program) or 192 (package); 256- or 4,096-byte fill; 2 extra `DSB SY` | Written input / 64 + read output / 64; no fill; no extra `DSB SY` |
| `DSB SY` per submit, one input range | 16 transport + 2 per cache call | 5; 4 with the new cache entry point; 2 after qualification |
| `DSB SY` per FWCCB drain, n events | 11n + 5 | 2, then 2 `DMB OSHLD` |
| `AT` per register read | 5 | 0 |
| GPU MMU leaf entries, 4 MiB buffer | 1,024 | 2 |
| Free extents after both sessions open and close | Not modeled | Identical to before |
| Hang detection | None | `max_job_us` + 4 ms |
| Firmware header decodes | 1 per call at 5 sites | 1 per boot |
| Firmware hashes | 0 (linked) | 1 per boot + 1 per reset; 1 per boot after read-only mapping |

The compute baseline is 64 lines for the input page, 64 for the filled output
page and 4 or 64 for the output read. Submit barriers: 2 `DSB SY` in the cache
call plus 3 on the transport (before the write-offset store, before and after
the doorbell). A cache entry point without the trailing `DSB SY` lets the
write-offset barrier complete the maintenance; its leading barrier stays,
because the client wrote the bytes on another core. After the board
qualification in [shared-helpers.md](../shared-helpers.md), a `DMB OSHST`
replaces the doorbell barriers.

New host cases: resubmission after RESET with correct output; refusal after
LOST; PENDING_RELEASE until an acknowledged stop; close and reset while a
target is DISPLAY; a full reply queue making only its session LOST; an
injected cross-session span; completion through the 4 ms bound without
interrupt callbacks.

## Left out

- **Containment after a latched failure.** Domain-off or reset during an
  unacknowledged hang would run I/O on a latched controller, and nothing shows
  the interconnect survives outstanding AXI transactions. It stays out until
  board evidence shows it completes within a deadline, and then becomes a
  separate operation that releases no lease.
- **Drain-only recovery without a firmware acknowledgement.** It needs the
  platform isolation that [gpu-power.md](../gpu-power.md) requires; the GPU
  memory-bus gate (CCU `0x5e0[16]`) is a candidate.
- **Per-session CONTEXT_RESET recovery and per-job deadlines.** The pinned
  driver does not rely on the first; neither consumer needs the second.
- **800 and 1,008 MHz.** Some speed bins need DCDC4 above 800 mV.
- **Load-based rates, priorities, preemption, more than two sessions,
  adopting a GPU found running at boot.** No consumer needs them, and a
  running GPU's page tables are unknown.

## Changes outside this package

- Primary agent: an `aarch64_device_cache` entry point without the trailing
  `DSB SY`, leaving the existing helper unchanged; `irq_table.c` linked into
  every GPU image (`a733-accelerators`, `a733-gpu-application`, the
  `gpu.cmake` images), so the 4 ms bound is only a fallback; a read-only
  mapping for the verified firmware buffer.
- Memory architecture design: `device_memory_set_state()` reruns the mapping
  check for the lease it changes.
- `drivers/gpu/pvr_mips.c` takes the decoded plan.
- Graphics and display packages: range classes in `pvr_render_service.c`, no
  per-frame depth fill, scanout without the RGB565 copy, the return of a
  DISPLAY target after vblank.
- `apps/accelerators/gpu.c` and the render applications use completion
  messages.

## Board evidence that would confirm the design

- SPI 63 delivered for every job over at least 1,000 jobs, with `DMB`
  transport barriers and no lost or stale completion; this removes the 4 ms
  bound.
- Over that run, every output matching its reference when read right after
  the timeline advances.
- Both sessions completing together with correct output. A page fault in one
  session leaves the other session's memory unchanged, and that session's
  job completes with RESET and then succeeds on resubmission.
- The readiness byte set after each warm boot, and unpark time and fallback
  ticks over 100 park-unpark cycles.
- An injected hang followed by an acknowledged stop, restart and correct job;
  a denied handshake ending in FAILED with no further I/O.
- Card-loaded firmware matching the pinned hash on a cold boot.
- At 600 MHz: correct output on both sessions, temperature under sustained
  load and the fallback to 400 MHz.
