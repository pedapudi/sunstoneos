# Design: exact integer sums from the NPU for mimus linear layers

Status: **design; no code.** All work runs on the host: the pinned compiler,
the SDK simulator, the native NBG decoder and assembler, the session host
model and Unicorn instruction emulation. No board or NPU executes anything.
Each result ends at implemented and host-modeled, with physical behavior
pending.

## What a bit-exact layer needs

A mimus linear layer multiplies int16 activation codes `x` (|x| ≤ 32,767,
one float32 scale `s` per row) by int8 weight codes `w` (one fp16 scale
`ws[n]` per output channel). It forms the exact integer sum
`S[n] = Σ_k x[k]·w[k,n]` over the reduction length `K`, then computes
`(float(S[n]) * ws[n]) * s + bias[n]` in float32
([npu-mimus-linear.md](../npu-mimus-linear.md)). The NPU matches this only by
returning `S` exactly; the CPU then applies the read-out unchanged.

- **Range.** |S| ≤ 32,767·127·K = 4,161,409·K, so a signed 32-bit sum holds
  for K ≤ 516. The benchmark model has 92 int8 layers: 74 with K of 9 or 176,
  and 18 with K of 704, 1,584 or 3,520. mimus marks a layer `fits32` when
  |S| < 2^31 and otherwise accumulates in 64 bits.
- **Scale.** A compiled NBG has one static input scale, while mimus scales
  each row separately. A layer that takes codes at unit scale and leaves `s`
  to the CPU read-out avoids this conflict.

## Determination in three steps

Steps run in order. If step 1 finds an INT32 code that is exact in the
simulator, that form is the exact layer and step 2 is not built: it needs a
quarter of the NPU work and no digit split. Step 2 runs only after step 1
refuses.

**1. Probe for an INT32 output encoding.** The INT32 form of the query
projection and of the second feed-forward linear already fails. The SDK's
graph verification returns status −3 ("cannot be supported"), and the
compiler reports it as "Fatal model generation error: 64768": exit status
253, which is −3 as an unsigned byte, inside a wait status. Both lines are in
`~/.cache/sunstoneos/build/audit-npu-runtime/compiler/work/raw-*/export.log`.
The pinned SDK's NN configuration also lists no 32-bit data type. The probe
settles the question from the descriptor:

- Compile the query projection with UINT8, INT8 and INT16 outputs and
  everything else equal. Diff the 192-byte NN records and the
  neural-transpose records to locate the output data-type, post-multiplier
  and post-shift fields.
- Write each unused data-type code into a copy of the INT16 graph, recompute
  the checksums with `checks/nbg-checksum-reference.py`, and run the copy in
  the simulator. Accept a code only if every sum of the three studied layers
  comes back exact.

Patched graphs are simulator-only probe artifacts. They are never added to
the session catalogue or run on the board, because catalogue admission rests
on reviewed compiler output ([npu-session.md](../npu-session.md)). Running
an accepted code on the board would need its own admission design. A refusal
is recorded in [npu-mimus-linear.md](../npu-mimus-linear.md) with the field
offsets and simulator logs.

**2. Exact partial sums at unit scale.** At unit output scale with no bias,
an INT16 output is exact when every sum lies within ±32,767. The INT16-input
study showed that the engine sums exactly and rounds once at output. For the
INT8-input, INT16-output layer below, exactness at unit scale is unproven
until the simulator run and the range fixture pass.

- **Digits.** The CPU writes `x = d0 + 16·d1 + 256·d2 + 4096·d3`, with
  balanced low digits `d0, d1, d2` in [−8, 7] and the top digit `d3` equal
  to the remainder, in [−8, 8]. The top digit reaches +8 for x = 32,767.
- **Chunks.** The reduction is cut into C chunks of L ≤ 32 terms. Each
  partial `P[j,c,n] = Σ_{k in chunk c} d_j[k]·w[k,n]` is bounded by
  L·8·max|w|. The tool refuses a build when any weight code is −128 or when
  L·8·max|w| > 32,767.
