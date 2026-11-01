# Exact mimus integer matrix products on the GPU

A mimus linear layer multiplies int16 activation codes by int8 weight codes,
sums the products exactly, then converts each sum to float once and scales it
in a fixed order (the **read-out**). This component computes the exact sums on
the PowerVR GPU and gives the CPU a read-out whose results are bit-identical
to mimus's CPU path. The [design](designs/gpu-mimus-compute.md) states the
budgets and [the measurements](designs/gpu-mimus-compute-measurements.md)
compare the result with them. The OS image's
[placement of linear layers](recognition-dispatch.md) decides which output
channels of each layer run here.

Status: **implemented**, with two standalone production exact-sum shapes
passing on the physical GPU under the conditions below. Current recognition
uses the [generic queue](gpu-mimus-service.md); the external mimus checkout
recorded in [the source record](../apps/mimus/source.json) owns its shader,
job adapter and engine hook. Paths beginning with `sunstoneos/` below belong
to that checkout. Complete recognition through the current queue remains
unqualified; [GPU status](status/gpu.md) identifies the retained evidence.
The standalone program misses two design budgets: 3.39 GPU instructions per
multiply-accumulate (MAC) against 1.75, and 7,624 code bytes against 4,000.

## Parts

| Part | Source | Role |
| --- | --- | --- |
| Kernel | [pvr_mimus_linear.cl](https://github.com/pedapudi/mimus/blob/port/sunstoneos-split/sunstoneos/shaders/pvr_mimus_linear.cl) | One job's exact sums, tags and completion order |
| Linked program | `sunstoneos/apps/mimus/gpu_linear_program.c` in mimus | The application-owned compiled USC program |
| Job adapter | `sunstoneos/apps/mimus/gpu_linear.c`, `gpu_linear_job.h` in mimus | Tensor admission, job bounds, arguments, cache extents and tag checks |
| Read-out entry | [pvr_compute_mimus_readout.h](https://github.com/pedapudi/mimus/blob/port/sunstoneos-split/sunstoneos/include/pvr_compute_mimus_readout.h) | Portable C read-out of int32 and int64 sums, plain and residual |
| Shared encoders | [pvr_compute_job.c](../drivers/gpu/pvr_compute_job.c) | Profile-three PDS data and program, fence program, CDM stream and firmware command, shared with compiled packages |
| Compiler form | [compile.py](../apps/gpu-program/compile.py) `--session` | Profile three without a package |

## Exact sums from 32-bit arithmetic

Activation codes satisfy |x| ≤ 32,767 and weight codes |w| ≤ 128, so the sum
of at most 512 products lies below 2^31 in magnitude in any order. The kernel
sums K in chunks of at most 512 products into an int32 `c`. It keeps two
32-bit values per output across chunks: `lo`, the sum modulo 2^32, and `t`,
the sum of `c >> 8` (arithmetic shift). With `E`, the sum of the discarded low
bytes `c & 255`, the exact sum is `256 t + E` with 0 ≤ E < 256 × chunks. The
high word is therefore

    hi = (t >> 24) + ((uint32)(t << 8) > lo)

because `(uint32)(t << 8)` equals `(sum − E) mod 2^32`, which exceeds `lo`
if and only if adding `E` wrapped past 2^32. The rule is exact while K < 2^16:
`t` stays below 2^30 in magnitude and `E` below 2^16. This costs three
operations per chunk and output; the carry rule in the design costs five.

A job writes one int32 per output when K ≤ 512 (mimus's `fits32` case) and one
int64 otherwise.

## Kernel

Each lane computes 4 rows by 4 channels; a workgroup of local shape (4, 8)
covers 16 channels by 32 rows. Per 4 K steps a lane loads 12 aligned 32-bit
words (four weight words, two activation words per row), extracts 32 signed
bytes and halfwords and performs 64 multiply-adds. Loads clamp to the job's
last row and channel, so padded lanes read admitted memory only.

The 32-byte argument record holds, as little-endian words:

| Words | Content |
| --- | --- |
| 0–1 | GPU address of channel 0's weight codes |
| 2–3 | GPU address of row 0's activation codes |
| 4 | K, and n_gpu in the high 16 bits |
| 5 | Rows |
| 6 | Weight stride, and activation stride in the high 16 bits, in bytes |
| 7 | Sequence |

The program reads no workgroup count, so the compiler allocates three
coefficients (the workgroup identifiers). The PDS still writes all seven; the
counts stay zero, and each job's CDM stream carries its grid.

**Completion proof.** Every lane, padded lanes included, stores the job's
sequence into its own tag word after its sum stores. The output region holds
64 KiB of tag words and then the sums; a job's tags occupy the top of the tag
area, directly below its sums. The pinned compiler removes `mem_fence` from a
32-lane workgroup and emits no instruction for it. The kernel therefore sets
the issue-data-fence flag on every sum store, which the compiler expands into
`st`, `idf` and `wdf` (store, issue a data fence on that address, wait for
it). The tag store follows the last such wait.
`checks/gpu-mimus-program.py` confirms this order in the compiler listing.

**Stores without branches.** A store to a channel outside the job, and the
high word of an int32 sum, goes to the lane's own tag word, which the tag
store then overwrites. The store loop over a lane's rows has a trip count the
compiler cannot fold, so it keeps one copy of the stores.

## GPU address layout

| Range | GPU address | Size | GPU access |
| --- | --- | --- | --- |
| Model regions | `0x43000000`, `0x43800000` | 8 MiB each | Read-only |
| Arena span (mimus's arena, holding the codes) | `0x46000000` | 12 MiB | Read-only |
| Output region: tags, then sums | `0x46C00000` | 2 MiB | Read-write |
| Job page, in the context's PDS heap | Runtime-chosen | 4 KiB | Read-only |
| Program pages, in the context's USC heap | Runtime-chosen | 8 KiB | Read-only |

The generic queue owns its job page and command encoding. Its
[layout](../include/pvr_compute_queue.h) reserves PDS data at byte 0,
a PDS program at byte 128, a fence program at byte 192, a CDM stream at
byte 256 and application arguments at byte 320.

## Application job adapter

Mimus opens one generic context with four leases: output, two models and arena.
Its `mimus_gpu_job_rows` bounds jobs by arithmetic work, output capacity and
available tag words. `mimus_gpu_linear_job` constructs the shape arguments,
lease-relative bindings and cache extents. `mimus_gpu_tags_complete` requires
every lane's sequence after completion.

The OS queue validates the leased extents, performs cache maintenance and
serializes the firmware commands. The engine hook consumes the returned sums
before another job reuses the output. The application owns numerical admission
and readout; the [queue interface](../include/gpu_queue.h) defines OS services.

## Read-out

`pvr_mimus_readout_i32`, `_i64`, `_i32_accumulate` and `_i64_accumulate`
compute `value = (0 + (float(sum) × ws[n]) × xs[r]) + (bias[n] or 0)`, and
for residual layers `y = y + alpha × value`, mimus's order in
`gemm_combine_portable.cc` and `kernels.cc`. They must be built with
`-ffp-contract=off`. int64 sums convert through double, which is exact below
2^53, so the single rounding to float equals `static_cast<float>(sum)` and
the compiler vectorizes it as `SCVTF` on 2D lanes and `FCVTN`.

## Checks

None of these checks executes USC code or touches hardware. The commands use
the Mesa build of [gpu-compute-programs.md](gpu-compute-programs.md) and the
mimus checkout that the image build uses.

```sh
export LD_LIBRARY_PATH=~/.cache/sunstoneos/gpu-producer-host/usr/lib:~/.cache/sunstoneos/gpu-producer-host/usr/lib/x86_64-linux-gnu
PY="env PYTHONDONTWRITEBYTECODE=1 $HOME/.cache/sunstoneos/venv/bin/python"
$PY checks/gpu-mimus-program.py --mesa-source ~/.cache/sunstoneos/gpu-producer-build/mesa \
  --mesa-build ~/.cache/sunstoneos/gpu-producer-build/build \
  --ninja ~/.cache/sunstoneos/gpu-producer-venv/bin/ninja --output OUT/program
$PY checks/gpu-mimus-layers.py --mimus ~/git/mimus-worktrees/sunstoneos --output OUT/layers
$PY checks/gpu-mimus-readout.py --output OUT/readout
$PY checks/gpu-mimus-time-model.py --calls OUT/layers/calls.txt \
  --program OUT/program/program-check.json --readout OUT/readout/readout-check.json
```

- **`gpu-mimus-program.py`** compiles the kernel with the pinned compiler,
  requires the linked constant to equal its code, length, register count and
  digests, counts the instruction groups of the inner loop per MAC, confirms
  the store order of the completion proof, compiles the session code for
  AArch64 with the image's options and `-Werror`, and counts the compares in
  `pvr_mimus_submit`.
- **`gpu-mimus-layers.py`** compiles the kernel source as C over its grid,
  with the session code, the linked program, the read-out entry and mimus's
  `gemm_combine_portable.cc`. A memory model gives the GPU memory and the CPU
  a write-back cache: codes, job words and program bytes reach the GPU only
  through DC CVAC, and the CPU sees GPU writes only after DC CIVAC. The output
  region is mapped `PROT_READ` outside the modeled GPU. The check runs the
  bench recording's 204 calls and compares every output bit for bit with
  mimus's CPU path. Its other cases: refusals of each admission rule, a stray
  CPU store (which faults), a three-job residual call with `n_gpu < N` whose
  middle job is refused or misses one tag, an earlier boot's tags with and
  without the boot clear, an omitted clean or clean-and-invalidate (which the
  model detects), every chunk sum at its limit for K = 1,584 and 3,520, and
  RESET and LOST before any CPU row. Every case except the bench also runs
  under AddressSanitizer and UndefinedBehaviorSanitizer and compares every
  GPU sum with the exact int64 sum.
- **`gpu-mimus-readout.py`** builds the read-out entry for AArch64 with
  mimus's compute options, runs it under Unicorn on edge inputs (−0, ±(2^24+1),
  the int32 limits, ±2^34, sums rebuilt from 32-bit chunks with the kernel's
  rule) and compares each output bit for bit with a float32 evaluation of
  mimus's order. It counts instructions per output.
- **`gpu-mimus-time-model.py`** replays the call list on core 6, core 7 and
  the GPU; `--program` and `--readout` replace its instruction and temp
  assumptions with the measured counts. It prints issue-bound GPU times,
  which are lower bounds, and times for load latencies given by
  `--load-cycles` (default 50, 100, 200 and 400 GPU clocks) with the
  resident tasks that the program's temps allow.

## What other owners provide

- **GPU runtime:** provided by the GPU service's mimus component
  ([gpu-mimus-service.md](gpu-mimus-service.md)): the GPU mappings above with
  256 KiB pages for the large ranges, the compute context, the client ring, the
  worker on the recognition core, the completion record with `max_job_us`
  from the kick, and SPI 63. Per job it submits the 96-byte command that
  `pvr_mimus_session_command` wrote into a zeroed buffer and a timeline
  UPDATE. The OS image also zeroes and publishes the tag area at boot and maps
  the output region read-only at EL0 and EL1.
- **mimus engine:** the exact-sum hook that `apps/os/gpu_hook.cc`
  implements: the hook splits a call into jobs on 32-row boundaries and
  checks tags with `pvr_mimus_tags_complete`, and the engine runs its CPU
  channels, reads the sums out with its own `Combine()` and computes the rows
  no job returned ([gpu-recognition.md](designs/gpu-recognition.md)).

The returning [exact-sum caller](designs/gpu-exact-sum-caller.md) prepares two
bounded jobs through the firmware submission path. Both shapes passed on
physical hardware under the conditions below.

## Two exact-sum shapes pass on the board

On 2026-10-28, the returning caller executed K=512 and K=516, each with
33 rows and 17 channels, at 1008 MHz and 960 mV on port 3-1. All 1,122
sums matched the scalar reference, with no bad tags or guards. The context
cleaned up, firmware acknowledged idle and OFF, isolation completed and the
provider parked. The board returned to ROM with automatic USB access; no
manual reconnection was needed.

The frozen binary SHA-256 is
`305ab703202b95337b06ccaa4de1b3ae9c7fa9f4eb63fee7bde2a30f877e8144`.
Physical capture and validated terminal records are under
`~/.cache/sunstoneos/evidence/sol-resume-20261028/port-3-1/gpu-exact-sum/`.
Its DDR start, provider checkpoint, cache-off and survey records preserve the
same lifetime's prerequisites. The caller validates these two shapes;
complete GPU recognition, its read-only output mapping, other model shapes
and physical fault recovery remain unqualified.

The CPU-only [cache-permission probe](../apps/gpu-cache-permission/README.md)
also passed on 2026-10-28. It ran the unchanged EL1 cache helper over a read-only
4 MiB diagnostic mapping and preserved initialized bytes before normal ROM
return. The mapping has production permissions at a relocated address. This
result establishes the permission operation; production-address initialization,
GPU DMA visibility and recognition still need physical evidence.

## Remaining board evidence

- The bench recording's 204 jobs give text and output bytes identical to the
  CPU path with no tag mismatch, while cores 6 and 7 rewrite codes at the same
  addresses.
- A job with a lane removed from the grid reports a missing tag.
- GPU time per layer shape, against a single-load latency probe, gives the
  instruction rate per MAC, the load latency and the resident tasks, and
  shows whether the per-store data fences or the per-load waits limit the
  loop.
- At the production output address, boot clears the tags and EL1 cache
  maintenance completes over the read-only region. The relocated permission
  probe above does not establish this production-address result.
