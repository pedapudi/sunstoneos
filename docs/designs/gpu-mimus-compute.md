# Design: exact mimus integer matrix products on the GPU

Status: **design**; no code exists. Each item ends at implemented and
host-modeled, with physical behavior pending until the board evidence at the
end exists. It follows [performance-architecture.md](../performance-architecture.md).

The placement design for mimus linear layers
([recognition-dispatch.md](recognition-dispatch.md), the dispatcher design
below) specifies the data path, the placement table, the failure policy,
supervision of the recognizer's waits and the build gate. This design
specifies what that design takes as given: exact sums from 32-bit GPU
arithmetic, the kernel, job construction without a package, the GPU address
layout, a per-job completion proof, the CPU read-out of GPU sums, the time
model and the checks. "Responses to the dispatcher design" answers each change
that design requests. This design also depends on the GPU runtime and memory
architecture designs (`docs/designs/gpu-runtime.md`,
`docs/designs/memory-architecture.md`).

A mimus linear layer multiplies int16 activation codes by int8 weight codes
and sums the products exactly. mimus then converts each sum to float once and
applies scales, a bias and, for residual layers, `y += alpha × value`, in a
fixed float order: the **read-out**. The GPU computes the sums and the CPU
runs the read-out, so output stays bit-identical to the host reference. On
one Cortex-A55 at 494 MHz the linear layers take 14.894 s of 19.476 s and
perform 4,167,910,560 multiply-accumulates (MACs) ([mimus.md](../mimus.md)).

The recognition core pair is the dispatcher design's build constant: cores 6
and 7 in the A76 build, cores 2 and 3 otherwise. Below, core 6 and core 7 name
the first and second core of the pair.

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
  by 0.7%. Lanes load 32-bit words and extract signed bytes and halfwords.
  Load indices are clamped to the job's declared ranges; a sum is stored only
  when `row < rows` and `channel < n_gpu`.
- **Completion proof.** After its sum stores, every lane, padded lanes
  included, executes `mem_fence(CLK_GLOBAL_MEM_FENCE)` and then stores the
  job's 32-bit sequence number into its own tag word. A matching tag shows
  that the lane reached its tag store after its sum stores; the firmware's
  end-of-job flush (below) makes both visible. Sequences start at 1 in each
  boot and continue across sessions, and the recognizer stops placing calls
  on the GPU before the counter wraps. The tag area is zeroed once per boot,
  before the CPU mapping of the output region becomes read-only (next
  section), so every tag word holds 0 or a sequence of this boot. A missing
  tag catches a job that completed OK without running every lane: a partial
  grid, a dropped CDM kick or a firmware completion without execution.
- **Job words.** `pvr_compute_job.c` reuses its profile-three PDS and CDM
  construction with per-job workgroup counts. The 32-byte argument record and
  the CDM block share one 128-byte, line-aligned area of the job page, which
  the worker writes and cleans only while no job is in flight. The
  31-workgroup cap stays for sealed package sessions.
- **Admission at open.** The worker refuses a tensor whose K is not a
  multiple of 4, whose weight stride is below K, whose weight offset or
  stride is not a multiple of 4, or whose weight extent
  `offset + (n_gpu − 1) × stride + K` does not lie inside that tensor's model
  region, checked with the overflow-safe helpers of `a733_range.h`. The two
  model regions are adjacent in GPU addresses, so an unchecked extent could
  read the other one. The worker computes each tensor's `job_rows` as
  `min(floor(2^26 / (K × n_gpu)), floor((2 MiB − 64 KiB) / (n_gpu × width)),
  32 × floor(512 / ceil(n_gpu / 16)))`: the 64 Mi MAC cap, the sum area and
  the 512 workgroups that fill the 64 KiB tag area. A tensor whose bound is
  below 32 runs on the CPU.
