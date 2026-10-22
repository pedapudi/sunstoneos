# Design: concurrent GPU sessions, recovery and power management

Status: **design**; no code exists yet. Each item ends at implemented and
host-modeled; physical behavior stays pending until the board evidence at the
end exists. Firmware event handling and the failure classes are in
[gpu-recovery.md](../gpu-recovery.md).

Today the GPU service runs one session and one job at a time, learns
completion by a STATUS poll every tick, returns results 32 words per reply and
retains the device after any fault. This design adds two isolated sessions
with jobs in flight together, interrupt completion, buffer hand-off by
ownership state, a reset bounded by a job deadline, firmware loaded from the
FAT volume and idle power-off.

Prerequisite: the memory architecture design
(`docs/designs/memory-architecture.md`, not yet merged) supplies
`device_memory_set_state()`, the holder rule and the 2 MiB-aligned `gpu`
class. Until it merges, the GPU MMU leaf-entry target cannot be measured.

## Data flow and ownership

The service worker alone touches GPU registers, firmware queues and client
rings, so no register sequence needs a cross-core lock. It is the only lease
holder: it alone calls `device_memory_set_state()` and maintains cache lines.

- **Sessions.** Fixed by the image configuration. The display renderer's
  session submits render jobs, one in flight. The mimus session has one
  compute context and two job slots. A static assertion allows at most one
  render session, because the free-list owner holds one set of lists. Each
  session has its own GPU page catalogue, a firmware timeline word and a
  maximum job deadline `max_job_us` (100 ms for the display session; the mimus
  entry is set from its largest measured layer). Every buffer is a lease
  acquired at session open; no allocation happens per job.
- **Slot states.** A slot is CPU (lent to the client) or DEVICE. The submit
  message orders the client's writes before the worker's clean; the completion
  message orders the worker's invalidate before the client's reads.
- **Submit.** A 32-byte message (session, slot, written input bytes, a flag for
  an output range written while CPU, deadline, sequence). The worker checks the
  per-request fields, refuses a deadline above `max_job_us` (class 1), cleans,
  writes the ring and submits one KICK. Every submit gets an ACCEPTED or
  refusal reply.
- **Completion.** One message per job: sequence, result, error. Result OK or
  RESET returns the slot to CPU; LOST leaves it DEVICE for the rest of the
  boot (see the reset section). The reply loses `compute_words[32]` and
  `render_pixels[64]`; an EL1 client reads output in place.
- **EL0 clients** register buffers at open, because device pools have no EL0
  mapping. The worker copies them with `memcpy` in chunks of at most 16 KiB
  and serves the other session between chunks.
- **Close.** Close is refused while the session has jobs in flight. It cleans
  up the session's firmware contexts (retrying CLEANUP_BUSY once per tick up to
  the close deadline), invalidates its translations through the existing MMU
  cache command, and marks its leases PENDING_RELEASE. They are released only
  when a later park or reset reaches STOPPED through the acknowledged
  handshake. Close never runs `pvr_gpu_stop` while another session has work;
  with that session idle, park follows within 50 ms.

## Interrupt and polling

The GPU interrupt (SPI 63, INTID 95) is routed to the worker's core. The
existing handler masks it and queues one coalesced wake. The worker then
drains the firmware-to-host queue (FWCCB), reads each busy session's timeline,
completes advanced jobs and rearms, as the pinned driver's
`pvr_device_process_active_queues` does. The worker blocks until the
earliest job deadline; until board evidence shows SPI 63 delivered for every
job, it also wakes every 4 ms, so a lost interrupt costs at most 4 ms. With
nothing in flight it blocks 50 ms and then parks; parked, it blocks without a
timeout. Images without interrupt callbacks keep the per-tick poll.

## Memory placement and cache policy

Each session range is classed at open by CPU access:

| Range | Placement | CPU maintenance |
| --- | --- | --- |
| (a) CPU-written input: vertices, indices, texture uploads, compute input | `gpu` class, write-back | Clean the written bytes at submit |
| (b) CPU-read output: compute results, evidence readback | `gpu` class, write-back | Invalidate the bytes the client will read at completion; clean and invalidate at submit only when the submit flag says the client wrote it |
| (c) Device-only: depth and stencil, parameter buffers, a scanout render target | `gpu` class, write-back | Clean and invalidate once at session open; never per job |
| Firmware queues, rings, timelines, runtime configuration, system-init page | `uncached` pool (attribute `0x44`) | None; barriers only |
| Firmware code, data, MIPS page table (buffers 0–2, 139,264 bytes) | write-back (attribute `0xff`) | Clean and invalidate at each start (`a733_gpu_memory_publish`), invalidate at each stop (`a733_gpu_memory_reclaim`) |
| GPU page tables | write-back | Cleaned once after construction |

CPU clears of render targets are not supported; the background object clears
color, depth and stencil, and depth is stored without a load. Counted from
`pvr_render_service.c` (graphics package) at 1280×720, profile 2 maintains
329,152 lines per frame: 213,888 preparing output, texture and geometry pages,
57,664 invalidating output and 57,600 cleaning the copy. Profile 4 also fills
a 16 MiB depth attachment with 1.0 on the CPU every frame, giving 591,552 lines
(1,069,696 at 1920×1080). The classes above remove this cost once the graphics
package adopts them and drops the depth fill.

**Display hand-off.** After the completion message, a class (c) render
target in a format the display engine scans out passes to the display owner as
a physical span, with no CPU maintenance or copy. Today each frame is copied
into an output slot and converted from RGBA8 to RGB565 on the CPU
([gpu-display.md](../gpu-display.md)); the graphics package owns that path.

**GPU MMU isolation.** Each session's catalogue maps only its own leases,
with 2 MiB GPU pages where the aligned page lies inside the lease
([gpu-vm.md](../gpu-vm.md) allows one page size per 2 MiB region). Admission
checks each span once (`device_memory_describe`, `a733_extent_within`), so the
per-access `AT` in `mapped()`, `output_span()`, `device_page()` and `io_span()`
is removed. The free-list pool is carved only from the render session's
leases, and each grow or reconstruction batch gets one `a733_extent_within`
against it. The new host check `checks/gpu-session-isolation.c` asserts every
catalogue entry, free-list entry and firmware address in each command against
its own session, and refuses an injected cross-session span.

## Synchronization across cores

- **Requests** use FreeRTOS queues. A client waits for each reply before its
  next request and has at most two jobs in flight, so the worker owes it at
  most three messages: two completions (OK, RESET or LOST, one per job) and
  one request reply. Each reply queue holds three. A post that still fails
  means the client broke the protocol; that session becomes LOST.
- **Single writer.** The worker writes slot states, session records and every
  device structure; a client writes only bytes of a CPU slot. No field has two
  writers, so session records need no cache-line alignment.
- **Placement.** The worker runs on the A55 core named in the INTID 95 route,
  so the handler wakes it without an inter-processor interrupt.

## Hang detection and bounded reset

Each job has a finite deadline no larger than its session's `max_job_us`. A
missed deadline is the hang detector, so detection takes at most
`max_job_us` plus 4 ms. Only the pinned driver's periodic KCCB-progress
watchdog is dropped; the start-up HEALTH_CHECK stays in start, unpark and
reset. CONTEXT_RESET (event 105) triggers a full reset in this version; a
per-session recovery path waits for board evidence
([gpu-recovery.md](../gpu-recovery.md)).

A full reset closes admission and runs `pvr_gpu_stop` from its first step:
forced idle and power off through the firmware handshake, retire, the register
drain list of `pvr_gpu_stop_init` (the single source for it), reset with
readback, reclaim and domain 5 off. Then:

- **STOPPED reached.** The orderly-stop contract of
  [pvr_gpu_stop.h](../../include/pvr_gpu_stop.h) holds. The worker invalidates
  the class (b) ranges of the failed jobs, posts RESET completions (slots
  return to CPU) and releases PENDING_RELEASE leases. It re-hashes the
  firmware buffer, restages firmware data, rebuilds the render session's
  free-list, HWRT-data and render-target objects with their parameter-manager
  state (one extent check per free-list batch), reformats the rings, recreates
  contexts, clears the readiness byte and starts.
