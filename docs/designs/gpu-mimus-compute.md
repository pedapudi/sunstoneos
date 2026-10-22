# Design: mimus integer matrix products on the GPU

Status: **design**. Nothing below is implemented. Each item ends at
implemented and host-modeled; physical behavior stays pending until the board
evidence at the end exists.

Recognition with mimus spends most of its time in linear layers: 14.894 s of
19.476 s on one Cortex-A55 at the configured 494 MHz with the NEON kernels
([mimus.md](../mimus.md)), for 4,167,910,560 multiply-accumulates (MACs). A
linear layer multiplies int16 activation codes by int8 weight codes, sums the
products exactly, converts each sum to float once and applies per-channel and
per-row scales, a bias and, for residual layers, `y += alpha × value`, in a
fixed float order. This design moves the integer sums to the GPU and keeps the
float read-out on the CPU, running mimus's own read-out code, so recognition
output stays bit-identical to the host reference.

## Exact sums from 32-bit GPU arithmetic

Activation codes satisfy |x| ≤ 32,767 and weight codes |w| ≤ 128, so one
product is at most 4,194,176 in magnitude. A partial sum over at most 512
products is at most 2,147,418,112, below 2^31. GPU integer addition and
multiplication wrap modulo 2^32, so a 32-bit sum over at most 512 products
equals the true sum whatever order the GPU adds in. The kernel splits K, the
reduction length, into chunks of at most 512. After each chunk it adds the
chunk sum into a 64-bit value held as two words, with one carry test. This
happens once per 512 MACs.

