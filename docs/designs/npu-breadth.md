# Design: loading, switching and sharing compiled NPU models at runtime

Status: **design**; no code exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The runtime, `drivers/npu/a733_npu_runtime.c` with the interface
`include/npu_runtime.h`, replaces the concurrent-session code in
[npu_session.c](../../drivers/npu/npu_session.c). Up to eight client tasks each
own one **slot**: fixed memory and a command-table entry that hold one model at
a time. A client can switch its slot's model while other slots run jobs,
which share the NPU with round-robin starts. Each slot has a **read-only region** (NN descriptors, coefficients, shader instructions
and a 524,288-byte command allocation), a **read window** (inputs) and a
**write window** (working pool and outputs). The image fixes the slot count
and region sizes. The first consumer is `apps/npu-runtime-large`
with one slot. Times are estimated worst cases at Boot0's 494 MHz
core 0 clock ([docs/cpu-clock.md](../cpu-clock.md)).

## Data flow and ownership

1. **Initialization** fills each command allocation with END pairs, cleans it,
   and sets the slot's **END mark**, the offset beyond which every command byte
   is an END pair, to zero.
2. **LOAD** carries a catalogue index. The worker accepts it only when the slot
   has no job and holds the reply until the slot is READY or the load is
   refused. Only the worker, at EL1, writes the read-only region.
3. **Single read.** Parsed sections pass through a 4 KiB worker-private buffer.
   Coefficients and shader instructions are copied into the slot. Each byte is
   read once: the SHA-256 against the catalogue's per-section digest and the
   CRC32C reference for the interim checks are computed in the same pass over
   the destination bytes.
4. **Assembly** builds the command stream from load-state records that must
   match an accepted template, so the stream cannot program the NPU MMU or a
   register outside the template set. Each relocated field must lie in the
   region of its role (659 comparisons for ShuffleNetV2). The worker writes END
   pairs from the stream end up to the END mark and moves the mark to the
   stream end; a refused LOAD sets it to the highest offset it wrote, if
   larger. The controller fetches 524,280 bytes from the command address at
   every trigger ([docs/npu-controller.md](../npu-controller.md)), so every
   byte after the stream must be an END pair.
5. **Job.** The client writes inputs in place and sends SUBMIT. The reply is
   held until DONE or FAULT, and the read and write windows are device-owned in
   between. Outputs then stay client-owned until the next SUBMIT, LOAD or
   CLOSE. In `apps/npu-runtime-large` the client sends its next request only
   after USB0 has transmitted the last output byte, and USB0 keeps one burst
   per tick.

## Worker loop and completion

The runtime binds the NPU interrupt (INTID 97) through `npu_service_bind_irq`
and has no tick-polled mode. Clients block on their reply queues, and the
`vTaskDelay(1)` loops in `apps/npu-runtime-large` become USB0 notifications.
Bulk work runs in steps of at most C = 100 µs, and the worker does not block
while a step remains. Between steps it polls its request queue with a zero
timeout. While INTID 97 delivery is unqualified, it also reads the idle status
register at offset `0x4`, which does not acknowledge events. Full idle
(`0x7fffffff`) is only a hint, because idle alone does not establish
completion. On that hint the worker runs the service's masked progress
sequence from `irq_progress`: mask INTID 97, call `a733_npu_advance` as for a
notification once the mask has propagated (it samples event status `0x10`,
reads it again after idle and acknowledges), then rearm. The event register is
therefore never read while the source is unmasked. A deferred mask
(`irq_ops.mask` returns 0) leaves the job pending until the next step. A
notification that arrives after a hint-driven completion only makes the next
job's progress sample `0x10` early. The controller requires clear event status
before each trigger and an event read after it, so the stale notification
cannot complete the next job. The cost is one MMIO read per step inside the
loop's 20 µs margin and one masked sequence per job. With no step left, the
worker blocks on its queue and the notification, with a one-tick timeout while
a job runs (section 1.3). Board evidence of delivery removes the idle read and
the timeout.

Steps run in this order:

1. finishing a post-job check that has started;
2. preparing the next job while the NPU is idle, then G and the trigger; after
   each trigger one pending LOAD step runs;
3. starting the post-job check of a finished job, which releases its reply;
4. preparing the next job while the NPU runs, then pool preparation;
5. LOAD steps.

