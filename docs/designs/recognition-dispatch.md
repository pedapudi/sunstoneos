# Design: placing mimus linear layers on the CPU or GPU in the OS image

Status: **design**; no code exists. Each item ends at implemented and
host-modeled, with physical behavior pending until the board evidence at the
end exists. It follows [performance-architecture.md](../performance-architecture.md).
It depends on three unmerged designs: the GPU runtime, the GPU integer matrix
product for mimus (`gpu-mimus-compute.md`) and the device memory classes.

The integrated OS image ([os-image.md](../os-image.md)) runs mimus at EL0 on
the CPU. On the bench recording (8.87 s of audio, one Cortex-A55 at 494 MHz,
NEON kernels) the linear layers take 14.894 s of 19.476 s and perform 4.17
billion multiply-accumulates (MACs) ([mimus.md](../mimus.md)). The goal allows
0.4435 s. Mimus forms exact integer sums of int16 activation codes and int8
weights, then converts each sum to float and applies scales and bias in a
fixed order, the **read-out**. The GPU integer matrix product returns the
exact sums and leaves the read-out on the CPU, so output stays bit-identical.

This package adds that GPU path to the OS image and decides, per tensor, how
much of each linear call the GPU computes. It uses the data path of
`gpu-mimus-compute.md`; the adjustments it needs are listed under "Changes
outside this package".

## One data path, with zero copies

- **Hook.** The mimus port already calls `backend::RunPackedI8Block`
  (mimus 3e05158, `kernels.cc` line 1881), compiled in today only for the
  ESP32-P4. The Orange Pi backend implements it as `gpu-mimus-compute.md`
  describes: one block over every output channel, residual `alpha` passed in,
  and the number of completed rows returned so the CPU computes the rest. The
  hook also receives the tensor's ordinal in the model file, which the port
  adds to `TensorView` at load.
- **Weights.** The GPU maps the base and overlay model regions (16 MiB, at
  the extents in the generated `assets.h`) read-only and reads mimus's own
  int8 layout in place.
- **Activations.** Quantized rows stay where mimus writes them in the arena,
  which the GPU maps read-only.
- **Sums.** Each of two job slots owns a 2 MiB output region that the GPU
  writes. The EL0 recognizer reads sums in place and runs mimus's per-row
  read-out. Device pools have no EL0 mapping, so both regions lie inside the
  recognition arena (layout below).
- **Jobs.** The recognizer writes a 48-byte descriptor into its slot's entry
  in the descriptor page and sends the GPU runtime's 32-byte submit message.
  The worker copies the descriptor, then takes K, N, the output width and the
  tensor offset from its own copy of the placement table. It derives the
  activation bytes (`rows × stride`) and sum bytes (`rows × n_gpu × width`)
  and checks both with the overflow-safe helpers of `a733_range.h`: the
  activations inside the arena's GPU-readable part, the sums inside the slot's
  region. It cleans the activation bytes, writes the argument record and kicks.
- **Overlap.** A call becomes jobs of at most `job_rows` rows, alternating
  between the two slots. The recognizer reads out one slot while the GPU
  computes the other; a call of at least 64 rows always splits into at least
  two jobs.

**Bounded row blocks.** Every layer except the subsampling convolution
already holds its full code buffer in the arena on the CPU path, and its call
reaches the hook unchanged. The subsampling convolution is 1,584 → 176 over
4,440 rows, and its full code buffer would be 14,065,920 bytes. The port
therefore produces its rows in blocks of 240 into two fixed code buffers of
760,320 bytes and calls the hook once per block: 19 jobs carrying 1.24 G MACs.

**Output width.** Mimus sets `fits32` when `kp × 32,767 × 128 < 2^31`, where
`kp` is the padded K, so K ≤ 512. The 34 bench calls with K above 512 (both
subsampling layers and the 32 second feed-forward linears) carry 2.26 G of the
4.17 G MACs, 54%. The dependency kernel writes int64 for those and int32
otherwise from one program, so this design adds no kernel variant.

## A fixed table places each tensor

A host tool in `apps/os/` reads the model files whose SHA-256 the build embeds
in `mimus_asset_sha256` and writes one row per tensor, indexed by ordinal:

- **engine:** CPU or GPU. An NPU value is reserved, with no code behind it.
  GPU requires int8 weights with no int4 rescale, and K and N within the
  kernel's shape rules.
- **min_rows:** `ceil(32,768 / (K × N))`. Break-even is about 29,000 MACs per
  call, from the 280 M MAC/s A55 rate, the GPU estimate of 7.3 G MAC/s (0.57 s
  for 4.17 G MACs at 400 MHz) and two jobs of 50 µs fixed cost. The starting
  minimum rounds that up to 32,768 MACs, at most 2 rows for every bench tensor.