It writes one int32 per output when K ≤ 512 (mimus's `fits32` case) and one
int64 otherwise. The CPU converts with `static_cast<float>`, the conversion
mimus's AArch64 kernels use. For the 222-row bench recording, the linear
shapes below come from the model's structure and account for all but 0.06% of
the profiled MACs:

| Layer, inputs → outputs | Calls | MACs per call | Chunks |
| --- | --- | --- | --- |
| Subsampling convolution as a matrix product, 1,584 → 176, 4,440 rows | 1 | 1,237,800,960 | 4 |
| Subsampling output, 3,520 → 176 | 1 | 137,533,440 | 7 |
| Feed-forward first and second linears, 176 → 704 and 704 → 176 | 64 | 27,506,688 | 1 and 2 |
| Query, key, value, attention output and pointwise convolution 2, 176 → 176 | 80 | 6,876,672 | 1 |
| Positional projection, 176 → 176, 443 rows | 16 | 13,722,368 | 1 |
| Pointwise convolution 1, 176 → 352 | 16 | 13,753,344 | 1 |
| Output head, 176 → 1,025 | 1 | 40,048,800 | 1 |

## Data flow and ownership

- **Hook.** The mimus port already calls `backend::RunPackedI8Block` with the
  int8 weight tensor, its first output channel and the quantized rows. The
  Orange Pi backend implements it. The mimus port needs three changes. The
  call covers every output channel in one block while the GPU session is
  open. Residual layers reach the hook with their `alpha`. The hook returns
  the number of completed rows, and the CPU computes only the remaining rows.
- **Weights, zero copies.** The base model and overlay already sit in fixed
  8 MiB regions at `0x43000000` and `0x43800000` ([mimus-usb.md](../mimus-usb.md)).
  The GPU maps both read-only at session open. A tensor's GPU address is the
  region base plus its offset from the CPU base. The SHA-256 check at model
  load stays the only integrity pass; a model reload closes and reopens the
  session.
- **Activations, zero copies.** The quantized rows already sit in the 16 MiB
  recognition arena at `0x46000000`, which the GPU also maps read-only. The
  submit message names their offset and length.
- **Sums.** Each of the session's two job slots owns a 2 MiB output region,
  which the GPU can write. The CPU reads the sums in place; for each row it
  converts them to a float row on the stack and calls mimus's per-row combine
  and accumulate code. It copies no sum.
- **Jobs.** The recognition task copies nothing into GPU memory. It writes a
  48-byte descriptor into its slot's descriptor page, which no GPU mapping
  covers: tensor offset, row stride, K, N,
  activation offset and stride, first row and row count. The GPU worker copies
  the descriptor into its own memory before checking it. From that copy it
  writes a 32-byte argument record and one compute-data-master (CDM) kernel
  word set, then kicks. A call with at least 64 rows becomes two jobs over
  row halves, one per slot, so the CPU reads out the first half while the GPU
  computes the second. A job is capped at 64 M MACs and one output region.
- **Program.** One host-compiled kernel, `shaders/pvr_mimus_linear.cl`,
  reuses the profile-three register interface of
  [gpu-compiled-programs.md](../gpu-compiled-programs.md): two pointers (the
  argument record and the output region), the local index, and workgroup
  identifiers and counts. A 2D grid covers channel tiles and row tiles; each
  lane loops over the K chunks. Each lane computes 4 rows × 4 channels, and a 32-lane workgroup arranged
  8 × 4 covers 32 channels × 16 rows. The kernel loads 32-bit words
  and extracts signed bytes and halfwords with shifts. Its arithmetic uses
  only 32-bit operations that the existing affine kernel already compiles. A
  new profile four removes the 31-workgroup cap and the geometry sealed at
  OPEN. The existing compile tool, review record and envelope still apply.

**Failure classes** (performance architecture, section 1.8):

- Refused before device access, with no retention: tensor or activations
  outside their regions, misaligned stride, sums larger than a slot, a
  session that is not open. The hook returns zero rows and the CPU computes
  the layer with identical output.
- Known device state: a RESET completion from the GPU runtime's acknowledged
  stop. Rows already read out stay valid; the CPU computes the rest.
- Unknown device state: a LOST completion, which follows a missed deadline
  whose reset the firmware did not acknowledge. The slot stays
  device-owned, and the regions, slots and lease are retained until power-off.
  The backend refuses further jobs, so recognition finishes on the CPU. The
  GPU can only read the model and arena, so the CPU keeps using them safely.

## Interrupts and polling

Completion follows the GPU runtime design
([gpu-runtime.md](gpu-runtime.md)). The GPU interrupt (SPI 63) wakes the
worker, which reads the timeline, invalidates the job's sum bytes and posts
one completion message. The recognition task blocks on that message with the
job deadline (`max_job_us`) as its timeout and polls nothing. Until SPI 63
delivery is qualified, the worker's 4 ms fallback bound applies. The path
then works for bit-exactness evidence but misses its time budget: about 380
jobs per recording at up to 4 ms each. `max_job_us` starts at 100 ms. After
the board item below it becomes four times the largest measured job.

## Memory placement and cache policy

| Region | GPU mapping | CPU writes | Cache maintenance |
| --- | --- | --- | --- |
| Base model and overlay, 16 MiB | Read-only | At upload only | Clean 16 MiB once at session open |
| Recognition arena, 16 MiB | Read-only | Every call | Clean the job's activation bytes at submit |
| Two output regions, 2 MiB each | Read-write | None | Invalidate the bytes the CPU reads, at completion |
| Kernel program, argument records, CDM words | Read-only | Program at open; under 300 bytes per job | Clean the bytes written |
| Rings, timeline | Existing uncached pool | Existing | None |

Cleaning arena bytes is safe on lines shared with other arena data, because a
clean discards nothing. Only output regions are invalidated, and no other data
shares them. GPU pages are 4 KiB until the GPU runtime's large-page board item
passes: 9,216 leaf entries for the model, arena and output regions, then 18
with 2 MiB pages. These regions replace the
compute service's 4 KiB pages built offline and its 64-page buffer limit for
this session. The barriers stay as the GPU runtime design places them; this
design adds none.

## Synchronization across cores

The recognition task writes activations, its descriptor and nothing else in
GPU-visible memory. Once the A76 cores are admitted it runs on core 6. The
worker runs on the A55 core named in the SPI 63 route and is the only writer
of rings, argument records and cache state. The FreeRTOS queue send orders the
task's writes before the worker's clean. The completion message orders the
worker's invalidate before the task's reads. A job costs two inter-processor
interrupts: the submit and the completion. While a job is in flight the task
writes neither its slot nor its activations, because the hook returns only
after its jobs complete or are abandoned.

## Reuse and removal

Reused: the mimus backend hook and read-out code, the profile-three register
interface, the host compile tool, the review record and envelope, the GPU
runtime's sessions, two job slots, completion messages, deadline and reset,
and the model's verify-once load.

Removed for this session: the 4,096-byte input copy and replacement path, the
32-word output paging, guard and poison fills, the 31-workgroup cap, geometry
sealed at OPEN, and mimus's per-call int8-to-int16 weight expansion (0.111 s
per recording in the portable profile). Profiles one to three stay for their
evidence applications.

## Budgets