G, the pre-trigger work, runs as one unsplit block with no queue poll or idle
read: the NPU is idle while it runs, and new requests only queue. Each other
step size is the largest power of two that fits 80 µs at the worst-case rates
below. The sizes are a LOAD chunk copied, hashed and checksummed in one
pass (1 KiB with portable SHA-256, 4 KiB with the SHA-2 instructions), 32 KiB
of CRC32C or END compare, and 16 KiB of maintenance or of pool zeroing with
its clean and invalidate. Until board evidence records `ID_AA64ISAR0_EL1`, the
portable SHA-256 runs.

On core 0 the worker runs at priority 3, below the USB0 loop at 4 and above the
single-core client at 2. USB0 preempts the worker for at most one 200 µs burst
per 1 ms tick, so worker terms take a factor of 1.25 where USB0 shares core 0.
While W ≥ D the worker occupies core 0 under steady load, so tasks below
priority 3 there do not run; in the first consumer those are the waiting client
and the idle task, which has no required work.

## Memory placement and cache policy

One parent allocation comes from the write-back cached DDR device pool, mapped
at EL1 with 2 MiB blocks; loads allocate nothing. The read-only regions with the
NPU table arena, the read windows and the write windows form three groups, each
starting and ending on a 2 MiB boundary, separated by unmapped device pages.
The NPU page tables are built once with 4 KiB entries (section 2, item 8); the
read-only group's entries carry no write permission, which the hardware is not
yet shown to enforce ([docs/npu-mmu.md](../npu-mmu.md)). Each
tensor is a 4 KiB-aligned allocation padded to whole 64-byte lines.

The NPU is not coherent; maintenance is by address through
`aarch64_device_cache`, whose trailing `DSB SY` completes it before the
`a733_npu_submit` register write and before a reply. Per job, preparation
cleans the inputs and completion invalidates the outputs. LOAD cleans the
read-only bytes it wrote. Pool preparation zeroes the pool with `DC ZVA` and
cleans and invalidates it; initialization refuses a `DCZID_EL0` that disagrees
with the 64-byte block or prohibits `DC ZVA`. In the interim, job preparation
also cleans and invalidates the output lines, so a dirty client line cannot be
evicted over NPU output. The controller keeps its barrier between the
command-address and trigger writes, and no other barrier is added.

**Fixed mappings,** requested from the primary agent, make the read-only and
write groups read-only at EL0 and EL1, the read group read-write, and add an
EL1-only read-write alias of the read-only and write groups for loads, pool
clears and `DC IVAC`. The tables stay sealed, so a stray CPU write faults at
the writing instruction.

## Interim integrity checks

Until the fixed mappings exist, every task can write the parent allocation. No
check stops a deliberate writer, so the checks target accidental CPU writes,
which CRC32C detects. A change to the command payload matters most: it could
program the NPU MMU base and reach memory outside the allocation, such as the
monitor at `0x42000000`. All interim checks sit in
`drivers/npu/a733_npu_runtime_interim.c`, which the fixed-mapping change
deletes.

| Check | Bytes read per job | Time | Covered afterwards by |
| --- | --- | --- | --- |
| T: every NPU table word, after each completion and before a trigger that does not follow one | 20,480 per scan, one or two scans | 0.04 ms per scan | Table arena in the read-only group |
| A, before each trigger: CRC32C of the slot's address-bearing bytes and a word compare of its END tail | 27,056 and 521,224 | 1.11 ms, 1.06 ms of it the tail | Read-only group |
| H, while the next job runs: CRC32C of the slot's read-only bytes; the SUBMIT reply waits for H | About 920,000 | 1.86 ms | Read-only group |
| Output clean and invalidate at preparation | 64 | Under 1 µs | Write group read-only |

H does not compare the END tail. The controller fetches the command span only
at a trigger, and A compares the tail immediately before every trigger of the
slot, so a tail change made during a job is caught before the NPU can fetch it.
T, A and H read through the CPU cache, and the read-only lines are cleaned but
never invalidated, so they detect CPU writes only. An NPU write into the
read-only group stays undetected until the board shows NPU MMU write permission
enforced. A CPU write to the pool or outputs between preparation and trigger
also goes undetected until the fixed mappings.

## Synchronization across cores

