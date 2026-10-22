# Design: mimus integer matrix products on the GPU

Status: **design**. Nothing below is implemented. Each item ends at
implemented and host-modeled; physical behavior stays pending until the board
evidence at the end exists. Three designs it depends on are not merged into
this branch: the GPU runtime (`docs/designs/gpu-runtime.md` on branch
`audit/gpu-runtime`), the memory architecture
(`docs/designs/memory-architecture.md` on `audit/memory-architecture`) and the
placement of mimus linear layers in the OS image
(`docs/designs/recognition-dispatch.md` on `audit/recognition-dispatch`,
called the dispatcher design below). Links replace these paths when they merge.
This design and the dispatcher design describe one data path; the dispatcher
changes it needs are listed at the end.

Recognition with mimus spends 14.894 s of 19.476 s in linear layers on one
Cortex-A55 at the configured 494 MHz with the NEON kernels
([mimus.md](../mimus.md)), for 4,167,910,560 multiply-accumulates (MACs). A
linear layer multiplies int16 activation codes by int8 weight codes and sums
the products exactly. mimus then converts each sum to float once and applies
per-channel and per-row scales, a bias and, for residual layers,
`y += alpha × value`, in a fixed float order. That last step is the
**read-out**. This design computes exact sums for some output channels on the
GPU while the A76 cores compute the rest, and leaves every read-out to
mimus's own CPU code, so recognition output stays bit-identical to the host
reference.

## Exact sums from 32-bit GPU arithmetic