- **Admission at submit.** The worker refuses `rows == 0`, `rows > job_rows`,
  sequence 0, an activation offset or row stride that is not a multiple of 4,
  and a stride below 2K. Lanes load 32-bit words, and a misaligned word load
  has no established GPU behavior. After the `rows ≤ job_rows` compare, the
  activation extent `offset + (rows − 1) × stride + 2K`, computed in 64 bits
  with the helpers of `a733_range.h`, must lie inside the arena span. The
  `rows ≤ job_rows` compare also bounds the workgroup count to 512 and the
  sums to the sum area. A host case submits `job_rows + 1` rows for every
  tensor. These are at most 12 compares.

## One job in flight

Each call becomes one job, and a call with more than `job_rows` rows becomes
consecutive jobs split on 32-row boundaries. The session has one job in
flight and one output region. The bench recording's 204 calls are 204 jobs:
the largest, a 224-row subsampling block, has 62,447,616 MACs, below the
2^26 cap.

A second job in flight, with the first job's read-out on core 6 overlapping
the second job, needs a second output region, sequence-ordered waits and
held early refusals. For the best channel split on the A76 pair, the time
model gives it 0.629 s → 0.593 s (5.8%) at 494 MHz and 0.396 s → 0.394 s
(0.6%) at 2,002 MHz. The dispatcher design's model gives no gain on the A55
pair (0.813 s against 0.812 s). It is left out under the rule in "Left out":
the A76 pair at 2,002 MHz is the only configuration whose other stages fit
the 0.4435 s pipeline goal, and the 494 MHz configuration misses that goal
with or without the second job.

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
fifteen 2 MiB pages and two 4 KiB pages. The time model assumes that, with
2 MiB pages, the GPU TLB covers the 30 MiB that jobs stream and adds no
TLB-miss cost; a board item measures this.

**CPU mapping of the output region.** The primary agent maps
`0x46C00000`–`0x47000000` read-only at EL0 (`A733_GPU_OUTPUT_EL0_READ_ONLY`).
AArch64 stage-1 permissions cannot combine EL1 write with EL0 read-only, so
the region is read-only at EL1 too and has no CPU writer. The architecture
checks `DC IVAC` as a write, so the worker uses clean-and-invalidate
(`DC CIVAC`, `A733_GPU_OUTPUT_CLEAN_INVALIDATE`), which needs read permission
only and finds no dirty line. Zeroing the tag area therefore falls to the
primary agent's start code, which stores zeros over it and cleans them to the
point of coherency before the read-only mapping takes effect.

A job's tags occupy the top of the tag area, `[64 KiB − tags × 4, 64 KiB)`,
directly below its sums at `[64 KiB, 64 KiB + sums)`. Tag words therefore
never hold sums, and one maintenance call covers both. Per job the worker
makes three `aarch64_device_cache` calls, six `DSB SY` in total, and three
once the primary agent removes the helper's leading `DSB SY`:

- clean the activation extent, rounded outward to 64-byte lines; the span is
  2 MiB aligned, so the rounded range stays inside it;
- clean the 128-byte job-word area, already line-aligned;
- after completion, clean and invalidate tags and sums, from a
  128-byte-aligned start to the end rounded up to a line, inside the region.

A refusal from the helper is a class-1 refusal: no kick has happened, and the
call runs on the CPU.

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

The GPU worker runs at EL1 on core 6 and performs all maintenance. Completion
arrives by SPI 63 (INTID 95) on core 6. In order: the worker cleans and
invalidates the job's tags and sums, whose trailing `DSB SY` completes the
maintenance for every core; it posts the completion; the recognizer compares
every tag with the job's sequence; then it reads out the sums.

- **GPU-only calls (`n_gpu = N`).** The recognizer runs the read-out through
  mimus's `ParallelFor` over rows on cores 6 and 7. The `ParallelFor`
  hand-off is a release followed by an acquire, so core 7 reads after the
  maintenance completed. This path stays because on the A55 pair, the core
  set admitted today, the dispatcher design's table places every tensor
  GPU-only on the strength of the shared read-out: the 176 → 704 layer at
  222 rows takes 6.25 ms GPU-only against 7.95 ms split.