Clients run at EL0 on cores 1 to 5, or on core 0 in the single-core image;
the A76 cores 6 and 7 take clients after admission shows them in core 0's
inner-shareable domain. Each client
is the only sender for its slot and has one request pending, which serializes
its LOAD and SUBMIT without a lock. Only the worker writes the slot state word,
which clients read with acquire ordering, on a line apart from client-written
fields (section 2, item 6). The leading `DSB SY`
in `aarch64_device_cache` orders the input clean after the queue acquire of
the client's stores.

## Start, reply and load bounds

D is the image's job deadline cap, G = T + A, P the job and pool preparation,
H the post-job check, and W = P + H + C the worker's bulk work per job. With S
busy slots, the wait before a start is at most

(S − 1) × (G + max(D, W)) + P + G + C,

and a held reply arrives within that wait plus
max(D, H + C) + P + G + H + 2C. Here H is the check time of the largest
read-only region of any slot, because any catalogue model can be loaded. A
started post-job check finishes before any later preparation, so no further
job's P + G can enter a reply. The previous job's check can still delay one:
it starts after this job's trigger, and the worker handles this job's
completion only after that check ends, up to H + C after the trigger. The order costs no steady-state throughput: the worker
does the same work in each period G + max(D, W), and a start that waits for the
rest of one H stays inside that period because W > H. A LOAD with n steps
completes within n × (G + max(D, W)) under steady load. For ShuffleNetV2 with
W > D that is about 1,000 steps and 7.4 s portable, or about 290 steps and
2.2 s with the SHA-2 instructions; with one slot, a load runs on an idle worker.
Worker terms take the 1.25 factor where USB0 shares core 0. A host check
computes the three bounds for each image, using these formulas and factors, and
refuses an image whose client collect timeout is shorter than the reply bound.

While W > D the NPU completes one job per G + W. Each step below applies while
the measured W / D still exceeds 1, and each is a later change with its own
design:

1. When the board qualifies a fetch count of the stream length plus the END
   pair: remove A's tail compare (92 % of G), the END mark and 512 KiB of
   command allocation per slot.
2. When the CPU clock driver qualifies CPU-L at 1196 MHz: request it.
3. When the conformance pool comparison shows no catalogue model depends on
   the pool's initial contents: drop the clear for every model (3.87 ms of W).
   If one model still depends on it, a DMA zero-fill replaces `DC ZVA` for
   every model.
4. When D is below H plus the input clean: run H on an idle A55 core.
5. When D is below about 0.5 ms: move the input clean to the client
   (`SCTLR_EL1.UCI`).

## Budgets and measurement

Figures are for ShuffleNetV2 in one slot: 964,224-byte model, 150,528-byte
input, 2-byte output, 1,093,632-byte pool, 27,056 address-bearing bytes. The
baseline is counted from `npu_session.c` with ShuffleNetV2 as its only model.
Estimated worst-case rates at 494 MHz, which the board replaces:

- `DC ZVA` 16 cycles per line; `DC CVAC`, `DC CIVAC`, `DC IVAC` 96 cycles per
  dirty line;
- DDR streaming read (CRC32C, END compare, table scan) 64 cycles per line;
- copy 128 cycles per line, a DDR read plus a write-allocate read;
- SHA-256 25 cycles per byte portable, 3 with the SHA-2 instructions; CRC32C
  on cached data 1 cycle per 8 bytes.

| Metric | Baseline | Interim | Fixed mappings and pool result |
| --- | --- | --- | --- |
| Requests and replies per inference | 3 and 3 | 1 and 1 | 1 and 1 |
| Client wake-ups per inference | 3, plus one per idle tick | 1 | 1 |
| `AT` translations per inference | 2,126 | 0 | 0 |
| SHA-256 bytes per inference | About 920,000 | 0 | 0 |
| Integrity-check bytes read per inference | About 540,000 | About 1,510,000 | 0 |
| CPU bytes zeroed or copied per inference | 1,244,162 | 1,093,632 zeroed | 0 |
| Cache lines maintained per inference | 36,672 | 19,442 | 2,353 |
| P / H / G | No worker | 4.33 / 1.86 / 1.15 ms | 0.46 / 0 / 0 ms |
| W, and W with USB0 on core 0 | No worker | 6.29 and 7.86 ms | 0.56 and 0.70 ms |
| NPU idle per inference, S = 1 | About 57 ms | 7.3 ms plus client time | 0.46 ms plus client time |
| Largest non-yielding worker block | No worker | G = 1.15 ms; other steps C | C |
| One catalogue load, idle worker | Not supported | About 56 ms portable, 13 ms with SHA-2 | Same |

