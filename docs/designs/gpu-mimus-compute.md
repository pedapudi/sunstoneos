# Design: exact mimus integer matrix products on the GPU

Status: **implemented** and **host-tested**; two exact-sum shapes are
**qualified on the board** through the returning caller
([gpu-exact-sum-caller.md](gpu-exact-sum-caller.md)); recognition through this
kernel is **open**. Two budgets are not met: the program has 3.39 USC
instruction groups per multiply-accumulate against 1.75, and 7,624 code bytes
against 4,000, so it takes two 4 KiB program pages. The pinned compiler emits
each signed field extraction as two groups and adds register moves. The
component's description is [gpu-mimus.md](../gpu-mimus.md); the kernel, job
adapter, read-out entry and checks live in the mimus checkout recorded in
`apps/mimus/source.json`, under its `sunstoneos/` directory. Host results and
planned board runs are in `docs/evidence/gpu-mimus-compute.jsonl`. The design
follows [performance-architecture.md](../performance-architecture.md).

The placement design for mimus linear layers
([recognition-dispatch.md](recognition-dispatch.md)) specifies the data path,
the placement table, the failure policy, supervision of the recognizer's waits
and the build gate. This design specifies what that design takes as given:
exact sums from 32-bit GPU arithmetic, the kernel, job construction without a
package, the GPU address layout, a per-job completion proof, the CPU read-out
of GPU sums, the time model and the checks. It depends on the GPU runtime and
memory architecture designs (`docs/designs/gpu-runtime.md`,
`docs/designs/memory-architecture.md`) and on the generic queue
([gpu-mimus-service.md](../gpu-mimus-service.md)).

A mimus linear layer multiplies int16 activation codes by int8 weight codes and
sums the products exactly. mimus then converts each sum to float once and
applies scales, a bias and, for residual layers, `y += alpha x value`, in a
fixed float order: the **read-out**. The GPU computes the sums and the CPU runs
the read-out, so output stays bit-identical to the host reference. On one
Cortex-A55 at 494 MHz the linear layers take 14.894 s of 19.476 s and perform
4,167,910,560 multiply-accumulates (MACs) ([mimus.md](../mimus.md)).

The recognition core pair is the dispatcher design's build constant: cores 6
and 7 in the A76 build, cores 2 and 3 otherwise. Below, core 6 and core 7 name
the first and second core of the pair.

## Exact sums from 32-bit GPU arithmetic

Activation codes satisfy |x| <= 32,767 and weight codes |w| <= 128, so a sum of
at most 512 products is at most 2,147,418,112, below 2^31, in any order. The
kernel sums the reduction length K in chunks of at most 512 into an int32 `c`
and keeps two 32-bit values per output across chunks: `lo`, the sum modulo
2^32, and `t`, the sum of `c >> 8` (arithmetic shift). With `E`, the sum of the
discarded low bytes `c & 255`, the exact sum is `256 t + E` with
0 <= E < 256 x chunks, and the high word is

    hi = (t >> 24) + ((uint32)(t << 8) > lo)

