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
flight together, interrupt completion, ownership hand-off of job buffers, a
bounded reset after a missed job deadline, firmware loaded from the FAT volume,
idle power-off and two operating points.

## Data flow and ownership

The service worker stays the only task that touches GPU registers, the
firmware queues and the client command rings, so no register sequence needs a
cross-core lock.

- **Sessions are fixed by the image configuration.** Each entry names one job
  kind. The display renderer's session submits render jobs only, one in
  flight, as the graphics design allows; it has the render context pair, its
  two rings and the supporting compute context that the graphics resources
  already include ([gpu-render.md](../gpu-render.md), cleanup order), but no
  compute ring. The mimus compute dispatcher's session has one compute context,
  one ring and two job slots. A static assertion on the configuration table
  allows at most one render session, because the free-list and render-target
  owner (`a733_gpu_graphics_memory`) holds one set of lists. Each session also
  has its own GPU page catalogue and a timeline word that the firmware advances
  after each job. Every buffer is a device-memory lease acquired at session
  open; no allocation happens per job.
- **Leases and slot states.** The worker is the only lease holder under the
  holder rule of the memory architecture: it alone calls
  `device_memory_set_state()` and maintains cache lines. A slot's bytes are
  lent to the client while the slot is CPU. SUBMITTED (cleaned, ring written)
  and COMPLETE (output invalidated, completion posted) are states of the
  session record; the lease itself moves only between CPU and DEVICE. The
  submit message orders the client's writes before the worker's clean, and the
  completion message orders the worker's invalidate before the client's reads.
- **Submit.** A 32-byte message (session, slot, input and output lengths,
  deadline, sequence) on the existing request queue. The worker checks only
  the per-request fields, writes the ring and submits one KICK on the kernel
  command queue (KCCB). Every submit gets an ACCEPTED or refusal reply.
- **Completion.** One message per job on the client's reply queue: sequence,
  result and error. The reply loses `compute_words[32]` and `render_pixels[64]`.
  An EL1 client reads output in place.
- **EL0 clients.** Device pools have no EL0 mapping. An EL0 client registers
  its input and output buffers at session open, where they are checked once.
  The worker copies with `memcpy` in chunks of at most 16 KiB per step and
  serves pending completions and kicks of the other session between chunks, so
  render completion latency does not grow with compute payload size.

## Completion by interrupt

The GPU interrupt (SPI 63, INTID 95) is routed to the worker's core through
the image's IRQ table. The existing handler masks it and queues one coalesced
wake. On each wake the worker drains the firmware-to-host queue (FWCCB), reads
the timeline of each session with work in flight, completes advanced jobs and
rearms. The pinned driver detects completion the same way
(`pvr_device_irq_thread_handler` calls `pvr_device_process_active_queues`).

The worker's waits:

- jobs in flight: block until the earliest job deadline; until board evidence
  shows SPI 63 delivery for every job, also wake every 4 ms, so a lost
  interrupt costs at most 4 ms (the timeout path of section 1.3 of
  [performance-architecture.md](../performance-architecture.md));
- nothing in flight, GPU powered: one timed block of 50 ms, then park;
- parked: block without a timeout.

Images without interrupt callbacks keep polling once per tick.

**Transport cost per operation.** The KCCB and FWCCB control words, firmware
events and timelines live in Normal non-cacheable memory. `WRAP_MASK` and
`COMMAND_SIZE` are written by the host and read by no one after start, so
they are checked once at start and at unpark. The hot path reads only the
cursors the firmware writes, each range-checked against the host mask when
read (section 1.4 of the performance architecture). The host keeps its own
write and read cursors and does not re-read them. Events are copied with
`memcpy` from Normal non-cacheable memory instead of a volatile byte loop.
Mappings are verified at admission, so no path below issues `AT`.

| Operation | `DSB SY` now (counted) | Interim | After qualification |
| --- | --- | --- | --- |
| KCCB submission | 16 | 2: before the write-offset store, before the doorbell | 2 `DMB OSHST` |
| FWCCB drain, empty | 5 | 0; one load of the write offset | 0 |
| FWCCB drain, n events | 11n + 5 | 2: after the write-offset load, before the read-offset store | 2 `DMB OSHLD` |
| KCCB return poll, k slots pending | 6 + k | 1, after the return-word loads | 1 `DMB OSHLD` |
| Timeline read | Pending | 1 when advanced | 1 `DMB OSHLD` |