- **n_gpu:** the GPU computes output channels `[0, n_gpu)`, a multiple of 32
  or N. Mimus's existing `ParallelFor` workers compute `[n_gpu, N)` on the CPU
  at the same time. The table starts with `n_gpu = N`. The smallest split of a
  176-channel call gives 16 channels to the CPU and pays only above 0.73 G
  MAC/s, which the A55 does not reach. A board measurement of the A76 rate
  changes only the table.
- **job_rows:** `min(floor(2^26 / (K × n_gpu)), floor(2 MiB / (n_gpu × width)))`,
  from the dependency's per-job cap of 64 Mi MACs and one output region.

The per-call test is one bound check on the ordinal, one compare
`min_rows ≤ rows`, and one load of the dispatcher's own session state. A
column split needs the CPU kernel to run while GPU jobs are in flight, which
the synchronous hook cannot express. For tensors with `n_gpu < N` the port
calls a two-part form of the same hook: begin submits the GPU jobs and
returns `n_gpu`; end reads out and returns completed rows.

**Admission follows authentication.** The `gpu-linear` start entry runs before
the scheduler. It creates the GPU worker and opens nothing. The recognizer
hashes the model assets at EL0 (`apps/mimus-board/recognition.cc`), and sends
OPEN only after every hash matches. The worker then:

- leases the model regions and the arena's GPU-readable part read-only, and
  the output regions read-write;
- cleans the model regions once (16 MiB), and cleans and invalidates both
  output regions once (4 MiB), so no dirty line from loading or zeroing is
  later evicted over GPU writes;
- copies the placement table into its own memory.

No byte is copied or hashed again. A model upload closes the session; the
next successful hash reopens it. A refused or failed open puts the dispatcher
on the CPU for the boot.

## Memory placement and cache policy

| Region | GPU | CPU | Maintenance |
| --- | --- | --- | --- |
| Model regions, 16 MiB | Read-only | Read; written only by upload | Clean once at open |
| Arena `0x46000000` + 0, 12 MiB − 4 KiB: mimus's arena span | Read-only | EL0 read-write | Clean the job's activation bytes at submit |
| Descriptor page, the 4 KiB below 12 MiB | Read-only (same mapping) | EL0 writes one 48-byte entry per job | None; the worker reads it after the queue hand-off |
| Two output regions, 12–16 MiB | Read-write while DEVICE | EL0 reads only | Clean and invalidate at open; invalidate written sum bytes at completion |

Each region starts on a 2 MiB boundary except the descriptor page, which
shares the read-only mapping. The OS recognizer passes mimus a 12 MiB − 4 KiB
arena span in place of 16 MiB. Its bench peak is 1,761,840 bytes; the two
subsampling code buffers add 1,520,640 bytes. The output regions and code
buffers have fixed sizes, so a longer utterance adds jobs and no memory. GPU
pages are 4 KiB until the GPU runtime's large-page board item passes: 8,192
leaf entries, then 16 with 2 MiB pages.

## Interrupt, cores and synchronization

Under `SUNSTONE_A733_OS_GPU_LINEAR`, off by default, the interrupt table gains
INTID 95 (SPI 63) at priority `0xd8` and the start list gains `gpu-linear`.
The recognizer, the GPU worker and the INTID 95 route share the recognition
core: core 6 once the A76 cores are admitted, core 2 until then. Core 0 keeps
USB0, thermal and DMA. Affinities are set with `xTaskCreateStaticAffinitySet`,
the recognizer's in [recognition.patch](../../apps/os/recognition.patch). The
worker runs above the recognizer's priority. Without the option the image is
unchanged.

On one core a job costs no inter-processor interrupt. The submit wakes the
worker by a same-core queue send. The completion interrupt wakes the worker,
which posts one completion to the recognizer. FreeRTOS queues order the codes
before the worker's clean, and the worker's invalidate before the
recognizer's reads. When `ParallelFor` workers on other cores quantize rows,
the engine's existing join orders their stores before the submit, and
cleaning by address reaches lines held in other cores' caches. The
dispatcher's counters have one writer, the recognizer.

For the mimus session a submit gets exactly one reply: a refusal, or the
completion. The worker sends no ACCEPTED. With at most two jobs in flight the
worker owes at most two messages, within the reply queue's depth of three.