because `(uint32)(t << 8)` equals `(sum - E) mod 2^32`, which exceeds `lo` if
and only if adding `E` wrapped past 2^32. The rule is exact while K < 2^16
(`t` stays below 2^30 in magnitude, `E` below 2^16) and costs three operations
per chunk and output; a carry rule on a 64-bit pair costs five. A job writes
one int32 per output when K <= 512 (mimus's `fits32` case) and one int64
otherwise. Every sum satisfies |sum| <= 3,520 x 32,767 x 128 < 2^34.

## Kernel and job construction

- **Program.** `shaders/pvr_mimus_linear.cl` (in the mimus checkout) is
  compiled on the host by `apps/gpu-program/compile.py --session` with the
  profile-three register interface of
  [gpu-compiled-programs.md](../gpu-compiled-programs.md): the argument record
  and the output region, the local index, and workgroup identifiers and counts.
  Its USC code is linked into the application and copied into the program pages
  at session open. No package, input copy, output limit or sealed envelope
  applies.
- **Shape.** Each lane computes 4 rows x 4 channels; a 32-lane workgroup of
  local shape (4, 8) covers 16 channels x 32 rows, which pads the bench MACs by
  0.7%. Per 4 K steps a lane loads 12 aligned 32-bit words, extracts 32 signed
  bytes and halfwords and performs 64 multiply-adds. Load indices are clamped to
  the job's declared ranges; a sum is stored only when `row < rows` and
  `channel < n_gpu`.
- **Completion proof.** After its sum stores, every lane, padded lanes
  included, stores the job's 32-bit sequence number into its own tag word. The
  pinned compiler removes `mem_fence` from a 32-lane workgroup and emits
  nothing for it, so the kernel sets the issue-data-fence flag on every sum
  store; the compiler expands each into `st`, `idf` and `wdf`, and the tag store
  follows the last wait. A matching tag shows that the lane reached its tag
  store after its sum stores; the firmware's end-of-job flush makes both
  visible. Sequences start at 1 in each boot and continue across sessions, and
  the recognizer stops placing calls on the GPU before the counter wraps. The
  tag area is zeroed once per boot, before the CPU mapping of the output region
  becomes read-only, so every tag word holds 0 or a sequence of this boot. A
  missing tag catches a job that completed OK without running every lane: a
  partial grid, a dropped CDM kick or a firmware completion without execution.
- **Stores without branches.** A store to a channel outside the job, and the
  high word of an int32 sum, goes to the lane's own tag word, which the tag
  store then overwrites.
- **Argument record.** Thirty-two bytes: GPU address of channel 0's weight
  codes (words 0-1), address of row 0's activation codes (words 2-3), K with
  `n_gpu` in the high 16 bits (word 4), rows (5), weight stride with the
  activation stride in the high 16 bits, in bytes (6), and the sequence (7).
- **Job words.** `pvr_compute_job.c` reuses its profile-three PDS and CDM
  construction with per-job workgroup counts. The argument record and the CDM
  block share one 128-byte, line-aligned area of the job page, which the worker
  writes and cleans only while no job is in flight. The 31-workgroup cap stays
  for sealed package sessions.
- **Admission at open.** The adapter refuses a tensor whose K is not a multiple
  of 4, whose weight stride is below K, whose weight offset or stride is not a
  multiple of 4, or whose weight extent `offset + (n_gpu - 1) x stride + K` does
  not lie inside that tensor's model region, using the overflow-safe helpers of
  `a733_range.h`. The two model regions are adjacent in GPU addresses, so an
  unchecked extent could read the other one. It computes each tensor's
  `job_rows` as `min(floor(2^26 / (K x n_gpu)), floor((2 MiB - 64 KiB) /
  (n_gpu x width)), 32 x floor(512 / ceil(n_gpu / 16)))`: the 64 Mi MAC cap, the
  sum area and the 512 workgroups that fill the 64 KiB tag area. A tensor whose
  bound is below 32 runs on the CPU.
- **Admission at submit.** The adapter refuses `rows == 0`, `rows > job_rows`,
  sequence 0, an activation offset or row stride that is not a multiple of 4,
  and a stride below 2K, because a misaligned word load has no established GPU
  behavior. After the `rows <= job_rows` compare, the activation extent
  `offset + (rows - 1) x stride + 2K`, computed in 64 bits, must lie inside the
  arena span. The `rows <= job_rows` compare also bounds the workgroup count to
  512 and the sums to the sum area. These are at most 12 compares.

## One job in flight

Each call becomes one job, and a call with more than `job_rows` rows becomes
consecutive jobs split on 32-row boundaries. The session has one job in flight
and one output region. The bench recording's 204 calls are 204 jobs: the
largest, a 224-row subsampling block, has 62,447,616 MACs, below the 2^26 cap.

A second job in flight would need a second output region, sequence-ordered
waits and held early refusals. The time model gives it a 0.6% saving at the
2,002 MHz A76 pair, below the 2% rule in "Limitations".

## GPU address layout and cache contract

| Range | Size | GPU access |
| --- | --- | --- |
| Model regions at `0x43000000` and `0x43800000` | 8 MiB each | Read-only |
| Arena span at `0x46000000`, mimus's arena | 12 MiB | Read-only |
| Output region at `0x46C00000`: 64 KiB tag area, then sums | 2 MiB | Read-write while DEVICE |
| Program pages; job page | 8 KiB; 4 KiB | Read-only |

The GPU runtime maps the ranges ([gpu-mimus-service.md](../gpu-mimus-service.md)
gives the page sizes). An unmapped gap follows every range in GPU virtual
addresses, so a lane that overruns faults and its job ends in RESET. The time
model assumes the GPU TLB covers the 30 MiB that jobs stream and adds no
TLB-miss cost; a board item measures this.

**CPU mapping of the output region.** The OS image maps
`0x46C00000`-`0x47000000` read-only at EL0 and EL1
(`A733_GPU_OUTPUT_EL0_READ_ONLY`), because AArch64 stage-1 permissions cannot
combine EL1 write with EL0 read-only; the region has no CPU writer. The
architecture checks `DC IVAC` as a write, so the worker uses
clean-and-invalidate (`DC CIVAC`, `A733_GPU_OUTPUT_CLEAN_INVALIDATE`), which
needs read permission only and finds no dirty line. The start code zeroes the
tag area, stores and cleans it to the point of coherency before the read-only
mapping takes effect.

A job's tags occupy the top of the tag area, `[64 KiB - tags x 4, 64 KiB)`,
directly below its sums at `[64 KiB, 64 KiB + sums)`. Tag words never hold
sums, and one maintenance call covers both. Per job the worker makes three
`aarch64_device_cache` calls, six `DSB SY` in total. The Arm architecture
orders a data-cache operation by address only against accesses to the same line
from the same core, so the helper's leading `DSB SY` is the barrier that orders
two of these calls:

- clean the activation extent, rounded outward to 64-byte lines; the span is
  2 MiB aligned, so the rounded range stays inside it. Cores 6 and 7 write the
  codes through `ParallelFor`, and core 6 cleans them after the join. The
  leading barrier orders the clean after the join's acquire load; without it
  the clean can run before core 6 observes core 7's stores, and the GPU reads
  stale codes behind matching tags;
- clean the 128-byte job-word area, already line-aligned;
- after completion, clean and invalidate tags and sums, from a 128-byte-aligned
  start to the end rounded up to a line, inside the region. The leading barrier
  orders this after the worker's read of the completion record; without it a
  line can refill before the GPU's store lands and hold a stale sum next to a
  fresh tag.

A refusal from the helper is a class-1 refusal: no kick has happened, and the
call runs on the CPU.

Correctness depends on the firmware flushing and invalidating GPU caches for
every job, because activations, arguments, sums and tags reuse the same
addresses. That contract comes from the pinned driver and is not yet shown on
this board for recognition-sized runs. The image option for this path stays off
in every image except the validation image, which compares every output with the
CPU path, until the cache-contract, cross-core clean and read-only board items
pass.

## Ownership, interrupts and cores

Ownership and the interrupt route are the dispatcher design's. The arena span
has one session-scoped ownership state: the CPU owns and writes it, the GPU
reads it, it is registered at OPEN and released at close, and it has no per-job
hand-off. The recognizer cannot write a job's activation range while the job is
in flight, because the hook submits and then waits for the completion before
touching that call's codes. The per-job clean provides coherence, and the
per-job extent check stays.

The GPU worker runs at EL1 on core 6 and performs all maintenance. Completion
arrives by SPI 63 (INTID 95) on core 6. In order: the worker reads the
completion record; it cleans and invalidates the job's tags and sums behind the
helper's leading `DSB SY`, and the trailing `DSB SY` completes the maintenance
for every core; it posts the completion; the recognizer compares every tag with
the job's sequence; then it reads out the sums.

- **GPU-only calls (`n_gpu = N`).** The recognizer runs the read-out through
  mimus's `ParallelFor` over rows on cores 6 and 7. The `ParallelFor` hand-off
  is a release followed by an acquire, so core 7 reads after the maintenance
  completed. On the A55 pair the dispatcher design's table places every tensor
  GPU-only on the strength of the shared read-out: the 176 -> 704 layer at 222
  rows takes 6.25 ms GPU-only against 7.95 ms split.
- **Channel splits (`n_gpu < N`).** Both cores compute the CPU channels through
  `ParallelFor`. After the join and the completion, core 6 reads out the GPU
  share alone.

Until delivery of INTID 95 is qualified, the worker's 4 ms timed wake finds
completions and adds up to 0.82 s over 204 jobs. Placement does not depend on
qualification, so bit-exactness runs use the production table; only timing
records require qualified delivery. The dispatch record carries `wake=irq` or
`wake=timed` so timing records can exclude timed wakes.

## Failure handling

The dispatcher design's classes and policy apply:

- **Refusal before device access (class 1).** The CPU computes rows
  `[returned, rows)` over channels `[0, n_gpu)` only, because the CPU branch
  already computed, and for residual layers accumulated, channels `[n_gpu, N)`.
  The session stays open.
- **Tag mismatch (class 2).** The firmware reported OK for a job that did not
  run every lane, so lanes may still run. The recognizer reads nothing more from
  the output region, and the worker performs the GPU runtime's stop. After the
  acknowledgement, the hook calls `os_dispatch_incomplete`, and the CPU computes
  rows `[returned, rows)` over channels `[0, n_gpu)`, which includes the rows of
  the call's later jobs. Every later call runs on the CPU, and the output
  region is never read again in the boot. Without the acknowledgement within
  `T_stop`, the LOST path applies.
- **RESET or LOST (class 3).** The hook calls the dispatcher's failure callback
  before it computes any row on the CPU; the callback records the event and
  calls `board_fail`, and the watchdog resets the board. Until the reset, the
  GPU runtime's class-3 rule holds: SPI 63 masked, no further GPU or power I/O,
  and every lease, firmware page and GPU page table kept, which covers the
  model regions, the arena span and the output region.
- **No reply.** The recognizer has no timer. The GPU runtime's `max_job_us`,
  counted from the kick, plus its 4 ms wake ends every job in OK, RESET or LOST.
  A reply that never arrives expires the mimus service's 3 s window, which leads
  to `board_fail` and the watchdog reset.

## CPU read-out of GPU sums

The read-out of each tensor equals the CPU path that tensor takes on the pinned
AArch64 build (mimus 3e05158, compiled with `-ffp-contract=off`):

- **Plain calls** (subsampling convolution, feed-forward first linear, query,
  key, value, positional projection, pointwise convolution 1, output head) use
  `GemmCombine` in `gemm_combine_portable.cc`:
  `y = (0 + float(sum) x ws[n] x xs[r]) + (bias[n] or 0)`.
- **Residual calls** (feed-forward second linear with alpha = 0.5, attention
  output, pointwise convolution 2) use `MimusGemmS16Accumulate`:
  `y = y + alpha x value`.
- **The subsampling output** (K = 3,520, at most 32 rows) uses mimus's
  transposed path, `Int40ToFloat` then `Combine`. `Int40ToFloat` rounds the
  exact sum once with one fused multiply-add, so it gives the same float as
  `static_cast<float>`, and `Combine` applies the plain order.
- **A row post-function** (`RawReadout::post`), where the CPU path sets one,
  runs on each row after its outputs, as in `gemm_epilogue.cc`. A call with a
  block sink stays on the CPU unless the layers check shows the sink's output
  unchanged when it receives all columns at once.

The read-out entry (`pvr_mimus_readout_i32`, `_i64`, `_i32_accumulate`,
`_i64_accumulate`) is portable C++ over plain int32 or int64 sum arrays built
with `-ffp-contract=off`. The compiler emits `SCVTF` on 4S lanes for int32 sums
and `SCVTF` on 2D lanes followed by `FCVTN` for int64 sums, then the same `FMUL`
and `FADD` sequence. The 2D conversion is exact because |sum| < 2^34 < 2^53, and
`FCVTN` rounds once, so both widths give the bit pattern of the scalar
`static_cast<float>`. NEON intrinsics are added only for a width whose compiled
loop misses its budget.

## Time model

`checks/gpu-mimus-time-model.py` (in the mimus checkout) replays every call on
three timelines: core 6, core 7 and the GPU. It reads the call list (rows, K, N,
residual) that `checks/gpu-mimus-layers` prints. Its named constants are the
assumptions, and board items replace them:

- the USC issues one 32-lane instruction per clock at 400 MHz, at the measured
  instruction groups per MAC;
- memory latency adds a second bound: each 32-lane task waits L GPU clocks after
  each of its 12 loads per pass of 4 K steps and after each of its 32 sum
  stores, and R tasks are resident to hide those waits, where R follows from the
  program's temps and the unified store (8 at 106 temps; Mesa's formula for the
  BXM-4-64 gives 28,672 temps, 16 instances of 256 rows x 8 banks less two
  reserved 128-register lines). A task takes `max(issue, chain / R)` clocks, with
  `chain` its serial time, 217 + 12 L clocks per pass plus 32 L for the stores.
  The model prints issue-bound times, which are lower bounds, and times for L of
  50, 100, 200 and 400 clocks. With 8 tasks resident a pass is latency-bound
  above about 127 clocks, and above 112 temps fewer than 8 tasks fit;