The interim column keeps `DSB SY` at every ordering point and removes only
duplicates between accesses that another `DSB SY` already orders. The `DMB`
forms, and dropping the `DSB SY` after the doorbell, are a weaker barrier and
carry the board qualification that [shared-helpers.md](../shared-helpers.md)
requires.

## Memory placement, cache policy and GPU MMU isolation

| Data | Placement | CPU cache policy |
| --- | --- | --- |
| Firmware queues, command rings, timelines, firmware data | `uncached` pool, as today | None; barriers as above |
| Job inputs, outputs, vertices, textures, render targets | `gpu` class (stage 2 of the memory architecture), write-back | At submit: clean the declared input ranges and clean and invalidate the declared output ranges. At completion: clean and invalidate the output ranges. 2 `DSB SY` per transition |
| GPU page tables | write-back, built at session open | Cleaned once after construction; never written while the session is open |
| Firmware image read from the FAT volume | EL1 memory, 131,072 bytes | Re-hashed before each restage |

A client may write an output range while its slot is CPU, for example to clear
a render target. The clean and invalidate at submit removes those dirty lines,
so no eviction can overwrite GPU output while the slot is DEVICE, as arm64
does for `DMA_FROM_DEVICE`. A range the GPU both reads and writes, such as a
blend or depth load, belongs to both sets and gets one clean and invalidate.

Each session's page catalogue maps only that session's leases. A 2 MiB or
64 KiB GPU page is used only where the whole physically aligned page lies
inside the lease; the remainder uses smaller pages, within the
[gpu-vm.md](../gpu-vm.md) rule of one page size per 2 MiB region. Admission
checks each span once against its lease (`device_memory_describe` and
`a733_extent_within`) and keeps the rule that no page-table or firmware page
appears in any span.

The GPU also reaches memory by physical address outside the catalogue. The
free-list tables hold page numbers that the parameter manager writes through,
and FREELIST_GROW and reconstruction add pages while a job runs. Every page
number written to a free-list table, at INSERT, grow or reconstruction, must
lie inside the render session's leases, checked when it is written. Firmware
addresses in render and compute commands must name that session's own
firmware objects. The new host check `checks/gpu-session-isolation.c` asserts
that every catalogue entry, every free-list entry after grow and
reconstruction, and every address in each encoded command lies inside its
session's leases or firmware objects. It also asserts that an injected
cross-session span or page number is refused.

CPU mappings do not change after preparation, so mappings are verified once
at admission or bind. The per-access `AT` translations are removed from
`mapped()` in `platform/a733/gpu_memory.c`, `output_span()` and the per-access
`device_page()` calls in `platform/a733/gpu_backend.c`, and `io_span()` in
`drivers/gpu/pvr_compute_service.c`; `device_pages()` still checks every
register page at bind. Bounds checks against the admitted span remain.
Register sequences check the deadline once per step instead of before and
after each access.

## Synchronization across cores

- **Requests.** FreeRTOS queues, which the SMP kernel lock serializes. A client
  waits for each reply before sending its next request, and has at most two
  jobs in flight. The worker can therefore owe a client at most three
  messages, and each reply queue holds three. Admission refuses a third job
  before accepting it. The worker posts without blocking; if a post still
  fails, which only a client that breaks the protocol can cause, that session
  fails and is retained. A host case fills a reply queue to show this.
- **Single writer.** The worker writes slot states, session records and all
  device structures. A client writes only the bytes of a slot in state CPU.
  No field is written by two cores, so session records need no cache-line
  alignment.
- **Interrupt.** The existing critical sections around mask and rearm stay;
  they run once per interrupt.
- **Placement.** The worker's affinity is the single core named by the INTID
  95 route, normally an A55 core, so the handler wakes it without an
  inter-processor interrupt; service initialization refuses a mismatch. mimus
  compute clients run on cores 6 and 7 once the multicore work admits them.

## Firmware events, hang detection and bounded reset

