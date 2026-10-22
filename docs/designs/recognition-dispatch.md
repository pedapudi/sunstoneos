# Design: placing mimus linear layers on the CPU, GPU or NPU in the OS image

Status: **design**; no code exists. Each item ends at implemented and
host-modeled, with physical behavior pending until the board evidence at the
end exists. It follows [performance-architecture.md](../performance-architecture.md)
and depends on four unmerged designs: the GPU runtime, the GPU integer matrix
product for mimus, the device memory classes and the rail ledger.

The integrated OS image ([os-image.md](../os-image.md)) runs mimus on one CPU
core. On the bench recording (8.87 s of audio, one Cortex-A55 at 494 MHz, NEON
kernels) the linear layers take 14.894 s of 19.476 s and perform 4.17 billion
multiply-accumulates ([mimus.md](../mimus.md)); the goal allows 0.4435 s.
The bench models hold 186 linear tensors with int8 weights and int16
activations, 13,090,704 bytes as stored. For each, mimus quantizes every
activation row with its own scale `s[r]`, forms exact integer sums, and then
computes `(float(sum) * ws[n]) * s[r] + bias[n]` in float32. This last step
is the **read-out**. A device that returns the exact sums leaves the read-out
on the CPU, so the result is bit-identical. The GPU can return exact sums; the
NPU returns INT16 codes half an output step from the mimus result
([npu-mimus-linear.md](../npu-mimus-linear.md)).

## A fixed table places each tensor

A host tool reads the model files at build time and writes one table per model
SHA-256, naming each tensor's engine. A tensor goes to the GPU only when:

- the GPU kernel's accumulator holds its largest possible sum, 32,767 times
  its largest absolute weight-row sum (mimus bounds every sum below 2^39);
- its K and N meet the kernel's shape rules;
- its weights fit the reservation.

A tensor goes to the NPU only when the NPU returns exact sums or the build selects the
approximate NPU variant. That variant is in no default build, and
`link-check.json` records it so that its output is never reported as parity.

Two build options, off by default until board evidence exists, select the
engines: `SUNSTONE_A733_OS_GPU_LINEAR` and `SUNSTONE_A733_OS_NPU_LINEAR`.
Without them the image is unchanged. At run time the dispatcher compares two
per-call values: the row count against the staging size, and `rows × K × N`
against the table's minimum, below which the fixed cost of a device job
exceeds the work. The minimum starts from the GPU package's per-layer estimate
against the CPU's 280 million multiply-accumulates per second; a board
measurement replaces it.

## Data flow and ownership

The pinned engine offers only `ParallelFor` and one asynchronous item, so it
cannot hand a layer to another engine. It needs one hook, to be filed as a
mimus issue beside issues 61 to 65:

- `claim(tensor, rows)`: whether the platform takes the call. If so, the engine
  quantizes all rows into one code buffer in its arena, also on the fused paths
  that otherwise quantize four rows at a time.
