# Design: loading, switching and sharing compiled NPU models at runtime

Status: **implemented and host-modeled**; physical behavior is pending, and
no board image links the runtime yet. The section
[Implementation, measurements and remaining work](#implementation-measurements-and-remaining-work)
records how the code differs from this design and what the emulator counts.
Section numbers refer to
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
cannot complete the next job.

While a job runs, interrupt progress reads the configuration registers
`0x3b4`, `0x388` and `0x14`. The worker therefore runs it only after a
notification or hint, while INTID 97 is masked or its rearm is deferred (after
each start), after a tick timeout, or once the job deadline has expired; other
passes read only the generic timer. A step that overlaps a running job costs
one register read, the idle read. On the native path every register access
also makes one `AT S1E1R` translation with an `ISB` in the adapter's mapping
check and ends with a `DSB SY`. An estimated 2 µs per read, which board timing
replaces, fits the loop's 20 µs margin. A job costs at most 16 further
register accesses: 5 reads and 2 writes at the trigger, 3 reads at the rearm
and 6 at completion. In the four-slot image a ShuffleNetV2 job overlaps about
110 worker steps (input clean, 67 pool steps, 31 post-job check steps), so
about 126 register accesses and translations per job. With no step left, the worker
blocks on its queue and the notification, with a one-tick timeout while a job
runs (section 1.3); each timeout costs four register reads. Board evidence of
delivery removes the idle read and the timeout.

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
| S, per LOAD rather than per job: CRC32C of each staged chunk as it is hashed, recomputed over the staging area after the coefficient copy; a mismatch refuses the LOAD | 964,224 per ShuffleNetV2 LOAD | 1.95 ms in 30 steps | Staging area in the read-only group |
| Output clean and invalidate at preparation | 64 | Under 1 µs | Write group read-only |

H does not compare the END tail. The controller fetches the command span only
at a trigger, and A compares the tail immediately before every trigger of the
slot, so a tail change made during a job is caught before the NPU can fetch it.
T, A and H read through the CPU cache, and the read-only lines are cleaned but
never invalidated, so they detect CPU writes only. An NPU write into the
read-only group stays undetected until the board shows NPU MMU write permission
enforced. A CPU write to the pool or outputs between preparation and trigger
also goes undetected until the fixed mappings.

S covers the staging area from hashing to the end of the coefficient copy, so
the slot takes only bytes that match the authenticated file. One window stays
open: a CPU write to a slot's read-only bytes after the LOAD writes them and
before it records the A and H references enters those references, and the slot
then runs the changed bytes until it is loaded again. For ShuffleNetV2 the
window runs from the first assembly step to the reference pass, about 1,000
worker steps. Closing it needs the fixed mappings.

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
W > D that is about 1,030 steps and 7.6 s portable, or about 320 steps and
2.4 s with the SHA-2 instructions; with one slot, a load runs on an idle worker.
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
| `AT` translations of buffer addresses per inference | 2,126 | 0 | 0 |
| Register accesses per inference, each with one `AT` translation on the board | Not counted | At most 16, plus one read per overlapping worker step | Same; no translations once the adapter checks the register mapping only at initialization |
| SHA-256 bytes per inference | About 920,000 | 0 | 0 |
| Integrity-check bytes read per inference | About 540,000 | About 1,510,000 | 0 |
| CPU bytes zeroed or copied per inference | 1,244,162 | 1,093,632 zeroed | 0 |
| Cache lines maintained per inference | 36,672 | 19,442 | 2,353 |
| P / H / G | No worker | 4.33 / 1.86 / 1.15 ms | 0.46 / 0 / 0 ms |
| W, and W with USB0 on core 0 | No worker | 6.29 and 7.86 ms | 0.56 and 0.70 ms |
| NPU idle per inference, S = 1 | About 57 ms | 7.3 ms plus client time | 0.46 ms plus client time |
| Largest non-yielding worker block | No worker | G = 1.15 ms; other steps C | C |
| One catalogue load, idle worker | Not supported | About 58 ms portable, 15 ms with SHA-2 | Same |

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
| LOAD refused: format, digest, staging checksum, template, role or size | 1 | Nothing; slot EMPTY |
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
- **Native adapter** (`platform/a733/npu_native.c`): check the register page
  mapping once at initialization instead of at every register access, which
  removes one `AT` translation per access.
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

## Implementation, measurements and remaining work

The runtime is [a733_npu_runtime.c](../../drivers/npu/a733_npu_runtime.c)
with the interface [npu_runtime.h](../../include/npu_runtime.h); the interim
checks T, A and H are in
[a733_npu_runtime_interim.c](../../drivers/npu/a733_npu_runtime_interim.c).
The service gains a runtime mode (`npu_service_runtime`, `npu_service_start`,
`npu_service_release` and `npu_service_retain`, described in
[npu-service.md](../npu-service.md#runtime-mode-holds-load-and-submit-replies)),
and the assembler gains a stepwise form (`nbg_assembly_begin` and
`nbg_assembly_next`, described in
[nbg-assembly.md](../nbg-assembly.md#a-stepwise-form-assembles-in-bounded-steps)).

### Differences from the design

- **LOAD reads the model through a staging copy.** The decoder and assembler
  need the complete file in contiguous memory, so a LOAD copies the catalogue model into a
  CPU-only staging area in the read-only group, 1 KiB per step, and hashes each
  chunk after copying it. The source is read once, and the digest is the
  catalogue's existing whole-file SHA-256, so the session catalogue needs no
  per-section digests. A source byte changed before its chunk is copied fails
  the digest; a change after it does not affect the load. The digest and the
  A and H references come from two passes, over the staged bytes and over the
  slot bytes built from them, with decoding, assembly and the coefficient copy
  in between. The interim check S closes the gap in the staging area, and the
  remaining window in the slot is recorded under
  [Interim integrity checks](#interim-integrity-checks). The staging area is
  the size of the largest catalogue model (964,224 bytes for ShuffleNetV2),
  and coefficients are copied a second time, from staging into the slot. The
  4 KiB section buffer of the design needs a decoder that reads sections in
  order, which is not written.
- **One LOAD step exceeds C.** The step that decodes the staged copy and checks
  the plan runs 58,000 instructions for ShuffleNetV2 in an instrumented QEMU
  count with `-icount`, an estimated 0.12 ms at one cycle per instruction and
  494 MHz. The bound check takes it at two cycles per instruction, 0.235 ms,
  as the longest step L in place of C. Assembly steps of eight operations run
  29,000 instructions in the same count.
- **The worker handles every completion at once.** Interrupt progress runs on
  the pass after a notification or hint, so a completion is handled between
  the steps of a running post-job check; only the next start and the next
  check wait for that check.
  The reply bound of the design still holds.
- **Relocated fields are bounded by construction.** The assembler places each
  allocation inside the region of its role and refuses a relocation outside
  its allocation, so the runtime adds no separate comparison per field.
- **The session check is kept.** [npu-session.c](../../checks/npu-session.c)
  still checks the session code until that code is deleted, and now prints the
  baseline counters. The runtime has its own check,
  [npu-session-runtime.c](../../checks/npu-session-runtime.c), run by
  [npu-session.py](../../checks/npu-session.py) `--mode runtime`.

### Emulator measurements

QEMU runs two Cortex-A55 cores with a register model in place of the NPU. The
baseline is the session code in the five-model reserved configuration; the
runtime runs the same five models in four slots. Figures are per ShuffleNetV2
inference, from the COUNT lines of three jobs each.

| Metric | Session code | Runtime |
| --- | ---: | ---: |
| Requests and replies | 3 and 3 | 1 and 1 |
| Address translations of buffers | 3,216 | 0 |
| Register reads and writes, each job complete before its rearm and overlapping no worker step | Not counted | 12 and 2 |
| SHA-256 bytes | 920,616 | 0 |
| Integrity-check bytes read: tables, address-bearing bytes, END tail, read-only bytes | 541,656 | 1,509,856 |
| CPU bytes zeroed or copied | 1,244,162 | 1,093,632 zeroed |
| Cache lines maintained | 36,672 | 19,442 |
| Worker steps: preparation, pool, post-job check | none | 11, 67 and 31 |

The baseline's translations cover the buffers of all five models; the design's
figure of 2,126 counts ShuffleNetV2 alone. The runtime's integrity bytes are
two table scans of 20,480 bytes, 27,112 address-bearing bytes, an END tail of
521,168 bytes and 920,616 read-only bytes. One ShuffleNetV2 LOAD runs 1,135
steps, hashes 964,224 bytes, checksums the staging area again (964,224 bytes),
copies 1,856,832 bytes and reads 920,616 bytes for the check references; the
three other slots completed 1,171 jobs while it ran. In the case whose
interrupts arrive late, four jobs overlap 508 worker steps and 11 ticks and
read 528 registers in that time, about one per step.

[npu-runtime-bounds.py](../../checks/npu-runtime-bounds.py) computes the
bounds from these byte counts at the design's rates and compares its byte
counts with the COUNT line. For the emulator check image (four slots, a 1 s
deadline cap) the start wait is at most 3.01 s and the reply 4.02 s, inside the
20 s collect timeout. The planned first consumer has one ShuffleNetV2 slot,
USB0 on core 0 and a 1 s deadline cap. With the USB0 factor, G is 1.44 ms,
P 5.41 ms, H 2.52 ms and W 8.22 ms; the start wait is at most 7.15 ms and the
reply 1.02 s, inside the host's 5 s request deadline. H is bounded by the model file size;
the counted read-only bytes are smaller.

Reproduce, with the session artifacts map of
[npu-session.md](../npu-session.md#emulator-checks):

```sh
checks/npu-session.py BUILD_DIRECTORY NEW_OUTPUT_DIRECTORY --mode runtime --artifacts ARTIFACTS.json
checks/npu-runtime-bounds.py --output NEW_OUTPUT_DIRECTORY --evidence RUNTIME_OUTPUT/result.json
```

All five runtime cases pass; the cases are listed at the top of the check.
Removing the END fill of a LOAD fails case 0, and removing the pool's
`DC ZVA` fails case 1. Skipping the comparison of S fails case 0, which
changes a staged coefficient byte after it was hashed. Running interrupt
progress on every pass fails cases 0 to 2, at about four register reads per
step (253 reads in 64 steps). The emulator cannot change `DCZID_EL0`, so the refusal
of a wrong DC ZVA block size is not exercised. The `longest` step times that
the runtime records use the generic timer; in QEMU they include the other
core's time slices and are diagnostic only, and on the board they give D, G, P
and H.

### Remaining work

- **First consumer.** `apps/npu-convolution/native.c` must construct the
  runtime for `apps/npu-runtime-large`, and the client in
  `apps/npu-runtime/main.c` then sends one LOAD and one SUBMIT per inference and
  blocks on a USB0 notification instead of `vTaskDelay(1)`.
- **Shader and kind-3 templates.** The assembler does not yet accept shader
  operations or kind-3 records, and `nbg_sh_nt.c` remains separate, so the
  relative-shift graph cannot be loaded into a slot.
- **A section-order decoder** would remove the staging copy and its memory.
- **Other owners:** the deletion of `npu_session.c`, the fixed mappings with
  the EL1 alias, per-buffer mapping checks once at initialization, A76
  admission, the hang-reset quiesce and the pool comparison, as listed above.