- **Channel splits (`n_gpu < N`).** Both cores compute the CPU channels
  through `ParallelFor`. After the join and the completion, core 6 reads out
  the GPU share alone.

Until delivery of INTID 95 is qualified, the worker's 4 ms timed wake finds
completions and adds up to 0.82 s over 204 jobs. Placement does not depend on
qualification, so bit-exactness runs use the production table; only timing
records require qualified delivery.

## Failure handling

The dispatcher design's classes and policy apply:

- **Refusal before device access (class 1).** The CPU computes rows
  `[returned, rows)` over channels `[0, n_gpu)` only, because the CPU branch
  already computed, and for residual layers accumulated, channels
  `[n_gpu, N)`. The session stays open.
- **Tag mismatch (class 2).** The firmware reported OK for a job that did not
  run every lane, so lanes may still run. The recognizer reads nothing more
  from the output region, and the worker performs the GPU runtime's stop.
  After the acknowledgement, the hook calls `os_dispatch_incomplete`, and the
  CPU computes rows `[returned, rows)` over channels `[0, n_gpu)`, which
  includes the rows of the call's later jobs. Every later call runs on the
  CPU, and the output region is never read again in the boot. Without the
  acknowledgement within `T_stop`, the LOST path applies.
- **RESET or LOST (class 3).** The hook calls the dispatcher's failure
  callback before it computes any row on the CPU; the callback records the
  event and calls `board_fail`, and the watchdog resets the board. Until the
  reset, the GPU runtime's class-3 rule holds: SPI 63 masked, no further GPU
  or power I/O, and every lease, firmware page and GPU page table kept, which
  covers the model regions, the arena span and the output region.
- **No reply.** The recognizer has no timer. The GPU runtime's `max_job_us`,
  counted from the kick, plus its 4 ms wake ends every job in OK, RESET or
  LOST. A reply that never arrives expires the mimus service's 3 s window,
  which leads to `board_fail` and the watchdog reset.

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

`checks/gpu-mimus-time-model.py` replays every call on three timelines:
core 6, core 7 and the GPU. It reads the call list (rows, K, N, residual)
that `checks/gpu-mimus-layers` prints; until that check exists, it carries
the bench recording's 204 calls. Its named constants are the assumptions, and
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
prints every time row below for the A76 pair at 494 MHz and 2,002 MHz, and
the saving of each left-out change.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Output bytes differing from the CPU kernel | No GPU path | 0; checksums and text equal the host reference | `checks/gpu-mimus-layers` |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | Copy counter, same check |
| Jobs per recording | Not possible: 3,968-byte output, 31 workgroups | 204, one per call | Same |
| USC instructions per MAC; code bytes | None | At most 1.75; at most 4,000 | `checks/gpu-mimus-program` |
| GPU compute, all channels | 14.894 s on the A55 | 0.574 s; 0.72 s or 0.90 s if an assumption fails | Time model; board |
| DRAM bytes read per MAC | None | 0.75 per lane; 0.156 per workgroup with cache reuse | `checks/gpu-mimus-layers` |
| Lines maintained on core 6 per recording, GPU only | None | 597,601 cleaned; 988,060 cleaned and invalidated, tags included | `aarch64_device_cache` wrap counters, same check |
| `DSB SY` per job | None | 6, from three helper calls; 3 without the leading barrier | Barrier wraps in `checks/pvr-compute-service` |
| Tag words per recording, GPU only | None | 815,008 | `checks/gpu-mimus-layers` |
| Read-out instructions per recording | Inside the CPU kernel | At most 43.3 M: 3 per int32 output, 4 per int64, 1 more per residual output; 155 M scalar | `checks/gpu-mimus-readout` under Unicorn, times outputs per call |
| Checks per job | Native resource admission and a CPU mapping check | At most 12 compares; no translation, no hash | `checks/pvr-compute-service` |
| Ring and record bytes per job | 368-byte bound | At most 300 | Same |
| Linear stage, best channel split (the selected placement) | 14.894 s | 0.629 s, 90% of MACs on the GPU, at 494 MHz; 0.396 s, 63%, at 2,002 MHz | Time model; board |
| Linear stage, GPU only (reference) | 14.894 s | 0.677 s at 494 MHz; 0.612 s at 2,002 MHz | Time model |
| GPU leaf entries | 4 KiB only, at most 256 table pages | 17: fifteen 2 MiB, two 4 KiB | `checks/pvr-gpu-vm.c` |

