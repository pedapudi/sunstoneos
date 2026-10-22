# Design: placing mimus linear layers on the CPU or GPU in the OS image

Status: **design**; no code exists. Each item ends at implemented and
host-modeled, with physical behavior pending until the board evidence at the
end exists. It follows [performance-architecture.md](../performance-architecture.md)
and depends on three unmerged designs: the GPU runtime, the memory
architecture and the GPU integer matrix product for mimus
(`docs/designs/gpu-mimus-compute.md` at commit 05e3c7c on branch
`audit/gpu-mimus-compute`, called the kernel design below).

The integrated OS image ([os-image.md](../os-image.md)) runs mimus at EL0 on
the CPU. On the bench recording (8.87 s of audio, one Cortex-A55 at the
configured 494 MHz, NEON kernels) the linear layers take 14.894 s of 19.476 s
and perform 4.17 billion multiply-accumulates (MACs) ([mimus.md](../mimus.md)).
The goal allows 0.4435 s. Mimus forms exact integer sums of int16 activation
codes and int8 weights, then converts each sum to float and applies scales and
bias in a fixed order, the **read-out**. The GPU returns the exact sums and the
CPU keeps the read-out, so output stays bit-identical.

This package adds the GPU path to the OS image and decides, per tensor,
whether a linear call goes to the GPU. The kernel design owns the kernel, the
hook into mimus, the two job slots and the one CPU/GPU column split; both
designs use zero copies and carry job fields in the submit message.

## Data flow and ownership

- **Weights.** The GPU maps the base and overlay model regions (16 MiB)
  read-only and reads mimus's int8 layout in place. A model-slot upload is
  refused while the GPU session holds these regions, and the refusal closes
  the session; the next successful hash reopens it.
- **Activations.** Mimus writes codes into its arena, the first 12 MiB of the
  recognition arena at `0x46000000`, which the GPU maps read-only. The
  recognizer passes mimus exactly 12 MiB.
- **Sums.** Two 2 MiB output regions, at `0x46C00000` and `0x46E00000`, are
  the two job slots. The GPU writes a slot while it is DEVICE; the recognizer
  reads sums in place while it is CPU and runs the read-out.
- **Jobs.** The recognizer sends the GPU runtime's 32-byte submit message:
  session, slot, tensor ordinal, activation offset, row stride, row count and
  sequence. The FreeRTOS queue copies it by value, so the worker checks its
  own copy once with the `a733_range.h` helpers. K, N, `n_gpu`, the sum width
  and the weight address come from the worker's copy of the placement table.
  The worker refuses a job unless offset and stride are multiples of 4,
  stride ≥ 2K, and the extent `(rows − 1) × stride + 2K` lies inside the
  12 MiB span. It cleans that extent rounded outward to 64-byte lines, which
  stays inside the span because the span is 2 MiB aligned. At completion it
  invalidates the written sums rounded outward inside the slot.
- **Job split.** A call of more than 32 rows becomes two jobs split on a
  32-row boundary near its middle, in alternating slots; each job holds at
  most `job_rows` rows. The bench recording has 203 calls and 399 jobs: the
  seven calls of at most 32 rows are one job each.
- **Read-out on both inference workers.** When `n_gpu = N`, the recognizer
  runs each completed slot's read-out through mimus's existing `ParallelFor`
  over rows on both workers; rows are independent, so output is unchanged.
  The `ParallelFor` dispatch orders the worker's one invalidate before the
  second worker's reads. When `n_gpu < N` the second worker computes CPU
  channels instead.

**One subsampling path.** The subsampling convolution is 1,584 → 176 over
4,440 rows; its full code buffer would be 14,065,920 bytes. The port produces
its rows in blocks of 240 into one fixed 760,320-byte code buffer, and both
the CPU kernel and the GPU job read that buffer. This producer replaces the
port's current 160-row calls, so the port has one subsampling path, and the
buffer counts in the arena peak with or without the GPU. Each block is one
call of two jobs: 38 jobs carrying 1.24 G MACs. The GPU idles while the CPU
produces the next block; that gap is budgeted below.

## A fixed table places each tensor

A host tool in `apps/os/` reads the model files whose SHA-256 the build embeds
in `mimus_asset_sha256`. For the build's admitted core set and CPU clock it
writes one row per tensor, indexed by the tensor's ordinal:

- **min_rows:** `ceil(32,768 / (K × N))`, or `UINT32_MAX` for a tensor the
  GPU cannot run (int4 rescale, or K and N outside the kernel's shape rules).
  At 494 MHz the break-even is about 29,000 MACs per call, from the
  0.28 G MAC/s A55 rate, the kernel design's 7.1 G MAC/s GPU estimate and two
  jobs of 50 µs fixed cost.
- **n_gpu:** the kernel design's split, seeded from its time model. A split of
  a 176-channel call gives the CPU at least 16 channels and pays only when the
  two CPU workers together exceed 0.73 G MAC/s. Two A55 workers at 494 MHz
  give 0.56 G MAC/s, so the A55 table has `n_gpu = N` for every tensor. Two
  A76 workers at 2,002 MHz give about 4.5 G MAC/s under the kernel design's
  assumption of twice the A55's work per clock, and its seed puts 61% of the
  MACs on the GPU. At the 1,404 MHz A55 point two workers give 1.6 G MAC/s,
  so admitting it regenerates the table with a split.
- **job_rows:** `min(floor(2^26 / (K × n_gpu)), floor(2 MiB / (n_gpu × width)))`
  rounded down to a multiple of 32: the kernel design's 64 Mi MAC job cap and
  one output region.

The per-call test is one bound check on the ordinal, one compare
`min_rows ≤ rows`, and one load of the session state.

**Admission follows authentication.** The `gpu-linear` start entry runs before
the scheduler, creates the GPU worker and opens nothing. The recognizer hashes
the model assets at EL0 and sends OPEN only after every hash matches. The
worker then leases the model regions and the 12 MiB span read-only and the
output regions read-write, cleans the model regions once (16 MiB), cleans and
invalidates both output regions once (4 MiB), and copies the placement table.
A refused or failed open puts the dispatcher on the CPU for the boot.

## Memory placement and cache policy

| Range | GPU | CPU | Maintenance |
| --- | --- | --- | --- |
| Model regions, 16 MiB | Read-only | Read; upload only while the session is closed | Clean once at open |
| `0x46000000`, 12 MiB, mimus arena | Read-only | EL0 read-write | Clean the job's extent at submit |
| `0x46C00000`, two 2 MiB output regions | Read-write while DEVICE | EL0 reads; no CPU writer | Clean and invalidate at open; invalidate written sums at completion |

The bench arena peak is 1,761,840 bytes. The subsampling buffer adds
760,320, and moving another call's codes from a ring into the arena, as the
kernel design's port change may, adds at most 312,576 (222 rows × 1,408 bytes
of the second feed-forward linear). A longer utterance adds jobs and no
memory. GPU pages are 4 KiB until the GPU runtime's large-page
board item passes: 8,192 leaf entries, then 16 with 2 MiB pages.

**EL0 and the output regions.** The image maps the 16 MiB arena read-write at
EL0, and `application.patch` checks it that way. A stray EL0 store above
12 MiB while a slot is DEVICE would leave a dirty line whose later eviction
overwrites GPU sums. The primary agent maps `0x46C00000`–`0x47000000`
read-only at EL0, and the mapping check in `application.patch` then verifies
12 MiB writable and 4 MiB read-only. Until that change lands, the guard is
mimus's exact 12 MiB span plus a host case that runs the recognizer with the
top 4 MiB protected read-only.

**GPU-side caches.** The code buffer and the output regions are rewritten at
the same addresses for every job. The GPU reads fresh codes and the CPU reads
fresh sums only if the firmware invalidates the GPU's caches and the
system-level cache before each job and writes GPU stores back to the point of
coherency before the timeline update. [gpu-compiled-programs.md](../gpu-compiled-programs.md)
leaves that property unqualified, and CPU cleans and invalidates do not
establish it. Bit-exactness on the board stays pending until its board item
passes.

## Interrupts, cores and synchronization

Under `SUNSTONE_A733_OS_GPU_LINEAR`, off by default, the interrupt table gains
INTID 95 (SPI 63) at priority `0xd8` and the start list gains `gpu-linear`.
The recognition core pair is a build-time constant: cores 6 and 7 in the A76
admission build, cores 2 and 3 otherwise. The recognizer, the GPU worker and
INTID 95 use the first core; mimus's second inference worker uses the second.
Core 0 keeps USB0, thermal and DMA. Affinities use
`xTaskCreateStaticAffinitySet`. An interrupt on another core takes the
status-4 stop, so `os-image-link.py` checks that the INTID 95 entry's core
equals the constant in the ELF.

Completion is interrupt-driven. The submit wakes the worker by a same-core
queue send; INTID 95 wakes the worker, which posts one completion; the
recognizer blocks on its reply queue and polls nothing. Submit and completion
therefore cost no inter-processor interrupt. Until INTID 95 delivery is
qualified, the worker keeps the runtime's 4 ms timed wake, which gives correct
results and adds up to 4 ms per job.

The engine's `ParallelFor` join orders codes written on the second core before
the submit. The queue send orders them before the worker's
`aarch64_device_cache` clean, whose leading `DSB SY` orders those stores
before the clean; clean by address reaches lines in the other core's cache.
The completion message orders the invalidate before the read-out. The
dispatcher's counters have one writer, the recognizer.