- per job, firmware overhead (kick to start, end to interrupt, cache flush) is
  25 us, and submit to kick and completion to wake take 25 us each;
- a maintained cache line costs 20 cycles; handing a read-out half to core 7
  costs 10 us;
- an A76 core does twice an A55's NEON MACs per clock and retires two read-out
  instructions per clock.

Rerunning the model with measured constants sets `n_gpu` for each shape. A 4 x 4
tile over 4 K steps does 64 integer multiply-adds, 12 word loads, 32 signed
extractions and about 4 loop instructions: 1.75 instructions per MAC with a
single-instruction signed bitfield extract and a full-rate multiply-add;
two-instruction extraction gives 2.19 and a half-rate multiply-add 2.75.
Without cache reuse each lane reads 12 bytes per 16 MACs (3.1 GB per recording,
5.5 GB/s); reusing a workgroup's 32 activation rows and 16 weight channels
through the GPU caches reads 80 bytes per 512 MACs (0.65 GB).

## Qualified figures

All values are host-measured (builds, host checks and the time model; no USC
instruction has executed). The record is `gpu-mimus-compute-host` in
`docs/evidence/gpu-mimus-compute.jsonl`.

| Metric | Budget | Result | Met |
| --- | --- | --- | --- |
| Output bytes differing from mimus's CPU path | 0 | 0 of 12,923,134 bench outputs; 0 in every other case | Yes |
| Copies per weight, code and sum byte | 0, 0, 0 | 0, 0, 0 | Yes |
| Jobs per recording | 204 | 204 | Yes |
| USC instruction groups per MAC | At most 1.75 | 3.39 (217 groups per 64 MACs) | No |
| Program code bytes | At most 4,000 | 7,624, two 4 KiB program pages | No |
| Temps per instance | At most 112 (8 tasks resident) | 106 | Yes |
| GPU compute, all channels | 0.574 s issue-bound | At least 1.111 s; 1.726 s with 200-clock loads | No |
| DRAM bytes read per MAC, per lane | 0.75 | 0.75 | Yes |
| Lines maintained per recording, GPU only | 597,601 cleaned; 988,060 cleaned and invalidated | 597,397; 988,060 | Yes |
| `DSB SY` per job; tag words per recording | 6; 815,008 | 6; 815,008 | Yes |
| Read-out instructions per recording | At most 43.3 M | 37.6 M | Yes |
| Compares per job; translations; hashes | At most 12; none; none | 9; none; none | Yes |
| Ring and job-word bytes per job | At most 300 | 276 | Yes |
| Linear stage, best channel split, 494 MHz | 0.629 s | At least 0.997 s (80.6% of MACs on the GPU) | No |
| Linear stage, best channel split, 2,002 MHz | 0.396 s | At least 0.535 s (45.5%); misses the 0.4435 s pipeline goal | No |
| Linear stage, GPU only, 2,002 MHz | 0.612 s | At least 1.149 s | No |
| GPU leaf entries | 17 | 18 | No |
| `.text` of images that link `pvr_compute_job.c` | No growth | Compute service model 512 B smaller, render application 300 B smaller, others unchanged | Yes |