**Liveness.** The worker owns each job's deadline and always answers: OK,
RESET after the GPU runtime's acknowledged stop, or LOST. Its answer bound
from submit is `T_answer = 2 × T_clean + max_job_us + 4 ms + T_stop + 1 ms`:

- `T_clean` cleans the largest job's activations, 760,320 bytes, about 0.76 ms
  at an assumed 1 GB/s; the other slot's clean can precede it;
- 4 ms is the GPU runtime's hang detection after `max_job_us`;
- `T_stop` is the runtime's bound on its stop sequence.

The recognizer waits for each completion with a timeout of `T_answer + 10 ms`
and calls `board_fail` on expiry, which stops watchdog renewal. That timeout
is the only liveness check for the worker; a worker that dies while idle is
caught at its next job. The worker is not added to `apps/os/services.c`. The
recognizer's existing 3 s report window covers a recognizer that stops.

Until INTID 95 delivery is qualified, the worker keeps the runtime's 4 ms
timed wake: at most 3 timed wakes for the largest estimated job (about 9 ms),
and at most 25 at `max_job_us` = 100 ms. Time budgets apply only after
qualification.

## Failure classes

| Failure | Class (section 1.8) | Action | Retained |
| --- | --- | --- | --- |
| Refusal before device access: ordinal out of range, extent check, session closed, GPU off after its fourth reset | 1 | CPU computes the job's rows; a closed or off session puts the dispatcher on the CPU for the boot | Nothing |
| RESET after an acknowledged stop | 2 | Rows already read out stay; the CPU computes the rest of the call; the next call uses the GPU (the runtime allows three resets per boot) | Nothing; the GPU cannot write the weights or the arena span |
| LOST | 3 | CPU for the boot | Output regions stay device-owned; mimus never allocates from them |
| No answer by `T_answer + 10 ms` | Worker dead; unknown | `board_fail` | Arena and output regions; the watchdog resets the board |

The weights need no re-verification after RESET or LOST, because the GPU maps
them read-only. The CPU and GPU read the same verified bytes, so the GPU path
adds no integrity exposure beyond the CPU path. Read-only CPU mappings of the
model regions after the hash, which remove the remaining EL1 and EL0 writers,
are requested from the primary agent. A CPU fallback keeps the transcript
correct but is not accelerator use; the dispatch record counts every fallback.

## Reused and not linked

- **Reused:** `backend::RunPackedI8Block`, the read-out and the `ParallelFor`
  workers in mimus; the dependency's kernel, descriptors, two slots and
  read-out entry; the GPU runtime's sessions, deadlines and bounded reset; the
  image's interrupt table, start list and supervision; the recognizer's
  asset hash; the rail ledger and `checks/device-memory-capacity.c`.
- **No runtime arbiter.** The GPU takes its rail and domain from the rail
  ledger, which counts references, and the capacity replay proves the leases
  fit. An arbiter would repeat both checks.
- **Not linked:** `apps/accelerators/gpu.c`, which polls each tick and returns
  4 KB in 31 replies.

## Budgets and measurement

Counts are for the bench recording; the baseline is the CPU-only image. The
new host check `checks/os-image-dispatch.c` runs the dispatcher and a modeled
worker over the bench call list and over the longest capture the image
accepts. The dependency's `checks/gpu-mimus-layers` runs the pinned mimus host
engine with the GPU kernel executed as host C.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Output | CPU path | Features, log-probability CRCs and text identical to the host reference, nominal and with a refusal, RESET and LOST injected | `gpu-mimus-layers` with the dispatcher |
| Calls on the GPU | 0 | Every call with `rows ≥ min_rows`; all 4,440 subsampling rows, in 19 jobs | Dispatch record |
| Jobs per recording | 0 | At most 400 (376 from the shapes) | `os-image-dispatch` |
| Copies per weight, activation, sum byte | No device copy; 1 int8-to-int16 weight expansion per call | 0, 0, 0 | `gpu-mimus-layers` copy counter |
| Placement work per call | None | 1 index, 2 compares, 1 load; no search, multiply or flag load | `os-image-dispatch` |
| Messages; recognizer wake-ups; IPIs; tick waits per job | None | 2; 1; 0; 0 | Queue and notification counters |
| Worker wake-ups per job beyond the interrupt | None | 1, the submit; fallback as above | Same |
| `DSB SY` per job | None | 6, from three `aarch64_device_cache` calls, plus the runtime's doorbell barriers; `DMB` only inside the two queue operations | Barrier wraps |
| Cache lines per job | None | Activation bytes / 64 cleaned, sum bytes / 64 invalidated, at most 5 record lines; per recording 38,206,784 and 59,975,800 bytes | `aarch64_device_cache` wrap |
| Arena peak | 1,761,840 bytes | At most 3,282,480 bytes at any utterance length | Host mimus arena high-water mark |
| Read-out | Inside the CPU kernel | At most 12 instructions per output; 12,923,134 outputs | `gpu-mimus-readout` (Unicorn) |
| Per-job overhead, submit to kick plus completion to wake | No GPU path | At most 50 µs | Board item |
| Linear time per recording | 14.894 s measured | Goal 0.4435 s; estimate below | Board item |
| Dead worker detection | No worker | `board_fail` within `T_answer + 10 ms` of the submit | `checks/os-image-model.c` |
| GPU leaf entries | None | 8,192; 16 after large pages | `checks/pvr-gpu-vm.c` |