- **Handshake denied, a step times out, or the firmware does not answer.**
  `pvr_gpu_stop` latches FAILED as today. The worker asserts reset and asks
  the power owner for domain 5 off as containment only, posts LOST
  completions, and retains every GPU lease, firmware memory and page table for
  the boot (class 3). No drain is claimed, so the contracts of
  `pvr_gpu_stop.h` and [gpu-power.md](../gpu-power.md) are unchanged.

Three acknowledged resets are allowed per boot. After a fourth deadline miss
the GPU stays off; leases are released only if that stop reached STOPPED.

## Idle power-off

**Park.** After 50 ms with nothing in flight (the pinned driver's autosuspend
delay), the worker masks SPI 63 and runs `pvr_gpu_stop` from its first step.
The stop's isolate callback reclaims firmware buffers 0–2. Sessions, page
tables and leases stay. Register diagnostics are refused while parked.

**Unpark.** While domain 5 is off and reset is asserted, the worker clears the
readiness byte (system-init offset 352, uncached) with a store and `DSB SY`,
through a new `a733_gpu_memory` operation valid only in the OFFLINE phase.
It then powers the domain on, enables the clock and runs the existing start
sequence: publish (clean and invalidate buffers 0–2), the stale-flag check,
wrapper writes and the boot wait. The pinned driver never clears this byte
after its first boot, so its resume wait proves nothing; clearing it lets the
boot wait observe a fresh boot. Unpark never restages firmware data.

The target unpark time, power-on to first kick, is 5 ms, so that the 50 ms
delay equals ten unpark times. No board record measures it yet; the pinned
driver gives only a 5 s boot failure bound. The park delay is max(50 ms,
10 × measured unpark time), so the GPU parks only after an idle gap of ten
unpark times. A 60 Hz display never parks.

**Rate.** `A733_GPU_INITIAL_HZ` (400 MHz) stays an image constant written into
`core_clock_speed` of the runtime configuration when firmware data is staged.
The pinned table gives 800 mV for both 400 and 600 MHz, both from fixed PERI0
outputs, so neither needs a rail write. 600 MHz becomes a configuration value
once qualified; a thermal hook to 400 MHz comes with that change.

## Firmware from the FAT volume

The storage owner loads the firmware with `fatfs_port_load` into a static
131,072-byte buffer, hashes it with `storage_digest_bytes()` and compares the
result with the pinned value. On a match, service initialization calls
`pvr_fw_requirements` once and stores the plan in the service context;
reservation, MIPS setup, start and stop take the plan. `checks/pvr-firmware.c`
already covers the decoder against the pinned file. Because the buffer is
writable, each reset re-hashes it. The hash costs 4,593,001 AArch64
instructions (Unicorn count of `storage_digest_bytes` built with the image's
flags), about 2.6 ms at one instruction per cycle at 1.8 GHz. If the boot-time
exclusion check fails, the GPU is refused and reported as an unexcluded remote
writer ([remote-writers.md](../remote-writers.md)).

## Performance budgets

Counts come from the QEMU compositions `checks/pvr-gpu-service.{c,py}` and
`checks/pvr-compute-service.{c,py}` (queue receives, worker passes, timed
waits, `memcpy` bytes, `--wrap` counters on `aarch64_device_translate` and
`aarch64_device_cache`), `checks/a733-gpu-backend-emulate.py`,
`checks/pvr-gpu-vm.c`, `checks/device-memory-capacity.c` and disassembly.
Display baselines are counted from `pvr_render_service.c`.