For comparison, the two A76 cores alone take 3.72 s at 494 MHz and 0.918 s at
2,002 MHz. The dispatcher design sets the linear stage's goal at 0.15 s; no
modeled placement meets it.

**Instruction-count cause.** The inner loop per 4 K steps of one lane holds 64
multiply-adds, 32 signed extractions, 32 selects the compiler adds after each
extraction for a zero field width, 42 register moves, 36 address adds, loads and
load waits, and 11 loop-control groups. The pinned compiler translates every
signed field extraction, including those written as two shifts, as `ibfe`
followed by a select on the field width, even when the width is the constant 8
or 16. Extraction with `mul_hi`, pointers incremented per load stream, a
separate per-row product sum and a 64-bit accumulator each failed to shorten
the loop; keeping the 64-bit base of each load stream in a register is in the
shipped loop. Two changes would reduce the count, and each needs board evidence
first: loads of 16 bytes (once the board shows their alignment rule) and shift
amounts passed at run time (the profile forbids the scratch memory a first
attempt used).

**Code size.** `image` in `pvr_compute_job.c` keeps `noinline`: without it the
compiler inlines one of its two calls into `pvr_compute_job_build` and the
object grows by 2,644 bytes. The shared encoders carry no `noinline`, because
as out-of-line calls they added 256 bytes of `.text` to the IRQ model and 324
bytes to the render application. The two grid encoders are `always_inline`, and
the word-copy loop is not unrolled so constant tables stay in `.rodata`.