For comparison, the two A76 cores alone take 3.72 s at 494 MHz and 0.918 s
at 2,002 MHz. The dispatcher design sets the linear stage's goal at 0.15 s;
no modeled placement meets it.

**Instruction mix.** A 4 × 4 tile over 4 K steps does 64 integer
multiply-adds, 12 word loads, 32 signed extractions and about 4 loop
instructions: 1.75 instructions per MAC. That needs a single-instruction
signed bitfield extract and a full-rate 32-bit multiply-add. Two-instruction
extraction gives 2.19 per MAC (0.72 s); a half-rate multiply-add gives 2.75
(0.90 s).

**Bandwidth.** Without cache reuse each lane reads 12 bytes per 16 MACs:
3.1 GB per recording, 5.5 GB/s. Reusing a workgroup's 32 activation rows and
16 weight channels through the GPU caches reads 80 bytes per 512 MACs:
0.65 GB.

## Checks

`checks/gpu-mimus-layers` builds the existing `checks/mimus-linear.cc`
harness with a backend that runs the kernel source as C over its grid. The
host maps the output region with `PROT_READ` outside the modeled GPU, so a
CPU store into it faults the check, and the check fails if the completion
maintenance is anything but clean-and-invalidate. It enforces codes inside
the arena span, `rows ≤ job_rows`, job boundaries on 32-row multiples,
complete tags and total MACs within 0.06% of the profile. It prints each
call's rows, K, N, residual flag and read-out path for the time model. It
compares each tensor class's GPU read-out with that tensor's CPU path. Cases:

- the bench shapes nominal; with a refusal; and with a RESET and a LOST, each
  calling the failure callback before any CPU row;
- a residual layer with `n_gpu < N` in a three-job call whose middle job is
  refused, and the same call with one missing tag in the middle job followed
  by the stop and its acknowledgement, each compared byte for byte with the
  CPU path;
- a tag area holding an earlier boot's tags for the same sequences, then the
  boot clear, then a job with one lane missing;
- a stray CPU store into the output region, which faults;
- negative chunk sums at K = 1,584 and 3,520;
- refusals of `rows == 0`, `rows = job_rows + 1`, sequence 0, an activation
  offset or stride not a multiple of 4, K not a multiple of 4, a weight
  stride below K and a weight extent that reaches the other model region.

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
second output region and the second job in flight.

## Left out

A left-out performance change returns when the time model, fed with
board-measured constants, gives it a saving above 2% of the linear stage of
the best channel split on the A76 pair at 2,002 MHz, the configuration in
which the other stages fit the pipeline goal. Model figures at that clock:

- **A second job in flight:** 0.6%; see "One job in flight".
- **Read-out of the GPU share on both cores in channel splits:** 0.3%
  (2.8% at 494 MHz). mimus's `IntMatmul::SinkClaim` could carry it.
- **A larger CPU-channel share on core 7, sized by core 6's cache
  maintenance:** 0.1%, because core 6 mostly waits on the GPU.
- **Producer cores cleaning their own code lines before the join:** at most
  half of the 6.0 ms serial clean, 0.8%.
- **One maintenance pass per submit, or job words in the uncached pool:** at
  most two `DSB SY` per job, 408 per recording, under 0.1% even at 1 µs each.

Also left out:

- **Skipping the activation clean when codes are unchanged.** Every call
  quantizes its input, so the skip would never apply. It belongs with the
  port change that quantizes the shared input of the query, key and value
  projections once.