Each job carries a mandatory finite deadline (`timeout_us`). A missed job
deadline is the only hang detector, and the bound on hang detection is that
deadline. The pinned driver's KCCB-progress watchdog and HEALTH_CHECK command
are not used: the watchdog runs only while work is in flight, when a deadline
is always armed. Control commands keep their KCCB return deadlines.

| FWCCB event | Action |
| --- | --- |
| 103 grow, 104 reconstruct free lists | Existing handlers, with the page-number check above |
| 105 CONTEXT_RESET, reason GUILTY or INNOCENT, LOCKUP or OVERRUNNING, one context | Fail that session's in-flight jobs; the session is retained until it closes |
| 105 with FW_WATCHDOG, FW_PAGEFAULT or FW_EXEC_ERR, the all-contexts flag, or an unknown reason | Full reset |
| 109 REQUEST_GPU_RESTART, 112 CONTEXT_FW_PF_NOTIFICATION | Full reset (the pinned driver answers 109 with `pvr_power_reset`) |
| 101, 102 depth-buffer backing | Full reset; depth storage is static, so the firmware never sends them |
| 106 debug dump, 107 statistics, 108 clock rate | Recorded |
| Any other type | Full reset |

The per-session row relies on the firmware having recovered the hardware
before it reports the reset. The pinned driver does not handle CONTEXT_RESET,
so that is unproven firmware behavior until the board evidence below exists.
If the other session then misses a deadline, a full reset follows.

A full reset reuses `pvr_gpu_stop` and the existing start sequence:

1. Close admission and fail every in-flight job; slots stay DEVICE.
2. If the firmware answers, run `pvr_gpu_stop` from its first step: forced
   idle, power off, retire, then the register steps. Otherwise abort the
   control component and enter the same machine at `PVR_GPU_STOP_REGISTERS`.
3. **Drain shown** means that every poll of the register steps in
   `pvr_gpu_stop_init` (`drivers/gpu/pvr_gpu_stop.c`, the single source)
   passed: SIDEKICK_IDLE `0x3c8` before and after, BIFPM_STATUS_MMU `0x1358`,
   BIF_STATUS_MMU `0x1350`, BIF_READS_EXT_STATUS `0x1320` where the firmware
   feature bits require it, BIFPM_READS_EXT_STATUS `0x1338`, SLC_STATUS1
   `0x3870` = 0, SLC_IDLE `0x3898` = `0xff` and the GARTEN idle bit of
   `0x3c8`. The soft reset is then asserted with readback, and the power owner
   confirms domain 5 off after checking the CCU gate bits. A timeout on any
   step is class 3.
4. Re-hash the retained firmware buffer, restage the firmware data, reformat
   the rings, recreate the configured sessions' firmware contexts and run the
   start sequence. Page tables and leases stay.

The service allows three full resets per boot. A fourth leaves the GPU off and
closes both sessions; their leases are released only because a drain was
shown. Session close retries firmware CLEANUP_BUSY once per tick up to the
close deadline, because the firmware releases a context only after its
current job leaves the hardware, a wait of job length.

## Cold initialization and firmware from the FAT volume

The storage owner task loads the firmware with `fatfs_port_load` into a
static 131,072-byte buffer, hashes it once with `storage_digest_bytes()`
([a733_storage_digest.h](../../include/a733_storage_digest.h)), compares the
result with the pinned value compiled into the image and hands the buffer to
the GPU service before the first OPEN. A matching hash proves the file equals
the pinned image, so the firmware reservation plan (102,400 code bytes, 20,480
data bytes, its load records and device-information offsets) is a constant
generated from the pinned image at build time. A host check compares that
constant with `pvr_fw_requirements` on the pinned file. The five runtime
decodes of the firmware header, in service initialization, MIPS setup, start,
stop and reservation, use the constant instead. The buffer is ordinary
writable EL1 memory, so each reset re-hashes it before restaging, about 1–2 ms
at 1.8 GHz and at most three times per boot. Images without a storage owner
keep their linked copy through the same span interface and the same re-hash.

At boot the existing initial-exclusion check (local clock off, reset asserted,
bus gate off) must hold. If it does not, the GPU is refused as
[gpu-power.md](../gpu-power.md) states, and the image reports it as an
unexcluded remote writer under [remote-writers.md](../remote-writers.md):
memory state is unknown image-wide, and no lease reaches the GPU.

## Idle power-off and operating points