**Replies.** A mimus submit gets one reply, a refusal or a completion,
and every reply carries slot and sequence. The recognizer waits for jobs in
sequence order. A refusal for the other slot that arrives early is held until
that slot is waited for. A reply whose sequence matches neither job in flight
is handled as a missed deadline. With two jobs in flight the worker owes at
most two messages, within the reply queue's depth of three.

## Liveness

The worker answers every job: OK, RESET after the runtime's acknowledged
stop, or LOST. Its hang deadline for a job starts when the job begins
executing: at its kick if the GPU is idle, otherwise at its predecessor's
completion. The recognizer's timer for a job starts when it begins waiting for
that job, after the predecessor's answer, so a job queued behind a slow
predecessor still gets a full bound. From that point the answer bound is
`T_answer = 2 × T_clean + max_job_us + 4 ms + T_stop + 1 ms`:

- `T_clean` = 0.26 ms: the largest job's extent, 405,504 bytes (128
  subsampling rows), at 20 cycles per line at 494 MHz. The clean of the next
  submitted job can delay the answer by one more `T_clean`.
- `max_job_us` = 100 ms until the board measures the largest job (estimated
  at 5 ms), then four times the measured value.
- 4 ms is the runtime's hang detection after `max_job_us`.
- `T_stop` = 20 ms: the runtime passes `now + 20 ms` as the deadline of
  `pvr_gpu_stop_init`, and a stop that overruns it latches FAILED and ends in
  LOST.

`T_answer` is 125.5 ms. The recognizer calls `board_fail` at 135.5 ms, which
stops watchdog renewal. The 10 ms slack covers both cleans running 20 times
slower than estimated. A worker that spins starves the recognizer on the same
core, so this timeout cannot run; the recognizer's 3 s report window and the
watchdog catch that case.

## Failure classes

| Failure | Class (section 1.8) | Action | Retained |
| --- | --- | --- | --- |
| Refusal before device access: closed session, ordinal out of range, rows above `job_rows`, alignment, stride or extent check, slot not CPU, GPU off after its fourth reset | 1 | CPU computes the call's remaining rows; a closed or off session puts the dispatcher on the CPU for the boot | Nothing |
| RESET after an acknowledged stop | 2 | Interim: the recognition reports failure, the session closes, and the next recognition re-hashes the models before reopening. Qualified: rows already read out stay and the CPU computes the rest | Nothing |
| LOST | 3 | Interim: recognition ends for the boot and model uploads are refused. Qualified: CPU for the boot | Output regions stay device-owned; mimus never allocates from them |
| No answer 135.5 ms after the wait starts | Unknown | `board_fail` | Everything; the watchdog resets the board |

After RESET or LOST, the model regions and the arena remain trustworthy only
if the GPU MMU enforces read-only mappings. That is a pending hardware claim.
The interim policy applies until the board shows that a GPU write through a
read-only mapping faults and leaves memory unchanged; a build constant then
selects the qualified policy. The dispatch record counts every fallback.

## Reused, removed and not linked

- **Reused:** the kernel design's kernel, hook, slots and split; mimus's
  read-out and `ParallelFor` workers; the GPU runtime's sessions, deadlines
  and reset; the image's interrupt table, start list and link check; the
  recognizer's model hash; the rail ledger and capacity replay.
- **No runtime arbiter.** The rail ledger counts references and the capacity
  replay proves the leases fit; an arbiter would repeat both checks.
- **Not linked:** `apps/accelerators/gpu.c`, which polls each tick and
  returns 4 KB in 31 replies.

## Budgets and measurement

Baselines are the CPU-only image on the bench recording; CPU figures assume
494 MHz. `checks/os-image-dispatch.c` (new) runs the dispatcher and a modeled
worker over the bench call list and the longest accepted capture. The kernel
design's `checks/gpu-mimus-layers` runs the host mimus engine with the GPU
kernel as host C.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Output | CPU path | Text and CRCs equal to the host reference, nominal and with each failure injected | `gpu-mimus-layers` with the dispatcher |
| Calls on the GPU; jobs | 0; 0 | Every call with `rows ≥ min_rows`: 203 calls; 399 jobs | `os-image-dispatch` |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | Copy counter in `gpu-mimus-layers` |
| Placement work per call | None | 1 bound check, 1 compare, 1 load | `os-image-dispatch` |
| Messages; recognizer wake-ups; IPIs; tick waits per job | None | 2; 1; 1, the read-out dispatch to the second worker; 0 | Queue and notification counters |
| `DSB SY` per job | None | 6, from three `aarch64_device_cache` calls, plus the runtime's doorbell barriers | Barrier wraps |
| Cache lines per job, average | None | 1,497 cleaned (61 µs) and 2,349 invalidated (95 µs) at 20 cycles per line; 62 ms per recording | `aarch64_device_cache` wrap counters |
| Per-job overhead, excluding cache maintenance | No GPU path | At most 50 µs | Board item |
| Arena peak | 1,761,840 bytes | At most 2,834,736 bytes at any utterance length | Host arena high-water mark |
| GPU idle between consecutive subsampling jobs | None | At most 1 ms per gap on the A55 pair | `os-image-dispatch`, block producer counted under Unicorn |
| Read-out | Inside the CPU kernel | At most 12 instructions per output; 12,923,134 outputs | `gpu-mimus-readout` (Unicorn) |
| Dead or spinning worker | No worker | `board_fail` 135.5 ms after the wait starts; a spinning worker within the 3 s report window | `os-image-dispatch`, `os-image-model.c` |
| Linear stage time | 14.894 s | Goal 0.4435 s; estimates below | Board item |