- **Local-memory staging, a float kernel, int16 weights for the GPU.** Each
  pays only if the board shows bandwidth, integer multiply rate or extraction
  limiting the loop.
- **Attention products, 8-bit activations, int4 weights.** The Orange Pi model
  uses int16 activations and int8 weights in every linear layer.

## Responses to the dispatcher design

- **Supervision in place of a recognizer timeout:** adopted; see "No reply".
- **LOST, like RESET, ending in `board_fail` and the watchdog reset:** adopted.
- **A failure callback before any CPU row on RESET or LOST, and
  `os_dispatch_incomplete` after the stop acknowledgement:** adopted.
- **Clean-and-invalidate on the output region:** adopted.
- **The tag area cleared without a CPU store at open:** adopted as a
  once-per-boot clear by the start code before the read-only mapping,
  because a GPU fill job would be a second job type with no completion proof
  of its own.
- **Subsampling calls of 224 rows (last 184):** adopted; 204 jobs.
- **`rows == 0` refused and the extent computed in 64 bits after the
  `rows ≤ job_rows` compare:** adopted.
- **Producer-side cleans; one maintenance pass or uncached job words:** left
  out under the 2% rule.
- **No model-upload class in the OS image:** adopted; this design has no
  upload latch.
- **Time figures:** the dispatcher's placement tool counts 12 read-out
  instructions per sum and gives 0.414 s on the A76 pair at 2,002 MHz; this
  model counts 3 to 5 and gives 0.396 s. `checks/gpu-mimus-readout` settles
  the count, and both tools then use it.
- **Dispatch record:** it gains `wake=irq` or `wake=timed`, so that timing
  records can exclude timed wakes.

## Changes needed from other owners

- **mimus port:** `RunPackedI8Block` for AArch64 from `LinearProduced`,
  `LinearProducedParallel` and `LinearProducedAccumulate`, with codes in the
  arena span; the tensor ordinal; 224-row subsampling blocks; the two-part
  hook with the fallback channel range; the read-out entry for each tensor
  class.
- **GPU runtime:** session ranges outside the pools with 2 MiB pages and
  2 MiB gaps; one reply per mimus submit; a stop request answered after its
  acknowledgement; a stated `T_stop`; `max_job_us` from the kick; the worker
  on the recognition core.
- **Memory architecture:** the session-scoped device-read span; the output
  region as a fixed span in `0x4220_0000`–`0x4800_0000`; retention after a
  tag mismatch.
- **`checks/mimus-linear.cc` owner:** a backend compile definition, and
  read-out path and output counters per call.
- **Primary agent:** A76 admission with the inference workers on cores 6 and
  7; `0x46C00000`–`0x47000000` read-only at EL0; the tag area zeroed and
  cleaned in each boot before that mapping takes effect; removal of the
  leading `DSB SY` in `aarch64_device_cache`.

## Board evidence that would confirm the design

- The bench recording's 204 jobs give output bytes identical to the CPU
  kernel and no tag mismatch, while cores 6 and 7 rewrite activations at the
  same addresses: this confirms the per-job GPU cache contract and the
  cross-core clean.
- After boot the tag area reads zero, and `DC CIVAC` at EL1 on the read-only
  output region completes without a fault.
- From timeline timestamps: GPU time per layer shape (instructions per MAC,
  multiply-add rate, bandwidth) and firmware latency per job (kick to start,
  end to interrupt); and a job streaming 30 MiB through 2 MiB pages against
  one streaming 2 MiB, which bounds the TLB-miss cost.
- On an A76 at each admitted clock: nanoseconds per cleaned and per
  invalidated line, per read-out output and per `ParallelFor` hand-off.
- CPU and GPU branch times per shape. Fed into the time model, they set
  `n_gpu` and the savings of the left-out changes.
- SPI 63 delivery for every job, with submit-to-kick plus completion-to-wake
  under 50 µs.
- A GPU store through a read-only mapping faults, a load or store into a gap
  ends in RESET, and the session's ranges use 2 MiB GPU pages.