- **Graph.** One INT8 input, one fully connected layer and one INT16 output
  per chunk, with unit scales, zero points of 0 and no bias. Input c has
  shape [4R, L] for R activation rows; rows 4r to 4r+3 hold the four digits
  of row r. Output c has shape [4R, N]. If the compiler wraps each layer in
  neural-transpose steps, as it does for the INT16 layer, the transposed
  copies live in internal SRAM and add no DDR buffer.
- **Recombination.** The CPU forms `S[n] = Σ_j 16^j·Σ_c P[j,c,n]` and applies
  the mimus read-out with the row's own `s`. For K = 176 every intermediate
  is at most 6·30,480·(1 + 16 + 256 + 4,096) = 799,002,720 < 2^31, so
  recombination uses int32 NEON lanes. Only a layer whose bound exceeds 2^31
  would need int64, and this package builds none.

For the query projection (K = N = 176), six chunks of 29 or 30 terms give
six inputs, six outputs and 24 partials per output channel, with largest
partial 30·8·127 = 30,480. That fits the existing assembler
(`nbg_assemble_layout`: at most eight inputs and eight outputs). The NPU does
four times the multiply-accumulates of the INT16 layer. Base 4 would give 16
partials per channel but double the NPU work again, so the digit base is
fixed at 16. Layers with K of 704 or more need more than eight chunks and are
not built.

`tools/npu-mimus-linear.py` gains a `--digits` form that writes this graph,
the digit input and the recombination reference. `checks/npu-mimus-kernel.cc`
recombines the simulator's partials and compares the read-out bytes with the
unmodified mimus kernel `MimusGemmS16FC`. A range fixture drives partials to
+30,480 and −30,480 and includes x = +32,767 and x = −32,767.

**3. Fallback: measure the INT16 path.** Step 3 runs if the compiler refuses
the step 2 graph, the assembler refuses its operations, the simulator output
is not bit-exact, or the cost comparison below fails. The INT16 graph then
becomes a separate recorded variant; the shipped model and CPU kernel stay
unchanged. The accuracy model extends mimus `train/static_scales.py`, which
already simulates static per-tensor input scales with calibration
(`--calib`), saturation and the same scoring. Two pieces are added: the bias
rounded to an int32 code in units of the input scale times `ws[n]`, and one
rounding of each output to the INT16 step at the NPU output scale, taken
from the site's calibrated output range. Scoring uses `train/compare_wer.py`
with 10,000 bootstrap resamples on dev-other, dev-clean and replay. A set
passes when the 95% interval of the relative WER change against fp32
contains 0 and the point estimate is at most +1% relative
(`docs/int8_activations.md`). mimus reserves test-clean and test-other for
milestones its owner calls, so the dev sets serve as held-out data.

## Runtime path on the session runtime as it is today

The budgets below assume the session runtime ([npu-session.md](../npu-session.md))
as it runs today. One job runs one layer for R activation rows. The
comparison uses R = 8: the live stream's chunk of eight encoder frames (mimus
`kStreamConvChunk`) and the row count of the catalogued INT16 layer.

1. The client task on core 6 or 7 quantizes each row as today, writes the
   digits into its own buffer, and sends SUBMIT with six spans totalling
   704·R bytes. `npu_session_send` refuses spans inside the session
   allocation, clears the working pool (expected empty for this graph) and
   copies the spans into the READ buffers.
2. The service worker on core 0 receives SUBMIT. Each job publication runs
   `model_intact(s, m, 0)`: SHA-256 over the NN descriptor and coefficient
   buffers (about 32 KB for this layer), a digest of the command stream and
   a check of every later word of the 512 KiB fetch allocation. `live_mappings`
   translates every page of the MMU arena and of every buffer in the session
   group, and `canonical` rereads every page-table word. Publication then
   cleans each whole READ buffer and cleans and invalidates each whole WRITE
   buffer.