- `sums(tensor, codes, rows, ldx, fits32, out)`: fills `out` with exact sums,
  int32 when `fits32` (the engine's existing flag) and int64 otherwise, or
  returns false.
- After true, the engine applies its read-out in the order of `GemmCombine`,
  with bias, residual accumulation and column sinks. After false, it runs its
  existing kernel on the same codes, which gives the same bits.

The recognizer task runs at EL0 and calls the hook itself. One GPU call:

1. The dispatcher finds the table row by the tensor's data address, a binary
   search over at most 186 rows, and reads the session's revocation flag with
   one acquire load.
2. It sends the GPU runtime's 32-byte submit message: layer slot, row count,
   and the offsets of the code and sum buffers inside the recognizer arena,
   which the session registered at open. The message carries no pointers.
3. The GPU worker checks each offset and length against the registered window,
   copies the codes into input staging with one `memcpy`, cleans those bytes
   and kicks the job.
4. On the GPU interrupt, the worker invalidates the written sum bytes, copies
   them into the arena with one `memcpy` and sends one completion message.
5. The recognizer wakes once and the engine applies the read-out.

The recognizer lends both arena buffers from step 2 to step 5 and writes
neither meanwhile. The weights are device-owned after admission.

**Admission.** A `gpu-linear` start entry runs after the `mimus` entry. It
relies on the recognizer's existing SHA-256 of the model assets and adds no
hash. It opens the mimus GPU session, copies each placed tensor once into the
weight reservation in the kernel's layout, cleans it once and registers the
arena window. Weights load once per boot, so every recognition is warm.

**NPU.** The NPU build uses the NPU session runtime
([npu-session.md](../npu-session.md)) on the same path. Its catalogue holds at
most eight compiled models with built-in weights, so at most eight tensors can
be placed. The NPU exact-sum study supplies the result kind and, for INT16
output, the conversion to sums and its accuracy on held-out mimus audio.

## Device completion arrives by interrupt

GPU INTID 95 and NPU INTID 97 join the interrupt table at priority `0xd8`,
routed to core 2, which also runs both workers. Completion work then stays off
core 0, which runs USB0, thermal and DMA, and off the recognition cores. The
recognizer blocks on its task notification with a deadline of the session's
`max_job_us` plus 10 ms and polls nothing. Until INTID 95 delivery is qualified,
the GPU worker keeps the GPU runtime's 4 ms timed wake as a fallback.

## Memory placement and cache policy

| Buffer | Placement | Maintenance |
| --- | --- | --- |
| Weights | `gpu` class, 14 MiB in seven 2 MiB blocks, 2 MiB GPU pages | One clean at admission |
| Input and sum staging | `gpu` class, sized at build for the largest placed call at the longest utterance | Clean written code bytes; invalidate written sum bytes |
| Lent codes and sums | Recognizer arena at `0x46000000`, write-back, inner shareable | None; CPU traffic is coherent |

Maintenance uses `aarch64_device_cache` by address over the bytes written.
The arena grows by one sum buffer of at most `rows × N × 8` bytes against a
bench peak of 1,761,840 bytes in 16 MiB. All buffers are allocated at admission.

## Synchronization across cores

The recognizer has no core affinity today; it moves to the A76 cores once the
multicore work admits them. FreeRTOS queues order the two messages with
release and acquire semantics, so the worker reads the codes after the
recognizer stores them. Each job costs two cross-core wake-ups, one per
message. The dispatcher's counters have one writer, the recognizer.

**Supervision.** Workers block without a timeout while idle, so a progress
timestamp would need four idle wake-ups a second. Instead, each service has a
busy word on its own 64-byte line. The client stores the submit time there
with one atomic store; the worker clears it with the completion. The
supervising task treats an idle service as live and stops renewal when a busy
word is older than 1 s. A dead worker is caught within 1 s of its next job.

## Failure classes

| Failure | Class (section 1.8) | Action | Retained |
| --- | --- | --- | --- |
| BUSY, DENIED, closed session, rows over staging | Refused before device access | CPU runs the call | Nothing |
| RESET after the GPU runtime's bounded reset | Known terminal state | CPU reruns from the same codes; CPU until the next recognition | Nothing here |
| LOST, after the worker finished with the arena | Unknown device state | CPU for the session | GPU runtime keeps its leases |
| No completion by the deadline | Unknown; the worker may still write the arena | `board_fail` | Arena; the watchdog resets the resident image |

A CPU rerun keeps the transcript correct but is not accelerator use; the
dispatch record counts every fallback.

## Reused and removed

- **Reused:** the image's interrupt table, start list, admission and
  supervision; the GPU runtime's sessions, messages, hang reset and EL0 copy;
  the NPU session runtime; the device memory classes; the rail ledger and
  domain owner; the recognizer's asset hash; the mimus read-out.
- **No runtime arbiter.** The GPU and NPU take their rails and domains from
  the rail ledger, which counts references. The start list fixes reservation
  order (NPU before GPU, as in [accelerator-composition.md](../accelerator-composition.md)),
  and the replay in `checks/device-memory-capacity.c` proves the reservations
  fit. An arbiter would repeat both checks.
- **Not linked:** `apps/accelerators/gpu.c`, which polls each tick and returns
  4 KB in 31 replies, and any second GPU and PCK resource manager, the blocker
  that [npu-session.md](../npu-session.md) records for board-derived images.

## Budgets and measurement

Per placed call with `rows` rows, K inputs and N outputs. The CPU-only image is
the baseline: zero device work per call.

| Metric | Budget | Measured by |
| --- | --- | --- |
| Bench output | Identical features, log-probability CRCs and text to the host reference, nominal and with a refusal and a RESET injected | Host mimus build with the hook and an exact-sum GPU model |
| Placement work | One binary search over at most 186 rows, two compares, one acquire load | New `checks/os-image-dispatch.c` |
| Messages; recognizer wake-ups; tick waits | 2; 1; 0 | Same model, queue counters |
| Copies | One `memcpy` per code byte and per sum byte | `memcpy` wrap |
| Bytes moved per row | `2K + 4N` when `fits32`, else `2K + 8N` | Same |
| Cache lines | Written code bytes / 64 cleaned; written sum bytes / 64 invalidated | `aarch64_device_cache` wrap |
| Translations, hashes, allocations | 0 per call | Wraps, pool counters |
| Weight copies | One of 13,090,704 bytes per boot; 0 per recognition | Admission model |
| Idle worker wake-ups | 0 (no worker exists today) | `checks/os-image-model.c` |
| Dead worker detection | Renewal stops within 1 s of its next job | Same |
| Device reservations | Fit the replay; free extents unchanged after open and close (today: one 2 MiB pool) | `checks/device-memory-capacity.c` |
| Bench calls on the GPU | Every placed call above the minimum; no fallback | Dispatch record |

The record is one line beside mimus's worker record,
`DISPATCH gpu=G npu=P cpu=C refused=R reset=T lost=L`; the host parity check
must accept it.

## Left out

- **CPU and GPU splitting one layer's columns.** The recognition core idles
  during a GPU job, but the split ratio needs board timings. The hook can take
  a column range later without changing the read-out.
- **Two jobs in flight.** Overlap needs an asynchronous hook and dependency
  information that mimus does not expose.
- **One job for the query, key and value projections.** Board-measured job
  overhead decides whether the larger engine change pays.
- **A read-only EL0 mapping of sum staging,** which would remove the output
  copy. It needs an EL0 mapping class from the memory architecture design.
- **Thermal load shedding.** The image ignores THROTTLE; CRITICAL still ends
  the session.

## Changes outside this package

- **mimus:** the `claim` and `sums` hook, with host parity checks.
- **GPU runtime:** EL0 submits with offsets in a registered window, copy-out of
  the written sum bytes, and the mimus session's `max_job_us`.
- **GPU integer matrix product:** exact int32 and int64 sums from int16 codes
  and int8 weights, its shape rules and a time estimate per bench layer.
- **Memory architecture:** 14 MiB of `gpu` class weights plus staging in the
  capacity replay.
- **NPU exact-sum study:** the result kind and, if approximate, its accuracy.
- **Bluetooth voice recognition package:** it also edits `apps/os/os.h` and
  `docs/os-image.md`; this package only adds declarations and sections there
  and leaves `apps/os/headset.c` alone.
- **Primary agent:** nothing required.

## Board evidence that would confirm the design

- The GPU build recognizes the bench recording with text and CRCs identical to
  the host, and the dispatch record shows every placed call on the GPU.
- INTID 95 arrives on core 2 for every job, which removes the 4 ms fallback.
- The linear stage time against the CPU-only image at the same clock, which
  sets the per-call minimum.
- A resident run of at least one hour with idle workers, in which the
  watchdog renewal never stops.