Baselines come from the board profile in [mimus.md](../mimus.md) or are
counted from the current compute path. GPU times are estimates: they assume
the USC issues one 32-lane instruction per clock at 400 MHz, `A733_GPU_INITIAL_HZ`.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Float output bytes that differ from the CPU kernel, per linear call | No GPU path | 0; recognition checksums and text equal the host reference | `checks/gpu-mimus-layers` |
| Linear calls computed on the CPU in the evidence image | All | 0 | `checks/gpu-mimus-layers` |
| Copies per weight, activation and sum byte | 1 expansion per call; compute path: 2 per input byte, at least 5 per output byte | 0, 0, 0 | `checks/gpu-mimus-layers` |
| Jobs per bench recording | Not possible; 3,968-byte output, 31 workgroups | At most 400 | `checks/gpu-mimus-layers` |
| USC instructions per MAC in the inner loop | None | At most 1.75 | `checks/gpu-mimus-program` |
| DRAM bytes read per MAC, from the tile shape, ignoring GPU caches | None | At most 0.125 | `checks/gpu-mimus-layers` |
| Linear time per recording | 14.894 s measured | 0.57 s estimated, plus overheads | Both checks |
| Feed-forward linear, 27.5 M MACs | 98 ms, measured rate | 3.8 ms estimated | `checks/gpu-mimus-program` |
| Subsampling convolution, 1.24 G MACs | 4.42 s, measured rate | 169 ms estimated | `checks/gpu-mimus-program` |
| Ring and record bytes written per job | 368-byte bound | At most 300 | `checks/pvr-compute-service` |
| Cache lines per job | Whole input and output pages, plus fill | Activation bytes / 64 + sum bytes / 64 + 5 | `checks/pvr-compute-service` |
| Checks per job | Native resource admission and a CPU mapping check | At most 12 compares, no translation, no hash | `checks/pvr-compute-service` |
| Recognition-task wake-ups per job; tick waits from hook to kick | One per tick + 32; not bounded | 1; 0 | `checks/pvr-compute-service` |
| AArch64 read-out instructions per output, not residual | Folded into the CPU kernel | At most 12 | `checks/gpu-mimus-readout` (Unicorn) |
| GPU leaf entries for the session | 4 KiB only, at most 256 table pages | 9,216; 18 after large pages | `checks/pvr-gpu-vm.c` |

The feed-forward and subsampling baselines apply the measured rate of 0.28 G
MACs per second. `checks/gpu-mimus-layers` runs the pinned mimus host engine on
the bench recording with a backend that executes the kernel source as host C
over its grid. It records each call's output bytes against the CPU path.
`checks/gpu-mimus-program` counts loop instructions in the pinned compiler's
final listing and multiplies them by each shape's MACs. Neither check
executes USC code; compiler correctness on the board is a board item.

## Left out

- **Splitting a layer between the GPU and CPU workers.** GPU work alone,
  0.57 s estimated at 400 MHz, exceeds the 0.4435 s recognition target for
  the bench recording. The recognition dispatcher decides the split from
  board measurements.
- **A float kernel.** It splits each activation into two bytes and uses exact
  fp32 fused multiply-adds over chunks of 512. It is needed only if the board
  shows integer multiply below half the float rate.
- **Weights expanded to int16 for the GPU.** That doubles weight memory and
  adds an admission copy, and is needed only if byte extraction dominates
  the measured loop.
- **Attention matrix products, 8-bit activations and int4 weights.** The
  Orange Pi model uses int16 activations and int8 weights in every linear
  layer; attention products use different arithmetic.
- **Read-out split across both recognition workers, and more than two jobs in
  flight.** The two-slot row split hides most read-out time. Revisit if the
  board shows exposed read-out.

## Changes needed from other owners

- **mimus port:** the three hook changes above and a public per-row combine
  and accumulate entry.
- **GPU runtime:** the mimus session's leases over the model, overlay and
  arena regions; a submit message carrying a range inside a lease; descriptor
  copy and per-job CDM words.
- **Memory architecture:** the three fixed regions as `gpu`-class leases.
- **Primary agent:** recognition affinity to core 6 and read-only CPU
  mappings for the model regions after load.

## Board evidence that confirms the design

- Every linear call in the bench recording produces output bytes identical to
  the CPU kernel, with recognition checksums and text equal to the host
  reference.
- GPU time per layer shape from timeline timestamps, giving the realized
  instructions per MAC and the integer multiply rate.
- SPI 63 delivery for every job, and submit-to-kick plus completion-to-wake
  under 50 µs per job.
- No GPU MMU fault over a recording, and a fault when an evidence kernel
  writes the model region.
