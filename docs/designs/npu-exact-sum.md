# Design: exact integer sums from the NPU for mimus linear layers

Status: **host-tested**. The study runs on the host (pinned compiler, SDK
simulator, native NBG decoder and assembler, session host model and Unicorn
instruction emulation) and its result is recorded in
[npu-mimus-linear.md](../npu-mimus-linear.md). No board or NPU has executed the
exact layer. The study tools and checks live in the
[mimus repository](https://github.com/pedapudi/mimus/tree/port/sunstoneos-split/sunstoneos).

Results:

- **INT32 output encoding refused.** None of the eight three-bit output type
  codes returns 32-bit sums in the simulator. A fourth type bit, if one
  exists, was not located, so codes 8–15 are untested.
- **Digit layer exact, cost comparison failed.** Every simulator partial is
  exact for the query projection, the first feed-forward linear and the range
  fixture, and the read-out is bit-identical to `MimusGemmS16FC`. On the
  session runtime as it is, the exact layer costs 13.4 times the A76 kernel's
  CPU cycles.
- **INT16 variant passes.** It meets the float32 word-error-rate criterion on
  dev-other, dev-clean and the device replay in simulation. It is the layer the
  project uses; the shipped model and CPU kernel stay unchanged.

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

## Method

The study runs three steps in order. A step runs only when the previous one
refuses.

**1. Probe for an INT32 output encoding.** The SDK refuses INT32 outputs
for the query projection and the second feed-forward linear (graph
verification status −3), and its NN configuration lists no 32-bit data type.
The probe compiles the query projection with UINT8, INT8 and INT16 outputs,
diffs the 192-byte NN records and the neural-transpose records to locate the
output data-type, post-multiplier and post-shift fields, writes each unused
data-type code into a copy of the INT16 graph with repaired checksums, and
runs the copy in the simulator. A code is accepted only if every sum of the
three studied layers comes back exact. Patched graphs are simulator-only
artifacts. They never enter the session catalogue or run on the board, because
catalogue admission rests on reviewed compiler output
([npu-session.md](../npu-session.md)).

**2. Exact partial sums at unit scale.** At unit output scale with no bias,
an INT16 output is exact when every sum lies within ±32,767.

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
  of row r. Output c has shape [4R, N].
- **Recombination.** The CPU forms `S[n] = Σ_j 16^j·Σ_c P[j,c,n]` and applies
  the mimus read-out with the row's own `s`. For K = 176 every intermediate
  is at most 6·30,480·(1 + 16 + 256 + 4,096) = 799,002,720 < 2^31, so
  recombination uses int32 NEON lanes.

For the query projection (K = N = 176), six chunks of 29 or 30 terms give six
inputs, six outputs and 24 partials per output channel, with largest partial
30·8·127 = 30,480. This fits the assembler (`nbg_assemble_layout`: at most
eight inputs and eight outputs). The NPU does four times the multiply-accumulates
of the INT16 layer. The digit base is 16: base 4 would give 16 partials per
channel but double the NPU work. Layers with K of 704 or more need more than
eight chunks and are refused.

**3. INT16 variant.** The INT16 graph is a separate recorded variant. Its
accuracy model builds on mimus `train/static_scales.py`, which simulates static
per-tensor input scales with calibration, saturation and the same scoring, with
two additions: the bias rounded to an int32 code in units of the input scale
times `ws[n]`, and one rounding of each output to the INT16 step at the NPU
output scale, taken from the site's calibrated output range. Scoring uses
`train/compare_wer.py` with 10,000 bootstrap resamples. A set passes when the
95% interval of the relative WER change against fp32 contains 0 and the point
estimate is at most +1% relative (mimus `docs/int8_activations.md`). The dev
sets serve as held-out data because mimus reserves test-clean and test-other
for milestones its owner calls.

## Cost on the session runtime

One job runs one layer for R = 8 activation rows, the live stream's chunk of
eight encoder frames. The runtime path is [npu-session.md](../npu-session.md):
the client sends SUBMIT with six spans totalling 704·R bytes; the service
worker verifies the model (SHA-256 over the NN descriptor and coefficient
buffers, a command-stream digest and a scan of the 512 KiB fetch allocation),
translates every page of the MMU arena and the session buffers, and cleans or
invalidates each whole buffer; the NPU writes the partials; the client's WAIT
returns DONE and `npu_session_output` copies 8,448·R bytes; REAP returns the
buffers. A job takes three round trips. The exact path replaces the packed NEON
kernel on an A76 core, and passes only when both hold:

- total CPU cycles per job, client plus service worker, are below the kernel's
  cycles for 8 rows;
- the client core's time from SUBMIT to the copied output is below the kernel's
  time for 8 rows.

Unit costs are chosen at the expensive end: one cycle per instruction for
exact-path code, 100 cycles per translation, 20 per cache line maintained, half
a cycle per CPU DDR byte, 2,000 per queue hop. NPU execution is charged the
compiler architecture model's `CycleCount` estimates at 492 MHz, because the
SDK simulator reports zero cycles. The A76 kernel is charged its Unicorn
instruction count divided by two. The only measured kernel figure is the board
NEON run on one A55 core at 494 MHz ([mimus.md](../mimus.md)); the A76 is
unmeasured.

## Qualified figures

Per job, query projection, 8 rows. Evidence is in
`docs/evidence/npu-exact-sum.jsonl`.

| Quantity | INT16 layer | Digit layer | Record |
| --- | --- | --- | --- |
| Read-out floats differing from `MimusGemmS16FC` | 1,407 of 1,408 | 0 of 1,408 (0 of 5,632 for the first feed-forward linear) | `npu-exact-sum-digit-layer` |
| NN multiply-accumulates | 247,808 | 991,232 | same |
| CPU bytes copied in / out | 2,816 / 2,816 | 5,632 / 67,584 | same |
| Cache lines maintained | 192 | 2,688 | same |
| Bytes hashed with SHA-256 | 37,064 | 54,120 | same |
| CPU cycles, client plus worker | 1,619,380 | 2,231,903 | `npu-exact-sum-cost-comparison` |
| A76 kernel for the same rows | 166,177 cycles, 83.0 µs | same | same |

With the runtime changes below, the digit layer costs 105,571 cycles and 230 µs,
0.64 and 2.8 times the kernel, which still fails on time.

## Limitations and open work

- The exact layer does not pay off on the session runtime. Its cost is the
  per-job SHA-256 of about 54 KB and the scan of the command allocation.
  Making it competitive needs separate designs from the owners of
  `npu_session.c` and `a733_npu_memory.c`: an accessor for the READ and WRITE
  buffers valid only while the model is READY with no job; range cache
  maintenance over the tensor bytes; a completion reply that also performs
  REAP; and removal of the per-job digest, stream scan and per-page
  translations once read-only mappings cover the same writers.
- The digit graph is registered in the session catalogue only after it passes
  the simulator and the comparison.
- The board run (one NPU session on the fixed and range fixtures, byte-equal
  partials, interrupt completion, wall time from SUBMIT to copied output against
  the A76 kernel for the same 8 rows) is not performed; its prediction is the
  record `npu-exact-sum-board-session`.
- Layers with K of 704 or more and the mimus engine change follow once one layer
  passes.
- A shader kernel accumulating in 32 or 64 bits is not built: the SDK has none
  for this layer and the target has one shader core. A two-pass high-part and
  residual scheme fails because bias is per output channel and rewriting
  coefficients per job fails the per-job coefficient digest. FP16 output cannot
  hold the sums with its 11-bit significand.