D for ShuffleNetV2 is unmeasured, and W > D is likely in the interim. The
derivation of each figure and the counter list sit in the header comment of
the host bound check.
[checks/npu-session.c](../../checks/npu-session.c) and
[checks/npu-session.py](../../checks/npu-session.py) (two QEMU cores, a
register model that completes jobs on credits) move to the runtime after their
counters record the baseline against `npu_session.c`. New cases:

- load A, run, load shorter-stream B into the same slot, run; a refused LOAD
  past A's stream, then B: the END tail reaches the END mark;
- three slots running while a fourth loads with jobs shorter than W: starts
  and the load stay within their bounds;
- SUBMITs from two other slots as a job completes, where one slot's H exceeds
  P + G + C + D, so its check delays the next job's completion handling: the
  reply stays within the reply bound;
- a dropped interrupt while bulk work is pending: completion within C of full
  idle; no `0x10` read while INTID 97 is unmasked; a late notification does
  not complete the next job; a mask deferral is retried;
- refusals: a catalogue byte changed between section reads, a record outside
  the templates, a relocated field outside its region, a wrong `DCZID_EL0`;
- a descriptor byte changed while idle and a coefficient byte changed while
  the job runs: mismatch in A or H, every slot retained;
- CPU writes to output and pool before SUBMIT: preparation maintains both.

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| LOAD refused: format, digest, template, role or size | 1 | Nothing; slot EMPTY |
| SUBMIT refused: slot not READY, wrong input count or length | 1 | Nothing |
| *Interim:* T, A or H mismatch | 3 | Every slot and the controller |
| Job deadline or controller fault | 3 | Every slot and the controller until the hang-reset package supplies a verified quiesce |

A mismatch is class 3 because any task can write the allocation in the
interim, so the writer and the extent of the damage are unknown.

## Reuse, removal and omissions

- **Moved** from `npu_session.c`: placement, catalogue admission, handoff and
  binding.
- **Reused:** the service's dispatch, held replies and interrupt path; the
  memory policy; the controller; `nbg_model_open`; the USB0 loop; and the
  general assembler, which gains shader and kind-3 templates and absorbs
  [nbg_sh_nt.c](../../drivers/npu/nbg_sh_nt.c).
- **Removed:** per-job translations, pool `memset`, input and output copies,
  WAIT and REAP, per-job SHA-256, the private model copies and tick-polled
  completion.
- **Omitted:** model uploads (no consumer), isolation between slots
  (unqualified NPU MMU flush), on-device compilation and job cancellation.

## Changes needed from other owners

- **Session code:** delete `npu_session.c`; `tools/npu-session-catalogue.py`
  emits per-section SHA-256.
- **Single-core provider:** `apps/npu-convolution/native.c` constructs the
  runtime for `apps/npu-runtime-large`.
- **Memory policy:** per-buffer extents on the EL1 alias and mapping checks
  once at initialization.
- **Primary agent:** the fixed mappings with the EL1 alias, and A76 admission.
  If `aarch64_device_cache` loses its leading `DSB SY`, the input clean must
  follow a `DMB ISH`, which orders data cache maintenance after the acquire.
- **Hang-reset package:** a verified quiesce, making controller faults class 2.
- **Conformance package:** the pool comparison and new-operator conformance.

## Board evidence that would confirm the design

- Two models with different shader programs, loaded alternately into one slot,
  match their SDK outputs, also without a pool clear between runs.
- A trigger whose fetch count equals the stream length plus the END pair, in
  whole 8-byte units, produces matching outputs.
- Every job completes by interrupt, with no hint-driven completion.
- A descriptor that addresses a guard page raises an NPU MMU fault, and one
  that writes a read-only page raises a permission fault.
- Inputs written on an A76 core and cleaned by core 0 reach the NPU.
- A client kernel's time with and without the runtime running.
- `ID_AA64ISAR0_EL1`, `DCZID_EL0`, the per-line maintenance, copy and
  streaming rates, and D, G, H, P and W, timed with the generic timer.