**Time estimate.** On the A55 pair at 494 MHz, GPU compute is 0.585 s. In
each call, the first job's clean (12 ms per recording) and the last job's
invalidate (19 ms) and read-out across two workers (78 ms) are not hidden
behind the other slot. Job overhead adds 399 × 50 µs (20 ms). The estimate is
0.71 s, which misses the goal. Subsampling code production (7.0 M codes, about 7 to
14 ms on the pair at 1 to 2 cycles per code) lies outside the linear stage.
With the A76 cores at 2,002 MHz the kernel design's split model gives 0.39 s,
which meets the goal.

Host cases in `checks/os-image-dispatch.c` cover: no open before the hash;
refusal, RESET and LOST under both policies; two consecutive jobs near
`max_job_us` that end OK; a worker that never answers; an early refusal for
slot B and a reply with an unexpected sequence; the longest capture with the
top 4 MiB protected against EL0 writes; and the emitted table for each core
set and clock.

## Left out

- **NPU placement.** The pinned NPU compiler refuses the INT32 output that
  exact sums need, and its input quantization uses one scale where mimus uses
  one per row ([npu-mimus-linear.md](../npu-mimus-linear.md)). It returns,
  with an engine value in the table, when exact sums build with the pinned
  compiler.
- **Overlapping subsampling code production with GPU work.** It needs a
  second code buffer and an asynchronous hook form. It returns if the board
  shows the 19 gaps above 20 ms in total.
- **One job for the query, key and value projections.** Board-measured job
  overhead decides whether the larger engine change pays.
- **Thermal load shedding.** The image ignores THROTTLE; CRITICAL still ends
  the session.

## Changes outside this package

- **Kernel design:** read-out of a slot through `ParallelFor` over rows when
  `n_gpu = N`; the interim RESET policy beside its LOST policy; slot and
  sequence in every reply; job deadlines that start at execution.
- **mimus port:** the tensor ordinal in `TensorView`; the 240-row subsampling
  producer with one code buffer for both paths; the second inference worker's
  affinity; the hook changes the kernel design lists.
- **GPU runtime:** one reply per mimus submit, carrying slot and sequence; a
  job's hang deadline from the start of its execution; a 20 ms stop deadline;
  the worker on the recognition core.
- **Memory architecture:** the 12 MiB span and the output regions as fixed
  spans lent to the GPU; an EL0 read mapping of the output regions while they
  are device-owned, an exception to its rule that device memory has no EL0
  mapping; the leases in the capacity replay.
- **Native USB application:** status 2 for a model-slot upload while the
  session holds the model regions.
- **Primary agent:** A76 admission for the image; a read-only EL0 mapping of
  `0x46C00000`–`0x47000000`.
- **Bluetooth voice recognition package:** it also edits `apps/os/os.h` and
  `docs/os-image.md`; this package only adds declarations and sections there.

## Board evidence that would confirm the design

- The GPU build recognizes the bench recording with text and CRCs equal to
  the host, and the dispatch record shows every placed call on the GPU.
- Two consecutive jobs on the same code buffer with different contents each
  return the CPU's sums, and sums written into a reused output region are
  fresh. This qualifies the GPU-side cache contract.
- INTID 95 arrives on the recognition core for every job; per-job overhead is
  at most 50 µs; the clean and invalidate rate per line, and the clean time of
  405,504 bytes, are measured at 494 MHz.
- An acknowledged stop completes within 20 ms, an injected hang ends in
  RESET, and a one-hour resident run never stops watchdog renewal.
- A GPU write through a read-only mapping faults and leaves memory unchanged,
  which selects the qualified failure policy.
- Per-shape CPU and GPU rates at the admitted clocks replace the `min_rows`
  and `n_gpu` seeds.