## Checks

The mimus checkout holds `checks/gpu-mimus-layers`, `gpu-mimus-readout`,
`gpu-mimus-program` and `gpu-mimus-time-model`; [gpu-mimus.md](../gpu-mimus.md#checks)
lists their commands. None executes USC code or touches hardware.

- **Layers.** The kernel source runs as C over its grid against the bench
  recording's 204 calls, with the output region mapped `PROT_READ` outside the
  modeled GPU so a stray CPU store faults. The check fails unless a barrier
  separates the `ParallelFor` join from the activation clean and the completion
  read from the clean-and-invalidate, and it enforces codes inside the arena
  span, `rows <= job_rows`, 32-row job boundaries, complete tags and total MACs
  within 0.06% of the profile. Cases cover refusals of every admission rule,
  RESET and LOST before any CPU row, a three-job residual call with `n_gpu < N`
  whose middle job is refused or misses one tag, an earlier boot's tags with and
  without the boot clear, and negative chunk sums at K = 1,584 and 3,520.
- **Read-out.** The compiled read-out entry runs under Unicorn on edge inputs
  (-0, +-(2^24 + 1), the int32 limits, +-2^34, (2^24 + 1) x 2^k for k up to 9,
  int64 sums built from negative chunk carries) and matches the scalar CPU path
  bit for bit; it counts instructions per output.
- **Program.** The linked USC constant equals the compile manifest, the
  inner-loop instructions are counted from the compiler's listing, and the tag
  store follows every sum store and its data fence.
- **Shared code in this repository:** `checks/pvr-compute-service`
  and `checks/pvr-compute-job.py`.

## Limitations

A left-out performance change returns when the time model, fed with
board-measured constants, gives it a saving above 2% of the linear stage of the
best channel split on the A76 pair at 2,002 MHz, the configuration in which the
other stages fit the pipeline goal. Model figures at that clock:

- **A second job in flight:** 0.33% with the measured program (0.6% in the
  design model).
- **Read-out of the GPU share on both cores in channel splits:** 0.02% (2.8% at
  494 MHz in the design model). mimus's `IntMatmul::SinkClaim` could carry it.
- **A larger CPU-channel share on core 7, sized by core 6's cache maintenance:**
  0.00%, because core 6 mostly waits on the GPU.
- **Producer cores cleaning their own code lines before the join:** at most half
  of the 6.0 ms serial clean, 0.8%.
- **One maintenance pass per submit, or job words in the uncached pool:** at
  most two `DSB SY` per job, 408 per recording, under 0.1% even at 1 us each.

Also not provided:

- **Skipping the activation clean when codes are unchanged.** Every call
  quantizes its input, so the skip would never apply. It belongs with the port
  change that quantizes the shared input of the query, key and value
  projections once.
- **Local-memory staging, a float kernel, int16 weights for the GPU.** Each pays
  only if the board shows bandwidth, integer multiply rate or extraction
  limiting the loop.
- **Attention products, 8-bit activations, int4 weights.** The Orange Pi model
  uses int16 activations and int8 weights in every linear layer.
- **Real-GPU execution of the full recording.** The model assumes 8 resident
  tasks; if the hardware keeps fewer, the latency probe below shows it.

## Open work

1. **Recognition through the kernel.** Run the bench recording's 204 jobs on the
   board: output bytes identical to the CPU kernel and no tag mismatch, while
   cores 6 and 7 rewrite activations at the same addresses. This confirms the
   per-job GPU cache contract and the cross-core clean.
2. **Output-region mapping.** After boot the tag area reads zero, and `DC CIVAC`
   at EL1 on the read-only output region completes without a fault. A GPU store
   through a read-only mapping faults, a load or store into a gap ends in RESET,
   and the session's ranges use the planned GPU page sizes.
3. **Constants for the time model.** From timeline timestamps: GPU time per
   layer shape (instructions per MAC, multiply-add rate, bandwidth) against a
   single-load latency probe, which sets L and shows how many tasks stay
   resident; firmware latency per job; a job streaming 30 MiB against one
   streaming 2 MiB, which bounds the TLB-miss cost; on an A76 at each admitted
   clock, nanoseconds per cleaned and per invalidated line, per read-out output
   and per `ParallelFor` hand-off; CPU and GPU branch times per shape; SPI 63
   delivery for every job, with submit-to-kick plus completion-to-wake under 50
   us.
4. **Instruction count.** Reduce the 217 groups per pass of 4 K steps toward 112
   by 16-byte loads and run-time shift amounts once the board measurements in
   item 3 show they pay; the gap decides how many channels return to the CPU.
5. **mimus port.** `RunPackedI8Block` for AArch64 from `LinearProduced`,
   `LinearProducedParallel` and `LinearProducedAccumulate`, with codes in the
   arena span; the tensor ordinal; 224-row subsampling blocks; the two-part hook
   with the fallback channel range; the read-out entry for each tensor class;
   and a backend compile definition with per-call read-out path and output
   counters in `checks/mimus-linear.cc`.
6. **GPU runtime.** Session ranges outside the pools with gaps; one reply per
   mimus submit; a stop request answered after its acknowledgement; a stated
   `T_stop`; `max_job_us` from the kick; the worker on the recognition core.
7. **Memory architecture.** The session-scoped device-read span; the output
   region as a fixed span in `0x4220_0000`-`0x4800_0000`; retention after a tag
   mismatch.
8. **Primary agent.** A76 admission with the inference workers on cores 6 and 7;
   the tag area zeroed and cleaned in each boot before the read-only mapping
   takes effect.
