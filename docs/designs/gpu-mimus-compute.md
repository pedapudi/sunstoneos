# Design: exact mimus integer matrix products on the GPU

Status: **design**; no code exists. Each item ends at implemented and
host-modeled, with physical behavior pending until the board evidence at the
end exists. It follows [performance-architecture.md](../performance-architecture.md).

The placement design for mimus linear layers
(`docs/designs/recognition-dispatch.md` at commit f0dc65e on branch
`audit/recognition-dispatch`, the dispatcher design below) specifies the data
path, placement table, deadlines, failure classes and cross-core
synchronization. This design specifies what that design takes as given: exact
sums from 32-bit GPU arithmetic, the kernel, job construction without a
package, the GPU address layout, a per-job completion proof, the CPU read-out
of GPU sums, the time model and the checks. Where the two designs differ, the
differences are listed below. This design also depends on the unmerged GPU
runtime and memory architecture designs (`docs/designs/gpu-runtime.md`,
`docs/designs/memory-architecture.md`).

A mimus linear layer multiplies int16 activation codes by int8 weight codes
and sums the products exactly. mimus then converts each sum to float once and
applies scales, a bias and, for residual layers, `y += alpha × value`, in a
fixed float order: the **read-out**. The GPU computes the sums and the CPU
runs the read-out, so output stays bit-identical to the host reference. On
one Cortex-A55 at 494 MHz the linear layers take 14.894 s of 19.476 s and
perform 4,167,910,560 multiply-accumulates (MACs) ([mimus.md](../mimus.md)).

## Exact sums from 32-bit GPU arithmetic

