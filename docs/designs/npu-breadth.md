# Design: loading, switching and sharing compiled NPU models at runtime

Status: **implemented**, **host-tested** in QEMU with a register model in place
of the NPU. The runtime drives the OS NPU service and the
[supervised NPU board image](npu-board.md); the specific board confirmations
listed at the end are open. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The runtime, `drivers/npu/a733_npu_runtime.c` with the interface
`include/npu_runtime.h`, loads compiled models into **slots**. Each slot has a
client task, fixed memory and a command-table entry that hold one model at a
time. The number of slots is `NPU_RUNTIME_SLOTS` (the service's client limit)
and an image configures how many it uses. A client switches its slot's model
while other slots run jobs, which share the NPU with round-robin starts. Each
slot has a **read-only region** (NN descriptors, coefficients, shader
instructions and a 524,288-byte command allocation), a **read window** (inputs)
and a **write window** (working pool and outputs). The image fixes the slot
count and region sizes. The finite session code in
[npu_session.c](../../drivers/npu/npu_session.c) remains for the finite
session images and its host check. Times are estimated worst cases at Boot0's
494 MHz core 0 clock ([docs/cpu-clock.md](../cpu-clock.md)).

## Data flow and ownership

1. **Initialization** fills each command allocation with END pairs, cleans it,
   and sets the slot's **END mark**, the offset beyond which every command byte
   is an END pair, to zero.
2. **LOAD** carries a catalogue index. The worker accepts it only when the slot
   has no job and holds the reply until the slot is READY or the load is
   refused. Only the worker, at EL1, writes the read-only region.
3. **Staged single read.** The decoder and assembler need the whole file in
   contiguous memory, so a LOAD copies the catalogue model into a CPU-only
   staging area in the read-only group, 1 KiB per step, and hashes each chunk
   after copying it against the catalogue's whole-file SHA-256. A source byte
   changed before its chunk is copied fails the digest. Coefficients and shader
   instructions are then copied from staging into the slot, and the CRC32C
   references for the interim checks are computed over the destination bytes.
   The staging area has the size of the largest catalogue model (964,224 bytes
   for ShuffleNetV2).
4. **Assembly** builds the command stream from load-state records that must
   match an accepted template, so the stream cannot program the NPU MMU or a
   register outside the template set. The assembler places each allocation
   inside the region of its role and refuses a relocation outside its
   allocation. Templates cover neural-network operations and shader operations
   (kind 1); kind-3 records are refused. The worker writes END pairs from the
   stream end up to the END mark and moves the mark to the stream end; a
   refused LOAD sets it to the highest offset it wrote, if larger. The
   controller fetches 524,280 bytes from the command address at every trigger
   ([docs/npu-controller.md](../npu-controller.md)), so every byte after the
   stream must be an END pair.
5. **Job.** The client writes inputs in place and sends SUBMIT. The reply is
   held until DONE or FAULT, and the read and write windows are device-owned in
   between. Outputs then stay client-owned until the next SUBMIT, LOAD or CLOSE.

## Worker loop and completion

The runtime binds the NPU interrupt (INTID 97) through `npu_service_bind_irq`
and has no tick-polled mode. Clients block on their reply queues. Bulk work
runs in steps of at most C = 100 µs, and the worker does not block while a step
remains. Between steps it polls its request queue with a zero timeout.

`npu_runtime_bind_irq` takes an idle-hint option. With the hint on, the worker
also reads the idle status register at offset `0x4`, which does not acknowledge
events. Full idle (`0x7fffffff`) is only a hint, because idle alone does not
establish completion. On that hint the worker runs the service's masked
progress sequence: mask INTID 97, call `a733_npu_advance` as for a
notification once the mask has propagated (it samples event status `0x10`,
reads it again after idle and acknowledges), then rearm. The event register is
therefore never read while the source is unmasked. A deferred mask leaves the
job pending until the next step. A notification that arrives after a
hint-driven completion only makes the next job's progress sample `0x10` early;
the controller requires clear event status before each trigger and an event
read after it, so the stale notification cannot complete the next job. With the
hint off, completion comes from the interrupt and the job deadline alone.

While a job runs, interrupt progress reads the configuration registers
`0x3b4`, `0x388` and `0x14`. The worker runs it only after a notification or
hint, while INTID 97 is masked or its rearm is deferred (after each start),
after a tick timeout, or once the job deadline has expired; other passes read
only the generic timer. A step that overlaps a running job costs one register
read, the idle read. On the native path every register access also makes one
`AT S1E1R` translation with an `ISB` and ends with a `DSB SY`. A job costs at
most 16 further register accesses: 5 reads and 2 writes at the trigger, 3 reads
at the rearm and 6 at completion. With no step left, the worker blocks on its
queue and the notification, with a one-tick timeout while a job runs
(section 1.3); each timeout costs four register reads.

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
below: a LOAD chunk copied, hashed and checksummed in one pass (1 KiB with
portable SHA-256, 4 KiB with the SHA-2 instructions), 32 KiB of CRC32C or END
compare, and 16 KiB of maintenance or of pool zeroing with its clean and
invalidate. The portable SHA-256 runs until `ID_AA64ISAR0_EL1` is recorded on
the board. The worker handles every completion on the pass after its
notification, so a completion is handled between the steps of a running
post-job check; only the next start and the next check wait for that check.

On core 0 the worker runs at priority 3, below the USB0 loop at 4 and above the
single-core client at 2. USB0 preempts the worker for at most one 200 µs burst
per 1 ms tick, so worker terms take a factor of 1.25 where USB0 shares core 0.

## Memory placement and cache policy

One parent allocation comes from the write-back cached DDR device pool, mapped
at EL1 with 2 MiB blocks; loads allocate nothing. The read-only regions with the
NPU table arena, the read windows and the write windows form three groups, each
starting and ending on a 2 MiB boundary, separated by unmapped device pages.
The NPU page tables are built once with 4 KiB entries; the read-only group's
entries carry no write permission, which the hardware is not yet shown to
enforce ([docs/npu-mmu.md](../npu-mmu.md)). Each tensor is a 4 KiB-aligned
allocation padded to whole 64-byte lines.

The NPU is not coherent; maintenance is by address through
`aarch64_device_cache`, whose trailing `DSB SY` completes it before the
`a733_npu_submit` register write and before a reply. Per job, preparation
cleans the inputs and cleans and invalidates the output lines, so a dirty
client line cannot be evicted over NPU output; completion invalidates the
outputs. LOAD cleans the read-only bytes it wrote. Pool preparation zeroes the
pool with `DC ZVA` and cleans and invalidates it; initialization refuses a
`DCZID_EL0` that disagrees with the 64-byte block or prohibits `DC ZVA`. The
controller keeps its barrier between the command-address and trigger writes,
and no other barrier is added.

**Fixed mappings** are requested from the owner of the platform memory
management: the read-only and write groups read-only at EL0 and EL1, the read
group read-write, and an EL1-only read-write alias of the read-only and write
groups for loads, pool clears and `DC IVAC`. The tables are sealed, so a stray
CPU write faults at the writing instruction. They are not built.

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
| S, per LOAD: CRC32C of each staged chunk as it is hashed, recomputed over the staging area after the coefficient copy; a mismatch refuses the LOAD | 964,224 per ShuffleNetV2 LOAD | 1.95 ms in 30 steps | Staging area in the read-only group |
| Output clean and invalidate at preparation | 64 | Under 1 µs | Write group read-only |

H does not compare the END tail. The controller fetches the command span only
at a trigger, and A compares the tail immediately before every trigger of the
slot. T, A and H read through the CPU cache, and the read-only lines are
cleaned but never invalidated, so they detect CPU writes only. An NPU write into
the read-only group stays undetected until the board shows NPU MMU write
permission enforced. A CPU write to the pool or outputs between preparation and
trigger goes undetected until the fixed mappings. One window stays open: a CPU
write to a slot's read-only bytes after the LOAD writes them and before it
records the A and H references enters those references, and the slot then runs
the changed bytes until it is loaded again. For ShuffleNetV2 the window runs
about 1,000 worker steps.

## Synchronization across cores

Clients run at EL0 on cores 1 to 5, or on core 0 in the single-core image; the
A76 cores 6 and 7 take clients after admission shows them in core 0's
inner-shareable domain. Each client is the only sender for its slot and has one
request pending, which serializes its LOAD and SUBMIT without a lock. Only the
worker writes the slot state word, which clients read with acquire ordering, on
a line apart from client-written fields (section 2, item 6). The leading
`DSB SY` in `aarch64_device_cache` orders the input clean after the queue
acquire of the client's stores. If it were removed, the input clean would need
a `DMB ISH`.

## Start, reply and load bounds

D is the image's job deadline cap, G = T + A, P the job and pool preparation,
H the post-job check, and W = P + H + C the worker's bulk work per job. With S
busy slots, the wait before a start is at most

(S − 1) × (G + max(D, W)) + P + G + C,

and a held reply arrives within that wait plus
max(D, H + C) + P + G + H + 2C. H is the check time of the largest read-only
region of any slot, because any catalogue model can be loaded. A LOAD with n
steps completes within n × (G + max(D, W)) under steady load; for ShuffleNetV2
with W > D that is about 1,030 steps and 7.6 s portable, or 320 steps and 2.4 s
with the SHA-2 instructions. With one slot, a load runs on an idle worker.
`checks/npu-runtime-bounds.py` computes the three bounds for each image and
refuses an image whose client collect timeout is shorter than the reply bound.

While W > D the NPU completes one job per G + W. Later changes, each with its
own design and applying while the measured W / D exceeds 1:

1. When the board qualifies a fetch count of the stream length plus the END
   pair: remove A's tail compare (92 % of G), the END mark and 512 KiB of
   command allocation per slot.
2. When the CPU clock driver qualifies CPU-L at 1196 MHz: request it.
3. When the conformance pool comparison shows no catalogue model depends on
   the pool's initial contents: drop the clear for every model (3.87 ms of W).
   If one model still depends on it, a DMA zero-fill replaces `DC ZVA`.
4. When D is below H plus the input clean: run H on an idle A55 core.
5. When D is below about 0.5 ms: move the input clean to the client
   (`SCTLR_EL1.UCI`).

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| LOAD refused: format, digest, staging checksum, template, role or size | 1 | Nothing; slot EMPTY |
| SUBMIT refused: slot not READY, wrong input count or length | 1 | Nothing |
| *Interim:* T, A or H mismatch | 3 | Every slot and the controller |
| Job deadline or controller fault | 3 | Every slot and the controller until a verified quiesce exists |

A mismatch is class 3 because any task can write the allocation in the
interim, so the writer and the extent of the damage are unknown.

## Qualified figures

Per ShuffleNetV2 inference in QEMU with two Cortex-A55 cores. The session code
is the baseline. All rows are in `docs/evidence/npu-breadth.jsonl`.

| Quantity | Session code | Runtime | Record |
| --- | --- | --- | --- |
| Requests and replies | 3 and 3 | 1 and 1 | `npu-breadth-emulator-counts` |
| Address translations of buffers | 3,216 | 0 | same |
| Register reads and writes per job | not counted | 12 and 2 | same |
| SHA-256 bytes | 920,616 | 0 | same |
| Integrity-check bytes read | 541,656 | 1,509,856 | same |
| CPU bytes zeroed or copied | 1,244,162 | 1,093,632 zeroed | same |
| Cache lines maintained | 36,672 | 19,442 | same |
| G, P, H, W (one slot, USB0 on core 0) | no worker | 1.44, 5.41, 2.52, 8.22 ms | `npu-breadth-time-bounds` |
| One ShuffleNetV2 LOAD | not supported | 65.2 ms bound (portable SHA-256) | same |

Images that link the service but not the runtime keep their section sizes
(`npu-breadth-image-size`): the runtime state is a static object outside
`struct npu_service`, and the worker reaches the runtime loop through a
function pointer, so `--gc-sections` removes the loop from them.

Interim figures above the plan, each with its cause: H is bounded by the model
file size rather than the counted read-only bytes. W carries that H and the
longest step L = 0.235 ms (the LOAD step that decodes the staged copy runs
about 58,000 instructions, against C = 0.1 ms). A LOAD takes 65.2 ms against
about 58 ms because of the staging copy. D for ShuffleNetV2 is unmeasured and
W > D is likely.

## Limitations and open work

- **Fixed mappings** with the EL1 alias, per-buffer mapping checks at
  initialization, and the native adapter checking the register mapping once at
  initialization rather than at each access (one `AT` translation each) are
  requested from the primary platform owner; none is built. They would remove
  the interim checks and the pool clear, bringing P, H and G to 0.46, 0 and 0 ms.
- **Section-order decoder.** It would remove the staging copy and its memory and
  split the 58,000-instruction decode step.
- **Kind-3 and software operations** are refused by the assembler.
- **A76 admission** for clients, a verified quiesce after controller faults
  (making them class 2), and the pool comparison are owned elsewhere.
- **No isolation between slots**, because the NPU MMU flush is unqualified.
  Model uploads and on-device compilation have no consumer.
- **Board confirmations not yet recorded** (`npu-breadth-board-evidence`):
  two models with different shader programs loaded alternately into one slot
  match their SDK outputs, also without a pool clear between runs; a trigger
  with a fetch count of the stream length plus the END pair, in whole 8-byte
  units, produces matching outputs; every job completes by interrupt with no
  hint-driven completion; a descriptor addressing a guard page raises an NPU MMU
  fault and one writing a read-only page raises a permission fault; inputs
  written on an A76 core and cleaned by core 0 reach the NPU; a client kernel's
  time with and without the runtime; and `ID_AA64ISAR0_EL1`, `DCZID_EL0`, the
  per-line maintenance, copy and streaming rates, and D, G, H, P and W timed
  with the generic timer.