3. The NPU reads the inputs and coefficients and writes the partials into
   the WRITE buffers. Completion reaches the worker by task notification
   from the bound interrupt; the worker's one-tick wait while the controller
   is active remains the timeout path
   ([performance architecture](../performance-architecture.md) 1.3).
   `a733_npu_memory_finish` translates every page again and invalidates the
   whole WRITE buffers.
4. The client's held WAIT returns DONE. `npu_session_output` copies the six
   outputs, 8,448·R bytes, into the client's buffer. The client sends REAP,
   which returns the buffers to the session. A job therefore takes three
   round trips: SUBMIT, WAIT and REAP.
5. Recombination reads the client's copy and writes only the mimus
   activation buffer. The CPU never writes the WRITE buffers, so the
   publication's clean-and-invalidate of them finds no dirty line from this
   path.

The runtime allows one job per model, and the next layer needs this output,
so the client blocks from SUBMIT until the output is copied. The two cores
exchange only service queue messages. Session fields are written by the
registered client task alone, and the queue's ordering makes the copied
inputs visible to the worker before publication cleans them. This design
adds no lock, no tick wait and no state written by two cores. Failure
handling is the runtime's own; within this package a refusal, mismatch or
failed comparison is a recorded result that touches no device.

## Reuse and removal

- **Reused:** the fixture, quantize-file and comparison stages of
  `tools/npu-mimus-linear.py` and `tools/npu-mimus-linear.sh`; the host build
  of the unmodified mimus kernel in `checks/npu-mimus-kernel.cc`;
  `nbg_model_open` and `nbg_assemble_layout`, unchanged, to lay out the new
  graph; the checksum reference for the probe; mimus `train/static_scales.py`
  and `train/compare_wer.py` for step 3; the Unicorn harness
  `tools/aarch64_kernel_check.py` on the mimus `port/sunstoneos` branch.
- **Removed:** the `--raw-sums` INT32 form of the tool. The descriptor probe
  replaces it as the INT32 evidence; its refusal logs stay in the build cache.
- **New, owned:** `checks/npu-mimus-cost.c`, a native host check that
  assembles the INT16 and digit layouts with `nbg_assemble_layout` and
  derives each job's event counts from the runtime code paths in items 1–4;
  and `checks/npu-mimus-digits.cc`, the digit split and recombination,
  compiled with the backend's AArch64 flags for instruction counts and later
  handed to the mimus engine owner.

## Performance budgets

Figures are per job at R = 8 for the query projection. Event counters come
from `checks/npu-mimus-cost.c`; instruction counters come from Unicorn runs
of the compiled code. Baselines are counted for the catalogued INT16 layer on
the same runtime.

| Metric | Baseline (INT16 layer) | Budget (digit layer) | Counter |
| --- | --- | --- | --- |
| Read-out bytes equal to `MimusGemmS16FC` | None bit-exact; largest error half a step | All outputs of the three studied layers and the range fixture | mismatch count in `checks/npu-mimus-kernel.cc` |
| NN multiply-accumulates | 247,808 | At most 991,232 | MACs from the compiled graph's tensor shapes |
| CPU bytes copied in by SUBMIT | 2,816 | 5,632 | `copy_in_bytes` |
| CPU bytes copied out by `npu_session_output` | 2,816 | 67,584 | `copy_out_bytes` |
| Cache maintenance | Whole READ and WRITE buffers | Whole buffers, at most the page-rounded tensor allocations | `clean_bytes`, `clean_invalidate_bytes`, `invalidate_bytes` |
| Per-job verification | Counted from the code | The INT16 figures plus the growth from larger coefficient and tensor allocations | `digest_bytes`, `scan_words`, `translations`, `table_words` |
| Queue round trips | Three | Three | `round_trips` |
| CPU DDR bytes read and written | Counted from the rows above | Counted from the rows above | `ddr_read_bytes`, `ddr_write_bytes` |
| Client instructions for digit split and recombination | None | Counted | `split_insns`, `recombine_insns` |