**Park.** After 50 ms with nothing in flight and no request, the pinned
driver's autosuspend delay, the worker masks SPI 63 and runs `pvr_gpu_stop`
from its first step, with the full drain list, ending with domain 5 off.
Firmware memory, sessions, page tables and leases stay, and the firmware
buffers keep their GPU-side state: park invalidates nothing, and unpark
cleans nothing, because the CPU writes none of them while parked. While
parked, STATUS diagnostics that read registers are refused.

**Unpark.** The next submit powers domain 5 on, sets the clock and runs the
one existing start sequence against retained firmware memory, as the pinned
`pvr_power_fw_enable` does. Unpark never restages firmware data; only a full
reset does, as the pinned `pvr_fw_hard_reset` does. The worker repeats start
and power steps while they make progress, spinning at most 200 µs per burst,
and waits a tick only where the hardware wait is at least a tick, such as the
firmware boot.

The pinned operating-point table gives 800 mV at 400 MHz and at 600 MHz in all
23 voltage entries, and 800 mV is the DCDC4 level the domain-5 row already
checks. Both rates come from fixed PERI0 outputs (mux values 3 and 2 in CCU
`0x0B20` bits 24–26), so neither needs a rail write or PLL change. The worker
writes the rate into `core_clock_speed` of the firmware runtime configuration
and applies it at unpark. A change while powered is a park and an unpark. The
default is 400 MHz until the board qualifies 600 MHz; a thermal throttle
selects 400 MHz at the next unpark.

The 50 ms delay keeps a 60 Hz display (one job per 16.7 ms) powered. mimus
dispatches the layers of one inference back to back, so a burst parks at most
once after it ends. If the model's mimus trace shows bursts separated by less
than the unpark cost, the image raises the delay above that gap.

## Reused and removed mechanisms

- **Reused:** the KCCB and FWCCB transport, client rings, `pvr_gpu_stop` for
  park, reset and close, the one start sequence for unpark and reset,
  translation invalidation for session close, workload page tables, free
  lists, child disposal, the native backend's register allowlist, interrupt
  mask and rearm, GPU power and clock parent, firmware staging,
  `fatfs_port_load`, `storage_digest_bytes`, device-memory leases, the IRQ
  table.
- **Removed:** STATUS polling for completion (STATUS stays for diagnostics);
  the 32-word and 64-pixel reply payloads; `input_copy` and its `input_state`
  hand-off; per-access `AT`; hot-path reads of `WRAP_MASK`, `COMMAND_SIZE` and
  host cursors; duplicate `DSB SY`; the volatile byte copy of events; runtime
  firmware-header decodes; two deadline checks per register access; the tick
  poll while a job runs; mutual BUSY between compute and render; retention
  after a refusal.

## Performance budgets

Counts come from the QEMU FreeRTOS compositions
`checks/pvr-gpu-service.{c,py}` and `checks/pvr-compute-service.{c,py}`
(queue receives, worker loop passes, timed-out waits, ticks, `memcpy` bytes,
and calls to `aarch64_device_translate` and `aarch64_device_cache` with lines
maintained, counted with `--wrap`), the Unicorn backend model
`checks/a733-gpu-backend-emulate.py`, `checks/pvr-gpu-vm.c`,
`checks/device-memory-capacity.c` and `checks/gpu-software-failure.py`.
Barrier counts come from disassembly of the compiled submission, drain,
return-poll and timeline functions. Baselines marked pending are recorded,
with their inputs, by the first implementation commit.

