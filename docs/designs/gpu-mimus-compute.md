# Design: exact mimus integer matrix products on the GPU

Status: **design**; no code exists. Each item ends at implemented and
host-modeled, with physical behavior pending until the board evidence at the
end exists. It follows [performance-architecture.md](../performance-architecture.md).

The placement design for mimus linear layers
(`docs/designs/recognition-dispatch.md` on branch `audit/recognition-dispatch`,
the dispatcher design below) specifies the data path, placement table,
deadlines, failure handling and cross-core synchronization. This design
specifies what that design takes as given: exact sums from 32-bit GPU
arithmetic, the kernel, job construction without a package, the GPU address
layout, a per-job completion proof, the CPU read-out of GPU sums and the
checks. Its changes to the dispatcher design are listed as differences. It
also depends on the unmerged GPU runtime and memory architecture designs
(`docs/designs/gpu-runtime.md`, `docs/designs/memory-architecture.md`).

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
output when K ≤ 512 (mimus's `fits32` case) and one int64 otherwise.

## Kernel and job construction

- **Program.** `shaders/pvr_mimus_linear.cl` is compiled on the host by
  `apps/gpu-program/compile.py` with the profile-three register interface of
  [gpu-compiled-programs.md](../gpu-compiled-programs.md): the slot's
  argument record and output region, the local index, and workgroup
  identifiers and counts. Its USC code, at most 4,000 bytes, is linked into
  the image and copied into the program page at session open. No package,
  input copy, output limit or sealed envelope applies.
- **Shape.** Each lane computes 4 rows × 4 channels; a 32-lane workgroup of
  local shape (4, 8) covers 16 channels × 32 rows, which pads the bench MACs by
  2.6%. Lanes load 32-bit words and extract signed bytes and halfwords. Load
  indices are clamped to the job's declared ranges; a sum is stored only when
  `row < rows` and `channel < n_gpu`.
- **Completion proof.** After its sum stores, every lane, padded lanes
  included, stores the job's 32-bit sequence number into its own tag word in
  the slot. Before read-out the recognizer compares every tag of the job with
  the sequence it submitted. Sequences do not repeat within a boot, so no fill
  is needed. A mismatch catches a job that completed OK without running every
  lane (a partial grid, a dropped CDM kick, a firmware completion without
  execution), whose read-out would otherwise consume the previous job's sums.
- **Job words.** `pvr_compute_job.c` reuses its profile-three PDS and CDM
  construction with per-job workgroup counts. Each slot has its own 32-byte
  argument record and CDM block, written and cleaned only while the slot is
  CPU. The 31-workgroup cap stays for sealed package sessions.
- **Admission.** At open, refuse a tensor whose K is not a multiple of 4,
  whose weight stride is below K, or whose weight offset or stride is not a
  multiple of 4; word loads would otherwise add bytes outside the row. At
  submit, refuse `rows == 0`, whose zero-workgroup kick has no established
  behavior. The activation extent `offset + (rows − 1) × stride + 2K` and the
  sum extent `rows × n_gpu × width` are checked with the overflow-safe
  helpers of `a733_range.h`.

## GPU address layout and cache contract

Cache maintenance per range is the dispatcher design's ("Memory placement and
cache policy"), without the descriptor page.

| Range | Size | GPU access |
| --- | --- | --- |
| Model regions at `0x43000000` and `0x43800000` | 16 MiB | Read-only |
| Arena span at `0x46000000`, mimus's arena | 12 MiB | Read-only |
| Two output regions: sums, then a 64 KiB tag area | 2 MiB each | Read-write while DEVICE |
| Program page; argument record and CDM block per slot | 4 KiB each | Read-only |

An unmapped 2 MiB gap follows each range in GPU virtual addresses, including
between the output regions, so a lane that overruns faults and its job ends
in RESET. `job_rows` is bounded by the 2 MiB − 64 KiB sum area and by 512
workgroups, so tags fit at any utterance length; the largest bench job writes
180,224 sum and 33,280 tag bytes. The session uses 8,192 leaf entries with
4 KiB GPU pages, 16 with 2 MiB pages.

Correctness depends on the firmware flushing and invalidating GPU caches for
every job, because activations, arguments, sums and tags reuse the same
addresses. That contract comes from the pinned driver and has not been shown
on this board. The image option for this path stays off in every image except
the validation image, which compares every output with the CPU path, until
the cache-contract, cross-core clean and read-only board items pass.

## Ownership, interrupts and cores

Ownership, the interrupt route and cross-core ordering are the dispatcher
design's ("One data path, with zero copies"; "Interrupt, cores and
synchronization"). The arena span needs one session-scoped ownership state:
a fixed span the CPU owns and writes and the GPU reads, registered at OPEN
and released at close. No per-job hand-off exists. The recognizer cannot
write a job's activation range while the job is in flight because the hook
submits and then waits for the completion before touching that call's codes.
The per-job clean provides coherence, and the per-job extent check stays.

Completion arrives by SPI 63 (INTID 95) on the recognition core. Until its
delivery is qualified, the worker's 4 ms timed wake finds completions and adds
up to 1.6 s over 399 jobs; the path then serves bit-exactness runs only.
Channel splits (`n_gpu < N`) and timing records require qualified delivery.

The GPU runtime's `max_job_us` counts from the job's kick and is four times
the largest measured job, so a correct job queued behind another finishes in
half its deadline. The recognizer's timeout, `T_answer + 10 ms` from the
dispatcher design, counts from its own submit. If the earlier of two queued
jobs hangs, its deadline expires at most `T_clean + max_job_us + 4 ms` after
the later submit, where `T_clean` is the dispatcher's largest activation
clean, and the reset answers both within `T_answer`.

## CPU read-out of GPU sums

The pinned mimus source (3e05158) reads out AArch64 sums in portable C++ with
per-lane loops (`gemm_epilogue.cc`) and no NEON intrinsics; whether the
compiler vectorizes them is not established. GPU sums arrive as plain int32
or int64 arrays, so the port adds one read-out entry that applies mimus's
`Store` order to them. For int32 sums it handles four outputs per step:
`SCVTF` on 4S lanes, then the same `FMUL` and `FADD` sequence, including the
add of +0 that turns −0 into +0, with no fused multiply-add. Each lane rounds
once, as the scalar code does, so the floats are bit-identical. The int64 path
stays scalar, because vector int64-to-float conversion rounds twice. Of
12,923,134 bench outputs, 10,852,318 are int32.

## Time model and budgets

Assumptions, each replaced by a board item: the USC issues one 32-lane
instruction per clock at 400 MHz; firmware overhead per job (kick to start,
end to interrupt, cache flush) is 25 µs; submit to kick and completion to
wake take 25 µs each; a maintained cache line costs 20 cycles; an A76 core
does twice an A55's NEON MACs per clock and retires two read-out
instructions per clock. A76 figures are at 494 MHz and at 2,002 MHz.

Each core has its own timeline. Core 6 runs the recognizer, the worker and the
interrupt; core 7 runs the second `ParallelFor` half.

- **GPU only (`n_gpu = N`, the dispatcher's starting point).** Per call, core
  6 cleans and submits job 1 (exposed), then cleans and submits job 2 while
  the GPU runs. It checks tags, invalidates and reads out job 1 while the GPU
  computes job 2; job 2's read-out is exposed. Core 7 is idle. The split into
  two jobs adds 196 jobs and hides about half of each call's read-out and
  maintenance.
- **Channel split (`n_gpu < N`).** `ParallelFor` splits the CPU channels in
  half (`apps/mimus/parallel_runtime.cc`). Core 6 also runs the activation
  clean and the sum invalidate, so core 7 waits at the join for that time. The
  recognizer stays in the CPU branch until the join, and the GPU share's
  read-out follows it on core 6, so a second job would hide nothing. Split
  calls use one job.

`n_gpu` changes only after the board item that measures branch times per
shape; the model is the prediction those measurements are compared with.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Output bytes differing from the CPU kernel | No GPU path | 0; checksums and text equal the host reference | `checks/gpu-mimus-layers` |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | Copy counter, same check |
| Jobs per recording, GPU only | Not possible: 3,968-byte output, 31 workgroups | 399 (203 calls, 196 split) | Same |
| USC instructions per MAC; code bytes | None | At most 1.75; at most 4,000 | `checks/gpu-mimus-program` |
| GPU compute, all channels | 14.894 s on the A55 | 0.585 s; 0.73 s or 0.92 s if an assumption fails | Time model; board |
| DRAM bytes read per MAC | None | 0.75 per lane; 0.156 per workgroup with cache reuse | `checks/gpu-mimus-layers` |
| Lines maintained on core 6 per recording | None | 1,585,247: 597k code, 937k sum, 51k tag | `aarch64_device_cache` wrap |
| Tag words per recording | None | 818,176 | `checks/gpu-mimus-layers` |
| Read-out instructions per recording | Inside the CPU kernel | At most 57.4 M (3 per int32 output, 12 per int64), against 155 M scalar | Outputs per call in `checks/mimus-linear.cc`; instructions per output from the disassembly |
| Checks per job | Native resource admission and a CPU mapping check | At most 12 compares; no translation, no hash | `checks/pvr-compute-service` |
| Ring and record bytes per job | 368-byte bound | At most 300 | Same |
| Linear stage, GPU only | 14.894 s | 0.662 s; 0.619 s at 2,002 MHz | Time model; board |
| Linear stage, GPU only, one job per call | 14.894 s | 0.722 s; 0.630 s at 2,002 MHz | Time model; board |
| Linear stage, best channel split | 14.894 s | 0.654 s, 90% on the GPU; 0.431 s, 57%, at 2,002 MHz | Time model; board |
| GPU leaf entries | 4 KiB only, at most 256 table pages | 8,192; 16 with 2 MiB pages | `checks/pvr-gpu-vm.c` |

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
enforces invariants: codes inside the arena span, `rows ≤ job_rows`, job
boundaries on 32-row multiples, complete tags, and total MACs within 0.06% of
the profile. It prints each call's rows, K, N and jobs as evidence for the
budget rows. Cases:

- the bench shapes nominal and with a refusal, a RESET and a LOST;
- a residual layer with `n_gpu < N` whose middle job fails, compared byte for
  byte with the CPU path;
- a job with one missing tag;
- negative chunk sums at K = 1,584 and 3,520;
- `rows == 0`, K not a multiple of 4 and a weight stride below K, each refused;
- the int32 NEON read-out against the scalar one, bit for bit, including −0,
  ±(2^24 + 1) and the int32 limits;
- an activation write between two calls over the same range forcing a clean.

`checks/gpu-mimus-program` checks the linked USC constant against the compile
manifest and review record and counts inner-loop instructions in the
compiler's listing. Neither check executes USC code; that is a board item.

## Reused and removed

Reused: `backend::RunPackedI8Block`, mimus's NEON kernel, read-out order and
`ParallelFor` workers; the dispatcher's placement table and two-part hook; the
GPU runtime's sessions, slots, completions, deadline and reset; the
profile-three PDS and CDM construction; the compile tool and review record.

Removed for this session: copies of weights, codes and sums; the package
format and envelope; 32-word output paging; guard and poison fills, whose
missing-write detection the tags take over; the 31-workgroup cap; the
ACCEPTED reply; mimus's per-call weight expansion for GPU channels.

## Left out

- **Read-out on both A76 cores with chunked CPU channels.** mimus's
  `IntMatmul::SinkClaim` could carry it. In split calls it recovers at most
  about half of core 6's maintenance and read-out: 58 ms at 494 MHz, 10 ms at
  2,002 MHz in the model.
- **More than two jobs in flight.** Two slots already hide the non-final
  read-outs of GPU-only calls.
- **Local-memory staging, a float kernel, int16 weights for the GPU.** Each
  pays only if the board shows bandwidth, integer multiply rate or extraction
  limiting the loop.
- **Attention products, 8-bit activations, int4 weights.** The Orange Pi model
  uses int16 activations and int8 weights in every linear layer.

## Differences from the dispatcher design

- The 32-byte submit carries the job: session, slot, tensor ordinal,
  activation offset and stride, rows, sequence, code generation. No
  descriptor page exists; the arena span is 12 MiB.
- Jobs split on 32-row boundaries; the forced two-job split applies only when
  `n_gpu = N`.
- After a refusal, RESET, LOST or tag mismatch, the CPU computes rows
  `[returned, rows)` over channels `[0, n_gpu)` only, because the CPU branch
  already computed, and for residual layers accumulated, `[n_gpu, N)`.
- A tag mismatch is counted as `incomplete=` in the dispatch record, which
  also gains `wake=irq` or `wake=timed`; every later call runs on the CPU for
  the boot.
- LOST ends recognition for the boot, including the recording in progress,
  and retains the model regions and arena span with the output regions. A
  read-only mapping does not bound what firmware in an unknown state can
  write.
- One boot-level upload rule covers the CPU and GPU readers: once the
  recognizer's hash succeeds, USB refuses every slot 0 or 1 Begin with status
  2 until reset. The session close on upload and the reopen are removed.
- The worker skips the activation clean when offset, length and the port's
  code generation equal those of its last clean. The port increments the
  generation on every code write. Every call quantizes its input today, so
  the skip fires only once the port quantizes the shared input of the query,
  key and value projections once.

## Changes needed from other owners

- **mimus port:** `RunPackedI8Block` for AArch64 from `LinearProduced`,
  `LinearProducedParallel` and `LinearProducedAccumulate`, with codes in the
  arena span; the tensor ordinal; 240-row subsampling blocks; the two-part hook
  with the fallback channel range; the read-out entry; the code generation.
- **GPU runtime:** session ranges outside the pools with 2 MiB gaps; one reply
  per mimus submit; a stated `T_stop`; `max_job_us` from the kick; the worker
  on the recognition core.
- **Memory architecture:** the session-scoped device-read span; output regions
  as fixed spans in `0x4220_0000`–`0x4800_0000`; retention after LOST.
- **Native USB application:** the boot-level upload rule.
- **`checks/mimus-linear.cc` owner:** a backend compile definition and
  read-out output counters per call.
- **Primary agent:** the INTID 95 route and priority in
  `platform/a733-smp-cached/irq_table.c`; A76 admission with the inference
  workers on cores 6 and 7.

## Board evidence that would confirm the design

- The bench recording's 399 jobs give output bytes identical to the CPU
  kernel and no tag mismatch, while cores 6 and 7 rewrite activations at the
  same addresses: this confirms the per-job GPU cache contract and the
  cross-core clean.
- From timeline timestamps: GPU time per layer shape (instructions per MAC,
  multiply-add rate, bandwidth) and firmware latency per job (kick to start,
  end to interrupt).
- On an A76 at each admitted clock: nanoseconds per cleaned and per
  invalidated line, and per read-out output.
- CPU and GPU branch times per shape, which set `n_gpu`, and the measured
  saving of the two-job split.
- SPI 63 delivery for every job, with submit-to-kick plus completion-to-wake
  under 50 µs.
- A GPU store through a read-only mapping faults, a store into a gap ends in
  RESET, and the session's ranges use 2 MiB GPU pages.