The record is one line beside mimus's worker record,
`DISPATCH gpu=G cpu=C refused=R reset=T lost=L`, which the host parity check
accepts.

**Time estimate.** GPU work is 0.57 s at 400 MHz. Job overhead adds 0.02 s
(376 × 50 µs), and exposed activation cleans about 0.02 s. Two-slot overlap
hides about half of the read-out's 155 M instructions. The other half is
exposed: about 0.16 s on an A55 at 494 MHz, or about 0.02 s on an A76 at
2.0 GHz at an assumed two instructions per cycle. The estimate is 0.77 s
before A76 admission and 0.63 s after; both miss the goal. Reaching 0.4435 s
needs one of three things: the GPU above its initial 400 MHz, a measured GPU
rate above the estimate, or CPU columns adding about 3.6 G MAC/s through
`n_gpu`.

New host cases in `checks/os-image-dispatch.c`:

- no session opens before the hash, and a failed hash leaves the GPU closed;
- a refusal, a RESET and a LOST each finish on the CPU with identical output;
- the largest job at its maximum clean time completes inside `T_answer`;
- a hang followed by an acknowledged stop ends in RESET without `board_fail`;
- a worker that never answers ends in `board_fail` at `T_answer + 10 ms`;
- the longest capture keeps the arena peak at its bound;
- a forced `n_gpu < N` produces identical output.

## Left out

- **NPU placement.** The pinned NPU compiler refuses the INT32 output that
  exact sums need, and its static input quantization needs one scale for all
  rows where mimus uses one per row ([npu-mimus-linear.md](../npu-mimus-linear.md)).
  NPU placement returns when the NPU exact-sum study shows exact sums from a
  build the pinned compiler accepts.
- **More than two jobs in flight, and read-out across several cores.** The
  two-slot overlap hides half of the read-out; revisit if the board shows
  more exposed.
- **One job for the query, key and value projections.** Board-measured job
  overhead decides whether the larger engine change pays.
- **Thermal load shedding.** The image ignores THROTTLE; CRITICAL still ends
  the session.

## Changes outside this package

- **mimus port:** the three hook changes of `gpu-mimus-compute.md`; the tensor
  ordinal in `TensorView`; 240-row blocks for the subsampling convolution; the
  two-part hook used when `n_gpu < N`.
- **GPU integer matrix product:** output regions in the arena's top 4 MiB,
  because the EL0 recognizer reads them; a read-only GPU mapping over the
  descriptor page, which keeps the 12 MiB part in 2 MiB blocks; byte counts
  from the worker's table copy.
- **GPU runtime:** one reply per submit for the mimus session; a stated
  `T_stop`; worker affinity and the INTID 95 route on the image's
  recognition core.
- **Memory architecture:** the arena's GPU leases in the capacity replay.
- **Primary agent:** A76 admission for the image; read-only EL1 and EL0
  mappings of the model regions after the hash; removal of the leading
  `DSB SY` in `aarch64_device_cache`, which brings the count to 3 per job.
- **Bluetooth voice recognition package:** it also edits `apps/os/os.h` and
  `docs/os-image.md`; this package only adds declarations and sections there
  and leaves `apps/os/headset.c` alone.

## Board evidence that would confirm the design

- The GPU build recognizes the bench recording with text and CRCs identical
  to the host, and the dispatch record shows every placed call on the GPU.
- INTID 95 arrives on the recognition core for every job, and each job's
  overhead is at most 50 µs, which removes the 4 ms fallback.
- Linear stage time against the CPU-only image at the same clocks, and the
  per-core CPU rate, which set `min_rows` and `n_gpu`; then the stage time
  with that split.
- A GPU write to the model region faults, and the GPU runtime's large-page
  item passes.
- An injected hang ends in RESET with correct output, and a resident run of
  at least one hour never stops watchdog renewal.