| Metric | Baseline | Target | Mechanism |
| --- | --- | --- | --- |
| Jobs in flight | 1 | Render 1 + compute 2 | Removes mutual BUSY |
| Client wake-ups per compute job | 1 per tick + 32 | 2 | Completion message replaces STATUS polls |
| Worker timed wake-ups per second, job running | 1,000 | 0; 250 until SPI 63 is qualified | Interrupt, deadline wait |
| Ticks from send to kick, powered | Not measured | 0, at most one context switch | Existing request queue |
| Unpark, power-on to first kick | Not measured | 5 ms; 1 worker wake-up | Existing start sequence |
| `DC` lines per stop and start (firmware buffers 0–2) | 2,176 + 2,176 (counted) | Unchanged | Existing reclaim and publish |
| Queue round trips, 3,968-byte result | 31 | 0 beyond completion | Removes reply payloads |
| Copies per input byte; per result byte | 2; at least 5 | 0 for EL1 clients, 1 for EL0 | Removes `input_copy` |
| Copies per rendered frame byte | 2 (slot copy, RGB565 conversion) | 0 for scanout | Class (c) hand-off; graphics package |
| `DC` lines per display frame, 720p | 329,152 (profile 2); 591,552 (profile 4) | 0 for class (c); written input bytes/64 | Range classes |
| `DC` lines per compute job | Pending | Input bytes/64 + read output bytes/64 | Range classes |
| `DSB SY` per submit, one input range | 16 transport + 2 per cache call | 5 interim; 3 after the helper change; 1 after qualification | See below |
| `DSB SY` per FWCCB drain, n events | 11n + 5 | 2, then 2 `DMB OSHLD` | Cursors kept by host |
| `AT` per register read | 5 | 0 | Admission check |
| Allocations per job | 0 | 0 | Leases at open |
| GPU MMU leaf entries, 4 MiB buffer | 1,024 | 2 | 2 MiB pages (prerequisite) |
| Free extents after both sessions open and close | Not modeled | Identical to before | PENDING_RELEASE |
| Hang detection | None | `max_job_us` + 4 ms | Deadline |
| Firmware header decodes | 1 per call at 5 sites | 1 per boot | Plan in service context |
| Firmware hashes | 0 (linked) | 1 per boot + 1 per reset | `storage_digest_bytes` |

Submit barriers: the interim count is 2 `DSB SY` inside the one
`aarch64_device_cache` call plus 3 on the transport (before the write-offset
store, before the doorbell, after the doorbell). Once the primary agent drops
the leading `DSB SY` in `aarch64_device_cache` and offers a form without the
trailing one, the write-offset `DSB SY` also completes the maintenance,
leaving 3. After the board qualification of
[shared-helpers.md](../shared-helpers.md), a `DMB OSHST` replaces the barrier
before the doorbell and the one after it is dropped, leaving 1 `DSB SY`.

New host cases:

- a client acts on a RESET completion and resubmits with correct output;
- a client that received LOST is refused further submits;
- a closed session's leases stay PENDING_RELEASE until an acknowledged stop;
- a full reply queue makes only its own session LOST;
- an injected cross-session span is refused.

## Left out

- **Drain-only recovery when the firmware does not acknowledge.** It needs a
  platform isolation mechanism that [gpu-power.md](../gpu-power.md) requires
  and no code provides. A candidate is the GPU memory-bus gate (CCU
  `0x5e0[16]`); qualifying it needs board evidence that GPU-reachable sentinel
  pages stay unchanged after an unacknowledged hang, gate and reset.
- **Per-session CONTEXT_RESET recovery.** The pinned driver never relies on
  it.
- **800 and 1,008 MHz.** They need DCDC4 above 800 mV for some speed bins.
- **Load-based rate changes, priorities, preemption, more than two
  sessions.** Neither consumer needs them.
- **Adopting a GPU found running at boot.** Its page tables are unknown.

## Changes outside this package

- Primary agent: drop the leading `DSB SY` in `aarch64_device_cache` and add a
  form without the trailing one; link `platform/a733-smp-cached/irq_table.c`
  into the images that use interrupt completion.
- `drivers/gpu/pvr_mips.c` takes the decoded plan instead of decoding again.
- Graphics package: range classes in `pvr_render_service.c`, no per-frame
  depth fill, scanout of the render target without the RGB565 copy.
- `apps/accelerators/gpu.c` and the render applications move to completion
  messages.

## Board evidence that would confirm the design

- SPI 63 delivered for every job over at least 1,000 jobs (removes the 4 ms
  poll), with `DMB` transport barriers and no lost or stale completion.
- Both sessions completing together with correct output; an out-of-session
  address faulting only its session.
- The readiness byte set by the firmware after each warm boot, and the unpark
  time recorded in the park-unpark board record over 100 cycles.
- An injected hang followed by an acknowledged stop, restart and correct job;
  and a denied handshake ending in containment with memory retained.
- Card-loaded firmware matching the pinned hash on a cold boot.