**End-to-end comparison and pass condition.** The exact path replaces the
packed NEON kernel on an A76 core for the same 8 rows. It passes only when
both of these hold:

- total CPU cycles per job, client plus service worker, are below the A76
  kernel's cycles for 8 rows;
- the client core's time from SUBMIT to the copied output is below the A76
  kernel's time for 8 rows.

Every unit cost below is an estimate, chosen at the expensive end so that a
pass does not rest on optimistic figures. Exact-path code is charged one
cycle per instruction. Each translation is charged 100 cycles, the upper end
of section 1.5's estimate. Each cache line maintained is charged 20 cycles,
each CPU DDR byte half a cycle, and each queue hop 2,000 cycles for a context
switch. NPU execution is charged from the simulator's reported cycle count at
the NPU clock. The A76 kernel is charged its Unicorn instruction count
divided by two, crediting both NEON pipes. The only measured kernel figure is
the board NEON run: 0.28 billion multiply-accumulates per second on one core
at 494 MHz, about 55,000 cycles per row of this layer
([mimus.md](../mimus.md)). The A76 has not been measured.

With today's runtime the comparison is expected to fail by about an order of
magnitude. The per-job digest of about 32 KB at an estimated 15–25 cycles per
byte costs 0.5–0.8 million cycles, while the A76 kernel for 8 rows is an
estimated 60,000–120,000 cycles. The record states the computed shortfall,
the rows per job at which the two paths would break even, and the same
comparison for the INT16 variant. It also states the comparison with the
runtime changes below applied, as input to that runtime design; that figure
is outside this package's pass condition. A failed comparison sends the work
to step 3.

## Changes outside owned files

- **Session runtime and NPU memory (owners of `npu_session.c` and
  `a733_npu_memory.c`), each a separate design.** An accessor that returns
  READ and WRITE buffer pointers only while the model is READY with no job,
  revoked at SUBMIT and on any fault (sections 1.6 and 2.2). Range
  maintenance inside `a733_npu_memory` over the tensor bytes: clean READ
  ranges and clean-and-invalidate WRITE ranges before publication, and
  invalidate after finish (2.3). A completion reply that also performs REAP
  and keeps the ownership return explicit. Removal of the per-job digest,
  stream scan and per-page translations once read-only mappings cover the
  same writers (1.4, 1.5); those mappings need the primary agent's
  page-table work.
- **Session host model (`checks/npu-session.c`).** The event counters named
  above, run on the catalogued INT16 layer, so that the counts derived in
  `checks/npu-mimus-cost.c` become measured. Until then the baseline is
  counted.
- **Session catalogue.** The digit graph is registered only after it passes
  the simulator and the comparison.
- **mimus repository.** The two additions to `train/static_scales.py` for
  step 3, and an instruction-count option in `tools/aarch64_kernel_check.py`
  for the packed kernel and the digit functions.

## Left out

- **A shader kernel accumulating in 32 or 64 bits.** The SDK has none for
  this layer, the target has one shader core, and no throughput advantage
  over the A76 NEON kernel is established.
- **A two-pass high-part and residual scheme.** It needs a per-row bias,
  while bias is per output channel; rewriting coefficients per job would
  fail the per-job coefficient digest.
- **FP16 output.** Its 11-bit significand cannot hold the sums.
- **Layers with K of 704 or more, and the mimus engine change.** They follow
  once one layer passes here.
- **Board runs.** One scheduled NPU session, after catalogue registration,
  would run the exact layer on the fixed fixture and the range fixture. It
  passes when the partials are byte-equal to the simulator's and completion
  arrives by interrupt. The same session records the per-job wall time from
  SUBMIT to copied output and the A76 kernel's time for the same 8 rows.