Activation codes satisfy |x| ≤ 32,767 and weight codes |w| ≤ 128, so a sum
of at most 512 products is at most 2,147,418,112, below 2^31, in any order.
The kernel splits the reduction length K into chunks of at most 512 and adds
each chunk sum `c` into a 64-bit value held as two words:

    lo' = lo + (uint32)c;   hi' = hi + (lo' < lo) + (c >> 31)

where `c >> 31` is an arithmetic shift, 0 or −1. It writes one int32 per
output when K ≤ 512 (mimus's `fits32` case) and one int64 otherwise. Every
sum satisfies |sum| ≤ 3,520 × 32,767 × 128 < 2^34.

## Kernel and job construction

- **Program.** `shaders/pvr_mimus_linear.cl` is compiled on the host by
  `apps/gpu-program/compile.py` with the profile-three register interface of
  [gpu-compiled-programs.md](../gpu-compiled-programs.md): the argument
  record and the output region, the local index, and workgroup identifiers
  and counts. Its USC code, at most 4,000 bytes, is linked into the image and
  copied into the program page at session open. No package, input copy,
  output limit or sealed envelope applies.
- **Shape.** Each lane computes 4 rows × 4 channels; a 32-lane workgroup of
  local shape (4, 8) covers 16 channels × 32 rows, which pads the bench MACs
  by 2.6%. Lanes load 32-bit words and extract signed bytes and halfwords.
  Load indices are clamped to the job's declared ranges; a sum is stored only
  when `row < rows` and `channel < n_gpu`.
- **Completion proof.** After its sum stores, every lane, padded lanes
  included, executes `mem_fence(CLK_GLOBAL_MEM_FENCE)` and then stores the
  job's 32-bit sequence number into its own tag word. A matching tag shows
  that the lane reached its tag store after its sum stores; the firmware's
  end-of-job flush (below) makes both visible. Sequences start at 1 in each
  boot, and the recognizer stops placing calls on the GPU before the counter
  wraps. At every session open the worker writes 0 over the tag area, then
  cleans and invalidates it, so no tag can hold a sequence from an earlier
  boot or session. A missing tag catches a job that completed OK without
  running every lane: a partial grid, a dropped CDM kick or a firmware
  completion without execution.
- **Job words.** `pvr_compute_job.c` reuses its profile-three PDS and CDM
  construction with per-job workgroup counts. The 32-byte argument record and
  the CDM block share one 128-byte, line-aligned area of the job page, which
  the worker writes and cleans only while no job is in flight. The
  31-workgroup cap stays for sealed package sessions.
- **Admission at open.** The worker refuses a tensor whose K is not a
  multiple of 4, whose weight stride is below K, or whose weight offset or
  stride is not a multiple of 4. It computes each tensor's `job_rows` as
  `min(floor(2^26 / (K × n_gpu)), floor((2 MiB − 64 KiB) / (n_gpu × width)),
  32 × floor(512 / ceil(n_gpu / 16)))`: the 64 Mi MAC cap, the sum area and
  the 512 workgroups that fill the 64 KiB tag area. A tensor whose bound is
  below 32 runs on the CPU.
- **Admission at submit.** The worker refuses `rows == 0`, `rows > job_rows`,
  sequence 0, an activation offset or row stride that is not a multiple of 4,
  and a stride below 2K. Lanes load 32-bit words, and a misaligned word load
  has no established GPU behavior. The activation extent
  `offset + (rows − 1) × stride + 2K` must lie inside the arena span, checked
  with the overflow-safe helpers of `a733_range.h`. The `rows ≤ job_rows`
  compare also bounds the workgroup count to 512 and the sums to the sum
  area, because `job_rows` was derived from both at open; a host case submits
  `job_rows + 1` rows for every tensor. These are at most 12 compares.

## One job in flight

Each call becomes one job, and a call with more than `job_rows` rows becomes
consecutive jobs split on 32-row boundaries. The session has one job in
flight and one output region. The bench recording's 203 calls are 203 jobs:
the largest, a 240-row subsampling block, has 66,908,160 MACs, below the
2^26 cap.

The time model below gives a forced split of each call into two overlapped
jobs a saving of 35 ms of 0.688 s at 494 MHz, and 5 ms of 0.623 s at
2,002 MHz. The split adds 196 jobs, a second output region, sequence-ordered
waits, held early refusals and deadlines for a job queued behind another.
Calls that split channels between the CPU and the GPU, the only placement
that meets the 0.4435 s goal, gain nothing from it. The split is left out. It
returns only if the board item that measures it, fed into the model, shows a
saving above 2% of the linear stage at the admitted A76 clock.

## GPU address layout and cache contract

| Range | Size | GPU page | GPU access |
| --- | --- | --- | --- |
| Model regions at `0x43000000` and `0x43800000` | 16 MiB | 2 MiB | Read-only |
| Arena span at `0x46000000`, mimus's arena | 12 MiB | 2 MiB | Read-only |
| Output region at `0x46C00000`: 64 KiB tag area, then sums | 2 MiB | 2 MiB | Read-write while DEVICE |
| Program page; job page | 4 KiB each | 4 KiB | Read-only |

`pvr_gpu_vm` supports page shift 21 and requires one page size per 2 MiB
virtual region, so each 4 KiB page sits alone in its own region. An unmapped
2 MiB gap follows every range in GPU virtual addresses, so a lane that
overruns faults and its job ends in RESET. The session needs 17 leaf entries:
fifteen 2 MiB pages and two 4 KiB pages. With 2 MiB pages, the GPU TLB reach
covers the 30 MiB that jobs stream, and the time model adds no TLB-miss cost.

A job's tags occupy the top of the tag area, `[64 KiB − tags × 4, 64 KiB)`,
directly below its sums at `[64 KiB, 64 KiB + sums)`. Tag words therefore
never hold sums, and one invalidate covers both. Per job the worker makes
three `aarch64_device_cache` calls, six `DSB SY` in total:

- clean the activation extent, rounded outward to 64-byte lines; the span is
  2 MiB aligned, so the rounded range stays inside it;
- clean the 128-byte job-word area, already line-aligned;
- after completion, invalidate tags and sums, from a 128-byte-aligned start
  to the end rounded up to a line, inside the region.

A refusal from the helper is a class-1 refusal: no kick has happened, and the
call runs on the CPU. A two-range form of the helper, with one leading and
one trailing `DSB SY` over both cleans, would make four per job; that is a
change for the primary agent.

Correctness depends on the firmware flushing and invalidating GPU caches for
every job, because activations, arguments, sums and tags reuse the same
addresses. That contract comes from the pinned driver and has not been shown
on this board. The image option for this path stays off in every image except
the validation image, which compares every output with the CPU path, until
the cache-contract, cross-core clean and read-only board items pass.

## Ownership, interrupts and cores

Ownership and the interrupt route are the dispatcher design's. The arena span
has one session-scoped ownership state: the CPU owns and writes it, the GPU
reads it, it is registered at OPEN and released at close, and it has no
per-job hand-off. The recognizer cannot write a job's activation range while
the job is in flight, because the hook submits and then waits for the
completion before touching that call's codes. The per-job clean provides
coherence, and the per-job extent check stays.

The GPU worker runs at EL1 on core 6 and performs all maintenance, because
`DC IVAC` is not available at EL0. Completion arrives by SPI 63 (INTID 95) on
core 6. In order: the worker invalidates the job's tags and sums, whose
trailing `DSB SY` completes the invalidate for every core; it posts the
completion; the recognizer compares every tag with the job's sequence; then it
reads out the sums.

- **GPU-only calls (`n_gpu = N`).** The recognizer runs the read-out through
  mimus's `ParallelFor` over rows on both inference workers, cores 6 and 7, as
  the dispatcher design does. The `ParallelFor` hand-off is a release
  followed by an acquire, so core 7 reads after the invalidate completed.
- **Channel splits (`n_gpu < N`).** Both workers compute the CPU channels
  through `ParallelFor`. After the join and the completion, core 6 reads out
  the GPU share alone.

Until delivery of INTID 95 is qualified, the worker's 4 ms timed wake finds
completions and adds up to 0.81 s over 203 jobs; the path then serves
bit-exactness runs only. Channel splits and timing records require qualified
delivery. With one job in flight, the GPU runtime's `max_job_us` counts from
the job's kick, and the recognizer's timeout, `T_answer + 10 ms` from the
dispatcher design, counts from its submit.

## Failure handling

- **Refusal, RESET.** The dispatcher design's classes apply. The CPU computes
  rows `[returned, rows)` over channels `[0, n_gpu)` only, because the CPU
  branch already computed, and for residual layers accumulated, channels
  `[n_gpu, N)`.
- **Tag mismatch.** The firmware reported OK for a job that did not run every
  lane, so lanes may still run. The recognizer reads nothing more from the
  output region and asks the worker to stop the GPU. The worker performs the
  GPU runtime's stop and waits for its acknowledgement, as for RESET, before
  any session range changes state. With the acknowledgement, the session
  closes, the output region stays DEVICE for the boot and is never read
  again, and every later call runs on the CPU. Without it within `T_stop`,
  the LOST rule applies. The CPU then computes the job's rows. With one job
  in flight, no other job's reply is outstanding.
- **LOST.** Recognition ends for the boot. The worker retains the model
  regions, the arena span, the output region, the program page, the job page
  and the session's GPU page tables, because a read-only mapping does not
  bound what firmware in an unknown state can write. The recognizer stays
  parked: it keeps sending its report within its 3 s window with the status
  "GPU lost" and touches no retained range. `board_fail` stays reserved for a
  job with no answer.

## CPU read-out of GPU sums

The read-out of each tensor must equal the CPU path that tensor takes on the
pinned AArch64 build (mimus 3e05158, compiled with `-ffp-contract=off`):

- **Plain calls** (subsampling convolution, feed-forward first linear, query,
  key, value, positional projection, pointwise convolution 1, output head)
  use `GemmCombine` in `gemm_combine_portable.cc`:
  `y = (0 + float(sum) × ws[n] × xs[r]) + (bias[n] or 0)`.
- **Residual calls** (feed-forward second linear with α = 0.5, attention
  output, pointwise convolution 2) use `MimusGemmS16Accumulate`:
  `y = y + α × value`.
- **The subsampling output** (K = 3,520, at most 32 rows) uses mimus's
  transposed path, `Int40ToFloat` then `Combine`. `Int40ToFloat` rounds the
  exact sum once with one fused multiply-add, so it gives the same float as
  `static_cast<float>`, and `Combine` applies the plain order.
- **A row post-function** (`RawReadout::post`), where the CPU path sets one,
  runs on each row after its outputs, as in `gemm_epilogue.cc`. A call with a
  block sink stays on the CPU unless `gpu-mimus-layers` shows the sink's
  output unchanged when it receives all columns at once.

The port adds one read-out entry, written as portable C++ over plain int32 or
int64 sum arrays and built with `-ffp-contract=off`. The compiler is expected
to emit `SCVTF` on 4S lanes for int32 sums and `SCVTF` on 2D lanes followed
by `FCVTN` for int64 sums, then the same `FMUL` and `FADD` sequence. The
2D conversion is exact because |sum| < 2^34 < 2^53, and `FCVTN` rounds once,
so both widths give the bit pattern of the scalar `static_cast<float>`. NEON
intrinsics are added only for a width whose compiled loop misses its budget.

## Time model and budgets

`checks/gpu-mimus-time-model.py` (new) replays every call on three timelines:
core 6, core 7 and the GPU. It reads the call list (rows, K, N, residual)
that `checks/gpu-mimus-layers` prints; until that check exists, it carries
the bench recording's 203 calls. Its named constants are the assumptions, and
board items replace them:

- the USC issues one 32-lane instruction per clock at 400 MHz, at 1.75
  instructions per MAC;
- per job, firmware overhead (kick to start, end to interrupt, cache flush)
  is 25 µs, and submit to kick and completion to wake take 25 µs each;
- a maintained cache line costs 20 cycles; handing a read-out half to core 7
  costs 10 µs;
- an A76 core does twice an A55's NEON MACs per clock and retires two
  read-out instructions per clock.

Rerunning the model with measured constants sets `n_gpu` for each shape. It
prints every time row below at 494 MHz and 2,002 MHz.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Output bytes differing from the CPU kernel | No GPU path | 0; checksums and text equal the host reference | `checks/gpu-mimus-layers` |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | Copy counter, same check |
| Jobs per recording, GPU only | Not possible: 3,968-byte output, 31 workgroups | 203, one per call | Same |
| USC instructions per MAC; code bytes | None | At most 1.75; at most 4,000 | `checks/gpu-mimus-program` |
| GPU compute, all channels | 14.894 s on the A55 | 0.585 s; 0.73 s or 0.92 s if an assumption fails | Time model; board |
| DRAM bytes read per MAC | None | 0.75 per lane; 0.156 per workgroup with cache reuse | `checks/gpu-mimus-layers` |
| Lines maintained on core 6 per recording, GPU only | None | 597,598 cleaned; 988,258 invalidated, tags included | `aarch64_device_cache` wrap counters, same check |
| `DSB SY` per job | None | 6, from three helper calls; 4 with a two-range clean | Barrier wraps in `checks/pvr-compute-service` |
| Tag words per recording | None | 818,176 | `checks/gpu-mimus-layers` |
| Read-out instructions per recording | Inside the CPU kernel | At most 43.3 M: 3 per int32 output, 4 per int64, 1 more per residual output; 155 M scalar | `checks/gpu-mimus-readout` under Unicorn, times outputs per call |
| Checks per job | Native resource admission and a CPU mapping check | At most 12 compares; no translation, no hash | `checks/pvr-compute-service` |
| Ring and record bytes per job | 368-byte bound | At most 300 | Same |
| Linear stage, GPU only | 14.894 s | 0.688 s; 0.623 s at 2,002 MHz | Time model; board |
| Linear stage, best channel split | 14.894 s | 0.639 s, 90% on the GPU; 0.403 s, 63%, at 2,002 MHz | Time model; board |
| GPU leaf entries | 4 KiB only, at most 256 table pages | 17: fifteen 2 MiB, two 4 KiB | `checks/pvr-gpu-vm.c` |

For comparison, the two A76 cores alone take 3.72 s at 494 MHz and 0.918 s
at 2,002 MHz. Only the channel split at 2,002 MHz meets the 0.4435 s goal.

**Instruction mix.** A 4 × 4 tile over 4 K steps does 64 integer
multiply-adds, 12 word loads, 32 signed extractions and about 4 loop
instructions: 1.75 instructions per MAC. That needs a single-instruction
signed bitfield extract and a full-rate 32-bit multiply-add. Two-instruction
extraction gives 2.19 per MAC (0.73 s); a half-rate multiply-add gives 2.75
(0.92 s).

**Bandwidth.** Without cache reuse each lane reads 12 bytes per 16 MACs:
3.2 GB per recording, 5.5 GB/s. Reusing a workgroup's 32 activation rows and
16 weight channels through the GPU caches reads 80 bytes per 512 MACs:
0.67 GB.

## Checks

`checks/gpu-mimus-layers` builds the existing `checks/mimus-linear.cc`
harness with a backend that runs the kernel source as C over its grid. It
enforces codes inside the arena span, `rows ≤ job_rows`, job boundaries on
32-row multiples, complete tags and total MACs within 0.06% of the profile.
It prints each call's rows, K, N, residual flag and read-out path for the
time model. It compares each tensor class's GPU read-out with that tensor's
CPU path. Cases:

- the bench shapes nominal, and with a refusal, a RESET and a LOST;
- a residual layer with `n_gpu < N` whose middle job fails, compared byte for
  byte with the CPU path;
- a job with one missing tag, followed by the stop, its acknowledgement and
  the CPU fallback;
- a tag area holding an earlier boot's tags for the same sequences before
  open, with one lane missing;
- negative chunk sums at K = 1,584 and 3,520;
- refusals of `rows == 0`, `rows = job_rows + 1`, sequence 0, an activation
  offset or stride not a multiple of 4, K not a multiple of 4 and a weight
  stride below K.

`checks/gpu-mimus-readout` runs the compiled read-out entry under Unicorn,
counts instructions per output for each width and tensor class, and compares
it bit for bit with the scalar CPU path. Its inputs include −0, ±(2^24 + 1),
the int32 limits, ±2^34, (2^24 + 1) × 2^k for k up to 9 and int64 sums built
from negative chunk carries. `checks/gpu-mimus-program` checks the linked USC
constant against the compile manifest and review record, counts inner-loop
instructions in the compiler's listing, and confirms that the tag store
follows every sum store and the global fence. None of these checks executes
USC code; that is a board item.

## Reused and removed

Reused: `backend::RunPackedI8Block`, mimus's NEON kernel, read-out order and
`ParallelFor` workers; the dispatcher's placement table and two-part hook; the
GPU runtime's sessions, slots, completions, deadline and reset; the
profile-three PDS and CDM construction; the compile tool and review record.

Removed for this session: copies of weights, codes and sums; the package
format and envelope; 32-word output paging; guard and poison fills, whose
missing-write detection the tags take over; the 31-workgroup cap; the
ACCEPTED reply; mimus's per-call weight expansion for GPU channels; the
second output region and the forced two-job split.

## Left out

- **Read-out of the GPU share on both A76 cores in channel-split calls.**
  mimus's `IntMatmul::SinkClaim` could carry it. The model gives a saving of
  17 ms at 494 MHz and 1.3 ms at 2,002 MHz.
- **More than one job in flight.** The forced split saves 0.8% at 2,002 MHz
  in the model; see "One job in flight".
- **Skipping the activation clean when codes are unchanged.** Every call
  quantizes its input, so the skip would never apply. It belongs with the
  port change that quantizes the shared input of the query, key and value
  projections once.
- **Local-memory staging, a float kernel, int16 weights for the GPU.** Each
  pays only if the board shows bandwidth, integer multiply rate or extraction
  limiting the loop.
- **Attention products, 8-bit activations, int4 weights.** The Orange Pi model
  uses int16 activations and int8 weights in every linear layer.

## Differences from the dispatcher design

- The 32-byte submit carries the job: session, slot, tensor ordinal,
  activation offset and stride, rows and sequence. No descriptor page exists;
  the arena span is 12 MiB.
- One job per call and one output region, at `0x46C00000`; the call split
  into two jobs in alternating slots, sequence-ordered waits and held early
  refusals are not built. The jobs per recording become 203, and the average
  job maintains 2,944 cleaned and 4,868 invalidated lines.
- `job_rows` is `min(floor(2^26 / (K × n_gpu)), floor((2 MiB − 64 KiB) /
  (n_gpu × width)), 32 × floor(512 / ceil(n_gpu / 16)))`, without rounding
  down to 32; only boundaries between the jobs of one call are multiples of
  32. The placement table tool computes the same bound that the worker
  checks.
- A tag mismatch stops the GPU as described above, is counted as
  `incomplete=` in the dispatch record, which also gains `wake=irq` or
  `wake=timed`, and puts every later call on the CPU for the boot.
- LOST ends recognition for the boot with a parked recognizer and retains the
  ranges listed above.
- Model uploads: before hashing, the recognizer asks the USB owner to latch
  slots 0 and 1. The USB owner refuses to latch while a slot 0 or 1 upload is
  begun and not committed. Once latched, it refuses Begin, Write and Commit
  for both slots with status 2 until reset. A failed hash releases the latch.
  No writer exists between the hash and OPEN, and the session close on upload
  and the reopen are removed.
- Both designs take their time figures from `checks/gpu-mimus-time-model.py`:
  0.403 s for the channel split at 2,002 MHz, in place of the dispatcher
  design's 0.39 s, and 0.688 s for GPU-only calls with the A76 pair at
  494 MHz. Both measure read-out with `checks/gpu-mimus-readout`.

## Changes needed from other owners

- **mimus port:** `RunPackedI8Block` for AArch64 from `LinearProduced`,
  `LinearProducedParallel` and `LinearProducedAccumulate`, with codes in the
  arena span; the tensor ordinal; 240-row subsampling blocks; the two-part hook
  with the fallback channel range; the read-out entry for each tensor class.
- **GPU runtime:** session ranges outside the pools with 2 MiB pages and
  2 MiB gaps; one reply per mimus submit; a stop request answered after its
  acknowledgement; a stated `T_stop`; `max_job_us` from the kick; the worker
  on the recognition core.
- **Memory architecture:** the session-scoped device-read span; the output
  region as a fixed span in `0x4220_0000`–`0x4800_0000`; retention after
  LOST and after a tag mismatch.
- **Native USB application:** the slot 0 and 1 latch above.
- **`checks/mimus-linear.cc` owner:** a backend compile definition, and
  read-out path and output counters per call.
- **Primary agent:** the INTID 95 route and priority in
  `platform/a733-smp-cached/irq_table.c`; A76 admission with the inference
  workers on cores 6 and 7; optionally, a two-range clean in
  `arch/aarch64/device_memory.c` with one leading and one trailing `DSB SY`.

## Board evidence that would confirm the design

- The bench recording's 203 jobs give output bytes identical to the CPU
  kernel and no tag mismatch, while cores 6 and 7 rewrite activations at the
  same addresses: this confirms the per-job GPU cache contract and the
  cross-core clean.
- From timeline timestamps: GPU time per layer shape (instructions per MAC,
  multiply-add rate, bandwidth) and firmware latency per job (kick to start,
  end to interrupt).
- On an A76 at each admitted clock: nanoseconds per cleaned and per
  invalidated line, per read-out output and per `ParallelFor` hand-off.
- CPU and GPU branch times per shape. Fed into the time model, they set
  `n_gpu` and decide whether the forced split returns.
- SPI 63 delivery for every job, with submit-to-kick plus completion-to-wake
  under 50 µs.
- A GPU store through a read-only mapping faults, a store into a gap ends in
  RESET, and the session's ranges use 2 MiB GPU pages.