| Metric | Baseline | Target |
| --- | --- | --- |
| Jobs in flight | 1 (counted) | Render 1 + compute 2, together |
| Client wake-ups per compute job | One per tick of execution plus 32 | 2: submit reply, completion |
| Worker wake-ups per job beyond the interrupt | One per tick of execution | 1, the submit message |
| Worker timed wake-ups per second while a job runs | 1,000 | 0 beyond the deadline; 250 until delivery is qualified |
| Ticks between client send and kick, GPU powered | Not measured | 0, at most one context switch |
| Unpark, submit to kick in the model | Not measured | 0 ticks beyond the firmware boot, 1 worker wake-up |
| Bytes restaged and lines cleaned per unpark | Not applicable | 0 and 0 |
| Park and unpark cycles | Never before CLOSE | 0 at 60 Hz display; at most 1 per mimus burst |
| Worker busy time per step with an EL0 copy | Not measured | At most one 16 KiB `memcpy` |
| Queue round trips to deliver a 3,968-byte result | 31 | 0 beyond the completion message |
| Copies per input or result byte | 2 input, at least 5 result | 0 for EL1 clients, 1 for EL0 |
| `AT` per register read, submission, drain or timeline read | 5 per register read; others pending | 0 |
| `DSB SY` per operation | Transport table above (counted) | Interim column, then 0 |
| `DC` lines per job | Pending | Lines of input ∪ output at submit, output at completion; 2 `DSB SY` per transition |
| Allocations per job | 0 | 0 |
| GPU MMU leaf entries for a 4 MiB aligned buffer | 1,024 | 2 |
| Free extents after both sessions open and close | Not modeled | Identical to before |
| Time from last progress to reset start | None; faults retain | The job deadline |
| Recoveries per boot | 0 | 3 full resets |
| Firmware card reads and hashes | Linked into the image | One each per boot, plus one hash per reset |

New host cases: a client that dirties an output range before resubmitting
gets correct output; a full reply queue fails only its session; an injected
cross-session page number is refused.

## Failure classes

| Failure | Class | Retained |
| --- | --- | --- |
| Bad session, slot, length or state; third job; reset in progress; STATUS register read while parked | 1, refused before access | Nothing |
| CONTEXT_RESET, GUILTY or INNOCENT reason | 2, firmware-reported | That session's leases until it closes |
| Missed deadline, firmware restart or fault event, unpark boot failure, with drain shown | 2; counts against the three resets | Nothing after reset |
| Firmware hash mismatch | 1 | Nothing |
| Reply post failure | 3 for that session | That session's leases |
| Any drain-list timeout at reset, park or close; reset readback mismatch; domain 5 not off; missing cleanup return at close | 3, unknown | All GPU leases, firmware memory, page tables, clock, DCDC4 token, domain 5 |
| Inherited GPU that may be executing | Image-wide unknown | Reported as an unexcluded remote writer |

## Left out, with reasons

- **800 MHz and 1,008 MHz.** They need DCDC4 above 800 mV for some speed bins
  (up to 840 mV and 960 mV), a speed-bin read and rail writes, which the power
  owner does not provide.
- **Taking over a GPU found running at boot.** Its page tables are unknown, so
  a drain cannot prove what it wrote.
- **Copies in the EL0 client's own context.** They need a checked kernel entry,
  which is reserved code.
- **Dependencies between sessions, more than two sessions, priorities and
  preemption.** There are two consumers and neither needs them.
- **Load-based frequency changes.** A change costs a park and an unpark.
- **Multi-page compute and new render features.** Other packages own them.

## Changes outside this package

- `apps/accelerators/gpu.c` and the render applications move to completion
  messages; the removed reply payloads go when they do.
- Each image that uses interrupt completion adds an IRQ table entry for INTID
  95 with the worker's core and priority `0xd8` (the existing `apps/os/irq.c`,
  and a new `irq.c` for the accelerator and GPU display images). Request to
  the primary agent: link `platform/a733-smp-cached/irq_table.c` into the
  images that do not use it yet. No change to `irq_table.c` or `boot.patch`.
- The power owner's domain-5 release and DCDC4 token, and the memory
  architecture's `gpu` class with 2 MiB-aligned leases.

## Board evidence that would confirm the design

- SPI 63 delivered for every completed job over at least 1,000 jobs; this
  removes the 4 ms poll.
- `DMB OSHST` before the doorbell and `DMB OSHLD` after the write-offset,
  return-word and timeline loads, with no lost or stale completion over the
  same run.
- Compute and render completing together with correct outputs, and an
  injected out-of-session address faulting only its session.
- An injected shader lockup in one session reported as CONTEXT_RESET while the
  other session's jobs complete correctly.
- Every drain-list register reaching idle after an injected hang the firmware
  does not recover, followed by a successful restart and job.
- Domain 5 off and on through park and unpark, with the firmware booting each
  time, the boot time recorded, and a correct job at 600 MHz.
- The firmware loaded from the card matching the pinned hash on a cold boot.