Activation codes satisfy |x| ≤ 32,767 and weight codes |w| ≤ 128, so one
product is at most 4,194,176 in magnitude. Any sum of at most 512 products is
at most 512 × 4,194,176 = 2,147,418,112, below 2^31, so no partial sum over a
chunk of at most 512 products leaves the int32 range in any order. The kernel
splits K, the reduction length, into such chunks and adds each chunk sum `c`
into a 64-bit value held as two words:

    lo' = lo + (uint32)c;   hi' = hi + (lo' < lo) + (c >> 31)

where `c >> 31` is an arithmetic shift, 0 or −1. This runs once per 512 MACs.
The kernel writes one int32 per output when K ≤ 512 (mimus's `fits32` case)
and one int64 otherwise. The CPU converts with `static_cast<float>`, as mimus's
AArch64 kernels do.

For the 222-frame bench recording, the calls below follow from the model's
structure, mimus's `kOutChunk` of 32 frames and the dispatcher's 240-row
blocks for the subsampling convolution (the current port issues 28 calls of
160 rows). They account for 4,165,556,384 MACs, all but 0.06% of the profile:

| Layer, K → N | Rows per call | Calls | MACs per recording | Sums |
| --- | --- | --- | --- | --- |
| Subsampling convolution as a matrix product, 1,584 → 176 | 240 (last 120) | 19 | 1,237,800,960 | int64, 4 chunks |
| Subsampling output, 3,520 → 176 | 32 (last 30) | 7 | 137,533,440 | int64, 7 chunks |
| Feed-forward first linear, 176 → 704 | 222 | 32 | 880,214,016 | int32 |
| Feed-forward second linear, 704 → 176 | 222 | 32 | 880,214,016 | int64, 2 chunks |
| Query, key, value, attention output, pointwise convolution 2, 176 → 176 | 222 | 80 | 550,133,760 | int32 |
| Positional projection, 176 → 176 | 443 | 16 | 219,557,888 | int32 |
| Pointwise convolution 1, 176 → 352 | 222 | 16 | 220,053,504 | int32 |
| Output head, 176 → 1,025 | 222 | 1 | 40,048,800 | int32 |

`checks/gpu-mimus-layers` records each claimed call's rows, K, N, code
address and weight address on the host engine and fails if a call differs
from this table or its codes lie outside the arena span below. mimus keeps
codes of at most 32 KiB in its internal arena and streams some produced rows
through rings, so the port places the codes of every claimed call in the
arena span (listed under changes).

## Data flow and ownership

The recognizer task runs at EL0 and the GPU worker at EL1. Every range the CPU
touches lies in `0x4220_0000`–`0x4800_0000`, which EL0 and EL1 already map
read-write and write-back, so the path needs no new CPU mapping. The GPU
session's catalogue maps:

- **Weights, read-only, in place.** The base model and overlay regions at
  `0x43000000` and `0x43800000` (16 MiB). Tensor row n starts at the tensor's
  offset plus n times its row stride. The session's table records each placed
  tensor's offset, row stride, K and N; admission checks once that each lies
  inside its region and that offset and stride are multiples of 4.
- **Codes, read-only, in place.** The first 12 MiB of the recognition arena at
  `0x46000000`, which the recognizer passes to mimus as its arena. The
  recognizer holds this span throughout; only the in-flight job's declared
  activation range is lent to the GPU, from submit to completion.
- **Sums, read-write.** Two 2 MiB output regions, the top 4 MiB of the arena.
  Each is a job slot that moves between CPU and DEVICE. The recognizer reads
  sums in place and runs the read-out.

Besides the rings, the worker writes only the 32-byte argument record and the
CDM words, in a `gpu`-class page. Copies per weight, code and sum byte are 0, 0 and 0.

- **Hook.** The Orange Pi backend implements `backend::RunPackedI8Block` over
  output channels [0, n_gpu), with the residual `alpha` passed in. For
  n_gpu < N the dispatcher's two-part form applies: begin submits the GPU jobs,
  the engine's `ParallelFor` workers on cores 6 and 7 compute channels
  [n_gpu, N) with the existing NEON kernel, and end reads out the GPU's
  channels. The dispatcher's per-tensor `n_gpu` is seeded from the time model
  under Budgets; n_gpu = N is the GPU-only case.
- **Jobs.** A call becomes jobs of at most `job_rows` rows, split on 32-row
  boundaries and alternating between the two slots, so the recognizer reads
  out one slot while the GPU computes the other. `job_rows` keeps each job
  within 64 Mi MACs and one output region. The bench recording needs 399 jobs.
- **Submit by value.** The recognizer sends the GPU runtime's 32-byte submit
  message carrying the job: session, slot, tensor ordinal, activation offset
  and row stride inside the arena span, row count and sequence. The FreeRTOS
  queue copies the message, so the worker checks its own received copy once.
  K, N, n_gpu, the sum width and the weight address come from the worker's
  copy of the placement table.
- **Program.** One kernel, `shaders/pvr_mimus_linear.cl`, is compiled on the
  host by `apps/gpu-program/compile.py` with the profile-three register
  interface of [gpu-compiled-programs.md](../gpu-compiled-programs.md): two
  pointers (the argument record and the output region), the local index, and
  workgroup identifiers and counts. Its USC code is linked into the image as a
  constant and copied into the program page at session open. It uses no
  package, so the 8,288-byte format, its copied input, header, output limit
  and sealed envelope do not apply. `pvr_compute_job.c` reuses its
  profile-three PDS and CDM construction, taking the group counts from the
  per-job CDM words. The 31-workgroup admission policy stays for sealed package
  sessions only; the largest bench job has 260 workgroups.
- **Kernel shape.** Each lane computes 4 rows × 4 channels; a 32-lane
  workgroup with local shape (4, 8) covers 16 channels × 32 rows. Padding adds
  2.6% to the bench MACs. Each lane loops over the K chunks, loading 32-bit
  words and extracting signed bytes and halfwords. A lane clamps its row and
  channel indices for loads, so padded lanes read only the job's declared
  ranges, and it stores only when `row < rows` and `channel < n_gpu`.

## Failure classes

Performance architecture section 1.8:

- **Refused before device access:** a closed or revoked session, an ordinal
  out of range, rows above the tensor's `job_rows`, an activation offset or
  stride that is not a multiple of 4, a stride below 2K, an activation range
  outside the arena span, a slot that is not CPU. Nothing is retained; the CPU
  computes those rows.
- **Known device state:** a RESET completion after the GPU runtime's
  acknowledged stop. Read-out is strictly in job order. After the first
  non-OK completion of a call, later jobs of that call are not read out; their
  completions are drained by sequence. The hook returns the rows before the
  failed job, and the CPU computes the rest. A residual layer therefore
  accumulates each row once.
- **Unknown device state:** a LOST completion. Output regions stay DEVICE
  until power-off, and mimus never allocates from them. The model regions and
  arena span stay mapped read-only by the GPU. They are exempt from class-3
  retention only because the GPU cannot write through a read-only mapping, and
  the board item below must show that. Until it does, LOST ends recognition
  for the boot: the recognizer refuses further starts and USB refuses
  model-slot uploads.
- **No answer by the deadline.** The dispatcher's recognizer waits for each
  completion for `T_answer` + 10 ms. Here `T_answer` = 2 × `T_clean` +
  `max_job_us` + 4 ms + `T_stop` + 1 ms, where `T_clean` is the largest job's
  activation clean, 4 ms the runtime's hang detection and `T_stop` its
  stop-sequence bound. Every GPU hang therefore
  reaches the recognizer as RESET or LOST first. Expiry means the worker
  overran its own bound; the recognizer calls `board_fail` and sends no later
  submit, so no late completion is ever consumed.
- **Model upload.** The GPU reads the model regions, and the USB service
  writes them with CPU copies. A Begin asset on slot 0 or 1 is refused with
  status 2 while the mimus GPU session is open, pending release or retained,
  and the refusal asks the dispatcher to close the session. Close marks the
  leases PENDING_RELEASE; the next park (after 50 ms idle) releases them, and
  a retried Begin then succeeds. The next recognition hashes the new model and
  reopens the session, which cleans the 16 MiB again. This design needs no
  read-only CPU mapping of the model regions.

## Interrupts and polling

The GPU interrupt (SPI 63, INTID 95) is routed to the recognition core, core 6
once the A76 cores are admitted and core 2 until then, where the worker runs
above the recognizer's priority. The worker invalidates the job's sum bytes
and posts one completion, the only reply a mimus submit gets. The recognizer
blocks on it with the deadline above and polls nothing; it does not block
while CPU channels remain. Until SPI 63 delivery is qualified, the worker's
4 ms fallback wait applies. The path then works for bit-exactness evidence
but adds up to 4 ms to each of 399 jobs. `max_job_us` starts at 100 ms and
becomes four times the largest measured job after the board item below.

## Memory placement and cache policy

| Range | GPU mapping | CPU writes | Maintenance |
| --- | --- | --- | --- |
| Model regions, 16 MiB | Read-only | USB upload, session closed | Clean 16 MiB at each open |
| Arena span, 12 MiB | Read-only | Recognizer, every call | Clean the job's activation bytes at submit |
| Two output regions, 2 MiB each | Read-write while DEVICE | None | Clean and invalidate at open; invalidate written sums at completion |
| Program, argument record, CDM words | Read-only | Program at open; under 300 bytes per job | Clean the bytes written |
| Rings, timeline | Existing uncached pool | Existing | None |

In GPU virtual addresses an unmapped 2 MiB gap follows each range, including
between the two output regions, so a lane that overruns faults and its job
ends in RESET while the CPU reads the other slot. Three checks make the guard
and poison fills redundant: `job_rows` keeps the padded grid's sums inside one
slot, the kernel guards every store, and the gaps fault any other overrun. The
validation image compares every output with the CPU path. The session uses
8,192 leaf entries with 4 KiB GPU pages and 16 with 2 MiB pages.

The arena and output regions are rewritten at the same addresses for every
job. The GPU reads fresh bytes only if the firmware flushes and invalidates
the GPU's caches per job, the contract that
[gpu-compiled-programs.md](../gpu-compiled-programs.md) inherits from the
pinned driver. Cleaning activation bytes discards nothing, so lines shared
with other arena data are safe. Cores 6 and 7 quantize codes; the worker's
clean on core 6 must reach lines in core 7's cache, which clean by address to
the point of coherency does architecturally. A board item covers both.

## Synchronization across cores

The `ParallelFor` workers on cores 6 and 7 quantize the codes, and the
engine's join orders their stores before the recognizer's submit. The submit
is a same-core queue send that orders them before the worker's clean. The
worker alone writes rings, records and cache state. The completion message
orders the worker's invalidate before the recognizer's reads. From submit to
completion the recognizer writes neither the job's activation range nor its
slot. A job costs no inter-processor interrupt once the worker, recognizer
and interrupt share core 6.

## Reuse and removal

Reused: `backend::RunPackedI8Block` and mimus's NEON kernel, read-out and
`ParallelFor` workers; the dispatcher's placement table and two-part hook; the
GPU runtime's sessions, slots, completion messages, deadline and reset; the
profile-three register interface and its PDS and CDM construction; the compile
tool and review record as build-time provenance; the recognizer's SHA-256 of
the model.

Removed for this session: every copy of weights, codes and sums; the package
format and envelope; the 4,096-byte input copy and replacement path; 32-word
output paging; guard and poison fills; the 31-workgroup cap; geometry sealed at
OPEN; the ACCEPTED reply; and mimus's per-call int8-to-int16 weight expansion
for GPU channels (0.111 s per recording in the portable profile).

## Budgets

Baselines come from the board profile in [mimus.md](../mimus.md) or are
counted from the current compute path. Estimates assume: the USC issues one
32-lane instruction per clock at 400 MHz (`A733_GPU_INITIAL_HZ`); 50 µs of
CPU work per job; 20 cycles per maintained cache line; an A76 core does twice
an A55's NEON work per clock and retires two read-out instructions per clock.
A76 figures are given at 494 MHz and at the 2,002 MHz table maximum.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Float output bytes differing from the CPU kernel, per call | No GPU path | 0; checksums and text equal the host reference | `checks/gpu-mimus-layers` |
| Copies per weight, code and sum byte | 1 weight expansion per call; compute path 2 per input byte, at least 5 per output byte | 0, 0, 0 | Copy counter in the same check |
| Jobs per recording; CPU work at 50 µs each | Not possible: 3,968-byte output, 31 workgroups | 399; 20 ms | `checks/gpu-mimus-layers` |
| Lines maintained on core 6 per recording | None | 1,534,103 (38,206,784 code and 59,975,800 sum bytes); 62 ms, or 15 ms at 2,002 MHz | `aarch64_device_cache` wrap |
| USC instructions per MAC, inner loop | None | At most 1.75 | `checks/gpu-mimus-program` |
| GPU compute, all channels | 14.894 s on the CPU | 0.585 s; 0.73 s or 0.92 s if an assumption fails | Both checks |
| DRAM bytes read per MAC | None | 0.75 per lane; 0.156 per workgroup with cache reuse | `checks/gpu-mimus-layers` |
| Read-out per output, not residual | Inside the CPU kernel | At most 12 AArch64 instructions | `checks/gpu-mimus-readout` (Unicorn) |
| Read-out per recording, 12,923,134 outputs | Inside the CPU kernel | 0.31 s on one A55; 0.157 s on one A76, 0.039 s at 2,002 MHz | Same, times the clock |
| Exposed read-out, last job of each call | None | 69 ms; 12 ms at 2,002 MHz | Time model |
| Linear stage, channel split | 14.894 s | 0.60 s with 88% on the GPU; 0.39 s with 61% at 2,002 MHz | Time model |
| Core-6 wait at the hook's end, per call | None | At most the modeled GPU branch minus the CPU branch, plus 50 µs | `checks/gpu-mimus-layers` |
| Ring and record bytes per job | 368-byte bound | At most 300 | `checks/pvr-compute-service` |
| Checks per job | Native resource admission and a CPU mapping check | At most 12 compares; no translation, no hash | `checks/pvr-compute-service` |
| Recognizer wake-ups; IPIs; tick waits, per job | One per tick + 32; not bounded | At most 1; 0; 0 | `checks/pvr-compute-service` |
| GPU leaf entries | 4 KiB only, at most 256 table pages | 8,192; 16 with 2 MiB pages | `checks/pvr-gpu-vm.c` |

**Instruction mix.** A 4 × 4 tile over 4 K steps does 64 integer
multiply-adds, 12 word loads (8 activation, 4 weight), 32 signed extractions
and about 4 loop instructions: 112 instructions, 1.75 per MAC. That needs a
single-instruction signed bitfield extract and a full-rate 32-bit
multiply-add. Two-instruction extractions give 2.19 per MAC and 0.73 s; a
half-rate multiply-add gives 2.75 per MAC and 0.92 s. The `n_gpu` seeds use
the figure `checks/gpu-mimus-program` counts.

**Bandwidth.** Without cache reuse, each lane's 12 bytes per 16 MACs give
3.2 GB per recording, 5.5 GB/s over 0.585 s. With reuse of a workgroup's
32 activation rows and 16 weight channels through the USC and system-level
caches, 80 bytes per 512 MACs give 0.67 GB.

**Time model.** For GPU share s of the MACs, the GPU branch takes
s × 0.585 s plus 20 ms of job work. The two A76 cores take (1 − s) × 3.72 s
for the CPU channels (0.92 s at 2,002 MHz), plus core 6's read-out and cache
maintenance for the GPU's share, halved across the pair. Balancing the
branches gives the split rows above; the exposed tail is the last job's
read-out. The two-slot overlap hides half of the GPU share's read-out,
69 ms at 494 MHz and 12 ms at 2,002 MHz, for 196 extra jobs costing 9.8 ms.
It stays while the board-measured saving exceeds that cost. The non-linear
stages, 4.58 s of the profile, are outside this design.

`checks/gpu-mimus-layers` builds the existing `checks/mimus-linear.cc` harness
with a GPU-grid backend that runs the kernel source as host C over its grid,
and adds no second engine setup. Its cases are:

- the bench shapes, with refusal, RESET and LOST;
- a residual layer whose first job is RESET while the second completes OK;
- negative chunk sums at K = 1,584 and 3,520;
- a missed deadline, after which no submit is sent;
- model uploads while the session is open, pending release and after LOST.

The dispatcher's whole-recording parity check links the same backend.
`checks/gpu-mimus-program` checks the linked USC constant against the compile
manifest and review record and counts inner-loop instructions in the pinned
compiler's listing. Neither check executes USC code; compiler correctness on
the board is a board item.

## Left out

- **More than two jobs in flight, and read-out on both A76 cores.** Either
  would shrink the exposed tail of 12 ms at 2,002 MHz; revisit if the board
  shows more.
- **Local-memory staging in the kernel.** Needed if the board shows a layer's
  read bandwidth near the 0.75 bytes-per-MAC figure with its time above the
  instruction estimate.
- **A float kernel.** Needed only if the board shows integer multiply below
  half the float rate.
- **Weights expanded to int16 for the GPU.** That doubles weight memory and
  adds a copy, and is needed only if extraction dominates the measured loop.
- **Attention matrix products, 8-bit activations and int4 weights.** The
  Orange Pi model uses int16 activations and int8 weights in every linear
  layer; attention products use different arithmetic.

## Changes needed from other owners

- **Dispatcher:** job fields in the submit message, with no descriptor page,
  so the arena span is exactly 12 MiB; job splits on 32-row boundaries; LOST
  ending recognition for the boot until the read-only board item passes; the
  close request on a refused model-slot Begin; `n_gpu` seeds from the time
  model.
- **mimus port:** `RunPackedI8Block` compiled for AArch64 and reached from
  `LinearProduced`, `LinearProducedParallel` and `LinearProducedAccumulate`,
  with the codes of every claimed call in the arena span; the tensor ordinal;
  240-row subsampling blocks; the two-part hook.
- **GPU runtime:** session ranges outside the pools (model regions, arena
  span, output regions) with unmapped 2 MiB gaps; per-job CDM group counts;
  one reply per mimus submit; a stated `T_stop`; the worker and INTID 95 on
  the recognition core.
- **Memory architecture:** an ownership state for a fixed span that the CPU
  holds and writes while the GPU reads it, lent per job only over the declared
  activation range; output slots as fixed spans in `0x4220_0000`–`0x4800_0000`.
  If such spans are refused, the fallback copies each code and sum byte once
  into `gpu`-class staging, 98 MB per recording.
- **Native USB application:** status 2 for a model-slot Begin while the
  session holds the model regions.
- **`checks/mimus-linear.cc` owner:** a compile definition that selects the
  backend.
- **Primary agent:** A76 admission, with the inference workers on cores 6
  and 7.

## Board evidence that confirms the design

- Every call in the bench recording produces output bytes identical to the
  CPU kernel, with checksums and text equal to the host reference. Its 399
  jobs read activations rewritten at the same addresses by cores 6 and 7,
  which confirms the per-job device-cache contract and the cross-core clean.
- GPU time per layer shape from timeline timestamps, giving instructions per
  MAC, the integer multiply-add rate and read bandwidth per layer.
- CPU and GPU branch times per shape at the admitted clocks, replacing the
  `n_gpu` seeds, and the measured saving of the two-slot overlap.
- SPI 63 delivery for every job, and submit-to-kick plus completion-to-wake
  under 50 µs.
- An evidence kernel that writes through a read-only mapping faults and leaves
  memory unchanged, and a store into a gap ends in RESET.
- 2 MiB GPU pages for the session's ranges.
